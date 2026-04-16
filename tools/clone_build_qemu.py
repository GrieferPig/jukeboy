import argparse
import os
import shlex
import shutil
import stat
import subprocess
from pathlib import Path
from typing import Sequence


DEFAULT_REPO_URL = "https://github.com/GrieferPig/qemu.git"
DEFAULT_BRANCH = "qemu-official-9.0.0"
DEFAULT_CLONE_DIR = "qemu-official-9.0.0"
DEFAULT_TARGET = "xtensa-softmmu"
DEFAULT_VERSION = "local"
MSYS2_PACKAGES = [
    "mingw-w64-x86_64-gcc",
    "mingw-w64-x86_64-glib2",
    "mingw-w64-x86_64-libgcrypt",
    "mingw-w64-x86_64-libiconv",
    "mingw-w64-x86_64-meson",
    "mingw-w64-x86_64-miniaudio",
    "mingw-w64-x86_64-ninja",
    "mingw-w64-x86_64-pixman",
    "mingw-w64-x86_64-pkg-config",
    "mingw-w64-x86_64-python",
    "mingw-w64-x86_64-python-distlib",
    "mingw-w64-x86_64-SDL2",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Clone and build the local QEMU fork for ESP32 firmware tests"
    )
    parser.add_argument("--repo-url", default=DEFAULT_REPO_URL)
    parser.add_argument("--branch", default=DEFAULT_BRANCH)
    parser.add_argument("--clone-dir", default=DEFAULT_CLONE_DIR)
    parser.add_argument("--git", default="git", help="Git executable to use")
    parser.add_argument("--msys-bash", help="Path to the MSYS2 bash executable")
    parser.add_argument("--target", default=DEFAULT_TARGET)
    parser.add_argument("--version", default=DEFAULT_VERSION)
    parser.add_argument("--jobs", type=int, help="Optional ninja -j value")
    parser.add_argument(
        "--skip-deps",
        action="store_true",
        help="Skip pacman dependency installation inside MSYS2",
    )
    parser.add_argument(
        "--skip-build", action="store_true", help="Clone only and skip the build"
    )
    parser.add_argument(
        "--reuse-existing",
        action="store_true",
        help="Reuse an existing clone instead of deleting it first",
    )
    return parser.parse_args()


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def resolve_path(base_dir: Path, value: str) -> Path:
    path = Path(value)
    if not path.is_absolute():
        path = base_dir / path
    return path.resolve()


def format_command(command: Sequence[str]) -> str:
    return " ".join(shlex.quote(str(part)) for part in command)


def run_checked(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> None:
    print(f"+ {format_command(command)}")
    subprocess.run(command, cwd=cwd, env=env, check=True)


def build_msys_env() -> dict[str, str]:
    env = os.environ.copy()
    env["MSYSTEM"] = "MINGW64"
    env["CHERE_INVOKING"] = "1"
    env["MSYS2_PATH_TYPE"] = "inherit"
    return env


def find_msys_bash(explicit_path: str | None) -> str:
    candidates: list[str] = []
    if explicit_path:
        candidates.append(explicit_path)

    env_path = os.environ.get("MSYS2_BASH")
    if env_path:
        candidates.append(env_path)

    candidates.append(r"C:\msys64\usr\bin\bash.exe")

    path_bash = shutil.which("bash")
    if path_bash:
        candidates.append(path_bash)

    seen: set[str] = set()
    for candidate in candidates:
        resolved = shutil.which(candidate) or candidate
        if resolved in seen:
            continue
        seen.add(resolved)

        try:
            result = subprocess.run(
                [
                    resolved,
                    "--login",
                    "-c",
                    "command -v pacman >/dev/null 2>&1 && command -v cygpath >/dev/null 2>&1",
                ],
                env=build_msys_env(),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        except OSError:
            continue

        if result.returncode == 0:
            return resolved

    raise SystemExit(
        "Unable to find an MSYS2 bash with pacman and cygpath available. "
        "Install MSYS2 or pass --msys-bash C:/msys64/usr/bin/bash.exe."
    )


def retry_remove_with_write_permissions(function, path: str, excinfo: object) -> None:
    _ = excinfo
    os.chmod(path, stat.S_IWRITE)
    function(path)


def remove_existing_clone(clone_dir: Path) -> None:
    if not clone_dir.exists():
        return

    print(f"Removing existing clone: {clone_dir}")
    shutil.rmtree(clone_dir, onexc=retry_remove_with_write_permissions)


def clone_repo(args: argparse.Namespace, clone_dir: Path) -> None:
    if clone_dir.exists():
        if not (clone_dir / ".git").exists():
            raise SystemExit(
                f"Clone directory exists but is not a git checkout: {clone_dir}"
            )
        print(f"Reusing existing clone: {clone_dir}")
        return

    run_checked(
        [
            args.git,
            "-c",
            "core.autocrlf=false",
            "clone",
            "--depth",
            "1",
            "--branch",
            args.branch,
            "--single-branch",
            args.repo_url,
            str(clone_dir.name),
        ],
        cwd=clone_dir.parent,
    )


def build_qemu(args: argparse.Namespace, clone_dir: Path, bash_path: str) -> None:
    pacman_command = "true"
    if not args.skip_deps:
        pacman_command = "pacman -S --needed --noconfirm " + " ".join(MSYS2_PACKAGES)

    ninja_command = "ninja -C build"
    if args.jobs:
        ninja_command += f" -j {args.jobs}"
    ninja_command += " qemu-system-xtensa.exe"

    shell_script = "\n".join(
        [
            "set -euo pipefail",
            pacman_command,
            r"sed -i 's/\r$//' .github/workflows/scripts/configure-win.sh",
            (
                f"VERSION={shlex.quote(args.version)} "
                f"TARGET={shlex.quote(args.target)} "
                "bash .github/workflows/scripts/configure-win.sh"
            ),
            ninja_command,
            "mkdir -p install/qemu/bin",
            "cp build/qemu-system-xtensa.exe install/qemu/bin/",
        ]
    )

    run_checked(
        [bash_path, "--login", "-c", shell_script],
        cwd=clone_dir,
        env=build_msys_env(),
    )

    staged_exe = clone_dir / "install" / "qemu" / "bin" / "qemu-system-xtensa.exe"
    if not staged_exe.exists():
        raise SystemExit(
            f"Build finished but staged QEMU binary is missing: {staged_exe}"
        )

    print(f"Built QEMU successfully: {staged_exe}")


def main() -> int:
    args = parse_args()
    root = repo_root()
    clone_dir = resolve_path(root, args.clone_dir)

    if clone_dir == root:
        raise SystemExit("Refusing to use the repository root as the clone directory")

    if not args.reuse_existing:
        remove_existing_clone(clone_dir)

    clone_repo(args, clone_dir)

    if args.skip_build:
        print(f"Cloned QEMU fork to {clone_dir}")
        return 0

    bash_path = find_msys_bash(args.msys_bash)
    build_qemu(args, clone_dir, bash_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
