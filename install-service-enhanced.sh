#!/bin/bash

# Enhanced installation script for F1sh-Camera-TX service
# This script ensures all dependencies are properly installed

set -e

echo "=== F1sh-Camera-TX Enhanced Service Installation ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run this script as root (sudo)"
    exit 1
fi

# Get the actual user who ran sudo
REAL_USER=${SUDO_USER:-$(whoami)}
echo "Installing for user: $REAL_USER"

# Install required GStreamer plugins
echo "1. Installing/updating GStreamer plugins..."
apt-get update
apt-get install -y \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav \
    gstreamer1.0-tools \
    gstreamer1.0-x \
    gstreamer1.0-alsa \
    gstreamer1.0-gl \
    gstreamer1.0-gtk3 \
    gstreamer1.0-qt5 \
    gstreamer1.0-pulseaudio \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev

# Install additional codec support
echo "2. Installing additional codecs..."
apt-get install -y \
    x264 \
    libx264-dev \
    libv4l-dev \
    v4l-utils

# Create service directory
echo "3. Creating service directory..."
mkdir -p /etc/f1sh-camera-tx
chown $REAL_USER:$REAL_USER /etc/f1sh-camera-tx

# Copy binary
echo "4. Copying application binary..."
if [ -f "builddir/F1sh-Camera-TX" ]; then
    cp builddir/F1sh-Camera-TX /etc/f1sh-camera-tx/
    chown $REAL_USER:$REAL_USER /etc/f1sh-camera-tx/F1sh-Camera-TX
    chmod +x /etc/f1sh-camera-tx/F1sh-Camera-TX
else
    echo "Error: builddir/F1sh-Camera-TX not found. Please build first with 'ninja -C builddir'"
    exit 1
fi

# Install service file
echo "5. Installing systemd service..."
cp f1sh-camera-tx.service /etc/systemd/system/
systemctl daemon-reload

# Test GStreamer plugins
echo "6. Testing GStreamer plugin availability..."
echo "Available H.264 encoders:"
sudo -u $REAL_USER gst-inspect-1.0 | grep -i h264 | grep -i enc || echo "  No H.264 encoders found!"

echo ""
echo "Testing specific encoders:"
for encoder in v4l2h264enc x264enc omxh264enc; do
    if sudo -u $REAL_USER gst-inspect-1.0 $encoder >/dev/null 2>&1; then
        echo "  ✓ $encoder - Available"
    else
        echo "  ✗ $encoder - Not available"
    fi
done

# Add user to required groups
echo "7. Adding user to required groups..."
usermod -a -G video,render,gpio $REAL_USER || echo "Some groups may not exist, continuing..."

# Test manual execution
echo "8. Testing manual execution..."
echo "Running test as user $REAL_USER..."
if sudo -u $REAL_USER timeout 10s /etc/f1sh-camera-tx/F1sh-Camera-TX 2>&1 | head -20; then
    echo "Manual test completed (may have timed out, which is expected)"
else
    echo "Manual test failed - check the output above for errors"
fi

# Enable and start service
echo "9. Enabling and starting service..."
systemctl enable f1sh-camera-tx.service
systemctl start f1sh-camera-tx.service

# Show status
echo "10. Service status:"
systemctl status f1sh-camera-tx.service --no-pager -l

echo ""
echo "=== Installation Complete ==="
echo ""
echo "Service commands:"
echo "  sudo systemctl status f1sh-camera-tx"
echo "  sudo systemctl stop f1sh-camera-tx" 
echo "  sudo systemctl start f1sh-camera-tx"
echo "  sudo systemctl restart f1sh-camera-tx"
echo "  sudo journalctl -u f1sh-camera-tx -f"
echo ""
echo "HTTP API will be available at: http://localhost:8888"
