# F1sh-Camera-TX Project Documentation

## Project Overview

**F1sh-Camera-TX** is a video streaming transmitter application designed for Raspberry Pi devices. It captures video from libcamera-compatible cameras and streams it over UDP using H.264 encoding, while providing control interfaces via gRPC and USB serial communication.

**Primary Use Case**: Raspberry Pi-based camera systems that stream video to a remote receiver while being controllable via USB serial connection (USB gadget mode) or gRPC network interface.

## Architecture

### Single-File Design
The entire application is implemented in [f1sh_camera_tx.c](f1sh_camera_tx.c) (~2,260 lines), using:
- **GStreamer** for video pipeline management
- **gRPC** for network control interface (port 50051)
- **USB Serial** for device-to-device communication (/dev/ttyGS0)
- **Avahi** for mDNS/Bonjour service advertisement (Linux only)

### Key Components

```
┌─────────────────────────────────────────────────────────────┐
│                     Main Application                         │
│                    (f1sh_camera_tx.c)                       │
├──────────────────┬──────────────────┬───────────┬───────────┤
│   GStreamer      │   gRPC Server    │ Serial I/O│   mDNS    │
│   Pipeline       │  (Port 50051)    │(/dev/ttyGS│ Discovery │
│                  │                  │     0)    │           │
├──────────────────┼──────────────────┼───────────┼───────────┤
│ libcamerasrc     │ Health           │ Status    │ _grpc._tcp│
│      ↓           │ GetStats         │ codes     │           │
│ capsfilter       │ GetConfig        │ JSON      │ _f1sh-    │
│      ↓           │ UpdateConfig     │ protocol  │ camera._  │
│ videoconvert     │ SwapResolution   │ Wi-Fi     │ tcp       │
│      ↓           │ UpdateHost       │ control   │           │
│ encoder (h264)   │ GetAvailableDevs │ Config    │           │
│      ↓           │                  │ sync      │           │
│ h264parse        │                  │           │           │
│      ↓           │                  │           │           │
│ rtph264pay       │                  │           │           │
│      ↓           │                  │           │           │
│ udpsink          │                  │           │           │
└──────────────────┴──────────────────┴───────────┴───────────┘
```

## Core Data Structures

### CustomData
Main application state containing:
- GStreamer pipeline and bus
- gRPC server instance
- Application configuration (AppConfig)
- Stream statistics (StreamStats)
- Serial context (SerialContext)
- State management mutexes
- mDNS context (MDNSContext, Linux only)

### AppConfig
Configuration parameters:
- `host`: UDP destination IP address
- `port`: UDP destination port
- `camera_name`: libcamera device name
- `encoder_type`: H.264 encoder selection
- `width`, `height`, `framerate`: Video parameters

### SerialContext
USB Serial interface:
- File descriptor for /dev/ttyGS0
- Reader thread
- Write mutex for thread-safe operations

## GStreamer Pipeline

**Default Pipeline Structure:**
```
libcamerasrc (camera-name)
  → capsfilter (resolution + framerate)
  → videoconvert
  → encoder (v4l2h264enc/omxh264enc/x264enc/etc.)
  → capsfilter (video/x-h264)
  → h264parse
  → rtph264pay
  → udpsink (host, port)
```

**Encoder Fallback Chain:**
1. v4l2h264enc (hardware, Raspberry Pi)
2. omxh264enc (legacy hardware)
3. x264enc (software fallback)
4. nvh264enc (NVIDIA hardware)
5. vaapih264enc (Intel hardware)

## gRPC API

### Service: F1shCameraService

All RPC methods use Protocol Buffers for message serialization. Server listens on port 50051.

**Health**
- Request: Empty
- Returns: `{status: "healthy"}`
- Purpose: Health check

**GetStats**
- Request: Empty
- Returns: Stream statistics (total_bytes, frame_count, current_bitrate)
- Purpose: Monitor streaming performance

**GetConfig**
- Request: Empty
- Returns: Current configuration (host, port, camera_name, encoder_type, width, height, framerate)
- Purpose: Query current settings

**UpdateConfig**
- Request: Optional fields for configuration updates (host, port, camera_name, encoder_type, width, height, framerate)
- Returns: Success status, updated configuration, message
- Updates: Any combination of configuration fields
- Triggers: Pipeline rebuild if encoder/resolution/framerate changed

**SwapResolution**
- Request: Empty
- Returns: Success status, new configuration, message
- Purpose: Swaps width and height (portrait/landscape toggle)
- Triggers: Full pipeline rebuild

**UpdateHost**
- Request: New host IP address
- Returns: Success status, message
- Purpose: Update UDP destination without full rebuild

**GetAvailableDevices**
- Request: Empty
- Returns: Lists of available cameras (name, path) and encoders (name, availability)
- Purpose: Device discovery

### Client Examples

**Using grpcurl:**
```bash
# List services
grpcurl -plaintext localhost:50051 list

# Health check
grpcurl -plaintext localhost:50051 f1sh_camera.F1shCameraService/Health

# Get stats
grpcurl -plaintext localhost:50051 f1sh_camera.F1shCameraService/GetStats

# Update config
grpcurl -plaintext -d '{"host": "192.168.1.100"}' \
  localhost:50051 f1sh_camera.F1shCameraService/UpdateConfig
```

**Using Python:**
```python
import grpc
import f1sh_camera_pb2
import f1sh_camera_pb2_grpc

channel = grpc.insecure_channel('localhost:50051')
stub = f1sh_camera_pb2_grpc.F1shCameraServiceStub(channel)

# Get stats
response = stub.GetStats(f1sh_camera_pb2.GetStatsRequest())
print(f"Bytes: {response.stats.total_bytes}")
print(f"Bitrate: {response.stats.current_bitrate} kbps")

# Update config
update = f1sh_camera_pb2.UpdateConfigRequest(host="192.168.1.100", port=5600)
response = stub.UpdateConfig(update)
print(f"Success: {response.success}")
```

## Serial Protocol

### Communication Format
- Newline-delimited JSON messages
- 115200 baud rate
- UTF-8 sanitization for safety

### Status Codes

**Incoming Commands:**
- **Status 1**: Ping/echo request
- **Status 5**: Get current configuration
- **Status 21**: Wi-Fi network scan
- **Status 22**: Wi-Fi connect (requires BSSID, passphrase)
- **Status 23**: Update UDP host
- **Status 24**: Swap resolution (portrait/landscape)

**Outgoing Responses:**
- **Status 1**: Echo response
- **Status 3**: Error/failure
- **Status 4**: Wi-Fi scan results
- **Status 5**: Configuration data
- **Status 21**: Wi-Fi scan complete
- **Status 22**: Wi-Fi connect result (with IP address)
- **Status 23**: Host update confirmed
- **Status 24**: Resolution swap confirmed

## Configuration Management

### Configuration Paths (in order of precedence)
1. `$F1SH_CONFIG_PATH` (environment variable)
2. `$XDG_CONFIG_HOME/f1sh-camera-tx/config.json`
3. `~/.f1sh-camera-tx/config.json`

### Configuration Format
```json
{
  "host": "192.168.1.100",
  "port": 5600,
  "camera_name": "/base/soc/i2c0mux/i2c@1/imx708@1a",
  "encoder_type": "v4l2h264enc",
  "width": 1920,
  "height": 1080,
  "framerate": 30
}
```

### Validation Ranges
- Width: 320-4608 pixels
- Height: 240-2592 pixels
- Framerate: 1-120 fps

### Hot-Swap Capabilities
- **UDP host/port**: Updated without pipeline rebuild
- **Resolution, framerate, encoder**: Requires full pipeline rebuild with 1-second delay

## Build System

### Build Tool: Meson + Ninja

**Setup and Compilation:**
```bash
meson setup build
meson compile -C build
```

**Testing:**
```bash
meson test -C build
```

### Dependencies

**Core Libraries:**
- gstreamer-1.0 (v1.26.3)
- gstreamer-app-1.0 (v1.26.3)
- gstreamer-video-1.0 (v1.26.3)
- grpc++ (gRPC C++ library)
- protobuf (Protocol Buffers)
- avahi-client (Linux only)
- avahi-glib (Linux only)
- GLib/GObject (v2.84.3)

**Required GStreamer Plugins:**
- libcamerasrc
- v4l2h264enc (or fallback encoders)
- videoconvert
- h264parse
- rtph264pay
- udpsink

## Deployment

### Systemd Service

**Service File:** [f1sh-camera-tx.service](f1sh-camera-tx.service)

**Installation:**
```bash
./install_service.sh
```

**Service Management:**
```bash
sudo systemctl start f1sh-camera-tx
sudo systemctl status f1sh-camera-tx
sudo systemctl enable f1sh-camera-tx  # Auto-start on boot
```

### USB Gadget Mode Setup

**Script:** [setup_gadgetonly.sh](setup_gadgetonly.sh)

Configures Raspberry Pi for USB gadget mode, enabling serial communication via /dev/ttyGS0.

## Wi-Fi Management

### Implementation
- Uses `wpa_cli` for network operations
- Scans networks on wlan0 (configurable)
- BSSID-based connection with passphrase validation
- Returns assigned IP address after successful connection

### Commands
- **Scan**: Triggers network scan via serial Status 21
- **Connect**: Connects to network via serial Status 22 (requires BSSID and passphrase)

## mDNS Service Discovery

### Overview
The application advertises its presence on the local network using **mDNS/Bonjour/Zeroconf**, allowing other devices to automatically discover the camera service without knowing its IP address.

### Platform Availability
- **Linux (Raspberry Pi)**: Fully supported via Avahi
- **macOS/Other**: Disabled at compile time (conditional compilation with `HAVE_AVAHI`)

### Advertised Services

**1. gRPC API Service (`_grpc._tcp`)**
- Service Name: "F1sh Camera TX"
- Port: 50051
- TXT Records:
  - `proto=f1sh_camera` - Protocol name
  - `version=0.1` - Application version
  - `type=camera` - Device type

**2. Camera Streaming Service (`_f1sh-camera._tcp`)**
- Service Name: "F1sh Camera TX"
- Port: UDP streaming port (from config)
- TXT Records:
  - `protocol=udp` - Streaming protocol
  - `encoding=h264` - Video encoding format
  - `control_port=50051` - gRPC control port

### Discovery Methods

**On Linux/Raspberry Pi:**
```bash
# Using avahi-browse
avahi-browse -rt _grpc._tcp
avahi-browse -rt _f1sh-camera._tcp

# Using Avahi utilities
avahi-resolve -n "F1sh Camera TX._grpc._tcp.local"
```

**On macOS:**
```bash
# Using dns-sd
dns-sd -B _grpc._tcp
dns-sd -B _f1sh-camera._tcp

# Using Bonjour browser tools
```

**In Python:**
```python
from zeroconf import ServiceBrowser, Zeroconf

class CameraListener:
    def add_service(self, zeroconf, service_type, name):
        info = zeroconf.get_service_info(service_type, name)
        print(f"Found camera: {name}")
        print(f"Address: {info.parsed_addresses()[0]}:{info.port}")

zeroconf = Zeroconf()
listener = CameraListener()
browser = ServiceBrowser(zeroconf, "_f1sh-camera._tcp.local.", listener)
```

### Implementation Details
- Uses Avahi client library with GLib integration
- Service registration is automatic on application startup
- Services are properly unregistered on shutdown
- Handles name collisions and network state changes
- Non-blocking operation via GLib main loop integration

### Troubleshooting mDNS
- Ensure `avahi-daemon` is running: `sudo systemctl status avahi-daemon`
- Check firewall allows mDNS (UDP port 5353)
- Verify network interface supports multicast
- Check logs for mDNS initialization messages

## Threading and Concurrency

### Thread-Safe Operations
- Mutex-protected configuration updates
- Serial write operations synchronized
- GStreamer bus monitoring in main thread
- Serial reader in separate thread

### State Management
All shared state accessed through mutexes to prevent race conditions.

## Error Handling

### GStreamer Pipeline
- Bus monitoring for errors and state changes
- Automatic pipeline restart on failure
- 1-second delay between rebuilds to release camera resources

### gRPC API
- Error responses with gRPC status codes
- Input validation for all configuration parameters
- Structured error messages in responses

### Serial Communication
- UTF-8 sanitization to prevent injection attacks
- Error status codes for failed operations
- Timeout handling for Wi-Fi operations

## Statistics and Monitoring

### Real-time Stats
- Bytes sent
- Frame count
- Current bitrate (Mbps)
- Collected via GStreamer pad probe on udpsink

### Access
- gRPC GetStats RPC method
- Serial Status 5 command (returns full configuration)

## Development

### VSCode Configuration
- IntelliSense configured for C development
- Meson build tasks integration
- Clang compiler with extensive warnings
- Compile commands: [builddir/compile_commands.json](builddir/compile_commands.json)

### Code Style
- GLib conventions and data types
- Consistent error handling patterns
- Comprehensive logging with g_print/g_printerr

## Important Notes

### Camera Resource Management
- Pipeline rebuilds require 1-second delay to properly release camera
- Only one pipeline instance should run at a time

### USB Serial Availability
- /dev/ttyGS0 must exist (requires USB gadget mode setup)
- Application continues without serial if device unavailable
- Check logs for serial initialization status

### Network Configuration
- UDP streaming requires reachable destination host
- Firewall rules may need adjustment for port 50051 (gRPC) and UDP streaming port

### Security Considerations
- gRPC server has no authentication (uses insecure channel)
- Serial commands should be from trusted sources only
- Wi-Fi credentials transmitted over serial connection
- Consider using TLS for gRPC in production environments

## Troubleshooting

### Pipeline Fails to Start
1. Check if camera is available: `libcamera-hello --list-cameras`
2. Verify encoder availability: Check logs for fallback chain
3. Ensure no other process is using the camera

### No Serial Communication
1. Verify /dev/ttyGS0 exists
2. Check USB gadget mode configuration
3. Review serial reader thread logs

### gRPC API Not Responding
1. Check if port 50051 is available: `sudo netstat -tulpn | grep 50051`
2. Verify firewall rules: `sudo ufw allow 50051/tcp`
3. Check application logs for gRPC server errors
4. Test with grpcurl: `grpcurl -plaintext localhost:50051 list`

### Wi-Fi Commands Failing
1. Ensure wpa_cli is installed and accessible
2. Verify wlan0 interface exists
3. Check wpa_supplicant is running

## File Structure

```
/Users/b4iterdev/F1sh-Camera-TX/
├── .github/
│   └── copilot-instructions.md    # Architecture and development guidelines
├── .vscode/                       # VSCode IDE configuration
├── build/                         # Meson build output
├── builddir/                      # Alternative build directory
├── f1sh_camera_tx.c              # Main application source (2,260 lines)
├── grpc_server.cpp               # gRPC server implementation (C++)
├── grpc_wrapper.h                # C wrapper header for gRPC
├── f1sh_camera.proto             # Protocol Buffers service definition
├── meson.build                    # Build configuration
├── f1sh-camera-tx.service        # Systemd service file
├── install_service.sh            # Service installation script
├── setup_gadgetonly.sh           # USB gadget mode setup script
├── CLAUDE.md                      # This file
├── MIGRATION_STEPS.md            # gRPC migration guide
└── GRPC_INTEGRATION.md           # gRPC integration documentation
```

## Additional Resources

For detailed architecture documentation, see [.github/copilot-instructions.md](.github/copilot-instructions.md).

---

**Last Updated**: 2026-01-06
**Target Platform**: Raspberry Pi OS 64-bit (aarch64)
**License**: Not specified in repository
