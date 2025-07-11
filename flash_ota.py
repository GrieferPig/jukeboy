import subprocess
import os

Import("env")

# --- Main Logic ---
# Only run this logic if the active environment is 'ota'
if env.subst("$PIOENV") == "ota":

    def custom_ota_upload(source, target, env):
        """Uploads the firmware using the otatool.py script via BLE."""
        print("--- Intercepting upload for 'ota' environment ---")

        # Get required paths from the environment
        firmware_path = str(source[0])
        project_dir = env.subst("$PROJECT_DIR")
        otatool_path = os.path.join(project_dir, "tools", "otatool", "otatool.py")
        python_executable = env.get("PYTHONEXE", "python")

        if not os.path.exists(otatool_path):
            raise Exception(f"Error: otatool.py not found at {otatool_path}")

        if not os.path.exists(firmware_path):
            raise Exception(f"Error: Firmware file not found at {firmware_path}")

        # --- Call the OTA tool to flash the 'app' partition ---
        cmd_ota = [
            python_executable,
            otatool_path,
            "flash",
            "--partition",
            "app",
            "--file",
            firmware_path,
        ]

        print(f"Executing OTA upload: {' '.join(cmd_ota)}")
        print("Please ensure the device is in recovery mode and advertising.")

        result = subprocess.run(cmd_ota)
        if result.returncode != 0:
            print("OTA upload failed. Please check the output above.")
            return result.returncode

        print("\n--- OTA upload reported success ---")

        # call otatool to reboot
        cmd_reboot = [
            python_executable,
            otatool_path,
            "reboot",
        ]
        print(f"Rebooting device with command: {' '.join(cmd_reboot)}")
        result = subprocess.run(cmd_reboot)
        if result.returncode != 0:
            print("Reboot command failed. Please check the output above.")
        return result.returncode

    # Replace the default "upload" command with our custom function
    env.Replace(UPLOADCMD=custom_ota_upload)
