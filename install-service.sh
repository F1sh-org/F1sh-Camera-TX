#!/bin/bash

# F1sh-Camera-TX Service Installation Script
# This script installs and configures the F1sh-Camera-TX as a systemd service

set -e

# Check if running as root
if [[ $EUID -eq 0 ]]; then
   echo "This script should not be run as root. Please run as a regular user with sudo privileges."
   exit 1
fi

# Configuration
SERVICE_NAME="f1sh-camera-tx"
INSTALL_DIR="/etc/f1sh-camera-tx"
BINARY_NAME="F1sh-Camera-TX"
SERVICE_FILE="f1sh-camera-tx.service"
BUILD_DIR="build"

echo "=== F1sh-Camera-TX Service Installation ==="
echo "Installing to: $INSTALL_DIR"
echo "Service name: $SERVICE_NAME"
echo

# Check if binary exists
if [ ! -f "$BUILD_DIR/$BINARY_NAME" ]; then
    echo "Error: Binary $BUILD_DIR/$BINARY_NAME not found!"
    echo "Please build the application first with: ninja -C builddir"
    exit 1
fi

# Check if service file exists
if [ ! -f "$SERVICE_FILE" ]; then
    echo "Error: Service file $SERVICE_FILE not found!"
    exit 1
fi

# Create installation directory
echo "Creating installation directory..."
sudo mkdir -p "$INSTALL_DIR"

# Copy binary
echo "Installing binary..."
sudo cp "$BUILD_DIR/$BINARY_NAME" "$INSTALL_DIR/"
sudo chmod +x "$INSTALL_DIR/$BINARY_NAME"

# Set ownership
echo "Setting ownership..."
sudo chown -R $USER:video "$INSTALL_DIR"

# Install service file
echo "Installing systemd service..."
sudo cp "$SERVICE_FILE" "/etc/systemd/system/"

# Update the service file to use the correct user
echo "Updating service file with current user..."
sudo sed -i "s/User=pi/User=$USER/" "/etc/systemd/system/$SERVICE_FILE"

# Reload systemd
echo "Reloading systemd daemon..."
sudo systemctl daemon-reload

# Enable service
echo "Enabling service..."
sudo systemctl enable "$SERVICE_NAME"

echo
echo "=== Installation Complete ==="
echo
echo "Service commands:"
echo "  Start service:    sudo systemctl start $SERVICE_NAME"
echo "  Stop service:     sudo systemctl stop $SERVICE_NAME"
echo "  Restart service:  sudo systemctl restart $SERVICE_NAME"
echo "  Check status:     sudo systemctl status $SERVICE_NAME"
echo "  View logs:        sudo journalctl -u $SERVICE_NAME -f"
echo
echo "The service will automatically start on boot."
echo "To start it now, run: sudo systemctl start $SERVICE_NAME"
echo
echo "HTTP API will be available at: http://localhost:8888"
echo "  Health check: curl http://localhost:8888/health"
echo "  Statistics:   curl http://localhost:8888/stats"
echo "  Devices:      curl http://localhost:8888/devices"
echo
