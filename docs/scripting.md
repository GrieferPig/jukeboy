# WASM Scripting

The firmware now embeds Espressif's WASMachine core and exposes a small host API for scripts.

## Script locations

The `script` console command resolves modules from these directories in order:

- `/lfs/scripts`
- `/tmp/scripts`
- `/sdcard/scripts`

Examples:

- `script ls`
- `script run hello`
- `script run lfs/hello`
- `script run /sdcard/scripts/demo.wasm first second`

If the provided path has no extension, the resolver also tries `.wasm`.

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

Playback mode values match `player_service_playback_mode_t`:

- `0`: sequential
- `1`: single repeat
- `2`: shuffle

Return values are `esp_err_t`-style integers for command functions.

## C import example

```c
__attribute__((import_module("jukeboy")))
__attribute__((import_name("log")))
int log_message(const char *message);

__attribute__((import_module("jukeboy")))
__attribute__((import_name("next_track")))
int next_track(void);
```

Build scripts with a WASI-capable toolchain if they need filesystem access. The runtime grants WASI access to `/lfs`, `/tmp`, and `/sdcard` when those mounts are available.