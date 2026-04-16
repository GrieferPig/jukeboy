# Jukeboy Rust WASM SDK

This workspace targets `wasm32-wasip1`, which matches the WAMR + WASI runtime configuration used by the firmware.

## Prerequisites

- Rust `1.89.0`
- `wasm32-wasip1` target installed through `rustup`

The checked-in `rust-toolchain.toml` and `.cargo/config.toml` pin the toolchain and default target so builds are reproducible.

## Workspace layout

- `jukeboy-sys`: raw host imports from the `jukeboy` module
- `jukeboy`: safe wrappers around the firmware host API
- `jukeboy-net`: preview1 socket bindings plus std-like TCP/UDP wrappers
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

The resulting modules are written to `target/wasm32-wasip1/release/`.

## Stage into firmware

Copy the rebuilt example modules into `../../flashfs/scripts/` before rebuilding the firmware image:

```bash
cp target/wasm32-wasip1/release/hello.wasm ../../flashfs/scripts/
cp target/wasm32-wasip1/release/google-get.wasm ../../flashfs/scripts/
cp target/wasm32-wasip1/release/player-control.wasm ../../flashfs/scripts/
cp target/wasm32-wasip1/release/net-echo.wasm ../../flashfs/scripts/
```

On Windows PowerShell, use `Copy-Item` with the same paths.

## Run the Google example

After staging `google-get.wasm` and rebuilding the firmware image, run it from the console:

```bash
script run google-get
```

Optional arguments let you override the host, path, and port:

```bash
script run google-get example.com / 80
```

This example uses plain HTTP over TCP. `google.com` normally answers on port `80` with a redirect to HTTPS, so the example is mainly useful for demonstrating outbound DNS + TCP + HTTP request/response handling from Rust.
