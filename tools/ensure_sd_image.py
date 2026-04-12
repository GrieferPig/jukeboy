import argparse
import shutil
import struct
import tempfile
import warnings
from pathlib import Path
from typing import Iterable

warnings.filterwarnings(
    "ignore",
    message="pkg_resources is deprecated as an API",
    category=UserWarning,
)

try:
    from pyfatfs.PyFat import PyFat
    from pyfatfs.PyFatFS import PyFatFS
except ModuleNotFoundError as exc:
    raise SystemExit(
        "pyfatfs is required to generate the QEMU SD image. "
        "Install it into the ESP-IDF Python environment with "
        "`python -m pip install pyfatfs`."
    ) from exc


SECTOR_SIZE_BYTES = 512
FAT_LOGICAL_SECTOR_SIZE_BYTES = 512
DEFAULT_PARTITION_START_LBA = 2048
PARTITION_TYPE_FAT32_LBA = 0x0C
IMAGE_SIZE_ALIGNMENT_BYTES = 64 * 1024 * 1024
MIN_POPULATED_FREE_SPACE_BYTES = 128 * 1024 * 1024
VOLUME_LABEL = "JUKEBOYSD"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create a QEMU SD image with an MBR and a FAT partition"
    )
    parser.add_argument(
        "--image-path", required=True, help="Path to the raw SD image file"
    )
    parser.add_argument(
        "--size-bytes", required=True, type=int, help="Total image size in bytes"
    )
    parser.add_argument(
        "--source-dir",
        required=True,
        help="Directory whose contents should be placed at the root of the FAT partition",
    )
    parser.add_argument(
        "--partition-start-lba",
        type=int,
        default=DEFAULT_PARTITION_START_LBA,
        help="Starting LBA for the FAT partition inside the SD image",
    )
    return parser.parse_args()


def build_partition_entry(start_lba: int, sector_count: int) -> bytes:
    entry = bytearray(16)
    entry[0] = 0x00
    entry[1:4] = b"\xfe\xff\xff"
    entry[4] = PARTITION_TYPE_FAT32_LBA
    entry[5:8] = b"\xfe\xff\xff"
    struct.pack_into("<I", entry, 8, start_lba)
    struct.pack_into("<I", entry, 12, sector_count)
    return bytes(entry)


def build_mbr(start_lba: int, sector_count: int) -> bytes:
    mbr = bytearray(SECTOR_SIZE_BYTES)
    mbr[446:462] = build_partition_entry(start_lba, sector_count)
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


def iter_payload_files(source_dir: Path) -> list[Path]:
    if not source_dir.exists():
        return []
    return [path for path in source_dir.rglob("*") if path.is_file()]


def total_payload_size_bytes(payload_files: Iterable[Path]) -> int:
    return sum(path.stat().st_size for path in payload_files)


def round_up(value: int, alignment: int) -> int:
    return ((value + alignment - 1) // alignment) * alignment


def round_up_to_power_of_two(value: int) -> int:
    if value <= 0:
        raise ValueError("value must be positive")
    return 1 << (value - 1).bit_length()


def calculate_total_image_size(
    minimum_size_bytes: int,
    payload_size_bytes: int,
    partition_start_lba: int,
) -> int:
    partition_offset = partition_start_lba * SECTOR_SIZE_BYTES
    minimum_partition_size = max(0, minimum_size_bytes - partition_offset)

    if payload_size_bytes == 0:
        required_partition_size = minimum_partition_size
    else:
        required_partition_size = max(
            minimum_partition_size,
            payload_size_bytes + MIN_POPULATED_FREE_SPACE_BYTES,
        )

    required_total_size = partition_offset + required_partition_size
    aligned_total_size = round_up(required_total_size, IMAGE_SIZE_ALIGNMENT_BYTES)

    # QEMU rejects SD card images whose total size is not a power of two.
    return round_up_to_power_of_two(aligned_total_size)


def ensure_parent_directories(fat_fs: PyFatFS, relative_path: str) -> None:
    parts = Path(relative_path).parts[:-1]
    current_parts: list[str] = []
    for part in parts:
        current_parts.append(part)
        fat_fs.makedir("/".join(current_parts), recreate=True)


def generate_fat_partition(
    source_dir: Path,
    output_path: Path,
    partition_size_bytes: int,
    payload_files: list[Path],
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.touch(exist_ok=True)

    fat = PyFat()
    fat.mkfs(
        filename=str(output_path),
        fat_type=PyFat.FAT_TYPE_FAT32,
        size=partition_size_bytes,
        sector_size=FAT_LOGICAL_SECTOR_SIZE_BYTES,
        label=VOLUME_LABEL,
    )
    fat.close()

    fat_fs = PyFatFS(str(output_path))
    try:
        for payload_file in sorted(payload_files):
            relative_path = payload_file.relative_to(source_dir).as_posix()
            ensure_parent_directories(fat_fs, relative_path)
            with payload_file.open("rb") as host_file, fat_fs.openbin(
                relative_path, "w"
            ) as fat_file:
                shutil.copyfileobj(host_file, fat_file)
    finally:
        fat_fs.close()


def write_disk_image(
    image_path: Path,
    total_size_bytes: int,
    partition_path: Path,
    partition_start_lba: int,
) -> None:
    partition_size_bytes = partition_path.stat().st_size
    if partition_size_bytes % SECTOR_SIZE_BYTES != 0:
        raise ValueError("partition image size must be a multiple of 512 bytes")

    partition_offset = partition_start_lba * SECTOR_SIZE_BYTES
    if partition_offset + partition_size_bytes > total_size_bytes:
        raise ValueError("partition does not fit inside the requested SD image size")

    partition_sector_count = partition_size_bytes // SECTOR_SIZE_BYTES
    image_path.parent.mkdir(parents=True, exist_ok=True)

    with image_path.open("w+b") as image_file:
        image_file.truncate(total_size_bytes)
        image_file.seek(0)
        image_file.write(build_mbr(partition_start_lba, partition_sector_count))
        image_file.seek(partition_offset)
        with partition_path.open("rb") as partition_file:
            shutil.copyfileobj(partition_file, image_file)


def main() -> int:
    args = parse_args()
    image_path = Path(args.image_path)
    size_bytes = args.size_bytes
    source_dir = Path(args.source_dir)
    partition_start_lba = args.partition_start_lba

    if size_bytes <= 0:
        raise ValueError("--size-bytes must be positive")
    if partition_start_lba <= 0:
        raise ValueError("--partition-start-lba must be positive")

    payload_files = iter_payload_files(source_dir)
    payload_size_bytes = total_payload_size_bytes(payload_files)
    total_image_size_bytes = calculate_total_image_size(
        minimum_size_bytes=size_bytes,
        payload_size_bytes=payload_size_bytes,
        partition_start_lba=partition_start_lba,
    )
    partition_offset = partition_start_lba * SECTOR_SIZE_BYTES
    partition_size_bytes = total_image_size_bytes - partition_offset

    with tempfile.TemporaryDirectory() as temp_dir:
        partition_image_path = Path(temp_dir) / "sd_partition.bin"
        generate_fat_partition(
            source_dir=source_dir,
            output_path=partition_image_path,
            partition_size_bytes=partition_size_bytes,
            payload_files=payload_files,
        )
        write_disk_image(
            image_path=image_path,
            total_size_bytes=total_image_size_bytes,
            partition_path=partition_image_path,
            partition_start_lba=partition_start_lba,
        )

    print(
        "Created QEMU SD image: "
        f"{image_path} ({total_image_size_bytes} bytes, {len(payload_files)} staged files, "
        f"{payload_size_bytes} payload bytes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
