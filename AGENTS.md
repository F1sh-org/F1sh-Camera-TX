# PROJECT KNOWLEDGE BASE

**Generated:** 2026-04-21 (Asia/Saigon)
**Commit:** 0248a11
**Branch:** main
**Mode:** update

## OVERVIEW
Raspberry Pi camera transmitter: captures via libcamera/GStreamer, streams H.264 over UDP, exposes control via gRPC and USB serial. Core runtime is intentionally monolithic in `f1sh_camera_tx.c` with gRPC/proto and deployment scripts at repo root.

## STRUCTURE
```text
F1sh-Camera-TX/
├── f1sh_camera_tx.c           # Primary runtime: pipeline, serial, Wi-Fi, config, mDNS, lifecycle
├── grpc_server.cpp            # gRPC C++ server implementation
├── grpc_wrapper.h             # C <-> C++ callback contract
├── f1sh_camera.proto          # gRPC API schema
├── meson.build                # Build + proto generation + optional features
├── install_service.sh         # systemd install flow (root-required)
├── setup_gadgetonly.sh        # Raspberry Pi USB gadget provisioning (root-required)
├── f1sh-camera-tx.service     # Service template (user placeholder replacement)
├── CLAUDE.md                  # Architecture + operations handbook
├── .github/copilot-instructions.md
└── build/                     # Build artifacts (generated/compiled outputs)
```

## WHERE TO LOOK
| Task | Location | Notes |
|---|---|---|
| Startup and shutdown flow | `f1sh_camera_tx.c:2020` (`main`) | Config path, serial init, pipeline start, gRPC start, loop, cleanup |
| Pipeline rebuild/runtime | `f1sh_camera_tx.c:1623` (`build_and_run_pipeline`) | Encoder fallback chain, sink config, restart behavior |
| gRPC behavior | `f1sh_camera_tx.c:1381-1558`, `grpc_server.cpp`, `f1sh_camera.proto` | C callbacks + C++ service + proto contract |
| Serial protocol handling | `f1sh_camera_tx.c:1140`, `:1241`, `:1317` | Status dispatch, reader thread, serial context lifecycle |
| Wi-Fi scan/connect | `f1sh_camera_tx.c:536-902` | `iwlist`/`wpa_cli` orchestration and validation |
| Host/resolution hot updates | `f1sh_camera_tx.c:994`, `:1052`, `:1423`, `:1506` | Host can be hot-patched; resolution triggers restart |
| mDNS advertisement | `f1sh_camera_tx.c:1876-2000` | Avahi-gated registration and shutdown |
| Build/dependency toggles | `meson.build` | required `protoc`, `grpc_cpp_plugin`; optional reflection/Avahi |
| Deployment | `install_service.sh`, `f1sh-camera-tx.service` | Binary copy + unit templating + systemd enable |
| Device provisioning | `setup_gadgetonly.sh` | Boot cmdline/config edits + package install |

## CODE MAP
| Symbol | Type | Location | Role |
|---|---|---|---|
| `main` | function | `f1sh_camera_tx.c:2020` | Process orchestration + control-plane bootstrap |
| `build_and_run_pipeline` | function | `f1sh_camera_tx.c:1623` | Pipeline construct/destroy/restart |
| `serial_reader_thread` | function | `f1sh_camera_tx.c:1241` | USB serial ingestion loop |
| `process_serial_request` | function | `f1sh_camera_tx.c:1140` | Status-code command router |
| `grpc_update_config_cb` | function | `f1sh_camera_tx.c:1423` | Config mutation, persistence, restart decisions |
| `grpc_update_host_cb` | function | `f1sh_camera_tx.c:1530` | UDP host hot-update path |
| `grpc_swap_resolution_cb` | function | `f1sh_camera_tx.c:1506` | Orientation swap + restart flagging |
| `init_mdns_service` | function | `f1sh_camera_tx.c:1967` | Avahi client/poll startup |

## CONVENTIONS (PROJECT-SPECIFIC)
- Treat `f1sh_camera_tx.c` as source-of-truth runtime state machine; cross-cutting changes usually land there.
- Keep config precedence fixed: `F1SH_CONFIG_PATH` -> `XDG_CONFIG_HOME` -> `~/.f1sh-camera-tx/config.json`.
- Persist config via `save_config_to_file()` after mutations; enforce width/height/framerate ranges.
- Protect shared state with existing mutexes (`state_mutex`, `stats_mutex`, `serial.write_mutex`).
- Preserve encoder fallback ordering and per-encoder property blocks.
- Keep host/port hot-update optimization (udpsink property update) for simple destination changes.
- Rebuild path must preserve camera release delay semantics.

## ANTI-PATTERNS (THIS PROJECT)
- Do not edit installed `/etc/systemd/system/f1sh-camera-tx.service` manually; update template `f1sh-camera-tx.service` and reinstall.
- Do not bypass mutex-protected mutation paths for new external interfaces.
- Do not change encoder handling in ways that silently continue after no encoder is available.
- Do not run hardware-touching tests blindly on non-device environments.
- Do not rely on `build/` artifacts as canonical source; they are generated outputs.

## UNIQUE STYLES
- Root-level, mixed-language layout (`.c`, `.cpp`, `.proto`) without `src/` split.
- Runtime integrates streaming, transport control, device provisioning assumptions, and service discovery in one executable.
- Serial protocol is newline-delimited JSON with status-code semantics, mirrored by gRPC feature surface.

## COMMANDS
```bash
meson setup build
meson compile -C build
meson test -C build
sudo ./install_service.sh
sudo ./setup_gadgetonly.sh
```

## NOTES
- `meson test -C build` runs the built app (`test('basic', exe)`) and may touch real camera/serial hardware.
- `meson.build` requires `protoc` and `grpc_cpp_plugin` at configure/build time.
- Service template expects user substitution (`REPLACE_WITH_USER`) via installer.
- Project currently has no `.github/workflows/` CI pipeline file.
