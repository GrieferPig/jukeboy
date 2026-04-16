# Local QEMU Fork — `qemu-official-9.0.0`

This document covers every change and concern that is specific to the local fork of the Espressif QEMU tree checked out at `qemu-official-9.0.0/`.  It is not a general QEMU reference.

---

## 1. Fork identity

| Item | Value |
|------|-------|
| Remote repository | <https://github.com/GrieferPig/qemu> |
| Branch | `qemu-official-9.0.0` |
| Espressif upstream base | `esp-develop` head `33cc9a8` (QEMU 9.2.2) |
| Official shipped binary baseline | tag `esp-develop-9.0.0-20240606` → commit `abb5ce24386972e048b401f9eca10e90b8427a20` (QEMU 9.0.0) |

The local fork targets a newer `esp-develop` head than Espressif's released Windows binary.  The two diverge at a QEMU core reset-framework boundary: commits `9607e262` (three-phase reset refactor) and `dcf57b8d` (CAN peripheral) are present in the fork but not in the `9.0.0` release.  This has no impact on correctness for audio testing but means the binaries are not drop-in replacements for each other.

---

## 2. New peripheral — `esp32_qemu_pcm`

### Purpose

The upstream Espressif fork models the ESP32 I2S0 peripheral as an `unimp` (unimplemented) stub, so any firmware that tries to use audio simply aborts.  This fork replaces the I2S0 page with a lightweight double-buffer PCM playback peripheral that the firmware firmware can drive without any real I2S DMA logic.

### Source files

| File | Role |
|------|------|
| `hw/misc/esp32_qemu_pcm.c` | Peripheral implementation |
| `include/hw/misc/esp32_qemu_pcm.h` | Public header / type definitions |
| `hw/misc/meson.build` | Build integration (added `esp32_qemu_pcm.c` to `CONFIG_XTENSA_ESP32`) |
| `hw/xtensa/esp32.c` | Machine wiring (object init + realize + buffer window mapping) |

### Memory map

| Region | Base address | Size |
|--------|-------------|------|
| Control MMIO | `0x3ff4f000` (`DR_REG_I2S_BASE`) | 4 KiB |
| Buffer 0 (PCM RAM) | `0x70000000` | 64 KiB |
| Buffer 1 (PCM RAM) | `0x70010000` | 64 KiB |

The control page overlaps what upstream maps as `esp32.i2s0 unimp`.  Upstream's unimp entry is kept for I2S1 (`DR_REG_I2S1_BASE`) since only I2S0 is replaced.

### Control register ABI

All registers are 32-bit, little-endian, read/write unless noted.  Offset is relative to the control MMIO base.

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| `0x00` | `MAGIC` | R | Always `0x514d4350` ("QEMU" in ASCII).  Firmware uses this to detect the device. |
| `0x04` | `VERSION` | R | `0x00000001` |
| `0x08` | `BUFFER0_ADDR` | R | Physical address of buffer 0 (`0x70000000`). |
| `0x0c` | `BUFFER1_ADDR` | R | Physical address of buffer 1 (`0x70010000`). |
| `0x10` | `BUFFER_SIZE` | R | Buffer size in bytes (`0x10000` = 64 KiB). |
| `0x14` | `SAMPLE_RATE` | R | `48000` |
| `0x18` | `CHANNELS` | R | `2` |
| `0x1c` | `BITS_PER_SAMPLE` | R | `16` |
| `0x20` | `CURRENT_BUFFER` | R | Index (0 or 1) of the buffer currently being consumed, or `0xFFFFFFFF` if idle. |
| `0x24` | `QUEUED_MASK` | R | Bitmask of buffers that have been submitted and not yet fully consumed. |
| `0x28` | `UNDERRUN_COUNT` | R | Number of times the host needed samples but both buffers were empty. |
| `0x2c` | `BUFFER0_LENGTH` | R/W | Number of valid bytes in buffer 0.  Write before submitting; clamped to buffer size and frame-aligned. |
| `0x30` | `BUFFER1_LENGTH` | R/W | Same for buffer 1. |
| `0x34` | `BUFFER0_SUBMIT` | W | Write any non-zero value to enqueue buffer 0 for playback. |
| `0x38` | `BUFFER1_SUBMIT` | W | Same for buffer 1. |
| `0x3c` | `CONTROL` | W | Bit 0 (`RESET`): resets all state, clears both buffers, and stops the timer. |

### Host-side audio

On Windows the peripheral uses **miniaudio** (WinMM backend) for real-time playback.  miniaudio calls `esp32_qemu_pcm_miniaudio_cb()` from its own thread, which takes the device lock and calls `esp32_qemu_pcm_pump_locked()`.

On non-Windows hosts miniaudio is not used.  A 10 ms `QEMU_CLOCK_REALTIME` timer ticks instead and simply discards consumed bytes silently, keeping the submit-sequence advancing so firmware does not stall.

### Capture-to-file (test support)

Two QOM device properties allow capturing the PCM stream for automated testing.  They are set via `-global` on the QEMU command line:

| Property | Type | Default | Meaning |
|----------|------|---------|---------|
| `misc.esp32.qemu-pcm.capture-file` | string | *(none)* | Path to output file.  `.wav` extension → standard PCM WAV header written and patched.  Any other extension → raw signed-16-bit stereo 48 kHz samples. |
| `misc.esp32.qemu-pcm.capture-limit-bytes` | uint64 | `0` | If non-zero, QEMU shuts down via `SHUTDOWN_CAUSE_HOST_SIGNAL` once this many bytes have been captured.  `0` means run until QEMU exits normally. |

**Example — capture 5 seconds then exit:**

```sh
# 48000 Hz × 2 ch × 2 bytes/sample × 5 s = 960 000 bytes
qemu-system-xtensa.exe \
  -L qemu/pc-bios -nographic -machine esp32 -m 4M \
  -drive file=build/qemu_flash.bin,if=mtd,format=raw \
  -drive file=build/qemu_efuse.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32efuse,property=drive,value=efuse \
  -drive file=build/sd_image.bin,if=sd,format=raw \
  -global misc.esp32.qemu-pcm.capture-file=test_out.wav \
  -global misc.esp32.qemu-pcm.capture-limit-bytes=960000
```

After exit, `test_out.wav` is a valid 48 kHz / 16-bit / stereo WAV file.  The WAV RIFF and data chunk size fields are placeholder zeros during the run and are patched correctly in `instance_finalize`.

---

## 3. Firmware counterpart — `qemu_pcm_service`

The ESP-IDF application contains a matching service in `main/qemu_pcm_service.c` that is only started when the firmware detects it is running under QEMU (blank eFuse MAC, see §5).

### Key constants (firmware side)

| Constant | Value | Notes |
|----------|-------|-------|
| `QEMU_PCM_SVC_REG_BASE` | `0x3ff4f000` | Must match QEMU control MMIO base |
| `QEMU_PCM_SVC_BUFFER_SIZE` | `0x10000` | 64 KiB; validated against `BUFFER_SIZE` register on init |
| `QEMU_PCM_SVC_SUBMIT_TARGET_BYTES` | `7680` | 40 ms worth of audio per submit (48000 × 4 × 40/1000) |
| `QEMU_PCM_SVC_POLL_MS` | `10` | Task poll interval; the service clamps the delay to at least 1 FreeRTOS tick |
| `QEMU_PCM_SVC_TASK_PRIORITY` | `6` | Pinned to core 1; keep it aligned with the decoder task to avoid host-side underruns |

### Initialisation flow

1. `qemu_pcm_service_init()` reads `MAGIC`; returns `ESP_ERR_NOT_SUPPORTED` if not `0x514d4350` (safe to call on hardware — it will just find garbage at that address and refuse).
2. Validates `BUFFER_SIZE == 0x10000`.
3. Reads `BUFFER0_ADDR` / `BUFFER1_ADDR` registers to get the physical buffer pointers.
4. Resets the device, creates the command queue, and pins the feeder task to core 1.
5. Caller then calls `qemu_pcm_service_register_pcm_provider()` passing `player_service_qemu_pcm_provider` and `qemu_pcm_service_start_audio()` to begin feeding.

### Feeder loop

Every `QEMU_PCM_SVC_POLL_MS` the task calls `qemu_pcm_service_fill_free_buffers()`, which reads `CURRENT_BUFFER` and `QUEUED_MASK`, and for each buffer that is neither active nor queued: calls the registered PCM provider to fill it up to `SUBMIT_TARGET_BYTES`, writes the actual byte count to the length register, and writes 1 to the buffer's submit register.

---

## 4. Build system changes

### `meson.build` (top-level)

Added Windows-only linker flags under `if host_os == 'windows'`:

```meson
qemu_ldflags += ['-liconv', '-Wl,--allow-multiple-definition']
```

`-Wl,--allow-multiple-definition` is required because miniaudio is a single-header library included with `MINIAUDIO_IMPLEMENTATION`; without it the linker may see duplicate symbol definitions when multiple translation units are linked.

### `hw/misc/meson.build`

`esp32_qemu_pcm.c` is added to the `CONFIG_XTENSA_ESP32` source set so it is compiled into the `xtensa-softmmu` target automatically.

### Required MSYS2 package

The Windows build requires `mingw-w64-x86_64-miniaudio`.  This must be installed before configuring:

```sh
pacman -S mingw-w64-x86_64-miniaudio
```

---

## 5. CI / GitHub Actions

The workflow is at `.github/workflows/build.yml`.  It builds `xtensa-softmmu` for two platforms:

| Platform | Runner | Notes |
|----------|--------|-------|
| `x86_64-linux-gnu` | `ubuntu-22.04` + `debian:11.11` container | Uses `--enable-slirp`, `-Werror`.  slirp enabled because the Linux binary is used for testing, not just Windows. |
| `x86_64-w64-mingw32` | `windows-2022` + MSYS2 | Uses `--disable-slirp`, `-Wno-error`, `--static`. |

### Notable workflow design decisions

- **`ACTIONS_RUNTIME_TOKEN` guard on artifact upload** — `actions/upload-artifact` steps are wrapped in `if: ${{ env.ACTIONS_RUNTIME_TOKEN != '' }}` so local `act` self-hosted test runs do not fail on missing artifact tokens.
- **Pre-step CRLF normalisation** (Linux leg) — script files in git may have CRLF on Windows checkouts; the workflow runs `sed -i 's/\r$//'` and `chmod +x` on all `.github/workflows/scripts` and `scripts` files before use inside the Debian container.
- **Build state reset** — `rm -rf build install dist` / PowerShell equivalent runs before configure so that re-runs in local `act` do not inherit stale generated files.
- **`configure-win.sh` libintl/libiconv path fix** — after `./configure`, the script uses `sed` to rewrite bare `/mingw64/lib/libintl.dll.a` paths in `build/build.ninja` to absolute MSYS2 paths because meson picks up the wrong relative placeholders for static libs.

### Packaging

The package step strips all `share/qemu/` files except `esp*.bin` (the ESP32 firmware ROMs), then archives the `install/qemu/` tree as a `.tar.xz`.  Archives are named `<project>-xtensa-softmmu-<version>-<platform>.tar.xz`.

Artifacts are uploaded only on tag pushes (Upload job is guarded with `if: startsWith(github.ref, 'refs/tags/')`).

---

## 6. Known limitations and workarounds

| Issue | Workaround |
|-------|-----------|
| `tests/unit/test-vmstate.exe` and `tests/qtest/qos-test.exe` fail on Windows because `qemu_ftruncate64` is defined in `block/file-win32.c` but not linked into test binaries. | The local build script (`scripts/build-win-local.sh`) builds `qemu-system-xtensa.exe` directly with `ninja` and copies it into `install/`, bypassing `ninja install` which would also try to link the failing tests. |
| ESP32 QEMU flash/cache emulation faults under sustained playback (cache-disabled panic in idle/task-wdt path). | This is a known upstream emulation gap.  The firmware's QEMU startup branch skips NVS init, LittleFS mount, and `open_eth` init to reduce cache pressure.  Playback still starts and produces audio before the fault appears. |
| Short millisecond delays can collapse to 0 ticks on this FreeRTOS tick config and starve the idle task. | Keep the feeder poll interval at or above one tick equivalent; the service now uses a 10 ms poll and clamps the computed delay to at least 1 tick. |
| SD images whose total size is not a power of two cause the ESP32 QEMU SD driver to stall before the monitor port opens. | `tools/ensure_sd_image.py` hardcodes a 1 GiB image (2³⁰ bytes). |
| FAT volumes with 4096-byte logical sectors fail to mount in QEMU's SDMMC emulation. | The SD image generator creates the FAT32 partition with 512-byte logical sectors. |

---

## 7. Running locally on Windows

### One-time build

```sh
# From an MSYS2 MINGW64 shell
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-glib2 mingw-w64-x86_64-libgcrypt \
          mingw-w64-x86_64-libiconv mingw-w64-x86_64-meson mingw-w64-x86_64-miniaudio \
          mingw-w64-x86_64-ninja mingw-w64-x86_64-pixman mingw-w64-x86_64-pkg-config \
          mingw-w64-x86_64-python mingw-w64-x86_64-python-distlib mingw-w64-x86_64-SDL2

cd qemu-official-9.0.0
VERSION=local TARGET=xtensa-softmmu bash .github/workflows/scripts/configure-win.sh
ninja -C build qemu-system-xtensa.exe
cp build/qemu-system-xtensa.exe install/qemu/bin/
```

The staged executable is at `qemu-official-9.0.0/install/qemu/bin/qemu-system-xtensa.exe`.

### Running the firmware

Before running, regenerate the merged flash image after any firmware rebuild:

```sh
# From the ESP-IDF build directory (bttest/build/)
esptool.py --chip esp32 merge_bin --fill-flash-size 8MB -o qemu_flash.bin @flash_args
```

Then launch:

```sh
qemu-official-9.0.0/install/qemu/bin/qemu-system-xtensa.exe \
  -L qemu-official-9.0.0/pc-bios \
  -nographic \
  -machine esp32 \
  -m 4M \
  -drive file=build/qemu_flash.bin,if=mtd,format=raw \
  -drive file=build/qemu_efuse.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32efuse,property=drive,value=efuse \
  -drive file=build/sd_image.bin,if=sd,format=raw
```

Run from the repo root so that relative paths resolve correctly.  Do **not** use Windows absolute paths in `-drive file=…` arguments — the MSYS2 path translation strips backslashes.

### Expected boot log milestones

```text
detected QEMU runtime from blank factory eFuse MAC; skipping Wi-Fi, Bluetooth, and I2S service init
...
qemu_pcm_svc: QEMU PCM service ready (buffer0=0x70000000 buffer1=0x70010000 size=65536)
qemu_pcm_svc: QEMU PCM playback enabled
player_svc: starting track 1/N
esp32>
```
