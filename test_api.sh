#!/bin/bash

# Test script for F1sh-Camera-TX HTTP API

API_BASE="http://localhost:8888"

echo "=== F1sh-Camera-TX API Test Script ==="
echo ""

echo "1. Testing health check..."
curl -s "$API_BASE/health" | python3 -m json.tool
echo -e "\n"

echo "2. Getting dynamically detected cameras and encoders..."
curl -s "$API_BASE/get" | python3 -m json.tool
echo -e "\n"

echo "3. Testing dynamic camera detection for specific camera..."
# First get the first available camera from the list
FIRST_CAMERA=$(curl -s "$API_BASE/get" | python3 -c "import sys, json; data=json.load(sys.stdin); print(data['cameras'][0] if data['cameras'] else 'auto-detect')")
echo "Testing camera: $FIRST_CAMERA"
curl -s "$API_BASE/get/$FIRST_CAMERA" | python3 -m json.tool
echo -e "\n"

echo "4. Getting current statistics..."
curl -s "$API_BASE/stats" | python3 -m json.tool
echo -e "\n"

echo "5. Testing configuration with dynamically detected encoder..."
# Get the first available encoder
FIRST_ENCODER=$(curl -s "$API_BASE/get" | python3 -c "import sys, json; data=json.load(sys.stdin); print(data['encoders'][0] if data['encoders'] else 'v4l2h264enc')")
echo "Using encoder: $FIRST_ENCODER"
curl -s -X POST "$API_BASE/config" \
  -H "Content-Type: application/json" \
  -d "{
    \"host\": \"127.0.0.1\",
    \"port\": 5000,
    \"encoder\": \"$FIRST_ENCODER\",
    \"width\": 1280,
    \"height\": 720,
    \"framerate\": 30
  }" | python3 -m json.tool
echo -e "\n"

echo "6. Testing UDP destination update only..."
curl -s -X POST "$API_BASE/config" \
  -H "Content-Type: application/json" \
  -d '{
    "host": "192.168.1.100",
    "port": 5001
  }' | python3 -m json.tool
echo -e "\n"

echo "7. Testing high resolution configuration..."
curl -s -X POST "$API_BASE/config" \
  -H "Content-Type: application/json" \
  -d '{
    "width": 1920,
    "height": 1080,
    "framerate": 30
  }' | python3 -m json.tool
echo -e "\n"

echo "8. Final statistics check..."
sleep 2
curl -s "$API_BASE/stats" | python3 -m json.tool
echo -e "\n"

echo "=== Test Complete ==="
