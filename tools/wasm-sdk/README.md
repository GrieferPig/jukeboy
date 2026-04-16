# Jukeboy Rust WASM SDK

This workspace targets `wasm32-unknown-unknown` and builds `no_std + alloc` modules for the firmware's libc-builtin runtime.

## Prerequisites

- Rust `1.89.0`
- `wasm32-unknown-unknown` target installed through `rustup`

The checked-in `rust-toolchain.toml` and `.cargo/config.toml` pin the toolchain and default target so builds are reproducible.

## Workspace layout

- `jukeboy-sys`: raw host imports from the `jukeboy` module
- `jukeboy`: safe wrappers around the firmware host API plus the shared no_std runtime helpers
- `jukeboy-net`: builtin `env.sock_*` bindings plus small TCP/UDP wrappers
- `examples/*`: example binaries staged into the firmware image
- `examples/google-get`: plain HTTP GET example that fetches `google.com` over port `80`

## Build

```bash
cargo build --release --workspace
```

Or build just the staged example modules:

```bash
cargo build-examples
```

The resulting modules are written to `target/wasm32-unknown-unknown/release/`.

## Stage into firmware

Copy the rebuilt example modules into their per-script LittleFS directories before rebuilding the firmware image:

```bash
cp target/wasm32-unknown-unknown/release/hello.wasm ../../flashfs/scripts/hello/hello.wasm
cp target/wasm32-unknown-unknown/release/google-get.wasm ../../flashfs/scripts/google-get/google-get.wasm
cp target/wasm32-unknown-unknown/release/player-control.wasm ../../flashfs/scripts/player-control/player-control.wasm
cp target/wasm32-unknown-unknown/release/net-echo.wasm ../../flashfs/scripts/net-echo/net-echo.wasm
```

On Windows PowerShell, use `Copy-Item` with the same paths.

If you also want the scripts available from the staged QEMU SD image, copy them into `../../tools/out/scripts/<name>/<name>.wasm` as well before running `tools/run_qemu_firmware.py`.

## Run the Google example

After staging `google-get.wasm` and rebuilding the firmware image, run it from the console:

```bash
script run google-get
```

Optional arguments let you override the host, path, and port:

```bash
script run google-get example.com / 80
```

This example uses plain HTTP over TCP. `google.com` normally answers on port `80` with a redirect to HTTPS, so the example is mainly useful for demonstrating outbound DNS + TCP + HTTP request/response handling from Rust over the builtin socket shim.
