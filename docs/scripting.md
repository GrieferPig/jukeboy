# WASM Scripting

The firmware embeds Espressif's WASMachine core and exposes a native host module named `jukeboy` to WASM scripts.

## Console commands

- `script status` prints the service state, host module name, and configured roots.
- `script roots` prints the root labels and backing directories.
- `script ls [root|path]` lists all configured roots when no argument is given, or one root/path when an argument is provided.
- `script resolve <path>` prints the fully resolved script path.
- `script run <path> [args...]` runs a module and forwards the remaining arguments to `_start`.

## Script roots and resolution

Configured roots:

- `lfs` -> `/lfs/scripts`
- `tmp` -> `/tmp/scripts`
- `sd` -> `/sdcard/scripts`

Resolver behavior:

- Absolute paths are tried first.
- Labeled paths such as `lfs/hello`, `tmp/demo`, and `sd/net-echo.wasm` resolve against the matching root.
- Bare names are resolved by searching the configured roots.
- If the provided path has no extension, the resolver also tries `.wasm` and, when AOT support is enabled, `.aot`.
- On hardware, bare names are searched in this order: `/lfs/scripts`, `/tmp/scripts`, `/sdcard/scripts`.
- Under QEMU, bare names are searched in this order: `/sdcard/scripts`, `/tmp/scripts`, `/lfs/scripts`.

Examples:

- `script ls`
- `script resolve hello`
- `script run hello`
- `script run lfs/hello`
- `script run /sdcard/scripts/demo.wasm first second`

## Run results

`script run` captures WASI stdout/stderr and prints:

- `Resolved`
- `Size`
- `Exit code`
- `Output` when the script wrote to stdout or stderr
- `Result` or `Error`

Captured output is currently limited to 2048 bytes. A non-zero script exit code makes the console command fail even when the runtime completed normally.

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

## WASI runtime behavior

Each run gets these environment variables:

- `JUKEBOY=1`
- `JUKEBOY_QEMU=0` or `JUKEBOY_QEMU=1`

The runtime enables WASI socket access for any address and wildcard DNS lookups.

Filesystem preopens are narrower than the earlier implementation:

- On hardware, the runtime preopens `/lfs/scripts` only when the resolved module lives under that LittleFS root.
- Scripts launched from `/tmp/scripts` or `/sdcard/scripts` still run, but they do not receive a default WASI filesystem preopen.
- Under QEMU, the runtime currently passes no preopened directories, so scripts can run from the staged SD image but do not get default WASI filesystem access.

Build scripts with a WASI-capable toolchain if they need stdio or sockets.

## QEMU validation flow

The current smoketest expects these staged scripts in the SD image:

- `hello.wasm`
- `player-control.wasm`
- `net-echo.wasm`

For the local Windows fork in `qemu-official-9.0.0`, rebuild from a clean tree so OpenETH user networking stays enabled:

```sh
cd qemu-official-9.0.0
rm -rf build install dist
./.github/workflows/scripts/configure-win.sh
ninja -C build install
```

Run the smoketest from the ESP-IDF Python environment and point it at the locally rebuilt binary:

```sh
python tools/qemu_wasm_smoketest.py \
  --qemu-exe qemu-official-9.0.0/install/qemu/bin/qemu-system-xtensa.exe
```

The smoketest boots the merged firmware image, waits for the console prompt and OpenETH DHCP lease, verifies `script ls`, runs `hello` and `player-control status`, then exercises `net-echo` in both guest-to-host and host-to-guest directions through QEMU user networking.
