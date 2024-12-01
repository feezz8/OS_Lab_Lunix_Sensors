#!/bin/bash

LUNIX_PATH="/home/user/shared/lunix-tng-helpcode-20241024"
DRIVER_NAME="lunix"
MODULE_NAME="./lunix.ko"
DEVICE_NODE_SCRIPT="./mk-lunix-devs.sh"
LINE_DISCIPLINE_CMD="./lunix-attach /dev/ttyS1"

if [[ "$EUID" -ne 0 ]]; then
	echo "Error: This script must be run as root!" >&2
	exit 1
fi

load_driver() {
	echo "Compiling the driver..."
	cd $LUNIX_PATH
	make
	echo "Loading the driver..."
	insmod $MODULE_NAME
	if [[ $? -ne 0 ]]; then
		echo "Error: Failed to load the driver!"
		exit 1
	fi
	echo "Driver loaded successfully!"
}

create_device_nodes() {
	echo "Creating device nodes..."

	if [[ -f $DEVICE_NODE_SCRIPT ]]; then
		bash $DEVICE_NODE_SCRIPT
		exit_code=$?
		if [[ $exit_code -eq 0 ]]; then
			echo "Device nodes created successfully."
		elif [[ $exit_code -eq 17 ]]; then
			echo "Error: Device nodes already exist (EEXIST)"
		else
			echo "Error: An unexpected error occured"
	exit $exit_code
		fi
	else
		echo "Error: Device node script ($DEVICE_NODE_SCRIPT) not found!"
		exit 1
	fi
}

attach_line_discipline() {
	echo "Attaching line discipline..."
	$LINE_DISCIPLINE_CMD &
	LINE_DISCIPLINE_PID=$!
	if [[ $? -ne 0 ]]; then
		echo "Error: Failed to attach line discipline!"
		exit 1
	fi
	echo "Line discipline attached successfully. (PID = $LINE_DISCIPLINE_PID)"
}


cleanup() {
	echo "Cleaning up..."
	rmmod --force $DRIVER_NAME 2>/dev/null
	echo "Driver unloaded."
	dmesg -C
	echo "Kernel buffer cleaned."
	cd $LUNIX_PATH
	make clean
	echo "Cleaned kernel object files"
}

showoutput() {
	echo ""
	echo "Monitoring kernel logs for Lunix driver (dmesg output)... "
	echo "Press Ctrl+C to stop monitoring."
	dmesg -w
}

case $1 in
    run)
        cleanup
        load_driver
        create_device_nodes
        attach_line_discipline
		showoutput
        ;;
    cleanup)
        cleanup
        ;;
    *)
        echo "Usage: $0 {run|cleanup}"
        echo "  run     - Load driver, create nodes, attach line discipline"
        echo "  cleanup - Unload driver"
        exit 1
        ;;
esac
