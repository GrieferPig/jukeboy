# Jukeboy — Codebase Architecture

## Overview

Jukeboy is an ESP32-based portable audio player with Bluetooth A2DP streaming,
Wi-Fi connectivity, SD card ("cartridge") support, and Opus audio decoding.
The firmware is built on ESP-IDF (FreeRTOS) and follows a service-oriented
architecture where each subsystem runs as an independent FreeRTOS task
communicating via queues and events.

---

## Directory Layout

```
bttest/
├── CMakeLists.txt              Root build file (ESP-IDF project)
├── partitions.csv              Flash partition table (dual-OTA + NVS + littlefs)
├── sdkconfig.defaults          Build-time Kconfig defaults
├── main/
│   ├── CMakeLists.txt          Component build file
│   ├── main.c                  Entry point — app_main()
│   ├── player_service.c/.h     Audio decode/playback state machine
│   ├── bluetooth_service.c/.h  Classic BT: A2DP source, AVRCP, GAP, SPP
│   ├── wifi_service.c/.h       Wi-Fi STA: connect, scan, NVS creds, SNTP
│   ├── cartridge_service.c/.h  SD card mount/unmount, metadata, async reader
│   ├── i2s_service.c/.h        I2S output (currently a no-op stub)
│   ├── audio_output_switch.c/.h PCM routing between BT and I2S outputs
│   ├── console_service.c/.h    UART console: commands + telemetry task
│   └── jukeboy_formats.h       On-disk format structs (.jba, .jbm, .jbs)
├── components/
│   ├── bt/                     Bluetooth stack component overrides
│   ├── esp_audio_codec/        Opus decoder component
│   └── mpy/                    MicroPython component (unused at runtime)
├── docs/
│   ├── architecture.md         This file
│   └── jukeboy_formats.md      Detailed file-format specification
└── tools/
    └── make_album.py           Host-side tool to produce .jba/.jbm files
```

---

## Service Architecture

All services follow the same pattern:

1. **Init** — creates queue + task, registers event handlers.
2. **Task loop** — blocks on a command queue; dispatches each message type.
3. **Public API** — thin wrappers that enqueue commands and return immediately.
4. **Events** — services post `esp_event` notifications for inter-service signalling.

```
┌─────────────┐  esp_event   ┌──────────────┐
│  cartridge  │─────────────▶│   player     │
│  service    │  INSERTED /  │   service    │
│             │  UNMOUNTED   │              │
└─────────────┘              └──────┬───────┘
                                    │ registers PCM provider
                             ┌──────▼───────┐
                             │ audio_output │
                             │   switch     │
                             └──┬───────┬───┘
                                │       │
                         ┌──────▼──┐ ┌──▼──────────┐
                         │  i2s    │ │  bluetooth   │
                         │ service │ │  service     │
                         │ (stub)  │ │  (A2DP src)  │
                         └─────────┘ └──────────────┘
```

### Task Pinning & Stack Allocation

| Task              | Core | Stack (bytes) | Memory     | Notes                                      |
|-------------------|------|---------------|------------|---------------------------------------------|
| `player_svc`      | 0    | 4096          | Internal   | Service coordinator                         |
| `player_reader`   | 1    | 4096          | Internal   | SD card → chunk queue                       |
| `player_decoder`  | 1    | 12288         | Internal   | Opus decode → PCM stream                    |
| `cart_reader`     | any  | 4096          | Internal   | Async 128 KB block reads from SD            |
| `bt_svc`          | any  | 2048          | Internal   | BT command dispatch (must stay internal)    |
| `wifi_svc`        | 0    | 4096          | Internal   | NVS/flash APIs require internal stack       |
| `i2s_svc`         | 0    | 2048          | Internal   | Stub — blocks on queue, no-op               |
| `console_svc`     | —    | managed by ESP | Internal  | UART REPL task (esp_console)                |
| `telemetry`       | any  | 2048          | PSRAM      | Periodic heap/CPU stats                     |

### Memory Placement

- **PSRAM (`EXT_RAM_BSS_ATTR` / `MALLOC_CAP_SPIRAM`)**: cartridge read buffers
  (2×128 KB), playlist array (dynamic), lookup tables, metadata blob, telemetry
  snapshot arrays, Wi-Fi scan results.
- **Internal DRAM**: task stacks for services that access NVS/flash (Wi-Fi, BT),
  PCM decode buffer, FreeRTOS primitives.

---

## Player Service — Detailed Design

### Pipeline

```
 SD card
   │
   ├── [cart_reader task]  ───▶  128 KB PSRAM double-buffer
   │
   ├── [player_reader task]
   │       Reads chunks from the double-buffer via cartridge_service_read_chunk_async().
   │       Validates chunk boundaries against the lookup table.
   │       Sends chunk messages through s_chunk_queue.
   │
   ├── [player_decoder task]
   │       Receives chunk messages from s_chunk_queue.
   │       Verifies CRC32 per chunk.
   │       Parses Opus packet framing (1- or 2-byte length header).
   │       Decodes packets via esp_opus_dec_decode().
   │       Writes decoded PCM to s_pcm_stream (StreamBuffer).
   │
   └── [PCM consumer callback]  — player_service_pcm_provider()
           Called from BT A2DP data callback or I2S pull loop.
           Reads from s_pcm_stream with zero-fill on underrun.
           Applies volume scaling (Q8 fixed-point gain table).
```

### Generation Counter

Each `player_service_start_track()` increments `s_active_generation`. Reader and
decoder tasks receive the generation ID at creation time and check it on every
loop iteration via `player_service_should_stop()`. This avoids stale data from
a previous track leaking into a new playback session.

### Playback Status Persistence

The player writes a `playback.jbs` file to the cartridge every
`PLAYER_SVC_STATUS_SAVE_INTERVAL_SECONDS` (10 s), recording the current track
index and second. Writes are crash-safe: data is written to a `playback.tmp` file first,
then atomically renamed to the final path.

### Playback Modes

The player supports three sequence modes, switchable at runtime:

| Mode            | Auto-advance behaviour                          | Explicit NEXT behaviour          |
|-----------------|-------------------------------------------------|----------------------------------|
| `sequential`    | Next track in order; wraps to first at end.     | Next track in order (default).   |
| `single_repeat` | Repeat the same track indefinitely.             | Skip to sequential next.         |
| `shuffle`       | Pick a random track (different from current).   | Pick a random track.             |

The mode is stored in `s_playback_mode` (RAM only, resets to `sequential` on boot).
Explicit PREV always behaves sequentially regardless of mode.
Change the mode via `player_service_set_playback_mode()` or the `media mode` console command.

### Playlist Management

The playlist is dynamically allocated in PSRAM when a cartridge is inserted.
Each entry stores a short filename (`NNN.jba`, max 16 chars) and a display
title (up to 128 chars from metadata). The playlist is freed on cartridge
unmount or re-insert.

---

## Cartridge Service

Manages the SD card lifecycle:

- **Mount/Unmount** — `esp_vfs_fat_sdmmc_mount()` with 1-bit SDMMC at 20 MHz.
- **Metadata** — loads `album.jbm` into PSRAM, validates CRC32, exposes fields.
- **Async Reader** — a dedicated task reads 128 KB blocks into a PSRAM
  double-buffer. The caller is notified via `xTaskNotifyGive()`.
- **Events** — posts `CARTRIDGE_SVC_EVENT_INSERTED`, `CARTRIDGE_SVC_EVENT_MOUNTED`,
  `CARTRIDGE_SVC_EVENT_UNMOUNTED`.

On unmount, the service posts `CARTRIDGE_SVC_EVENT_UNMOUNTED`, which the player
service listens for to immediately stop playback and free resources.

---

## Bluetooth Service

Implements an A2DP **source** (the ESP32 streams audio to a BT speaker/headset):

- **GAP** — device discovery, pairing, bonded device management.
- **A2DP** — connection state, audio state, SBC endpoint registration.
- **AVRCP** — passthrough commands (play/pause/next/prev), absolute volume.
- **SPP** — serial port profile server (for debug/control).

All GAP/A2DP/AVRCP callbacks post messages to a single BT service queue.
The BT service task processes them sequentially, avoiding re-entrant BT API
calls.

The PCM data callback (`esp_a2d_source_data_cb`) invokes the registered
provider function (wired through `audio_output_switch`) to pull PCM data.

---

## Audio Output Switch

A thin routing layer that directs the PCM provider to either the Bluetooth
A2DP endpoint or the I2S output:

- Maintains a `s_provider` function pointer (set by player service).
- `audio_output_switch_select()` unregisters from the old target and registers
  with the new one.
- Listens for `BLUETOOTH_SVC_EVENT_A2DP_CONNECTION_STATE` to automatically
  switch to BT when a speaker connects, and fall back to I2S on disconnect.

---

## Wi-Fi Service

Manages Wi-Fi STA mode:

- **Connect/Disconnect** — queued commands with NVS credential persistence.
- **Scan** — async scan with results stored in PSRAM.
- **Auto-reconnect** — optional 30-second periodic timer.
- **SNTP** — initialised on first successful connection.

The service task runs on core 0 with an internal stack (required for NVS/flash
API calls). All Wi-Fi/IP event callbacks post commands to the service queue
rather than calling Wi-Fi APIs directly, avoiding deadlocks.

---

## I2S Service

Currently a **stub** — the task blocks on its command queue and discards all
messages. No I2S hardware is driven. When real I2S support is added, the task
should be updated to periodically pull PCM data from the registered provider
and feed it to the I2S DMA.

---

## Console Service

Provides a UART-based interactive console (`esp_console`):

### Commands

| Command                        | Description                           |
|--------------------------------|---------------------------------------|
| `bt pair`                      | Discover and pair with best-RSSI device |
| `bt connect`                   | Connect to last bonded device         |
| `bt disconnect`                | Disconnect A2DP                       |
| `bt audio start\|stop`         | Start/stop A2DP audio streaming       |
| `bt media_key <key>`           | Send AVRCP passthrough key            |
| `bt bonded`                    | List bonded devices                   |
| `wifi connect <ssid> [pass]`   | Connect to a Wi-Fi AP                 |
| `wifi disconnect`              | Disconnect                            |
| `wifi scan`                    | Scan for APs                          |
| `wifi status`                  | Show connection state/IP              |
| `wifi autoreconnect [on\|off]` | Toggle 30 s auto-reconnect            |
| `sd mount`                     | Mount SD card                         |
| `sd unmount`                   | Unmount SD card (stops playback)      |
| `sd status`                    | Show cartridge info                   |
| `sd meta <field> [idx]`        | Query album/track metadata            |
| `media <control>`              | Playback control (next/prev/pause/…)  |
| `media mode [<mode>]`          | Get or set playback mode (sequential/single_repeat/shuffle) |
| `audio <a2dp\|i2s>`            | Switch audio output target            |
| `reboot`                       | Reboot the device                     |
| `meminfo`                      | Print heap statistics                 |
| `telemetry [on\|off]`          | Toggle periodic CPU/memory telemetry  |

### Telemetry Task

A background task prints memory and per-task CPU usage every 5 seconds
(when enabled). It uses double-buffered `TaskStatus_t` snapshots in PSRAM
to compute interval-based CPU percentages.

---

## On-Disk Formats

See [jukeboy_formats.md](jukeboy_formats.md) for full specification.

### Summary

| File          | Format  | Purpose                                     |
|---------------|---------|---------------------------------------------|
| `album.jbm`  | JBM v1  | Album metadata: name, artist, track list    |
| `NNN.jba`     | JBA v1  | Audio file: header + lookup table + Opus chunks |
| `playback.jbs`| JBS v1  | Playback resume state (track + second)      |

### JBA File Structure

```
┌─────────────────────────┐
│  jukeboy_jba_header_t   │  version, header_len_in_blocks, lookup_table_len
├─────────────────────────┤
│  uint32_t lookup_table  │  byte offset per second of audio
│  [lookup_table_len]     │  (relative to data_offset)
├─────────────────────────┤
│  padding to block align │
├─────────────────────────┤  ← data_offset = header_len_in_blocks × 512
│  Opus chunk 0           │  CRC32 (4B) + opus packets (1/2-byte len + data)
│  Opus chunk 1           │
│  ...                    │
└─────────────────────────┘
```

Each chunk represents 1 second of audio. Chunks are CRC32-verified at decode
time. Opus packets within a chunk use a compact framing format:

- If the first byte < 252: that byte is the frame length.
- Otherwise: frame_length = (second_byte × 4) + first_byte (2-byte header).
- Maximum frame length: 1275 bytes (per Opus specification).

---

## Build & Flash

```bash
idf.py set-target esp32
idf.py build
idf.py flash monitor
```

### Partition Layout

| Name       | Type | Size    | Notes                      |
|------------|------|---------|----------------------------|
| nvs        | data | 248 KB  | NVS key-value store        |
| otadata    | data | 8 KB    | OTA state                  |
| phy_init   | data | 4 KB    | PHY calibration            |
| ota_0      | app  | 3.4 MB  | Application slot 0         |
| ota_1      | app  | 3.4 MB  | Application slot 1         |
| littlefs   | data | 1 MB    | LittleFS storage           |

---

## Key Design Decisions

1. **Internal stacks for NVS-touching tasks** — BT and Wi-Fi service tasks keep
   their stacks in internal RAM because `esp_task_stack_is_sane_cache_disabled`
   asserts during flash operations would trip with PSRAM-backed stacks.

2. **Event-driven IPC, not direct calls** — services communicate via `esp_event`
   and FreeRTOS queues. This avoids calling Wi-Fi/BT APIs from event callbacks
   (which can deadlock) and keeps service state manipulation single-threaded
   within each task.

3. **Double-buffered cartridge reads** — two 128 KB PSRAM buffers allow the
   reader task to prepare the next block while the decoder processes the current
   one. A counting semaphore (`s_buf_pool_sem`) gates buffer checkout.

4. **Crash-safe status writes** — playback position is written to a temporary
   file and renamed, preventing corruption if power is lost mid-write.

5. **Dynamic playlist allocation** — the playlist is `heap_caps_calloc()`'d in
   PSRAM sized to the actual track count, rather than a fixed 128-entry array.
