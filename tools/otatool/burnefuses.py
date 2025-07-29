import subprocess
import sys
import os

# EFuse structure:
# - PCB Revision: 8 bits
# - Bootloader Revision: 8 bits
# - Flags: 8 bits
#   - Bit 0: Is special edition (w/ Owner Name)
#   - Bits 1-7: Other bits reserved
# - Owner Name: up to 26 bytes UTF-8 (no null termination, 0xFF padding)


def get_validated_input(prompt_text, max_value=255):
    """Gets and validates user input to ensure it's an integer within the valid range."""
    while True:
        try:
            value = int(input(prompt_text))
            if 0 <= value <= max_value:
                return value
            else:
                print(
                    f"Error: Value must be between 0 and {max_value}. Please try again."
                )
        except ValueError:
            print("Error: Invalid input. Please enter a whole number.")


def get_yes_no_input(prompt_text):
    """Gets yes/no input from user."""
    while True:
        response = input(prompt_text).strip().lower()
        if response in ["y", "yes"]:
            return True
        elif response in ["n", "no"]:
            return False
        else:
            print("Please enter 'y' for yes or 'n' for no.")


def get_owner_name():
    """Gets and validates owner name input (max 26 UTF-8 bytes)."""
    while True:
        owner_name = input("Enter Owner Name (max 26 UTF-8 bytes): ")
        # Check if UTF-8 encoded bytes fit within 26 bytes
        try:
            owner_bytes = owner_name.encode("utf-8")
            if len(owner_bytes) <= 26:
                return owner_name
            else:
                print(
                    f"Error: Owner name encodes to {len(owner_bytes)} bytes. Must be 26 bytes or less when UTF-8 encoded."
                )
        except UnicodeEncodeError:
            print("Error: Owner name contains invalid characters for UTF-8 encoding.")


def pack_owner_name(owner_name):
    """Packs owner name into up to 26 bytes with 0xFF padding."""
    # Convert to bytes and pad with 0xFF
    owner_bytes = owner_name.encode("utf-8")
    padded_bytes = owner_bytes + b"\xff" * (26 - len(owner_bytes))

    # Convert to hex string for espefuse
    hex_string = "0x" + padded_bytes.hex()
    return hex_string, padded_bytes


def check_total_data_size(packed_data, owner_name=None):
    """Checks if the total data size exceeds 26 bytes."""
    # Main data is 4 bytes (32 bits)
    main_data_size = 4

    # Owner name is up to 26 bytes if present
    owner_data_size = 26 if owner_name else 0

    total_size = main_data_size + owner_data_size

    if (
        total_size > 32
    ):  # Adjusted to accommodate 4 bytes main data + 26 bytes owner name + 2 bytes padding
        return False, total_size
    return True, total_size


def main():
    """Main function to gather data, construct, and run the espefuse command."""
    print("--- ESP32 eFuse Burner ---")
    print(
        "\n\033[91m" + "WARNING: Burning eFuses is an IRREVERSIBLE action." + "\033[0m"
    )
    print("You can only change bits from 0 to 1. You can NEVER change them back.")
    print(
        "Before proceeding, run 'espefuse --port YOUR_PORT summary' to ensure the target bits in BLK3 are free.\n"
    )

    # --- Get User Inputs ---
    port = input("Enter the serial port for your ESP32 (e.g., COM3 or /dev/ttyUSB0): ")
    if not port:
        print("Error: Port cannot be empty.")
        sys.exit(1)

    pcb_rev = get_validated_input("Enter PCB Revision (0-255): ")
    bl_rev = get_validated_input("Enter Bootloader Revision (0-255): ")

    # Get flag settings
    print("\n--- Flag Settings ---")
    is_special_edition = get_yes_no_input("Is this a special edition device? (y/n): ")

    # Build flags byte
    flags = 0
    if is_special_edition:
        flags |= 0x01  # Set bit 0

    # Get owner name only if special edition
    owner_name = None
    if is_special_edition:
        while True:
            owner_name = get_owner_name()

            # Check total data size constraint
            is_size_valid, total_size = check_total_data_size(None, owner_name)
            if is_size_valid:
                break
            else:
                print(
                    f"Error: Total data size ({total_size} bytes) exceeds the 32-byte limit."
                )
                print("Please enter a shorter owner name.")
                continue

    # --- Pack Data ---
    # We will pack the three 8-bit values into a single 32-bit integer.
    # The format will be: 0x00[pcb_rev][bl_rev][flags]
    # The first byte is unused (0x00).
    packed_data = (pcb_rev << 16) | (bl_rev << 8) | flags
    hex_string = f"0x{packed_data:08x}"  # Format as an 8-digit hex string

    # Pack owner name if needed
    if is_special_edition:
        owner_hex, owner_bytes = pack_owner_name(owner_name)

    print("\n--- Data to be Burned ---")
    print(f"  PCB Revision:       {pcb_rev}")
    print(f"  Bootloader Revision:  {bl_rev}")
    print(f"  Special Edition:      {'Yes' if is_special_edition else 'No'}")
    print(f"  Flags Value:          {flags} (0x{flags:02x})")
    if is_special_edition:
        print(f"  Owner Name:         '{owner_name}'")
    print(f"  Packed Hex Value:     {hex_string}")
    print(f"  Packed Bits:          {packed_data:032b}")
    if is_special_edition:
        print(f"  Owner Hex Value:      {owner_hex}")
    print(f"  Target Block:         BLOCK3")
    print(f"  Data Offset:          0")
    print(f"  Total Data Size:      32 bytes (combined)")
    print("-" * 29)

    # --- Construct Commands ---
    # Create single combined binary file
    combined_data = bytearray(
        32
    )  # 32 bytes total: 4 bytes main data + 26 bytes owner name + 2 bytes padding

    # Add main data at offset 0
    data_bytes = bytes.fromhex(hex_string[2:])  # Remove '0x' prefix
    combined_data[0:4] = data_bytes

    # Add owner name data at offset 4 if special edition
    if is_special_edition:
        owner_hex, owner_bytes = pack_owner_name(owner_name)
        combined_data[4:30] = owner_bytes
    else:
        # Fill owner name section with 0xFF if not special edition
        combined_data[4:30] = b"\xff" * 26

    # Pad remaining bytes (30-32) with 0x00
    combined_data[30:32] = b"\x00" * 2

    # Create temporary file for combined data in the same directory as the script
    script_dir = os.path.dirname(os.path.abspath(__file__))
    temp_file_path = os.path.join(script_dir, "efuse_data.bin")

    with open(temp_file_path, "wb") as temp_file:
        temp_file.write(combined_data)

    command = [
        "espefuse",
        "--port",
        port,
        "burn_block_data",
        "BLOCK3",
        temp_file_path,
        "--offset",
        "0",
        "--force-write-always",
    ]

    print("\nGenerated command to burn eFuses:")
    print(f"\033[93m" + " ".join(command) + "\033[0m")
    print(f"\nData file created: {temp_file_path}")
    print("\nTo proceed with burning the eFuses, run the command above.")
    print("\033[91m" + "WARNING: This operation is IRREVERSIBLE!" + "\033[0m")
    print("Make sure to verify the data file contents before running the command.")


if __name__ == "__main__":
    main()
