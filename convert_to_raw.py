import argparse
import os
import re
from pathlib import Path
from pydub import AudioSegment, exceptions

TARGET_SAMPLE_RATE = 44100
TARGET_CHANNELS = 2
TARGET_SAMPLE_WIDTH = 2  # 16-bit audio -> 2 bytes
SECTOR_SIZE = 512

# Global counter for filenames
file_counter = 0


def normalize_filename(filename):
    """Replaces non-ASCII and special characters with underscores, keeping extension."""
    name, ext = os.path.splitext(filename)
    # Remove non-alphanumeric characters, allowing dots and hyphens in the name part
    # Replace with underscore
    name = re.sub(r"[^\w\.-]", "_", name)
    # Consolidate multiple underscores
    name = re.sub(r"_+", "_", name)
    # Remove leading/trailing underscores
    name = name.strip("_")
    if not name:  # if name becomes empty
        name = "untitled"
    return name + ext


def convert_audio_file(input_path, output_path_template, current_file_number):
    """Converts a single audio file to raw PCM data, prepends counter, and pads it."""
    try:
        print(f"Processing {input_path}...")
        audio = AudioSegment.from_file(input_path)

        # Resample
        audio = audio.set_frame_rate(TARGET_SAMPLE_RATE)
        # Set channels
        audio = audio.set_channels(TARGET_CHANNELS)
        # Set sample width (bit depth)
        audio = audio.set_sample_width(TARGET_SAMPLE_WIDTH)

        # Construct the actual output filename with counter
        # output_path_template already has the normalized name and .tja suffix
        final_filename_stem = f"{current_file_number}_{output_path_template.stem}"
        final_output_path = output_path_template.with_stem(final_filename_stem)

        # Ensure output directory exists
        final_output_path.parent.mkdir(parents=True, exist_ok=True)

        audio.export(final_output_path, format="raw")
        print(f"Successfully converted and saved to {final_output_path}")

        # Pad the file to the end of a sector
        file_size = final_output_path.stat().st_size
        padding_needed = (SECTOR_SIZE - (file_size % SECTOR_SIZE)) % SECTOR_SIZE
        if padding_needed > 0:
            with open(final_output_path, "ab") as f:  # append binary
                f.write(b"\x00" * padding_needed)
            print(
                f"Padded with {padding_needed} bytes of silence. New size: {file_size + padding_needed}"
            )

        return True
    except exceptions.CouldntDecodeError:
        print(f"Error: Could not decode {input_path}. Skipping.")
    except FileNotFoundError:
        # This might be an ffmpeg/libav issue if pydub relies on it and it's not found
        print(
            f"Error: Could not process {input_path}. Ensure ffmpeg or libav is installed and in PATH."
        )
    except Exception as e:
        print(f"An unexpected error occurred with {input_path}: {e}")
    return False


def main():
    global file_counter
    parser = argparse.ArgumentParser(
        description="Convert audio files to 44.1kHz 16-bit stereo raw PCM (.tja)."
    )
    parser.add_argument(
        "input_path",
        type=str,
        help="Path to an audio file or a directory containing audio files.",
    )

    args = parser.parse_args()

    input_path = Path(args.input_path)
    script_dir = Path(__file__).parent
    output_dir = script_dir / "output"

    if not input_path.exists():
        print(f"Error: Input path {input_path} does not exist.")
        return

    supported_formats = (
        ".wav",
        ".mp3",
        ".aac",
        ".flac",
        ".ogg",
        ".m4a",
    )  # Add more if needed

    if input_path.is_file():
        if input_path.suffix.lower() in supported_formats:
            file_counter += 1
            normalized_name = normalize_filename(input_path.name)
            # output_file_path_template is used to derive the final name with counter
            output_file_path_template = output_dir / Path(normalized_name).with_suffix(
                ".tja"
            )
            convert_audio_file(input_path, output_file_path_template, file_counter)
        else:
            print(f"Skipping non-audio file (or unsupported format): {input_path}")
    elif input_path.is_dir():
        # Collect all files first to sort them for consistent numbering if needed,
        # though os.walk order is generally consistent on many systems.
        # For simplicity, we'll rely on os.walk order.
        for root, _, files in os.walk(input_path):
            # Sort files for more predictable numbering, especially across different OS
            files.sort()
            for file in files:
                if file.lower().endswith(supported_formats):
                    file_counter += 1
                    file_path = Path(root) / file
                    relative_path = file_path.relative_to(input_path)
                    normalized_name = normalize_filename(relative_path.name)

                    output_sub_dir = output_dir / relative_path.parent
                    # output_file_path_template is used to derive the final name with counter
                    output_file_path_template = output_sub_dir / Path(
                        normalized_name
                    ).with_suffix(".tja")

                    convert_audio_file(
                        file_path, output_file_path_template, file_counter
                    )
    else:
        print(f"Error: Input path {input_path} is not a file or directory.")


if __name__ == "__main__":
    main()
