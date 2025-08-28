#!/bin/bash

# Test script for F1sh-Camera-TX HTTP API

API_BASE="http://localhost:8888"

echo "=== F1sh-Camera-TX API Test Script ==="
echo ""

echo "1. Testing health check..."
curl -s "$API_BASE/health" | python3 -m json.tool
echo -e "\n"

echo "2. Getting available cameras and encoders..."
curl -s "$API_BASE/get" | python3 -m json.tool
echo -e "\n"

echo "3. Getting camera-specific information..."
curl -s "$API_BASE/get/auto-detect" | python3 -m json.tool
echo -e "\n"

echo "4. Getting current statistics..."
curl -s "$API_BASE/stats" | python3 -m json.tool
echo -e "\n"

echo "5. Testing configuration update (software encoder)..."
curl -s -X POST "$API_BASE/config" \
  -H "Content-Type: application/json" \
  -d '{
    "host": "127.0.0.1",
    "port": 5000,
    "encoder": "x264enc",
    "width": 1280,
    "height": 720,
    "framerate": 30
  }' | python3 -m json.tool
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
