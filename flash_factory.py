# flash_factory.py
import csv
import subprocess

Import("env")


# This helper function parses the partition CSV to find partition info
def get_partition_info(csv_file, partition_name):
    """Parses the partition CSV and returns a dict with offset and size."""
    try:
        with open(csv_file, "r") as f:
            # Skip comment lines
            reader = csv.reader(row for row in f if not row.startswith("#"))
            for row in reader:
                # Check if row is valid and matches the partition name
                if len(row) > 4 and row[0].strip() == partition_name:
                    return {"offset": row[3].strip(), "size": row[4].strip()}
    except IOError:
        print(f"Error: Could not open partition file {csv_file}")
    return None


# --- Main Logic ---
# Only run this logic if the active environment is 'factory'
if env.subst("$PIOENV") == "factory":

    def custom_upload_and_erase(source, target, env):
        """Flashes the factory app and erases the otadata partition."""
        print("--- Intercepting upload for 'factory' environment ---")

        # Get required paths and settings from the environment
        firmware_path = str(source[0])
        partitions_csv = env.subst(
            "$PROJECT_DIR/partitions.csv"
        )  # Use board_partitions
        esptool_path = env.get("ESPTOOL", "esptool")
        upload_port = env.get("UPLOAD_PORT")

        if not upload_port:
            raise Exception(
                "Error: Upload port not defined. Set upload_port or use --upload-port."
            )

        # Find info for both partitions
        factory_info = get_partition_info(partitions_csv, "factory")
        otadata_info = get_partition_info(partitions_csv, "otadata")

        if not factory_info:
            raise Exception("Error: Could not find 'factory' partition in CSV.")
        if not otadata_info:
            raise Exception("Error: Could not find 'otadata' partition in CSV.")

        # --- 1. Flash the factory application ---
        cmd_flash = [
            esptool_path,
            "--chip",
            env.get("BOARD_MCU"),
            "--port",
            upload_port,
            "--baud",
            str(env.get("UPLOAD_SPEED")),
            "write_flash",
            factory_info["offset"],
            firmware_path,
        ]

        print(f"Flashing factory app: {' '.join(cmd_flash)}")
        result = subprocess.run(cmd_flash)
        if result.returncode != 0:
            print("Factory app flashing failed. Aborting.")
            return result.returncode

        print("\n--- Factory app flashed successfully ---")

        return result.returncode

    # Replace the default "upload" command with our custom function
    env.Replace(UPLOADCMD=custom_upload_and_erase)
