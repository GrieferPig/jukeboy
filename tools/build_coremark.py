from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path


COREMARK_REF = "1f483d5b8316753a742cbf5590caf5bd0a4e4777"
COREMARK_REPO = "https://github.com/eembc/coremark.git"
DEFAULT_WAMRC = "wamrc-2.4.0"


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def to_wsl_path(path: Path) -> str:
    resolved = path.resolve()
    drive = resolved.drive.rstrip(":").lower()
    posix_path = resolved.as_posix()
    if len(posix_path) < 3 or posix_path[1:3] != ":/":
        raise ValueError(f"expected Windows drive path, got: {resolved}")
    return f"/mnt/{drive}{posix_path[2:]}"


def run_wsl(command: list[str]) -> None:
    print("+", " ".join(command))
    subprocess.run(["wsl.exe", "--", *command], check=True)


def ensure_coremark_source(source_dir: Path) -> None:
    source_dir.parent.mkdir(parents=True, exist_ok=True)
    if not source_dir.exists():
        run_wsl(["git", "clone", COREMARK_REPO, to_wsl_path(source_dir)])

    run_wsl(["git", "-C", to_wsl_path(source_dir), "fetch", "origin", COREMARK_REF])
    run_wsl(["git", "-C", to_wsl_path(source_dir), "checkout", COREMARK_REF])


def build_coremark(args: argparse.Namespace) -> None:
    root = repo_root()
    build_root = root / "build" / "coremark-work"
    source_dir = build_root / "coremark"
    out_dir = build_root / "out"
    port_dir = root / "tools" / "coremark"
    flashfs_dir = root / "flashfs" / "scripts" / args.script_name
    wamrc = (root / args.wamrc).resolve()

    if not wamrc.exists():
        raise SystemExit(f"wamrc binary not found: {wamrc}")

    ensure_coremark_source(source_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    flashfs_dir.mkdir(parents=True, exist_ok=True)

    wasm_out = out_dir / f"{args.script_name}.wasm"
    aot_out = out_dir / f"{args.script_name}.aot"

    compile_cmd = [
        "/opt/wasi-sdk/bin/clang",
        "-O3",
        "-nostdlib",
        "-DTOTAL_DATA_SIZE=2000",
        "-Wl,--allow-undefined",
        "-Wl,--no-entry",
        "-Wl,--strip-all",
        "-Wl,--initial-memory=262144",
        "-Wl,--export=main",
        "-Wl,--export=__main_argc_argv",
        "-Wl,--export=__heap_base",
        "-Wl,--export=__data_end",
        "-I",
        to_wsl_path(port_dir),
        "-I",
        to_wsl_path(source_dir),
        "-o",
        to_wsl_path(wasm_out),
        to_wsl_path(source_dir / "core_list_join.c"),
        to_wsl_path(source_dir / "core_main.c"),
        to_wsl_path(source_dir / "core_matrix.c"),
        to_wsl_path(source_dir / "core_state.c"),
        to_wsl_path(source_dir / "core_util.c"),
        to_wsl_path(port_dir / "core_portme.c"),
    ]
    run_wsl(compile_cmd)

    aot_cmd = [
        to_wsl_path(wamrc),
        "--target=xtensa",
        "--cpu=esp32",
        "-o",
        to_wsl_path(aot_out),
        to_wsl_path(wasm_out),
    ]
    run_wsl(aot_cmd)

    shutil.copy2(wasm_out, flashfs_dir / f"{args.script_name}.wasm")
    shutil.copy2(aot_out, flashfs_dir / f"{args.script_name}.aot")

    print(f"Staged {flashfs_dir / (args.script_name + '.wasm')}")
    print(f"Staged {flashfs_dir / (args.script_name + '.aot')}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build CoreMark as wasm and Xtensa AOT, then stage it into flashfs/scripts.")
    parser.add_argument("--script-name", default="coremark")
    parser.add_argument("--wamrc", default=DEFAULT_WAMRC, help="Path to the Linux wamrc binary relative to the repo root")
    return parser.parse_args()


if __name__ == "__main__":
    build_coremark(parse_args())