# F1sh-Camera-TX – Copilot Instructions

## Architecture & Responsibilities
- Entire application lives in `f1sh_camera_tx.c`; `CustomData` aggregates GStreamer pipeline, HTTP daemon, serial context, and config/state mutexes.
- GStreamer graph: `libcamerasrc → capsfilter → videoconvert → <encoder fallback> → capsfilter(video/x-h264) → h264parse → rtph264pay → udpsink`. `build_and_run_pipeline()` owns creation, linking, stats reset, and restart handling.
- Stream stats are gathered via a pad probe on the udpsink and exposed via `/stats`; keep access protected with `data->stats.stats_mutex`.
- HTTP control plane is built with libmicrohttpd on port 8888. `/health`, `/stats`, `/get`, `/get/<camera>` endpoints are hard-coded; `/config` POST mutates `data->config` and drives pipeline rebuilds or live UDP updates.
- USB serial gadget I/O is handled by `SerialContext`: `serial_reader_thread()` polls `/dev/ttyGS0` (override with `F1SH_SERIAL_DEVICE`), `handle_serial_message()` parses JSON, and `respond_with_status()` echoes status codes. Respect the existing newline-delimited protocol.

## Configuration & Persistence
- Defaults live in `init_config()`; runtime overrides arrive from config file, HTTP `/config`, or env vars (`F1SH_CONFIG_PATH`, `F1SH_SERIAL_DEVICE`).
- `resolve_config_path()` prefers `$F1SH_CONFIG_PATH`, then XDG config, then `~/.f1sh-camera-tx/config.json`; always call `ensure_directory_for_file()` before writing.
- Persist changes through `save_config_to_file()` once `config_modified` is true. Keep width/height/framerate within validated ranges to avoid build-time rejection.

## Build, Test, Deploy
- Use Meson/Ninja: `meson setup build` (once) then `meson compile -C build`. Dependencies are detected via pkg-config (GStreamer core/video/app, libmicrohttpd, jansson).
- `meson test -C build basic` simply executes the freshly built binary; running it on developer machines will try to access real camera/serial hardware—use with caution.
- `install_service.sh` expects `build/F1sh-Camera-TX`; it copies the binary into `/etc/f1sh-camera-tx/`, templatizes `f1sh-camera-tx.service`, reloads systemd, and enables the unit. Never edit the installed unit manually—change the template instead.
- `setup_gadgetonly.sh` configures Raspberry Pi 64-bit USB gadget mode (dwc2 overlay, cmdline modules, package installs). Run it with sudo on the Pi before deploying the service.

## Runtime Behavior Notes
- Pipeline rebuilds are serialized via `data->state_mutex`. When `/config` requires a rebuild, `pipeline_is_restarting` is flipped, and the main loop tears down & recreates the pipeline outside the HTTP handler.
- For simple host/port tweaks, `/config` hot-patches the udpsink via `gst_bin_get_by_name("sink")` without a full rebuild—preserve that optimization when changing sink logic.
- Serial writer uses `serial->write_mutex` and `g_atomic_int` flags; initialize/clear these exactly once in init/shutdown paths to avoid double-destroy.
- Service environment sets `GST_PLUGIN_PATH`/`LD_LIBRARY_PATH` for Pi-specific plugin locations. Honor those paths if you introduce new plugin dependencies.

## Debugging & Extensibility
- Favor `g_print`/`g_printerr` for logging so messages reach both stdout and systemd journal.
- When touching the encoder selection logic, keep the fallback order and encoder-specific property blocks in sync; failing to find an encoder must abort pipeline creation cleanly.
- Any new external interface (HTTP route, serial opcode) should funnel through the existing mutex-protected config/state mutations to avoid data races.
