# ESP32 Smart Relay Controller

ESP32-S3 firmware for temperature sensing, relay control, scheduling, MQTT telemetry, and browser-based management (including OTA).

This project is **source-available** under a noncommercial license.

## Table of Contents

- [What This Project Does](#what-this-project-does)
- [Core Functionality](#core-functionality)
- [Architecture](#architecture)
- [Hardware and Sensor Support](#hardware-and-sensor-support)
- [Configuration](#configuration)
- [Build and Flash](#build-and-flash)
- [Web Interface](#web-interface)
- [HTTP API Reference](#http-api-reference)
- [Security Model](#security-model)
- [Storage and Filesystem Behavior](#storage-and-filesystem-behavior)
- [Known Operational Notes](#known-operational-notes)
- [License](#license)

## What This Project Does

The firmware turns an ESP32-S3 into a multi-sensor controller and relay automation node with:

- DS18B20 temperature sensing (multi-sensor, multi-1-Wire bus)
- Optional SHT4x temperature/humidity sensing
- Optional MAX31855 K-type thermocouple sensing
- Optional DS3231 RTC integration with NTP synchronization
- Relay mode control (`off`, `low`, `mid`, `high`)
- Timer-based relay operation and schedule-based automation
- MQTT publishing (Home Assistant friendly, but not HA-only)
- Full web management UI + OTA arming/status workflow
- Runtime diagnostics, error history, performance cards, and debug log viewer

## Core Functionality

### Sensor subsystem

- DS18B20:
  - Configurable sensor count
  - Configurable bus count (up to 4 buses)
  - Address-based sensor mapping
  - Configurable measurement interval
  - Detection/scanning UI and API (`configured` vs `full` scan modes)
- SHT4x:
  - Enable/disable
  - Address/pin/precision/heater configuration
  - Temperature + humidity readout
- MAX31855K:
  - Enable/disable
  - Pin configuration
  - Thermocouple + internal junction readings
  - Fault status surfaced in monitoring

### Relay subsystem

- Relay modes: `off`, `low`, `mid`, `high`
- Configurable pin sets per mode
- Safety timeout enforcement between transitions
- Temperature safety shutdown logic (sensor + threshold + duration)
- Optional garage relay mode
- Manual web/API relay control
- Built-in timer control (`timer_start`, `timer_stop`)
- Scheduler immunity: manual relay control is blocked when schedules are enabled

### Scheduling

- Weekly schedule entries with:
  - Day index (`0..6`)
  - `start_time`, `start_level`, `end_time`
- End behavior is designed to return relay to `off`
- Schedule management integrated in the Settings UI

### Time and RTC

- NTP sync with configurable servers/timezone
- RTC support (DS3231) with options:
  - Enable/disable
  - Bus pins and address
  - Store UTC vs local
  - Boot-time seeding from RTC
  - Periodic RTC sync from NTP
  - Drift threshold (`max_step_seconds`)
- NTP and RTC health/status visible in Monitoring and NTP APIs

### MQTT

- Async MQTT transport/client stack
- Configurable broker/server/port/credentials/client ID
- Per-sensor topic mapping
- Publish interval control
- Runtime publish stats visible in monitoring/performance views

### OTA

- Explicit OTA arming flow (`POST /api/ota/arm`)
- OTA status polling (`GET /api/ota/status`)
- OTA page shows:
  - arming window
  - progress/state/error
  - host + deterministic IP upload command hints
  - OTA log text

## Architecture

- **Async-first web stack** via adapter/backend pattern (`WebRequestAdapter` + async backend)
- **Async-first MQTT stack** (`AsyncMqttTransport` / AsyncMqttClient-esphome)
- Main-controller orchestration model with queued control commands for web-triggered state changes
- Centralized runtime logging through `UnifiedLogger`
- LittleFS-backed configuration and operational logs

Key runtime modules include:

- `MainController`
- `WebServerManager`
- `ConfigManager`
- `MQTTManager`
- `NTPManager`
- `RelayManager` + `ScheduleManager`
- Sensor managers (`DS18B20`, `SHT4x`, `MAX31855`, chip temp)

## Hardware and Sensor Support

### Target board

- `esp32-s3-devkitc-1-n16r8v` (configured in `platformio.ini`)

### Libraries and frameworks

- Arduino framework on `espressif32`
- `ESPAsyncWebServer` + `AsyncTCP`
- `AsyncMqttClient-esphome`
- `ArduinoJson`
- `LittleFS`
- `DallasTemperature` / `OneWire`
- `Adafruit SHT4x`
- `Adafruit MAX31855`
- `RTClib`

## Configuration

### Main files

- Runtime config on device: `/config.json` (LittleFS)
- Local source config: `config.json` (not for public mirror)
- Example schema/documentation: `config.json.example`
- OTA config: `ota.json` + `ota.json.example`

### Major config sections

- `wifi`
- `mqtt`
- `sensors`
- `relay`
- `device_info`
- `network`
- `security`
- `ntp` (with `rtc` sub-block)
- `schedules`
- `debug`
- `logging`
- `fallback`

Notes:

- `config.json` and `data/config.json` should stay strict JSON.
- Password fields in settings APIs are write-only by design (empty string means keep current).

## Build and Flash

### Prerequisites

- [PlatformIO](https://platformio.org/)
- USB connection for first flash
- Valid local `config.json` for runtime

### Build

```powershell
pio run
```

### USB flash

```powershell
pio run -t upload
```

### OTA flash target

```powershell
pio run -e esp32-s3-devkitc-1-ota -t upload --upload-port <device-ip-or-hostname>
```

### Filesystem upload behavior

`post_upload.py` automatically syncs root config files into `data/` before LittleFS image build/upload, then runs `uploadfs` after firmware upload.

## Web Interface

Navigation pages:

- `/` Home
- `/settings`
- `/detection`
- `/monitoring`
- `/performance`
- `/relay`
- `/debug`
- `/ota`

### Home (`/`)

- Live sensor summary (DS18B20, SHT4x, MAX31855)
- Auto-refresh loop based on configured DS18B20 interval
- Readout-aware refresh using `X-DS18-Readouts` header

### Settings (`/settings`)

- Full configuration editor for WiFi, MQTT, sensors, relay, security, NTP/RTC, schedules, fallback
- Client-side validation for relay safety fields
- Save to `/api/settings`
- Reboot action via `/api/reboot`

### Detection (`/detection`)

- DS18B20 scan tools:
  - configured pins scan
  - full viable pin scan
- Scan queue/progress/result handling with cooldown protections

### Monitoring (`/monitoring`)

- Status cards for:
  - WiFi
  - MQTT
  - sensor subsystems
  - auth/security
  - memory/PSRAM
  - system details (uptime/code date/chip info)
  - NTP/RTC health
  - filesystem usage highlights
- Error monitoring + history stats
- Auto-refresh cards from `/api/monitoring/cards`

### Performance (`/performance`)

- Runtime performance cards and counters
- Component-level timings and API diagnostics
- Periodic refresh via `/api/performance/cards`

### Relay (`/relay`)

- Manual mode controls (`OFF/LOW/MID/HIGH`)
- Current mode and safety status display
- Timer start/stop (60..240 minutes)
- Progress/countdown display
- Scheduler lockout warning/disable behavior when scheduling is enabled

### Debug (`/debug`)

- Buffered log viewer with polling
- Single-client ownership/TTL model (avoids concurrent debug stream contention)
- Pause/resume, clear log buffer, auto-scroll

### OTA (`/ota`)

- Explicit arm button (no implicit arming)
- Live status polling and health indication
- Upload command hints for hostname and IP upload targets
- OTA progress/error/log text

## HTTP API Reference

### Core settings/config

- `GET /api/settings` - read current settings snapshot (sensitive fields omitted)
- `POST /api/settings` - save settings
- `POST /api/reboot` - reboot device
- `GET /api/config` - download `/config.json`

### Sensors and scan

- `GET /api/home/sensors` - HTML fragment for Home sensor block
- `GET /scan?mode=configured|full` - DS18B20 scan queue/status/results

### OTA

- `GET /api/ota/status`
- `POST /api/ota/arm`

### Monitoring/performance/errors

- `GET /api/monitoring/cards`
- `GET /api/performance/cards`
- `GET /api/errors`
- `GET /api/errors/logs`

### Time

- `GET /api/ntp/status`
- `GET /api/ntp/logs`

### Relay

- `GET /api/relay/status`
- `POST /api/relay/set` (`off|low|mid|high|timer_start|timer_stop`)
- `GET /api/relay/logs`
- `GET /relay/off|low|mid|high` (direct control endpoints used by Relay page)

### Debug viewer

- `POST /api/debug/enable`
- `GET /api/debug/logs`
- `POST /api/debug/clear`

## Security Model

- Optional HTTP Basic Auth
- Optional IP whitelist gate
- Authentication + whitelist checks applied consistently at handler entry
- Sensitive credentials are not returned in normal settings reads

## Storage and Filesystem Behavior

- LittleFS stores runtime config and operational logs
- Config save path validates by re-reading after write
- Versioned config backups are kept (`/config_vN.json`, rolling window)
- Monitoring page shows key file sizes and top space consumers

## Known Operational Notes

- Manual relay control is intentionally blocked when schedule mode is enabled.
- OTA arming is explicit and time-window based by design.
- Detection scans are queued and cooldown-limited to avoid repeated heavy operations.

## License

This project is licensed under **PolyForm Noncommercial 1.0.0**.

- Personal/noncommercial use is allowed under license terms.
- Commercial use requires a separate written agreement from the copyright holder.

See [LICENSE](LICENSE) for full terms.
