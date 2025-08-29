#!/bin/bash

# F1sh-Camera-TX Encoder Troubleshooting Script

echo "=== F1sh-Camera-TX Encoder Troubleshooting ==="
echo ""

USER_TO_TEST=${1:-pi}
echo "Testing for user: $USER_TO_TEST"
echo ""

echo "1. GStreamer version:"
sudo -u $USER_TO_TEST gst-launch-1.0 --version
echo ""

echo "2. Available GStreamer plugins:"
sudo -u $USER_TO_TEST gst-inspect-1.0 | grep -E "(v4l2|x264|omx|nv|vaapi)" | head -10
echo ""

echo "3. Testing specific H.264 encoders:"
encoders=("v4l2h264enc" "x264enc" "omxh264enc" "nvh264enc" "vaapih264enc")
for encoder in "${encoders[@]}"; do
    echo -n "  $encoder: "
    if sudo -u $USER_TO_TEST gst-inspect-1.0 $encoder >/dev/null 2>&1; then
        echo "✓ Available"
        # Get more details
        sudo -u $USER_TO_TEST gst-inspect-1.0 $encoder | grep -E "(Version|Description)" | head -2 | sed 's/^/    /'
    else
        echo "✗ Not available"
    fi
done
echo ""

echo "4. Testing libcamerasrc:"
if sudo -u $USER_TO_TEST gst-inspect-1.0 libcamerasrc >/dev/null 2>&1; then
    echo "  ✓ libcamerasrc available"
else
    echo "  ✗ libcamerasrc not available"
fi
echo ""

echo "5. Testing simple pipeline with working encoder:"
echo "  Trying to find a working encoder..."
for encoder in v4l2h264enc x264enc omxh264enc; do
    if sudo -u $USER_TO_TEST gst-inspect-1.0 $encoder >/dev/null 2>&1; then
        echo "  Testing pipeline with $encoder:"
        echo "  Command: gst-launch-1.0 videotestsrc num-buffers=10 ! $encoder ! fakesink"
        if sudo -u $USER_TO_TEST timeout 15s gst-launch-1.0 videotestsrc num-buffers=10 ! $encoder ! fakesink 2>&1; then
            echo "  ✓ Pipeline test successful with $encoder"
            break
        else
            echo "  ✗ Pipeline test failed with $encoder"
        fi
    fi
done
echo ""

echo "6. User groups:"
id $USER_TO_TEST
echo ""

echo "7. Device permissions:"
ls -la /dev/video* /dev/media* /dev/vchiq /dev/vcsm-cma 2>/dev/null || echo "  Some devices not found"
echo ""

echo "8. Environment check:"
echo "  GST_PLUGIN_PATH: ${GST_PLUGIN_PATH:-not set}"
echo "  LD_LIBRARY_PATH: ${LD_LIBRARY_PATH:-not set}"
echo ""

echo "9. Manual service test (10 seconds):"
echo "  Testing: sudo -u $USER_TO_TEST /etc/f1sh-camera-tx/F1sh-Camera-TX"
if [ -f "/etc/f1sh-camera-tx/F1sh-Camera-TX" ]; then
    sudo -u $USER_TO_TEST timeout 10s /etc/f1sh-camera-tx/F1sh-Camera-TX 2>&1 | head -30
    echo "  (Test completed - may have timed out which is expected)"
else
    echo "  ✗ Binary not found at /etc/f1sh-camera-tx/F1sh-Camera-TX"
fi
echo ""

echo "=== Troubleshooting Complete ==="
echo ""
echo "If encoders are missing, try:"
echo "  sudo apt update"
echo "  sudo apt install gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly"
echo "  sudo apt install x264 libx264-dev"
echo ""
echo "If hardware encoders don't work, try software encoding:"
echo "  curl -X POST http://localhost:8888/config -d '{\"encoder\":\"x264enc\"}'"
