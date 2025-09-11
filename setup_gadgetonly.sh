#!/bin/bash

CONFIG_TXT_PATH="/boot/firmware/config.txt"
CMDLINE_TXT_PATH="/boot/firmware/cmdline.txt"

# Function to check if the script is run as root (sudo)
check_sudo() {
    if [ "$EUID" -ne 0 ]; then
        echo "Please run as root (sudo)."
        exit 1
    fi
}

# Function to check if the OS is Raspberry Pi OS 64-bit
check_os() {
    if ! grep -q "Raspberry Pi" /etc/rpi-issue; then
        echo "This script is intended for Raspberry Pi OS only."
        exit 1
    fi
    if ! uname -m | grep -q "aarch64"; then
        echo "This script is intended for Raspberry Pi OS 64-bit only."
        exit 1
    fi
}

find_config_file() {
    local filename="$1"
    local subdir="$2"
    
    if [ -f "./${filename}" ]; then
        echo "./${filename}"
    elif [ -d "./${subdir}" ] && [ -f "./${subdir}/${filename}" ]; then
        echo "./${subdir}/${filename}"
    else
        echo ""
    fi
}

configure_usb_serial_gadget() {
    echo_info "Checking and updating ${CONFIG_TXT_PATH}..."
    
    # Check if the specific peripheral mode line already exists
    if grep -q "^dtoverlay=dwc2,dr_mode=peripheral" "${CONFIG_TXT_PATH}"; then
        echo_info "USB Serial Gadget (dtoverlay=dwc2,dr_mode=peripheral) already configured in ${CONFIG_TXT_PATH}."
    else
        echo_info "Adding dtoverlay=dwc2,dr_mode=peripheral to ${CONFIG_TXT_PATH}..."
        
        if confirm_action "Do you want to add USB Serial Gadget configuration to ${CONFIG_TXT_PATH}?"; then
            # Backup the original file
            cp "${CONFIG_TXT_PATH}" "${CONFIG_TXT_PATH}.backup.$(date +%F-%H%M%S)"
            
            # Check if there's any existing dtoverlay=dwc2 line (for informational purposes)
            if grep -q "^dtoverlay=dwc2" "${CONFIG_TXT_PATH}"; then
                echo_info "Found existing dtoverlay=dwc2 configuration. Adding peripheral mode configuration alongside it..."
            fi
            
            # Add the new configuration line at the end of the file
            echo "dtoverlay=dwc2,dr_mode=peripheral" >> "${CONFIG_TXT_PATH}"
            
            echo_info "USB Serial Gadget configuration added successfully to ${CONFIG_TXT_PATH}."
        else
            echo_warn "Skipping USB Serial Gadget configuration in ${CONFIG_TXT_PATH}."
        fi
    fi
    
    # Configure cmdline.txt
    echo_info "Checking and updating ${CMDLINE_TXT_PATH}..."
    if grep -q "modules-load=dwc2,g_serial" "${CMDLINE_TXT_PATH}"; then
        echo_info "USB Serial Gadget modules already configured in ${CMDLINE_TXT_PATH}."
    else
        echo_info "Adding modules-load=dwc2,g_serial to ${CMDLINE_TXT_PATH}..."
        if confirm_action "Do you want to add USB Serial Gadget modules to ${CMDLINE_TXT_PATH}?"; then
            cp "${CMDLINE_TXT_PATH}" "${CMDLINE_TXT_PATH}.backup.$(date +%F-%H%M%S)"
            sed -i 's/rootwait/rootwait modules-load=dwc2,g_serial/' "${CMDLINE_TXT_PATH}"
            echo_info "USB Serial Gadget modules added to ${CMDLINE_TXT_PATH}."
        else
            echo_warn "Skipping USB Serial Gadget modules configuration in ${CMDLINE_TXT_PATH}."
        fi
    fi
    
    # Enable getty service
    echo_info "Enabling getty service for USB Serial Gadget..."
    systemctl enable getty@ttyGS0.service
    echo_info "Getty service enabled for USB Serial Gadget."
    
    echo_info "USB Serial Gadget configuration completed."
}

# Main script execution starts here
check_sudo
check_os

# Update and upgrade packages
echo "Updating and upgrading packages..."
apt update && apt upgrade -y

# Install required packages
echo "Installing necessary packages..."
apt install wget gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libcamera libcamera-apps -y

