import argparse
import os
import re
import struct
import zlib
from pathlib import Path
from pydub import AudioSegment, exceptions
from mutagen import File as MutagenFile
import math
import subprocess
import tempfile

# --- Configuration Constants ---
TARGET_SAMPLE_RATE = 44100
TARGET_CHANNELS = 2
TARGET_SAMPLE_WIDTH = 2  # 16-bit audio

ADPCM_BLOCK_HEADER_SIZE = 6
ADPCM_BLOCK_SIZE = 44032
ADPCM_BLOCK_DATA_SIZE = ADPCM_BLOCK_SIZE - ADPCM_BLOCK_HEADER_SIZE

# --- Global State ---
file_counter = -1


def compile_c_encoder(encoder_name):
    """Compiles the C encoder if it doesn't exist or if the source is newer."""
    c_source = "encoder.c"
    if not Path(c_source).exists():
        print(f"Error: C source file '{c_source}' not found.")
        raise FileNotFoundError(f"{c_source} not found.")

    if (
        not Path(encoder_name).exists()
        or Path(c_source).stat().st_mtime > Path(encoder_name).stat().st_mtime
    ):
        print(f"Compiling {c_source}...")
        # Use -O3 for optimization
        compile_command = ["gcc", c_source, "-O3", "-o", encoder_name]
        result = subprocess.run(compile_command, capture_output=True, text=True)
        if result.returncode != 0:
            print("--- C COMPILER ERROR ---")
            print(result.stderr)
            print("------------------------")
            raise RuntimeError("Failed to compile C encoder.")
        print("C encoder compiled successfully.")
    return True


def convert_audio_file(input_path, output_dir, current_file_number, encoder_path):
    """
    Converts audio using an external C encoder for the ADPCM part.
    """
    try:
        print(f"Processing {input_path}...")
        audio = AudioSegment.from_file(input_path)

        # Resample and prepare audio
        audio = audio.set_frame_rate(TARGET_SAMPLE_RATE)
        audio = audio.set_channels(TARGET_CHANNELS)
        audio = audio.set_sample_width(TARGET_SAMPLE_WIDTH)

        # --- Use C Encoder ---
        # 1. Create temporary files for PCM input and ADPCM output
        with tempfile.NamedTemporaryFile(delete=False, suffix=".pcm") as pcm_temp_file:
            audio.export(pcm_temp_file.name, format="raw")
            pcm_temp_path = pcm_temp_file.name

        with tempfile.NamedTemporaryFile(
            delete=False, suffix=".adpcm"
        ) as adpcm_temp_file:
            adpcm_temp_path = adpcm_temp_file.name

        try:
            # 2. Run the compiled C encoder
            print("Running external C encoder...")
            command = [
                f"./{encoder_path}",
                pcm_temp_path,
                adpcm_temp_path,
                str(ADPCM_BLOCK_DATA_SIZE),
            ]
            result = subprocess.run(command, check=True, capture_output=True, text=True)
            print(result.stdout.strip())  # Print output from C program

            # 3. Read the resulting ADPCM data from the temporary file
            with open(adpcm_temp_path, "rb") as f:
                adpcm_data_with_headers = f.read()

        finally:
            # 4. Clean up temporary files
            os.remove(pcm_temp_path)
            os.remove(adpcm_temp_path)

        # --- File Assembly ---
        hex_filename = f"{current_file_number:02X}"
        final_output_path = output_dir / f"{hex_filename}.tja"
        final_output_path.parent.mkdir(parents=True, exist_ok=True)

        with open(final_output_path, "wb") as f:
            # --- Write Header ---
            header = bytearray(512)
            track_name, artist_name = extract_metadata(input_path)
            struct.pack_into("64s", header, 32, track_name.encode("ascii", "replace"))
            struct.pack_into("64s", header, 96, artist_name.encode("ascii", "replace"))
            f.write(header)

            # --- Write ADPCM Data ---
            f.write(adpcm_data_with_headers)

            # --- Finalize Header ---
            total_size_bytes = f.tell()
            len_pages = (total_size_bytes + 511) // 512
            checksum = zlib.crc32(adpcm_data_with_headers) & 0xFFFFFFFF

            f.seek(0)
            struct.pack_into("4s", header, 0, b"TJA\0")
            struct.pack_into("B", header, 4, 1)
            struct.pack_into("B", header, 5, 0)
            struct.pack_into("<I", header, 6, len_pages)
            struct.pack_into("<I", header, 10, checksum)
            f.write(header)

        print(f"Successfully created seekable TJA file: {final_output_path}")
        return True

    except exceptions.CouldntDecodeError:
        print(f"Error: Could not decode {input_path}. Skipping.")
    except FileNotFoundError:
        print(f"Error: Could not process {input_path}. Ensure ffmpeg is installed.")
    except subprocess.CalledProcessError as e:
        print("--- C ENCODER FAILED ---")
        print(e.stderr)
        print("------------------------")
    except Exception as e:
        print(f"An unexpected error occurred with {input_path}: {e}")
        import traceback

        traceback.print_exc()
    return False


# --- (Other functions: sanitize_to_ascii, extract_metadata, create_album_tjm) ---
# These functions remain the same as in your original script.
# They are included here for completeness.


def sanitize_to_ascii(text, max_length):
    ascii_text = text.encode("ascii", "replace").decode("ascii")
    return ascii_text[:max_length]


def extract_metadata(file_path):
    try:
        audio_file = MutagenFile(file_path)
        if audio_file is None:
            raise ValueError("Cannot parse")
        title = str(
            audio_file.get("TIT2") or audio_file.get("title", [Path(file_path).stem])[0]
        )
        artist = str(
            audio_file.get("TPE1") or audio_file.get("artist", ["Unknown Artist"])[0]
        )
    except Exception:
        title = Path(file_path).stem
        artist = "Unknown Artist"
    return sanitize_to_ascii(title, 63), sanitize_to_ascii(artist, 63)


def create_album_tjm(output_dir, track_count):
    """Creates an album.tjm metadata file with user input."""
    # This function is preserved as requested, though it's logically
    # separate from the self-contained TJA files now.
    print("\n--- Album Metadata Input ---")
    album_name = input("Album name (max 31 chars): ")[:31]
    artist_name = input("Artist name (max 63 chars): ")[:63]
    url = input("Album URL (max 63 chars): ")[:63]
    try:
        release_year_input = input("Release year (e.g., 2023): ")
        release_year = (
            int(release_year_input) - 1970 if release_year_input.strip() else 0
        )
    except ValueError:
        release_year = 0
    tags = [input(f"Tag {i+1}: ")[:15] for i in range(4)]

    tjm_path = output_dir / "album.tjm"
    with open(tjm_path, "wb") as f:
        header = bytearray(512)
        struct.pack_into("4s", header, 0, b"TJM\0")
        struct.pack_into("B", header, 4, 1)
        struct.pack_into("I", header, 5, 1)
        struct.pack_into("32s", header, 32, album_name.encode("ascii", "replace"))
        struct.pack_into("64s", header, 64, artist_name.encode("ascii", "replace"))
        struct.pack_into("64s", header, 128, url.encode("ascii", "replace"))
        struct.pack_into("H", header, 192, release_year)
        for i, tag in enumerate(tags):
            struct.pack_into(
                "16s", header, 194 + (i * 16), tag.encode("ascii", "replace")
            )
        struct.pack_into("B", header, 258, track_count)
        checksum = zlib.crc32(header[32:]) & 0xFFFFFFFF
        struct.pack_into("I", header, 9, checksum)
        f.write(header)
    print(f"Created album metadata file: {tjm_path}")


def main():
    global file_counter
    parser = argparse.ArgumentParser(
        description="Convert audio using an external C encoder."
    )
    parser.add_argument(
        "input_path", type=str, help="Path to an audio file or a directory."
    )
    parser.add_argument(
        "-o", "--output", type=str, default="output", help="Output directory path."
    )
    parser.add_argument(
        "--album-only",
        action="store_true",
        help="Generate only the album metadata file (album.tjm)",
    )
    args = parser.parse_args()

    # Handle album-only mode
    if args.album_only:
        output_dir = Path(args.output)
        track_count = int(input("Number of tracks in album: "))
        create_album_tjm(output_dir, track_count)
        return

    if not args.input_path:
        print("Error: Input path is required when not using --album-only")
        return

    encoder_name = "adpcm_encoder"
    if os.name == "nt":  # Add .exe for Windows compatibility
        encoder_name += ".exe"

    try:
        compile_c_encoder(encoder_name)
    except (RuntimeError, FileNotFoundError) as e:
        print(e)
        return  # Exit if compilation fails

    input_path = Path(args.input_path)
    output_dir = Path(args.output)

    if not input_path.exists():
        print(f"Error: Input path {input_path} does not exist.")
        return

    supported_formats = (".wav", ".mp3", ".aac", ".flac", ".ogg", ".m4a")

    if input_path.is_file():
        if input_path.suffix.lower() in supported_formats:
            file_counter += 1
            convert_audio_file(input_path, output_dir, file_counter, encoder_name)
    elif input_path.is_dir():
        audio_files = sorted(
            [p for p in input_path.rglob("*") if p.suffix.lower() in supported_formats],
            key=lambda x: x.name.lower(),
        )
        successful_conversions = 0
        for file_path in audio_files:
            file_counter += 1
            if convert_audio_file(file_path, output_dir, file_counter, encoder_name):
                successful_conversions += 1

        if successful_conversions > 0:
            create_album_tjm(output_dir, successful_conversions)
    else:
        print(f"Error: Input path {input_path} is not a file or directory.")


if __name__ == "__main__":
    main()
