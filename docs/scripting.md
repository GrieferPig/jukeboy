# WASM Scripting

The firmware embeds Espressif's WASMachine core and exposes a native host module named `jukeboy` plus builtin socket natives in the `env` module.

## Console commands

- `script status` prints the service state, host module name, fixed scripts root, the currently running script if one is active, and the most recent completed run summary. Active and completed runs are tagged with run IDs.
- `script ls [name]` lists `/lfs/scripts` when no argument is given, or one script directory when a filename is provided.
- `script log <name>` prints the temporary ramdisk log for one resolved script.
- `script run <name> [args...]` queues the script in the background, prints its run ID, and forwards the remaining arguments to `main(argc, argv)`.

## Script roots and resolution

Resolver behavior:

- The only lookup root is `/lfs/scripts`.
- Script inputs must be bare filenames. Absolute paths, directory separators, and traversal forms such as `..` are rejected.
- Accepted filename characters are ASCII letters, digits, `_`, and `-`, with an optional `.wasm`, `.cwasm`, or `.aot` suffix.
- `script run hello` resolves by probing `/lfs/scripts/hello/hello.wasm`, then `/lfs/scripts/hello/hello.cwasm`, and, when AOT support is enabled, `/lfs/scripts/hello/hello.aot`.
- `script run hello.wasm` resolves only `/lfs/scripts/hello/hello.wasm`.

Examples:

- `script ls`
- `script ls hello`
- `script run hello`
- `script run hello.cwasm`

## Run results

`script run` prints:

- `Run ID`
- `Resolved`
- `Mode`
- `Size`
- `Status`

Captured output is currently limited to 2048 bytes. Use `script status` to monitor the active run and inspect the latest completed result, including exit code, output, and message.

`script status` reports the active run ID while a script is still executing, then moves that run into the `Last ...` section when it finishes.

Each resolved script also gets a temporary text log in `/tmp/script-logs/<resolved-filename>.log`. The runtime records script stdout and stderr there, appends `jukeboy.log()` lines, and trims whole oldest lines after completion so the file stays within 32 KiB. Use `script log <name>` to print it from the console.

## Host module

Scripts can import native functions from the module name `jukeboy`.

Current host functions:

- `log(message: string) -> int`
- `next_track() -> int`
- `previous_track() -> int`
- `pause_toggle() -> int`
- `fast_forward() -> int`
- `rewind() -> int`
- `volume_up() -> int`
- `volume_down() -> int`
- `set_playback_mode(mode: int) -> int`
- `get_playback_mode() -> int`
- `is_paused() -> int`
- `get_volume_percent() -> int`
- `set_volume_percent(percent: int) -> int`
- `sleep_ms(milliseconds: int) -> int`
- `get_track_count() -> int`
- `get_track_title(index: int, buffer: char *, buffer_len: int) -> int`
- `wifi_is_connected() -> int`
- `get_free_heap() -> int`
- `get_uptime_ms() -> int64`

Playback mode values match `player_service_playback_mode_t`:

- `0`: sequential
- `1`: single repeat
- `2`: shuffle

Command-style functions return `esp_err_t`-style integers. Getter-style functions return the requested scalar value. `get_track_title` writes a NUL-terminated string into the caller-provided buffer and returns `ESP_OK` or an error.

## C import example

```c
__attribute__((import_module("jukeboy")))
__attribute__((import_name("log")))
int log_message(const char *message);

__attribute__((import_module("jukeboy")))
__attribute__((import_name("next_track")))
int next_track(void);

__attribute__((import_module("jukeboy")))
__attribute__((import_name("get_uptime_ms")))
long long get_uptime_ms(void);
```

## Builtin runtime behavior

Scripts now run only in libc-builtin mode. `.wasm` and `.cwasm` both resolve to builtin modules, and the runtime accepts either `main(argc, argv)` or `__main_argc_argv`.

The firmware exposes the `jukeboy` host module for device control and an `env` socket shim that implements the existing `sock_*` API on top of ESP-IDF BSD sockets. That gives builtin scripts DNS lookup plus TCP and UDP access without WASI.

The native `ScriptRunner` thread keeps its own stack in internal RAM, but it runs at idle priority so compute-heavy scripts do not starve the ESP-IDF idle tasks and trip the task watchdog. The managed WASM stack and heap are each 128 KiB and are allocated from PSRAM through the WAMR allocator when external RAM is enabled.

The service still runs one script at a time. Launches are therefore serialized: a second `script run ...` will fail while another script is active, and `script status` will report that active run until it completes.

## QEMU validation flow

The current workflow is manual. There is no supported `tools/qemu_wasm_smoketest.py` helper in this repo anymore.

Staged LittleFS scripts for QEMU live under:

- `flashfs/scripts/hello`
- `flashfs/scripts/google-get`
- `flashfs/scripts/player-control`
- `flashfs/scripts/net-echo`
- `flashfs/scripts/coremark`

The staged SD payload comes from `tools/out`, including the default `album.jbm` plus the numbered `.jba` track files.

### Build and launch

1. Build the firmware with the normal ESP-IDF flow.
2. If you changed the staged SD payload under `tools/out`, regenerate `build/sd_image.bin` with `python tools/ensure_sd_image.py --image-path build/sd_image.bin --source-dir tools/out`.
3. Launch QEMU with `python tools/run_qemu_firmware.py` from the ESP-IDF Python environment.

`tools/run_qemu_firmware.py` merges `build/qemu_flash.bin`, creates a blank `build/qemu_efuse.bin` on demand, and launches the default QEMU binary under `qemu-official-9.0.0`. It expects the firmware build outputs and `build/sd_image.bin` to already exist.

To use a different QEMU executable, pass it explicitly:

```sh
python tools/run_qemu_firmware.py --qemu-exe qemu-official-9.0.0/install/qemu/bin/qemu-system-xtensa.exe
```

Additional useful options from the current helper:

- `--skip-flash-merge` reuses the existing `build/qemu_flash.bin`
- `--no-openeth` disables the default user-network OpenETH NIC
- `--hostfwd tcp:127.0.0.1:7001-:7001` adds user-network port forwarding when OpenETH is enabled
- `--capture-file out.wav --capture-limit-bytes 960000` captures PCM output and optionally auto-shuts down QEMU
- `--dry-run` prints the merge and QEMU commands without executing them

If you only need to restage cartridge fixtures without launching QEMU, run:

```sh
python tools/ensure_sd_image.py --image-path build/sd_image.bin --source-dir tools/out
```

### Manual console checklist

After boot reaches the console prompt, run this baseline checklist:

- `help`
- `sd status`
- `sd meta album_name`
- `sd meta track_name 0`
- `script status`
- `script ls`
- `script run hello`
- `script log hello`
- `script run player-control status`

Optional networking checks:

- `script run google-get`
- `script run net-echo listen 7001` after launching QEMU with `--hostfwd tcp:127.0.0.1:7001-:7001`
- `script run net-echo client 10.0.2.2 7002 "hello from qemu"` while a host listener is already waiting on an unforwarded host port such as `7002`

Long-running script and bounded-log check:

- `script run coremark.wasm 0 0 0 10000`

There is no script wall-clock timeout in the current runtime. Use this run to observe long-running script behavior and confirm the console stays responsive while the worker is busy. Per-script logs are still trimmed to 32 KiB on completion.

### Malformed cartridge fixtures

Use `tools/out` as the staging root for malformed `album.jbm` or `.jba` fixtures. After replacing the staged files, regenerate the SD image with `python tools/ensure_sd_image.py --image-path build/sd_image.bin --source-dir tools/out` and boot QEMU again.

The target checks in this pass are:

- oversized metadata files
- invalid lookup-table lengths
- chunks that are too small to contain the CRC32 prefix
- invalid console metadata indices

These should fail with errors instead of crashes or hangs.

### Hardware-only follow-up

QEMU intentionally skips the live Wi-Fi and Bluetooth stacks in this firmware, so encrypted-credential validation, pairing confirmation, and runtime log-redaction checks still need real hardware.
