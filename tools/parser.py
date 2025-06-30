#!/usr/bin/env python3
import argparse
import os
import struct
import zlib
import math
from pathlib import Path
from pydub import AudioSegment

# --- Configuration Constants (must match the encoder) ---
TARGET_SAMPLE_RATE = 44100
TARGET_CHANNELS = 2
TARGET_SAMPLE_WIDTH = 2  # 16-bit audio

# ADPCM Block Configuration for dual-state stereo
ADPCM_BLOCK_SIZE = 44032
ADPCM_BLOCK_HEADER_SIZE = 6  # 2x (predictor + index)
ADPCM_BLOCK_DATA_SIZE = ADPCM_BLOCK_SIZE - ADPCM_BLOCK_HEADER_SIZE


class ADPCMDecoder:
    """A pure Python implementation of an IMA ADPCM decoder."""

    INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
    STEP_TABLE = [
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        16,
        17,
        19,
        21,
        23,
        25,
        28,
        31,
        34,
        37,
        41,
        45,
        50,
        55,
        60,
        66,
        73,
        80,
        88,
        97,
        107,
        118,
        130,
        143,
        157,
        173,
        190,
        209,
        230,
        253,
        279,
        307,
        337,
        371,
        408,
        449,
        494,
        544,
        598,
        658,
        724,
        796,
        876,
        963,
        1060,
        1166,
        1282,
        1411,
        1552,
        1707,
        1878,
        2066,
        2272,
        2499,
        2749,
        3024,
        3327,
        3660,
        4026,
        4428,
        4871,
        5358,
        5894,
        6484,
        7132,
        7845,
        8630,
        9493,
        10442,
        11487,
        12635,
        13899,
        15289,
        16818,
        18500,
        20350,
        22385,
        24623,
        27086,
        29794,
        32767,
    ]

    def __init__(self, predictor=0, index=0):
        self.predictor = predictor
        self.index = index

    def _clamp(self, value, min_val, max_val):
        return max(min_val, min(value, max_val))

    def decode_sample(self, nibble):
        """Decodes a 4-bit ADPCM nibble into a 16-bit PCM sample."""
        step = self.STEP_TABLE[self.index]

        # Reconstruct the difference from the nibble
        vpdiff = step >> 3
        if nibble & 4:
            vpdiff += step
        if nibble & 2:
            vpdiff += step >> 1
        if nibble & 1:
            vpdiff += step >> 2

        # Add the difference to the predictor
        if nibble & 8:  # Check sign bit
            self.predictor -= vpdiff
        else:
            self.predictor += vpdiff

        self.predictor = self._clamp(self.predictor, -32768, 32767)

        # Update the index for the next step
        self.index += self.INDEX_TABLE[nibble]
        self.index = self._clamp(self.index, 0, 88)

        return self.predictor


def parse_tjm_file(file_path: Path):
    """Parses an album.tjm file and prints its metadata."""
    print(f"--- Parsing Album Metadata File: {file_path.name} ---")
    try:
        with open(file_path, "rb") as f:
            header = f.read(512)
            if len(header) < 512:
                print("Error: TJM file is incomplete.")
                return

            # --- Unpack Header ---
            magic = struct.unpack_from("4s", header, 0)[0]
            if magic != b"TJM\0":
                print(
                    f"Error: Invalid file type. Expected 'TJM', got '{magic.decode()}'"
                )
                return

            stored_checksum = struct.unpack_from("<I", header, 9)[0]

            # --- Verify Checksum ---
            # The checksum in the provided creation script covers the metadata portion
            metadata_portion = header[32:]
            calculated_checksum = zlib.crc32(metadata_portion) & 0xFFFFFFFF

            if stored_checksum != calculated_checksum:
                print(f"Warning: Checksum mismatch! File may be corrupt.")
                print(f"  Stored: {stored_checksum}, Calculated: {calculated_checksum}")
            else:
                print("Checksum: OK")

            # --- Extract and Print All Metadata Fields ---
            album_name = (
                struct.unpack_from("32s", header, 32)[0]
                .split(b"\0", 1)[0]
                .decode("ascii", "replace")
            )
            artist_name = (
                struct.unpack_from("64s", header, 64)[0]
                .split(b"\0", 1)[0]
                .decode("ascii", "replace")
            )
            url = (
                struct.unpack_from("64s", header, 128)[0]
                .split(b"\0", 1)[0]
                .decode("ascii", "replace")
            )
            release_year_offset = struct.unpack_from("<H", header, 192)[0]
            release_year = (
                1970 + release_year_offset if release_year_offset > 0 else "N/A"
            )

            tags = []
            for i in range(4):
                tag = (
                    struct.unpack_from("16s", header, 194 + (i * 16))[0]
                    .split(b"\0", 1)[0]
                    .decode("ascii", "replace")
                )
                if tag:
                    tags.append(tag)

            track_count = struct.unpack_from("B", header, 258)[0]

            print(f"  Album:    {album_name}")
            print(f"  Artist:   {artist_name}")
            print(f"  URL:      {url or 'N/A'}")
            print(f"  Year:     {release_year}")
            print(f"  Tags:     {', '.join(tags) if tags else 'N/A'}")
            print(f"  Tracks:   {track_count}")

    except FileNotFoundError:
        print(f"Error: File not found at {file_path}")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")


def parse_tja_file(file_path: Path, output_dir: Path, diff_wav_path: Path = None):
    """
    Parses a dual-state .tja audio file, prints metadata, and converts it to a WAV file.
    Optionally compares the output with a reference WAV file.
    """
    print(f"--- Parsing Track Audio File: {file_path.name} ---")
    output_wav_path = output_dir / f"{file_path.stem}.wav"

    try:
        with open(file_path, "rb") as f:
            # --- 1. Read and Parse Header ---
            header = f.read(512)
            if len(header) < 512:
                print("Error: TJA file header is incomplete.")
                return

            magic = struct.unpack_from("4s", header, 0)[0]
            if magic != b"TJA\0":
                print(
                    f"Error: Invalid file type. Expected 'TJA', got '{magic.decode()}'"
                )
                return

            stored_checksum = struct.unpack_from("<I", header, 10)[0]
            track_name = (
                struct.unpack_from("64s", header, 32)[0]
                .split(b"\0", 1)[0]
                .decode("ascii", "replace")
            )
            artist_name = (
                struct.unpack_from("64s", header, 96)[0]
                .split(b"\0", 1)[0]
                .decode("ascii", "replace")
            )

            print(f"  Track:    {track_name}")
            print(f"  Artist:   {artist_name}")

            # --- 2. Read and Verify All Data Blocks ---
            f.seek(512)
            all_block_data = f.read()

            calculated_checksum = zlib.crc32(all_block_data) & 0xFFFFFFFF
            if stored_checksum != calculated_checksum:
                print(f"Warning: Checksum mismatch! File may be corrupt.")
            else:
                print("Checksum: OK")

            # --- 3. Decode Dual-State ADPCM to Raw PCM ---
            print("Decoding dual-state ADPCM to WAV...")
            raw_pcm_data = bytearray()

            for i in range(0, len(all_block_data), ADPCM_BLOCK_SIZE):
                block = all_block_data[i : i + ADPCM_BLOCK_SIZE]
                if not block:
                    continue

                # Unpack the 6-byte block header for dual-state stereo
                pl, il, pr, ir = struct.unpack_from("<hBhB", block, 0)

                # Initialize decoders with the state from the header
                decoder_l = ADPCMDecoder(predictor=pl, index=il)
                decoder_r = ADPCMDecoder(predictor=pr, index=ir)

                adpcm_chunk_data = block[ADPCM_BLOCK_HEADER_SIZE:ADPCM_BLOCK_SIZE]

                # Decode the block nibble by nibble
                for adpcm_byte in adpcm_chunk_data:
                    nibble_l = (adpcm_byte >> 4) & 0x0F
                    nibble_r = adpcm_byte & 0x0F

                    sample_l = decoder_l.decode_sample(nibble_l)
                    sample_r = decoder_r.decode_sample(nibble_r)

                    # Append interleaved PCM data
                    raw_pcm_data.extend(struct.pack("<h", sample_l))
                    raw_pcm_data.extend(struct.pack("<h", sample_r))

            print("Decoding complete.")

            # --- 4. Create WAV file ---
            audio_segment = AudioSegment(
                data=raw_pcm_data,
                sample_width=TARGET_SAMPLE_WIDTH,
                frame_rate=TARGET_SAMPLE_RATE,
                channels=TARGET_CHANNELS,
            )

            output_dir.mkdir(parents=True, exist_ok=True)
            audio_segment.export(output_wav_path, format="wav")
            print(f"Successfully converted to WAV: {output_wav_path}")

            # --- 5. Compare with reference WAV (Diff logic remains the same) ---
            if diff_wav_path and diff_wav_path.exists():
                print(f"\n--- Comparing with reference file: {diff_wav_path.name} ---")
                try:
                    reference_audio = AudioSegment.from_file(diff_wav_path)
                    reference_audio = reference_audio.set_frame_rate(TARGET_SAMPLE_RATE)
                    reference_audio = reference_audio.set_channels(TARGET_CHANNELS)
                    reference_audio = reference_audio.set_sample_width(
                        TARGET_SAMPLE_WIDTH
                    )

                    converted_data = audio_segment.raw_data
                    reference_data = reference_audio.raw_data
                    min_length = min(len(converted_data), len(reference_data))

                    if len(converted_data) != len(reference_data):
                        print(
                            f"✗ Length mismatch: Converted {len(converted_data)} bytes, Reference {len(reference_data)} bytes"
                        )

                    if min_length > 0:
                        converted_samples = struct.unpack(
                            f"<{min_length//2}h", converted_data[:min_length]
                        )
                        reference_samples = struct.unpack(
                            f"<{min_length//2}h", reference_data[:min_length]
                        )

                        mse = sum(
                            (c - r) ** 2
                            for c, r in zip(converted_samples, reference_samples)
                        ) / len(converted_samples)
                        rmse = math.sqrt(mse)
                        signal_power = sum(r**2 for r in reference_samples) / len(
                            reference_samples
                        )
                        snr_db = (
                            10 * math.log10(signal_power / mse)
                            if mse > 0
                            else float("inf")
                        )

                        print(f"  Comparison Results (over {min_length} bytes):")
                        print(f"  RMSE: {rmse:.2f}")
                        print(f"  SNR:  {snr_db:.2f} dB")

                except Exception as e:
                    print(f"Error comparing with reference file: {e}")
            elif diff_wav_path:
                print(
                    f"Warning: Reference file '{diff_wav_path}' not found - skipping comparison"
                )

    except Exception as e:
        print(f"An unexpected error occurred: {e}")
        import traceback

        traceback.print_exc()


def main():
    """Main function to parse arguments and dispatch to the correct parser."""
    parser = argparse.ArgumentParser(
        description="Parse TJA/TJM files, print metadata, and convert TJA to WAV."
    )
    parser.add_argument(
        "input_file", type=str, help="Path to the .tja or .tjm file to parse."
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default=".",
        help="Output directory for the converted .wav file.",
    )
    parser.add_argument(
        "-d",
        "--diff",
        type=str,
        help="Path to a reference audio file to compare against.",
    )
    args = parser.parse_args()

    input_path = Path(args.input_file)
    output_path = Path(args.output)
    diff_wav_path = Path(args.diff) if args.diff else None

    if not input_path.exists():
        print(f"Error: Input file '{input_path}' not found.")
        return

    with open(input_path, "rb") as f:
        magic_bytes = f.read(4)

    if magic_bytes == b"TJA\0":
        parse_tja_file(input_path, output_path, diff_wav_path)
    elif magic_bytes == b"TJM\0":
        parse_tjm_file(input_path)
    else:
        print(f"Error: Unknown or unsupported file type for '{input_path.name}'.")


if __name__ == "__main__":
    main()
