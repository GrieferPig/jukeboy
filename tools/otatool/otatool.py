import asyncio
import zlib
import hashlib
import os
import argparse
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

# --- UUID definitions ---
OTA_SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
DATA_CHAR_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
CONTROL_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# --- Control Commands ---
CMD_FLASH_PART = b"\x00"  # Previously CMD_START_OTA
CMD_SEND_HASH = b"\x01"
CMD_ERASE_PART = b"\x02"
CMD_VERIFY_PART = b"\x03"
CMD_REBOOT = b"\x04"

# --- ESP32 Notifications ---
NOTIFY_CMD_OK = b"\x10"  # Previously NOTIFY_OTA_OK
NOTIFY_HASH_OK = b"\x11"
NOTIFY_ERR_HASH = b"\x81"
NOTIFY_ERR_WRITE = b"\x82"
NOTIFY_ERR_ZLIB = b"\x83"
NOTIFY_ERR_CMD = b"\x84"
NOTIFY_ERR_PARTITION = b"\x85"

# --- Partition Type Enums (from part_mgr.h) ---
PARTITION_TYPE = {
    "app": b"\x00",  # PART_MGR_PARTITION_TYPE_APP
    "otadata": b"\x01",  # PART_MGR_PARTITION_TYPE_OTADATA
    "littlefs": b"\x02",  # PART_MGR_PARTITION_TYPE_LITTLEFS
    "nvs": b"\x03",  # PART_MGR_PARTITION_TYPE_NVS
}

# Global event for command completion
cmd_finished_event = asyncio.Event()
cmd_success = False
last_notification = None


def notification_handler(sender, data):
    """Handles notifications from the ESP32."""
    global cmd_success, last_notification
    print(f"[ESP32 NOTIFY] {data.hex()}")
    last_notification = data

    if data in [NOTIFY_CMD_OK, NOTIFY_HASH_OK]:
        cmd_success = True
        cmd_finished_event.set()
    elif data in [
        NOTIFY_ERR_HASH,
        NOTIFY_ERR_WRITE,
        NOTIFY_ERR_ZLIB,
        NOTIFY_ERR_CMD,
        NOTIFY_ERR_PARTITION,
    ]:
        cmd_success = False
        cmd_finished_event.set()


async def connect_to_device(device_name):
    """Find and connect to the ESP32 device."""
    print(f"--- Searching for device: {device_name} ---")
    device = await BleakScanner.find_device_by_name(device_name, timeout=20.0)
    if not device:
        print(f"❌ Could not find a device named '{device_name}'.")
        return None

    print(f"✅ Found device: {device.name} ({device.address})")

    client = BleakClient(device, winrt={"use_cached_services": False})
    try:
        await client.connect()
        if not client.is_connected:
            print("❌ Connection failed!")
            return None

        print("✅ Device connected.")
        await client.start_notify(CONTROL_CHAR_UUID, notification_handler)
        print("✅ Notifications enabled.")
        return client
    except BleakError as e:
        print(f"❌ Connection error: {e}")
        return None


async def flash_partition(client, partition_type, firmware_path):
    """Flash a firmware file to the specified partition."""
    # Reset event and status for the new command
    global cmd_success
    cmd_finished_event.clear()
    cmd_success = False

    if not os.path.exists(firmware_path):
        print(f"❌ Firmware file not found: {firmware_path}")
        return False

    # Read and compress the firmware file
    with open(firmware_path, "rb") as f:
        firmware_data = f.read()

    sha256_hash = hashlib.sha256(firmware_data).digest()
    compressed_data = zlib.compress(firmware_data, level=9)

    print(f"Firmware size: {len(firmware_data)} bytes")
    print(f"Compressed size: {len(compressed_data)} bytes")
    print(f"SHA256: {sha256_hash.hex()}")

    # Send flash command with partition type
    part_type_byte = PARTITION_TYPE.get(partition_type)
    if part_type_byte is None:
        print(f"❌ Invalid partition type: {partition_type}")
        return False

    payload = CMD_FLASH_PART + part_type_byte
    await client.write_gatt_char(CONTROL_CHAR_UUID, payload)

    # Wait for confirmation to start flashing
    print("--- Waiting for confirmation to start flashing... ---")
    try:
        await asyncio.wait_for(cmd_finished_event.wait(), timeout=15.0)
        if not cmd_success:
            print("❌ ESP32 did not acknowledge flash command. Aborting.")
            return False
        cmd_finished_event.clear()  # Reset for next operation
    except asyncio.TimeoutError:
        print("❌ Timeout waiting for flash command acknowledgement.")
        return False

    # Stream compressed data
    print("--- Streaming firmware data... ---")
    chunk_size = client.mtu_size - 3  # Adjust for ATT header
    total_chunks = (len(compressed_data) + chunk_size - 1) // chunk_size

    for i, chunk_start in enumerate(range(0, len(compressed_data), chunk_size)):
        chunk_end = chunk_start + chunk_size
        data_chunk = compressed_data[chunk_start:chunk_end]
        await client.write_gatt_char(DATA_CHAR_UUID, data_chunk, response=False)
        print(f"Sent chunk {i+1}/{total_chunks}", end="\r")
    print("\n✅ Firmware data sent.")

    # Send hash to finalize
    payload = CMD_SEND_HASH + sha256_hash
    await client.write_gatt_char(CONTROL_CHAR_UUID, payload)

    # Wait for verification result
    print("--- Waiting for final verification from ESP32... ---")
    try:
        await asyncio.wait_for(cmd_finished_event.wait(), timeout=60.0)
        if cmd_success:
            print("\n🎉 Flash successful! 🎉")
            return True
        else:
            print("\n❌ Flash failed. Check ESP32 logs.")
            return False
    except asyncio.TimeoutError:
        print("\n❌ Timeout waiting for final verification.")
        return False


async def erase_partition(client, partition_type):
    """Erase a partition."""
    global cmd_success
    cmd_finished_event.clear()
    cmd_success = False

    part_type_byte = PARTITION_TYPE.get(partition_type)
    if part_type_byte is None:
        print(f"❌ Invalid partition type: {partition_type}")
        return False

    print(f"--- Erasing partition: {partition_type} ---")
    payload = CMD_ERASE_PART + part_type_byte
    await client.write_gatt_char(CONTROL_CHAR_UUID, payload)

    try:
        await asyncio.wait_for(cmd_finished_event.wait(), timeout=30.0)
        if cmd_success:
            print("✅ Partition erased successfully.")
            return True
        else:
            print("❌ Partition erase failed.")
            return False
    except asyncio.TimeoutError:
        print("❌ Timeout waiting for erase confirmation.")
        return False


async def verify_partition(client, partition_type, firmware_path):
    """Verify a partition against a firmware file."""
    global cmd_success
    cmd_finished_event.clear()
    cmd_success = False

    if not os.path.exists(firmware_path):
        print(f"❌ Firmware file not found: {firmware_path}")
        return False

    # Read the firmware file to calculate hash and size
    with open(firmware_path, "rb") as f:
        firmware_data = f.read()

    sha256_hash = hashlib.sha256(firmware_data).digest()
    size = len(firmware_data)
    size_bytes = size.to_bytes(
        4, byteorder="little"
    )  # Convert size to 4 bytes, little-endian

    part_type_byte = PARTITION_TYPE.get(partition_type)
    if part_type_byte is None:
        print(f"❌ Invalid partition type: {partition_type}")
        return False

    print(f"--- Verifying partition: {partition_type} ---")
    print(f"Firmware size: {size} bytes")
    print(f"SHA256: {sha256_hash.hex()}")

    # Create the payload and check its length
    payload = CMD_VERIFY_PART + part_type_byte + sha256_hash + size_bytes
    print(f"Payload length: {len(payload)} bytes")

    if len(payload) != 38:  # 1(cmd) + 1(part_type) + 32(hash) + 4(size)
        print(f"❌ Invalid payload length: {len(payload)}. Expected 38 bytes.")
        print(f"Command: {CMD_VERIFY_PART.hex()}, Part type: {part_type_byte.hex()}")
        print(f"Hash length: {len(sha256_hash)}, Size bytes: {size_bytes.hex()}")
        return False

    # The MTU might be too small for the entire payload
    # Break it into multiple writes or use a different approach if necessary
    try:
        # Try to determine the MTU size
        mtu = (
            getattr(client, "mtu_size", 23) - 3
        )  # Default BLE MTU is 23, minus 3 for ATT overhead
        print(f"Estimated MTU size: {mtu}")

        if len(payload) > mtu:
            print(
                f"⚠️ Payload size ({len(payload)}) exceeds MTU ({mtu}). May need to fragment."
            )
            # For now, we'll still try to send it and see if the stack handles it

        await client.write_gatt_char(CONTROL_CHAR_UUID, payload)

        await asyncio.wait_for(cmd_finished_event.wait(), timeout=60.0)
        if cmd_success:
            print("✅ Verification successful. Hash matched.")
            return True
        else:
            print("❌ Verification failed. Hash mismatch.")
            return False
    except Exception as e:
        print(f"❌ Error during verify command: {e}")
        return False


async def reboot_device(client):
    """Reboot the ESP32."""
    global cmd_success
    cmd_finished_event.clear()
    cmd_success = False

    print("--- Rebooting device... ---")
    try:
        # Change to write without response (response=False) since the device will reboot immediately
        await client.write_gatt_char(CONTROL_CHAR_UUID, CMD_REBOOT, response=False)
    except OSError as e:
        pass

    print("✅ Reboot command sent. Device should reboot shortly.")
    return True


async def run_command(args):
    """Execute the requested command."""
    client = await connect_to_device(args.name)
    if not client:
        return

    success = False
    try:
        if args.command == "flash":
            success = await flash_partition(client, args.partition, args.file)
        elif args.command == "erase":
            success = await erase_partition(client, args.partition)
        elif args.command == "verify":
            success = await verify_partition(client, args.partition, args.file)
        elif args.command == "reboot":
            success = await reboot_device(client)
    finally:
        # Ensure we always disconnect
        await client.disconnect()
        print("Disconnected from device.")

    return success


def main():
    """Parse command-line arguments and run the requested command."""
    parser = argparse.ArgumentParser(description="ESP32 OTA Tool")
    parser.add_argument(
        "--name",
        "-n",
        default="jb_needs_your_help",
        help="BLE device name (default: jb_needs_your_help)",
    )

    subparsers = parser.add_subparsers(
        dest="command", required=True, help="Command to execute"
    )

    # Flash command
    flash_parser = subparsers.add_parser("flash", help="Flash a firmware file")
    flash_parser.add_argument(
        "--partition",
        "-p",
        required=True,
        choices=["app", "littlefs"],
        help="Partition to flash (app or littlefs)",
    )
    flash_parser.add_argument(
        "--file", "-f", required=True, help="Path to firmware file"
    )

    # Erase command
    erase_parser = subparsers.add_parser("erase", help="Erase a partition")
    erase_parser.add_argument(
        "--partition",
        "-p",
        required=True,
        choices=["app", "littlefs", "nvs"],
        help="Partition to erase (app, littlefs, or nvs)",
    )

    # Verify command
    verify_parser = subparsers.add_parser("verify", help="Verify a partition")
    verify_parser.add_argument(
        "--partition",
        "-p",
        required=True,
        choices=["app", "littlefs"],
        help="Partition to verify (app or littlefs)",
    )
    verify_parser.add_argument(
        "--file", "-f", required=True, help="Path to firmware file to verify against"
    )

    # Reboot command
    subparsers.add_parser("reboot", help="Reboot the device")

    args = parser.parse_args()

    # Run the command asynchronously
    asyncio.run(run_command(args))


if __name__ == "__main__":
    main()


if __name__ == "__main__":
    main()
