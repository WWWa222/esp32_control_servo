# Remote Power Controller

ESP32-S3 project for:

- driving an SG90 servo to press a laptop power button,
- exposing remote control APIs over Wi-Fi,
- receiving heartbeat updates from the host computer,
- sending timeout alerts through an HTTP webhook,
- keeping the firmware resilient with watchdog supervision.

## Prerequisites

This repository is the project source tree, not a standalone prebuilt package.

You can clone it and use it directly, but you still need a working `ESP-IDF` environment to configure, build, flash, and monitor the firmware.

## Environment Requirements

Recommended host environment:

- Windows 10 or Windows 11 with PowerShell
- or Linux / WSL with a normal ESP-IDF shell environment

Required software:

- `ESP-IDF 6.0`
- Python managed by ESP-IDF
- Xtensa toolchain for `esp32s3`
- CMake and Ninja installed through ESP-IDF tools
- Git

Required hardware:

- an `ESP32-S3` board
- USB data cable
- access to the board serial port such as `COM3`

Project assumptions:

- target chip is `esp32s3`
- flash size is configured for `16MB`
- OTA layout assumes a `16MB` flash chip

Recommended before first build:

- verify `idf.py --version` reports `ESP-IDF v6.0`
- verify the board can be detected on a serial port
- verify the ESP-IDF export / environment activation script works in your shell

Practical meaning:

- this repo already contains the application source code
- the vendored `esp_wireguard` component is already included in the repo
- you do not need to create a new ESP-IDF project from scratch
- you do need to install `ESP-IDF 6.0` and its toolchain on your machine

Minimum setup:

- `ESP-IDF 6.0`
- Python environment created by ESP-IDF
- Xtensa toolchain for `esp32s3`
- USB serial driver / access to the board's COM port

If you are on Windows, the simplest path is:

1. install Espressif's official ESP-IDF tools and `ESP-IDF 6.0`
2. open an ESP-IDF PowerShell or use this repo's helper script
3. build from this repository directory

Without ESP-IDF installed, this repository alone is not enough to compile firmware.

## Build

```powershell
. .\tools\enter_idf_env.ps1
cd H:\work\codex_workplace\ESP32\remote_power_controller
idf.py set-target esp32s3
idf.py build
```

If `export.ps1` cannot find Python on your machine, use the helper script above instead of calling the ESP-IDF export script directly.

First-time bring-up on a new machine usually looks like:

```powershell
git clone https://github.com/WWWa222/esp32_control_servo.git
cd esp32_control_servo
. .\tools\enter_idf_env.ps1
idf.py set-target esp32s3
idf.py build
```

## Configure

Edit project options with:

```powershell
. .\tools\enter_idf_env.ps1
idf.py menuconfig
```

Menu path:

`Remote Power Controller`

Important settings:

- `Wi-Fi SSID`
- `Wi-Fi password`
- `Enable fallback setup hotspot`
- `Fallback setup hotspot SSID`
- `Fallback setup hotspot password`
- `Use static IPv4 for station Wi-Fi`
- `Static IPv4 address`
- `Static IPv4 netmask`
- `Static IPv4 gateway`
- `Static DNS server 1`
- `Static DNS server 2`
- `API auth token`
- `Servo GPIO`
- `Servo rest angle`
- `Servo press angle`
- `Short press duration`
- `Long press duration`
- `Heartbeat timeout`
- `Alert webhook URL`
- `Enable experimental WireGuard tunnel`
- `WireGuard private key`
- `WireGuard peer public key`
- `WireGuard local tunnel IP`
- `WireGuard peer endpoint`

Note:

- `menuconfig` now acts as a fallback/default Wi-Fi source
- runtime Wi-Fi updates are stored in NVS and survive reboot/power loss
- clearing runtime Wi-Fi settings falls back to `menuconfig`; if those are empty, Wi-Fi stays disconnected until reconfigured
- static IPv4 is a compile-time station setting; if your hotspot subnet changes, update the static IP fields before rebuilding
- when no saved Wi-Fi can connect, the firmware enables a fallback setup hotspot; connect to it and open `http://192.168.4.1/`

## Flash

```powershell
. .\tools\enter_idf_env.ps1
idf.py -p COM3 flash monitor
```

## HTTP API

All API endpoints accept authentication via either:

- query parameter: `token=YOUR_TOKEN`
- header: `X-Auth-Token: YOUR_TOKEN`

If the token is left empty in `menuconfig`, the API is unauthenticated.

Endpoints:

- `GET /`
- `GET /api/help`
- `GET /api/status`
- `GET /api/wifi`
- `GET /api/ota`
- `GET /api/wireguard`
- `GET /api/servo/config`
- `GET /api/report`
- `POST /api/wifi/config`
- `POST /api/wifi/clear`
- `POST /api/servo?angle=45`
- `POST /api/press?mode=short`
- `POST /api/press?mode=long`
- `POST /api/press?ms=350`
- `POST /api/ota?url=https://example.com/firmware.bin`
- `POST /api/report?source=debian-node&payload=JSON_STRING`
- `POST /api/heartbeat?host=NB01&user=alice&cpu=22.4&mem=51.8&uptime=86400`
- `POST /api/alert/test`

Recommended pattern:

- use `POST /api/report` for new scripts and custom payloads
- keep `/api/heartbeat` only for old fixed-field compatibility
- view the last stored payload with `GET /api/report`

Example:

```powershell
$payload = @{
  schema = "rpc.host.v1"
  timestamp = (Get-Date).ToUniversalTime().ToString("o")
  host = $env:COMPUTERNAME
  service = "custom-check"
  result = "ok"
} | ConvertTo-Json -Compress

Invoke-WebRequest `
  -Method Post `
  -Uri "http://192.168.1.50/api/report" `
  -Headers @{ "X-Auth-Token" = "YOUR_TOKEN" } `
  -ContentType "application/x-www-form-urlencoded" `
  -Body ("source=windows-check&payload=" + [Uri]::EscapeDataString($payload))
```

## Web Console

Open the device IP directly in a browser:

```text
http://DEVICE_IP/
```

The built-in page is intentionally lightweight:

- no frontend framework
- no external assets
- same-origin `fetch` calls to the existing HTTP API
- 6-second status polling to keep MCU and Wi-Fi load low

The page includes:

- live device and heartbeat status
- servo angle slider + number input
- short press / long press / custom press buttons
- Wi-Fi save and clear controls
- optional API token input stored in browser local storage

Behavior notes:

- `POST /api/press` now queues the press in a background task and returns immediately
- only one press can run at a time; overlapping requests return `409 Conflict`
- when editing Wi-Fi slots in the web page, leaving a password blank keeps the old password for an unchanged SSID
- if you change a slot to a new SSID and leave password blank, it is treated as an open network
- after saving multiple profiles, the firmware tries the newly changed slot first and then falls back to other saved profiles
- if all saved Wi-Fi profiles fail repeatedly, the firmware enables the fallback setup hotspot at `192.168.4.1`
- WireGuard is now available as an experimental optional service; it stays disabled unless explicitly enabled in `menuconfig`
- when WireGuard is enabled, the firmware waits for Wi-Fi, performs an SNTP time sync, and then starts the tunnel
- upstream `esp_wireguard` is still alpha; IPv4 tunnel use is the realistic target, IPv6 transport should be treated as experimental

Update Wi-Fi at runtime with an authenticated form POST:

```powershell
Invoke-WebRequest `
  -Method Post `
  -Uri "http://192.168.1.50/api/wifi/config" `
  -Headers @{ "X-Auth-Token" = "YOUR_TOKEN" } `
  -ContentType "application/x-www-form-urlencoded" `
  -Body "ssid=YourWifiName&password=YourWifiPassword"
```

Clear the stored runtime Wi-Fi credentials:

```powershell
Invoke-WebRequest `
  -Method Post `
  -Uri "http://192.168.1.50/api/wifi/clear" `
  -Headers @{ "X-Auth-Token" = "YOUR_TOKEN" }
```

## OTA Notes

The project now includes a basic HTTPS OTA flow for `16MB` ESP32-S3 boards.

Current implementation:

- custom partition table with `factory`, `ota_0`, and `ota_1`
- OTA trigger endpoint: `POST /api/ota?url=https://.../firmware.bin`
- OTA status endpoint: `GET /api/ota`
- boot-time confirmation of a newly booted OTA image
- rollback support enabled in the bootloader

Current practical status:

- local build and partition layout have been verified
- the OTA control API has been verified on-device
- a full OTA download still depends on the ESP32 being able to reach the target HTTPS host

Important constraints:

- the OTA URL must be `https://`
- the firmware file must be reachable by the ESP32 itself, not just by your PC
- the image must fit inside the OTA app partition

Example trigger:

```powershell
Invoke-WebRequest `
  -Method Post `
  -Uri "http://192.168.1.50/api/ota?url=https://your-host/remote_power_controller.bin" `
  -Headers @{ "X-Auth-Token" = "YOUR_TOKEN" }
```

Example status check:

```powershell
Invoke-WebRequest `
  -Uri "http://192.168.1.50/api/ota" `
  -Headers @{ "X-Auth-Token" = "YOUR_TOKEN" }
```

## Remote Access Notes

This ESP32 can now optionally run a basic WireGuard tunnel, but the feature should still be treated as experimental.

Practical recommendation:

- prefer WireGuard over exposing the raw HTTP API to the public Internet
- use the ESP32 WireGuard client to join a private network, then access the device through the tunnel
- keep DDNS / ACME / reverse proxy duties on a router, NAS, mini PC, or VPS instead of the ESP32

Not recommended on the ESP32 itself:

- running a full ACME certificate lifecycle for public HTTPS exposure
- acting as a public reverse proxy
- handling direct Internet TLS termination long-term with certificate renewal and challenge management

If you want safer cross-network access, the practical next step is usually one of:

- router or NAS reverse proxy with ACME + DDNS
- a small VPS doing IPv6 reverse proxy
- Tailscale or a standard WireGuard server, with the ESP32 joining as a client

## Serial Console Commands

Commands can also be typed in `idf.py monitor`:

- `help`
- `status`
- `servo 45`
- `press short`
- `press long`
- `press 350`
- `wifi status`
- `wifi set <ssid> [password]`
- `wifi clear`
- `wg status`
- `wg reconnect`
- `wg disconnect`
- `heartbeat`
- `alert test`
- `reboot`

## WireGuard Notes

Current implementation details:

- the project vendors the community `esp_wireguard` component and applies small compatibility fixes for `ESP-IDF 6.0`
- the tunnel is managed by `wireguard_service`
- status is available from `GET /api/wireguard`, `GET /api/status`, and serial `wg status`
- the service currently uses compile-time configuration from `menuconfig`

Before enabling it:

- prepare a WireGuard server or peer first
- make sure the ESP32 has working DNS and outbound UDP access
- configure a reachable SNTP server, because the handshake depends on valid device time
- start with an IPv4 endpoint if you want the highest chance of success

## Windows Report Agent

Run the helper script from this repo on the laptop:

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\host_heartbeat_agent.ps1 `
  -Esp32BaseUrl http://192.168.1.50 `
  -AuthToken YOUR_TOKEN `
  -IntervalSeconds 5
```

The script now sends a single extensible JSON payload string to `POST /api/report`. You can add or remove fields in the script later without changing the ESP32 parser, and the last payload can be viewed from `GET /api/report` or the web console.

## Linux Report Agent

For Debian or other Linux hosts, use:

```bash
python3 ./tools/host_heartbeat_agent.py \
  --esp32-base-url http://192.168.1.50 \
  --auth-token YOUR_TOKEN \
  --interval-seconds 5
```

The Linux script reads `/proc` and is intended for Debian/Linux servers. It sends the same `source + payload` form format as the Windows script, so both sides stay compatible.

## Build Speed Tips

The project already uses `ccache`, so the biggest practical speedups are usually workflow changes:

- use `idf.py app` when you only changed app code
- use `idf.py -p COM5 app-flash monitor` to skip reflashing bootloader and partition table
- avoid touching `sdkconfig` unless you really need to, because it triggers wider rebuilds
- keep generated payload/report logic in the host scripts when possible, so ESP32 firmware changes are less frequent
- if you do a lot of builds, Linux or WSL is usually faster than native Windows for ESP-IDF

Current config is still a debug-style build (`-Og`), which is useful for bring-up but not the fastest possible compile. If you want, we can next make a dedicated faster day-to-day build profile.
