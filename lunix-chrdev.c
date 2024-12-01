/*
 * lunix-chrdev.c
 *
 * Implementation of character devices
 * for Lunix:TNG
 *
 * < Your name here >
 *
 */

#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mmzone.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>

#include "lunix.h"
#include "lunix-chrdev.h"
#include "lunix-lookup.h"

/*
 * Global data
 */
struct cdev lunix_chrdev_cdev;

/*
 * Just a quick [unlocked] check to see if the cached
 * chrdev state needs to be updated from sensor measurements.
 */

static int lunix_chrdev_state_needs_refresh(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	
	WARN_ON ( !(sensor = state->sensor));
	

	if(state->buf_timestamp < (sensor->msr_data[state->type]->last_update)){
		debug("Refresh: needs to be updated, return 1");
		return 1;
	}
	else{
		debug("Refresh does not need to be updated, return 0");
		return 0;
	}
}

/*
 * Updates the cached state of a character device
 * based on sensor data. Must be called with the
 * character device state lock held.
 */
static int lunix_chrdev_state_update(struct lunix_chrdev_state_struct *state)
{
	struct lunix_sensor_struct *sensor;
	uint32_t new_value;
	long fixed_value;
	unsigned long cpu_flags;
	
	debug("entering update\n");
	
	/*If no refresh is needed then stop and return "try again" */

	if(!lunix_chrdev_state_needs_refresh(state)){
		return -EAGAIN;
	}

	sensor = state->sensor;
	WARN_ON(!sensor);

	debug("Spinlock not locked yet\n");
	
	spin_lock_irqsave(&sensor->lock, cpu_flags);

	new_value = sensor->msr_data[state->type]->values[0];

	spin_unlock_irqrestore(&sensor->lock, cpu_flags);
	debug("Spinlock just unlocked\n");

	/*Whats our measurement type? Use lookup tables to make it decimal human readable*/

	switch (state->type){
	case BATT:
		fixed_value = lookup_voltage[new_value];
		break;
	case TEMP:
		fixed_value = lookup_temperature[new_value];
		break;
	case LIGHT:
		fixed_value = lookup_light[new_value];
		break;
	default:
		debug("lookup : rubbish");
	}

	/*Split integers and decimals*/

	debug("Fixed value = %ld\n", fixed_value);

	int integer_part = fixed_value / 1000;
	int decimal_part = fixed_value % 1000;

	debug("Final value = %d.%d\n", integer_part, decimal_part);

	/*Copy to buffer with respect to its capacity*/
	state->buf_lim = snprintf(state->buf_data, LUNIX_CHRDEV_BUFSZ, "%d.%d\n", integer_part, decimal_part);


	if(state->buf_lim >= LUNIX_CHRDEV_BUFSZ){
		debug("buffer length exceeded: snprintf truncated string\n");
	}

	/*Update new timestamp*/
	state->buf_timestamp = ktime_get_real_seconds();
	
	debug("leaving\n");
	return 0;
}

/*************************************
 * Implementation of file operations
 * for the Lunix character device
 *************************************/

static int lunix_chrdev_open(struct inode *inode, struct file *filp)
{
	/* Declarations */
	struct lunix_chrdev_state_struct *new_state;
	int type;
	int sensor_id;
	int ret;

	debug("fez: entering\n");
	ret = -ENODEV;

	/*Check if we have a correct device outherwise go to out*/
	if ((ret = nonseekable_open(inode, filp)) < 0){
		goto out;
	}

	/*
	 * Associate this open file with the relevant sensor based on
	 * the minor number of the device node [/dev/sensor<NO>-<TYPE>]
	 */
	
	sensor_id = iminor(inode) >> 3;
	type = iminor(inode) % 8;
	debug("fez: sensorId is  = %d\n", sensor_id);
	debug("fez: type is  = %d\n", type);

	/*if we have bigger sensor number than allowed then return no device*/
	if(type >= N_LUNIX_MSR){
		ret = -ENODEV;
		goto out;
	}

	/* Allocate a new Lunix character device private state structure */
	new_state = kmalloc(sizeof(struct lunix_chrdev_state_struct), GFP_KERNEL);
        /* Error handling */
	if(!new_state){
		printk(KERN_ERR "fez: Lunix_Open: Error with allocationg private state structure\n");	
		/* return bad-address */	 	
		ret = -EFAULT;
		goto out; 
	}

	/*Define our new state stuct*/

	new_state->type = type;
	new_state->sensor = &lunix_sensors[sensor_id];
	new_state->buf_lim = 0;
	new_state->buf_timestamp = 0;

	sema_init(&new_state->lock, 1);

	filp->private_data = new_state;

out:
	debug("leaving, with ret = %d\n", ret);
	return ret;
}

static int lunix_chrdev_release(struct inode *inode, struct file *filp)
{
	kfree(filp->private_data);
	return 0;
}

static long lunix_chrdev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return -EINVAL;
}

static ssize_t lunix_chrdev_read(struct file *filp, char __user *usrbuf, size_t cnt, loff_t *f_pos)
{
	ssize_t ret;
	struct lunix_sensor_struct *sensor;
	struct lunix_chrdev_state_struct *state;


	/*Warnings on differences in state and sensor between file and cached data*/
	state = filp->private_data;
	WARN_ON(!state);

	sensor = state->sensor;
	WARN_ON(!sensor);

	debug("Fez is in read\n");

	/* Lock */
	if(down_interruptible(&state->lock)){
		return -ERESTARTSYS;
	}

	/*Utilise f_pos to discover if we need to print to user*/
	if (*f_pos == 0) {
		while (lunix_chrdev_state_update(state) == -EAGAIN) {
			up(&state->lock);
			debug(" Read : No new data, i'm going to sleep!!!\n");

			if(wait_event_interruptible(sensor->wq, lunix_chrdev_state_needs_refresh(state))){
				debug("Wake up from interrupt\n");
				return -ERESTARTSYS;
			}
            	
			if (down_interruptible(&state->lock)){
					return -ERESTARTSYS; 	/* Lock because other procs may have the same state with you */

			}
		}
	}

	/* End of file */
	
	/* Determine the number of cached bytes to copy to userspace */

	/*No bytes read go print 0*/
	if(state->buf_lim == 0){
		ret = 0;
		goto out;
	}

	/*If our buf lim is smaller than count then print only the ones we can print*/
	int temp = state->buf_lim - *f_pos;
	if(temp < cnt) {
		cnt = temp;
	}

	debug("Start printing to user\n");

	/*Use copy_t0_user to send the data to user space buffer*/
	if(copy_to_user(usrbuf, state->buf_data + *f_pos, cnt)){
		ret = -EFAULT;
		goto out;
	}
	debug("completed print to user\n");


	/*Update new f_pos after printing*/
	*f_pos += cnt;

	if (*f_pos >= state->buf_lim){
		*f_pos = 0;
		ret = cnt;
		goto out;
	}

	/*Return amount of bytes read*/	
	ret = cnt;

out:
	/* Unlock? */
	up(&state->lock);
	return ret;
}


static int lunix_chrdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	return -EINVAL;
}

static struct file_operations lunix_chrdev_fops = 
{
	.owner          = THIS_MODULE,
	.open           = lunix_chrdev_open,
	.release        = lunix_chrdev_release,
	.read           = lunix_chrdev_read,
	.unlocked_ioctl = lunix_chrdev_ioctl,
	.mmap           = lunix_chrdev_mmap
};

int lunix_chrdev_init(void)
{
	/*
	 * Register the character device with the kernel, asking for
	 * a range of minor numbers (number of sensors * 8 measurements / sensor)
	 * beginning with LINUX_CHRDEV_MAJOR:0
	 */
	int ret;
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

	debug("initializing character device\n");
	cdev_init(&lunix_chrdev_cdev, &lunix_chrdev_fops);
	lunix_chrdev_cdev.owner = THIS_MODULE;
	
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
;
	ret = register_chrdev_region(dev_no, lunix_minor_cnt, "lunxTNG");
	
	if (ret < 0) {
		debug("failed to register region, ret = %d\n", ret);
		goto out;
	}


	ret = cdev_add(&lunix_chrdev_cdev, dev_no, lunix_minor_cnt);

	if (ret < 0) {
		debug("failed to add character device\n");
		goto out_with_chrdev_region;
	}

	debug("completed successfully\n");
	return 0;

out_with_chrdev_region:
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
out:
	return ret;
}

void lunix_chrdev_destroy(void)
{
	dev_t dev_no;
	unsigned int lunix_minor_cnt = lunix_sensor_cnt << 3;

	debug("entering\n");
	dev_no = MKDEV(LUNIX_CHRDEV_MAJOR, 0);
	cdev_del(&lunix_chrdev_cdev);
	unregister_chrdev_region(dev_no, lunix_minor_cnt);
	debug("leaving\n");
}
