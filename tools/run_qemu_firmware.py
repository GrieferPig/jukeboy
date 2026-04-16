import argparse
import importlib.util
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Sequence


DEFAULT_QEMU_DIR = "qemu-official-9.0.0"
DEFAULT_BUILD_DIR = "build"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Refresh storage images, merge the firmware flash image, and launch the local ESP32 QEMU fork"
    )
    parser.add_argument("--qemu-dir", default=DEFAULT_QEMU_DIR)
    parser.add_argument("--build-dir", default=DEFAULT_BUILD_DIR)
    parser.add_argument("--qemu-exe", help="Override the qemu-system-xtensa.exe path")
    parser.add_argument(
        "--skip-flash-merge",
        action="store_true",
        help="Use the existing build/qemu_flash.bin without regenerating it; storage images are still rebuilt",
    )
    parser.add_argument("--capture-file", help="Optional PCM capture output path")
    parser.add_argument(
        "--capture-limit-bytes",
        type=int,
        default=0,
        help="Optional auto-shutdown limit for PCM capture",
    )
    parser.add_argument(
        "--openeth",
        dest="openeth",
        action="store_true",
        default=True,
        help="Enable QEMU user networking with the ESP32 OpenETH model (default)",
    )
    parser.add_argument(
        "--no-openeth",
        dest="openeth",
        action="store_false",
        help="Disable the default QEMU OpenETH user networking",
    )
    parser.add_argument(
        "--hostfwd",
        action="append",
        default=[],
        help="Additional user-network host forwarding, e.g. tcp:127.0.0.1:12345-:12345",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the storage rebuild, flash merge, and QEMU commands without executing them",
    )
    parser.add_argument(
        "extra_qemu_args",
        nargs=argparse.REMAINDER,
        help="Extra arguments appended to the QEMU command. Prefix them with --.",
    )
    return parser.parse_args()


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def resolve_path(base_dir: Path, value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = base_dir / path
    return path.resolve()


def repo_relative_arg(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def format_command(command: Sequence[str]) -> str:
    return " ".join(shlex.quote(str(part)) for part in command)


def ensure_exists(path: Path, description: str) -> None:
    if not path.exists():
        raise SystemExit(f"Missing {description}: {path}")


def merge_flash(build_dir: Path, dry_run: bool) -> None:
    flash_args = build_dir / "flash_args"
    ensure_exists(flash_args, "flash arguments file")

    command = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32",
        "merge_bin",
        "--fill-flash-size",
        "8MB",
        "-o",
        "qemu_flash.bin",
        "@flash_args",
    ]
    print(f"+ {format_command(command)}")

    if dry_run:
        return

    if importlib.util.find_spec("esptool") is None:
        raise SystemExit(
            "The current Python interpreter does not have esptool installed. "
            "Run this script from the ESP-IDF Python environment or install esptool first."
        )

    subprocess.run(command, cwd=build_dir, check=True)


def build_qemu_command(args: argparse.Namespace, root: Path) -> list[str]:
    build_dir = resolve_path(root, args.build_dir)
    qemu_dir = resolve_path(root, args.qemu_dir)

    qemu_exe = (
        resolve_path(root, args.qemu_exe)
        if args.qemu_exe
        else qemu_dir / "install" / "qemu" / "bin" / "qemu-system-xtensa.exe"
    )
    pc_bios_dir = qemu_dir / "pc-bios"
    qemu_flash = build_dir / "qemu_flash.bin"
    qemu_efuse = build_dir / "qemu_efuse.bin"
    sd_image = build_dir / "sd_image.bin"

    ensure_exists(qemu_exe, "QEMU executable")
    ensure_exists(pc_bios_dir, "QEMU pc-bios directory")
    ensure_exists(qemu_flash, "merged QEMU flash image")
    if not qemu_efuse.exists():
        # QEMU expects a 128-byte blank eFuse backing file; create one on demand.
        print(f"Creating blank eFuse image: {qemu_efuse}")
        qemu_efuse.write_bytes(bytes(128))
    ensure_exists(sd_image, "QEMU SD image")

    command = [
        str(qemu_exe),
        "-L",
        repo_relative_arg(pc_bios_dir, root),
        "-nographic",
        "-machine",
        "esp32",
        "-m",
        "4M",
        "-drive",
        f"file={repo_relative_arg(qemu_flash, root)},if=mtd,format=raw",
        "-drive",
        f"file={repo_relative_arg(qemu_efuse, root)},if=none,format=raw,id=efuse",
        "-global",
        "driver=nvram.esp32efuse,property=drive,value=efuse",
        "-drive",
        f"file={repo_relative_arg(sd_image, root)},if=sd,format=raw",
    ]

    if args.capture_limit_bytes < 0:
        raise SystemExit("--capture-limit-bytes must be zero or greater")
    if args.capture_limit_bytes and not args.capture_file:
        raise SystemExit("--capture-limit-bytes requires --capture-file")

    if args.capture_file:
        capture_path = resolve_path(root, args.capture_file)
        capture_path.parent.mkdir(parents=True, exist_ok=True)
        command.extend(
            [
                "-global",
                f"misc.esp32.qemu-pcm.capture-file={repo_relative_arg(capture_path, root)}",
            ]
        )

    if args.capture_limit_bytes:
        command.extend(
            [
                "-global",
                f"misc.esp32.qemu-pcm.capture-limit-bytes={args.capture_limit_bytes}",
            ]
        )

    if args.openeth:
        nic_arg = "user,model=open_eth"
        for hostfwd in args.hostfwd:
            nic_arg += f",hostfwd={hostfwd}"
        command.extend(["-nic", nic_arg])

    extra_args = list(args.extra_qemu_args)
    if extra_args[:1] == ["--"]:
        extra_args = extra_args[1:]
    command.extend(extra_args)
    return command


def main() -> int:
    args = parse_args()
    root = repo_root()
    build_dir = resolve_path(root, args.build_dir)
    ensure_exists(build_dir, "firmware build directory")

    if not args.skip_flash_merge:
        merge_flash(build_dir, args.dry_run)

    qemu_command = build_qemu_command(args, root)
    print(f"+ {format_command(qemu_command)}")

    if args.dry_run:
        return 0

    completed = subprocess.run(qemu_command, cwd=root, check=False)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
