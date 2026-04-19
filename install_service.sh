#!/bin/bash

set -e

echo "F1sh-Camera-TX Service Installation"
echo "=================================="

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "This script must be run with sudo privileges."
    echo "Usage: sudo ./install_service.sh"
    exit 1
fi

# Detect current user
CURRENT_USER=${SUDO_USER:-$USER}
if [ -z "$CURRENT_USER" ] || [ "$CURRENT_USER" = "root" ]; then
    echo "Error: Cannot determine non-root user. Please run with 'sudo' from your user account."
    exit 1
fi

echo "Detected user: $CURRENT_USER"

# Verify user exists
if ! id "$CURRENT_USER" >/dev/null 2>&1; then
    echo "Error: User '$CURRENT_USER' does not exist on this system."
    exit 1
fi

# Check if service file exists
if [ ! -f "f1sh-camera-tx.service" ]; then
    echo "Error: f1sh-camera-tx.service file not found in current directory"
    exit 1
fi

# Check if binary exists
if [ ! -f "build/F1sh-Camera-TX" ]; then
    echo "Error: build/F1sh-Camera-TX not found. Please build first."
    exit 1
fi

echo "Installing service for user: $CURRENT_USER"

# Create service directory
echo "Creating service directory..."
mkdir -p /etc/f1sh-camera-tx

# Copy binary
echo "Copying application binary..."
cp build/F1sh-Camera-TX /etc/f1sh-camera-tx/
chmod +x /etc/f1sh-camera-tx/F1sh-Camera-TX

# Process service file - replace REPLACE_WITH_USER with actual user
echo "Installing systemd service..."
sed "s/REPLACE_WITH_USER/$CURRENT_USER/g" f1sh-camera-tx.service > /etc/systemd/system/f1sh-camera-tx.service

# Reload systemd
systemctl daemon-reload

# Enable service
systemctl enable f1sh-camera-tx.service

echo "Service installed successfully!"
echo ""
echo "To start the service: sudo systemctl start f1sh-camera-tx"
echo "To check status: sudo systemctl status f1sh-camera-tx"
echo "To view logs: sudo journalctl -u f1sh-camera-tx -f"
echo ""
echo "Note: Make sure user '$CURRENT_USER' is in the 'video' group:"
echo "sudo usermod -a -G video $CURRENT_USER"
