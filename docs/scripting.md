# WASM Scripting

The firmware embeds Espressif's WASMachine core and exposes a native host module named `jukeboy` plus builtin socket natives in the `env` module.

## Console commands

- `script status` prints the service state, host module name, and fixed scripts root.
- `script ls [name]` lists `/lfs/scripts` when no argument is given, or one script directory when a filename is provided.
- `script run <name> [args...]` runs a builtin module and forwards the remaining arguments to `main(argc, argv)`.

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

- `Resolved`
- `Mode`
- `Size`
- `Exit code`
- `Output` when the script wrote through the `jukeboy.log()` host hook
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

## Builtin runtime behavior

Scripts now run only in libc-builtin mode. `.wasm` and `.cwasm` both resolve to builtin modules, and the runtime accepts either `main(argc, argv)` or `__main_argc_argv`.

The firmware exposes the `jukeboy` host module for device control and an `env` socket shim that implements the existing `sock_*` API on top of ESP-IDF BSD sockets. That gives builtin scripts DNS lookup plus TCP and UDP access without WASI.

## QEMU validation flow

The current smoketest expects these staged scripts in LittleFS:

- `hello.wasm`
- `google-get.wasm`
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
