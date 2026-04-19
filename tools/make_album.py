#!/usr/bin/env python3

from __future__ import annotations

import argparse
import binascii
import math
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


SUPPORTED_EXTENSIONS = {
    ".aac",
    ".aif",
    ".aiff",
    ".flac",
    ".m4a",
    ".m4b",
    ".mka",
    ".mp2",
    ".mp3",
    ".ogg",
    ".oga",
    ".opus",
    ".wav",
    ".wma",
}

JBA_VERSION = 0x1
JBM_VERSION = 1
HEADER_BLOCK_SIZE = 512
TARGET_SAMPLE_RATE = 48000
TARGET_CHANNELS = 2
TARGET_BITRATE = 160000
FRAME_DURATION_MS = 20
PACKETS_PER_SECOND = 1000 // FRAME_DURATION_MS
MAX_OUTPUT_FILES = 999
MAX_CHUNK_BYTES = 24 * 1024
UINT32_MAX = 0xFFFFFFFF
JBM_ALBUM_NAME_BYTES = 128
JBM_ALBUM_DESCRIPTION_BYTES = 1024
JBM_ARTIST_BYTES = 256
JBM_GENRE_BYTES = 64
JBM_TAG_COUNT = 5
JBM_TAG_BYTES = 32
JBM_TRACK_NAME_BYTES = 128
JBM_TRACK_ARTISTS_BYTES = 256
JBM_FILENAME = "album.jbm"


@dataclass
class TrackMetadata:
    track_name: str
    artists: str
    duration_sec: int
    file_num: int


@dataclass
class AlbumMetadata:
    album_name: str
    album_description: str
    artist: str
    year: int
    duration_sec: int
    genre: str
    tags: list[str]
    tracks: list[TrackMetadata]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert audio files into the custom .jba format used by player_service.",
    )
    parser.add_argument(
        "input_path",
        type=Path,
        help="Source audio file or folder to scan recursively.",
    )
    parser.add_argument(
        "output_dir",
        nargs="?",
        type=Path,
        default=Path("out"),
        help="Directory that receives 000.jba..999.jba. Defaults to 'out' directory.",
    )
    parser.add_argument(
        "--ffmpeg",
        type=Path,
        default=None,
        help="Optional path to the ffmpeg executable.",
    )
    return parser.parse_args()


def resolve_ffmpeg(explicit_path: Path | None) -> str:
    if explicit_path is not None:
        if not explicit_path.is_file():
            raise FileNotFoundError(f"ffmpeg not found: {explicit_path}")
        return str(explicit_path)

    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise FileNotFoundError("ffmpeg is required but was not found in PATH")
    return ffmpeg


def collect_audio_files(input_path: Path) -> list[Path]:
    if not input_path.exists():
        raise FileNotFoundError(f"input path does not exist: {input_path}")

    if input_path.is_file():
        return [input_path]

    audio_files = [
        path
        for path in sorted(input_path.rglob("*"))
        if path.is_file() and path.suffix.lower() in SUPPORTED_EXTENSIONS
    ]
    return audio_files


def run_ffmpeg(ffmpeg: str, input_file: Path) -> bytes:
    with tempfile.TemporaryDirectory() as temp_dir:
        output_path = Path(temp_dir) / "transcoded.ogg"
        command = [
            ffmpeg,
            "-v",
            "error",
            "-nostdin",
            "-y",
            "-i",
            str(input_file),
            "-map",
            "a:0",
            "-vn",
            "-sn",
            "-dn",
            "-c:a",
            "libopus",
            "-b:a",
            str(TARGET_BITRATE),
            "-vbr",
            "off",
            "-application",
            "audio",
            "-frame_duration",
            str(FRAME_DURATION_MS),
            "-ar",
            str(TARGET_SAMPLE_RATE),
            "-ac",
            str(TARGET_CHANNELS),
            "-f",
            "ogg",
            str(output_path),
        ]

        result = subprocess.run(
            command,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            check=False,
        )
        if result.returncode != 0:
            stderr = result.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(
                f"ffmpeg failed for {input_file}: {stderr or 'unknown error'}"
            )
        if not output_path.is_file():
            raise RuntimeError(f"ffmpeg did not produce output for {input_file}")
        return output_path.read_bytes()


def require_uint32(value: int, description: str) -> int:
    if value < 0 or value > UINT32_MAX:
        raise ValueError(f"{description} must fit in uint32; got {value}")
    return value


def probe_duration_seconds(ffmpeg: str, input_file: Path) -> int:
    ffprobe_name = "ffprobe.exe" if Path(ffmpeg).suffix.lower() == ".exe" else "ffprobe"
    ffprobe_path = Path(ffmpeg).with_name(ffprobe_name)
    ffprobe = str(ffprobe_path) if ffprobe_path.is_file() else shutil.which("ffprobe")
    if ffprobe is None:
        return 0

    command = [
        ffprobe,
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "default=noprint_wrappers=1:nokey=1",
        str(input_file),
    ]

    result = subprocess.run(command, capture_output=True, check=False)
    if result.returncode != 0:
        return 0

    try:
        duration = float(result.stdout.decode("utf-8", errors="replace").strip())
    except ValueError:
        return 0

    return max(1, math.ceil(duration)) if duration > 0 else 0


def iter_ogg_packets(ogg_data: bytes):
    offset = 0
    packet_parts: list[bytes] = []

    while offset < len(ogg_data):
        if ogg_data[offset : offset + 4] != b"OggS":
            raise ValueError(f"invalid Ogg capture pattern at byte {offset}")
        if offset + 27 > len(ogg_data):
            raise ValueError("truncated Ogg page header")

        page_segments = ogg_data[offset + 26]
        segment_table_offset = offset + 27
        segment_table_end = segment_table_offset + page_segments
        if segment_table_end > len(ogg_data):
            raise ValueError("truncated Ogg segment table")

        segment_table = ogg_data[segment_table_offset:segment_table_end]
        body_len = sum(segment_table)
        body_offset = segment_table_end
        body_end = body_offset + body_len
        if body_end > len(ogg_data):
            raise ValueError("truncated Ogg page body")

        body = ogg_data[body_offset:body_end]
        cursor = 0
        for segment_len in segment_table:
            packet_parts.append(body[cursor : cursor + segment_len])
            cursor += segment_len
            if segment_len < 255:
                yield b"".join(packet_parts)
                packet_parts.clear()

        offset = body_end

    if packet_parts:
        raise ValueError("trailing partial Ogg packet")


def extract_opus_packets(ogg_data: bytes) -> list[bytes]:
    packets = list(iter_ogg_packets(ogg_data))
    if len(packets) < 3:
        raise ValueError("Ogg Opus stream is missing required packets")
    if not packets[0].startswith(b"OpusHead"):
        raise ValueError("Ogg stream does not start with OpusHead")
    if not packets[1].startswith(b"OpusTags"):
        raise ValueError("Ogg stream is missing OpusTags")

    audio_packets = [packet for packet in packets[2:] if packet]
    if not audio_packets:
        raise ValueError("no Opus audio packets found")
    return audio_packets


def encode_packet_length(packet_len: int) -> bytes:
    if packet_len < 0:
        raise ValueError("packet length cannot be negative")
    if packet_len < 252:
        return bytes((packet_len,))
    if packet_len > 1275:
        raise ValueError(f"packet length {packet_len} exceeds 2-byte container limit")

    first = 252 + ((packet_len - 252) & 0x3)
    second = (packet_len - first) // 4
    return bytes((first, second))


def build_chunks(opus_packets: list[bytes]) -> list[bytes]:
    chunks: list[bytes] = []
    for start in range(0, len(opus_packets), PACKETS_PER_SECOND):
        packet_group = opus_packets[start : start + PACKETS_PER_SECOND]
        payload = bytearray()
        for packet in packet_group:
            payload.extend(encode_packet_length(len(packet)))
            payload.extend(packet)

        chunk_crc = binascii.crc32(payload) & 0xFFFFFFFF
        chunk = struct.pack("<I", chunk_crc) + payload
        if len(chunk) > MAX_CHUNK_BYTES:
            raise ValueError(
                f"1-second chunk exceeds {MAX_CHUNK_BYTES} bytes; got {len(chunk)} bytes"
            )
        chunks.append(bytes(chunk))

    return chunks


def build_jba(chunks: list[bytes]) -> bytes:
    if not chunks:
        raise ValueError("cannot build JBA without chunks")

    lookup_table = []
    offset = 0
    for index, chunk in enumerate(chunks):
        require_uint32(offset, f"lookup offset for chunk {index}")
        lookup_table.append(offset)
        offset += len(chunk)
        require_uint32(offset, "combined JBA payload size")

    require_uint32(len(lookup_table), "lookup table entry count")
    lookup_bytes = b"".join(struct.pack("<I", entry) for entry in lookup_table)
    header_size = struct.calcsize("<BII") + len(lookup_bytes)
    header_blocks = (header_size + HEADER_BLOCK_SIZE - 1) // HEADER_BLOCK_SIZE
    require_uint32(header_blocks, "JBA header block count")
    header = struct.pack("<BII", JBA_VERSION, header_blocks, len(lookup_table))
    padding_len = header_blocks * HEADER_BLOCK_SIZE - (len(header) + len(lookup_bytes))
    padding = b"\x00" * padding_len
    require_uint32(header_blocks * HEADER_BLOCK_SIZE + offset, "JBA file size")
    return header + lookup_bytes + padding + b"".join(chunks)


def encode_fixed_string(value: str, size: int) -> bytes:
    encoded = value.encode("utf-8")
    if len(encoded) >= size:
        encoded = encoded[: size - 1]
    return encoded + b"\x00" + (b"\x00" * (size - len(encoded) - 1))


def prompt_string(prompt: str, default: str | None = None) -> str:
    suffix = f" [{default}]" if default else ""
    value = input(f"{prompt}{suffix}: ").strip()
    if value:
        return value
    return default or ""


def prompt_uint32(prompt: str, default: int | None = None) -> int:
    suffix = f" [{default}]" if default is not None else ""
    while True:
        raw = input(f"{prompt}{suffix}: ").strip()
        if not raw:
            if default is not None:
                return default
            return 0
        try:
            value = int(raw, 10)
        except ValueError:
            print("Please enter a valid unsigned integer.")
            continue
        if value < 0 or value > 0xFFFFFFFF:
            print("Value must be between 0 and 4294967295.")
            continue
        return value


def infer_album_name(input_path: Path) -> str:
    if input_path.is_dir():
        return input_path.name
    return input_path.stem


def normalize_track_text(value: str) -> str:
    return re.sub(r"\s+", " ", value).strip()


def strip_leading_track_number(value: str) -> str:
    return normalize_track_text(re.sub(r"^\s*\d+[\s._-]*", "", value, count=1))


def infer_track_info(input_file: Path) -> tuple[str, str]:
    stem = normalize_track_text(input_file.stem)
    if not stem:
        return "", ""

    parts = [
        normalize_track_text(part)
        for part in re.split(r"\s+-\s+", stem)
        if normalize_track_text(part)
    ]
    if not parts:
        return stem, ""

    if len(parts) >= 3 and re.match(r"^\d+\b", parts[0]):
        parts = parts[1:]

    if len(parts) >= 2:
        artists = strip_leading_track_number(parts[0])
        track_name = normalize_track_text(" - ".join(parts[1:]))
        if artists and track_name:
            return track_name, artists

    if len(parts) == 1:
        return strip_leading_track_number(parts[0]) or stem, ""

    return stem, ""


def collect_album_metadata(
    input_path: Path, track_infos: list[TrackMetadata]
) -> AlbumMetadata:
    album_name = infer_album_name(input_path)
    total_duration = sum(track.duration_sec for track in track_infos)

    print(
        "\nEnter album metadata. Fields inferred from the source files are filled automatically."
    )
    album_description = prompt_string("Album description")
    artist = prompt_string("Album artist")
    year = prompt_uint32("Release year")
    genre = prompt_string("Genre")

    tags: list[str] = []
    for index in range(JBM_TAG_COUNT):
        tags.append(prompt_string(f"Tag {index + 1}"))

    for index, track in enumerate(track_infos, start=1):
        default_artist = track.artists or artist
        track.artists = prompt_string(
            f"Track {index} artist(s) for '{track.track_name}'", default_artist
        )

    return AlbumMetadata(
        album_name=album_name,
        album_description=album_description,
        artist=artist,
        year=year,
        duration_sec=total_duration,
        genre=genre,
        tags=tags,
        tracks=track_infos,
    )


def build_jbm(album: AlbumMetadata) -> bytes:
    if len(album.tracks) > MAX_OUTPUT_FILES:
        raise ValueError(f"track count exceeds {MAX_OUTPUT_FILES}")

    require_uint32(album.year, "album year")
    require_uint32(album.duration_sec, "album duration")
    require_uint32(len(album.tracks), "track count")

    header = bytearray()
    header.extend(struct.pack("<I", JBM_VERSION))
    header.extend(struct.pack("<I", 0))
    header.extend(encode_fixed_string(album.album_name, JBM_ALBUM_NAME_BYTES))
    header.extend(
        encode_fixed_string(album.album_description, JBM_ALBUM_DESCRIPTION_BYTES)
    )
    header.extend(encode_fixed_string(album.artist, JBM_ARTIST_BYTES))
    header.extend(struct.pack("<I", album.year))
    header.extend(struct.pack("<I", album.duration_sec))
    header.extend(encode_fixed_string(album.genre, JBM_GENRE_BYTES))
    for index in range(JBM_TAG_COUNT):
        tag = album.tags[index] if index < len(album.tags) else ""
        header.extend(encode_fixed_string(tag, JBM_TAG_BYTES))
    header.extend(struct.pack("<I", len(album.tracks)))

    tracks = bytearray()
    for track in album.tracks:
        require_uint32(track.duration_sec, f"duration for track {track.file_num}")
        require_uint32(track.file_num, f"file number for track {track.file_num}")
        tracks.extend(encode_fixed_string(track.track_name, JBM_TRACK_NAME_BYTES))
        tracks.extend(encode_fixed_string(track.artists, JBM_TRACK_ARTISTS_BYTES))
        tracks.extend(struct.pack("<I", track.duration_sec))
        tracks.extend(struct.pack("<I", track.file_num))

    payload = header + tracks
    require_uint32(len(payload), "JBM metadata size")
    checksum = binascii.crc32(payload) & 0xFFFFFFFF
    struct.pack_into("<I", payload, 4, checksum)
    return bytes(payload)


def write_outputs(
    audio_files: list[Path], input_path: Path, output_dir: Path, ffmpeg: str
) -> None:
    if len(audio_files) > MAX_OUTPUT_FILES:
        raise ValueError(f"at most {MAX_OUTPUT_FILES} files are supported per run")

    output_dir.mkdir(parents=True, exist_ok=True)
    track_infos: list[TrackMetadata] = []

    for index, input_file in enumerate(audio_files):
        output_name = f"{index:03d}.jba"
        output_path = output_dir / output_name
        ogg_bytes = run_ffmpeg(ffmpeg, input_file)
        opus_packets = extract_opus_packets(ogg_bytes)
        chunks = build_chunks(opus_packets)
        jba_bytes = build_jba(chunks)
        output_path.write_bytes(jba_bytes)
        duration_sec = probe_duration_seconds(ffmpeg, input_file)
        if duration_sec == 0:
            duration_sec = len(chunks)
        track_name, track_artists = infer_track_info(input_file)
        track_infos.append(
            TrackMetadata(
                track_name=track_name or input_file.stem,
                artists=track_artists,
                duration_sec=duration_sec,
                file_num=index,
            )
        )
        print(f"{input_file} -> {output_path.name} ({len(chunks)} second chunk(s))")

    album = collect_album_metadata(input_path, track_infos)
    jbm_path = output_dir / JBM_FILENAME
    jbm_path.write_bytes(build_jbm(album))
    print(f"Wrote metadata: {jbm_path}")


def main() -> int:
    args = parse_args()
    try:
        ffmpeg = resolve_ffmpeg(args.ffmpeg)
        input_path = args.input_path.resolve()
        output_dir = (
            args.output_dir.resolve()
            if args.output_dir
            else (input_path if input_path.is_dir() else input_path.parent)
        )
        audio_files = collect_audio_files(input_path)
        if not audio_files:
            raise ValueError("no supported audio files found")
        write_outputs(audio_files, input_path, output_dir, ffmpeg)
        return 0
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
