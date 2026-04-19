## Plan: Firmware Bugfix Stabilization

Fix the memory-safety and trust-boundary bugs first, then close the concurrency/input-validation issues, then convert the identified runtime allocations to bounded static storage, and finish with a manual QEMU regression matrix. Preserve current script host API behavior for now, add an explicit Bluetooth confirm command, and treat Wi-Fi/Bluetooth runtime security validation as hardware-only where QEMU intentionally skips those services.

**Steps**
1. Phase 0 - Baseline and staging. Confirm the current QEMU workflow in main/main.c, tools/run_qemu_firmware.py, tools/ensure_sd_image.py, the staged scripts under flashfs/scripts, and the staged SD payload under tools/out. Update the stale scripting docs so they no longer rely on the missing historical smoketest helper. This phase unblocks verification and can run in parallel with implementation prep.
2. Phase 1 - Critical WASM safety fixes in main/script_service.c using the guest-pointer validation helpers already present in main/script_socket_env.c. Add guest pointer/string validation in log_wrapper and get_track_title_wrapper; stop dereferencing s_active_run_context after releasing the mutex; remove the unbounded heap path from wamr_host_vprintf_hook; add bounded argv/binary/context storage and reject oversize inputs up front. This phase blocks later QEMU script validation.
3. Phase 1a - Bluetooth pairing confirmation UX in main/bluetooth_service.c, main/bluetooth_service.h, and main/console_service.c. Replace unconditional SSP confirmation with a pending-confirm state machine, add service API(s) for accept/reject, and register a console command such as bt confirm <accept|reject>. This can run in parallel with step 2, but final console verification depends on the command registration landing.
4. Phase 2 - Input validation and trust-boundary hardening across main/cartridge_service.c, main/player_service.c, main/console_service.c, main/wifi_service.c, main/jukeboy_formats.h, and tools/make_album.py. Add explicit ftell/size guards before casts; bound CRC-length casts; ensure metadata strings are terminated or exposed through safe copy/accessor helpers; guard cmd_script and sd_parse_index; null-check metadata prints; restore/strengthen msg.len checks before CRC stripping; validate resume state against both playlist count and metadata count; add range guards in album generation before packing 32-bit lookup offsets; avoid fully buffering ffmpeg output. Wi-Fi/console and cartridge/player/tooling work can mostly run in parallel after step 2 starts.
5. Phase 2a - Wi-Fi credential hardening in main/main.c, main/wifi_service.c, partitions.csv, and likely sdkconfig.defaults. Explicitly terminate copied SSID/PSK buffers, zero temporary password buffers after use, and convert credential storage to encrypted NVS. Because the current partition table has no keys partition, add one and switch boot initialization from plain nvs_flash_init() to a secure-NVS setup path. This phase affects boot and should land before final QEMU boot verification.
6. Phase 3 - Concurrency fixes in main/bluetooth_service.c, main/cartridge_service.c, and main/console_service.c. Add synchronization around s_pcm_provider registration/use; guard cartridge mount state across enqueue and dequeue, preferably with a mount-generation or mutex-backed validity check; protect telemetry snapshot buffers/index with a mutex. This phase depends on the corresponding file-level refactors from steps 3-5 but can be implemented per subsystem in parallel.
7. Phase 4 - Static-allocation conversion and queue/backpressure cleanup in main/player_service.c, main/cartridge_service.c, main/script_service.c, main/console_service.c, main/bluetooth_service.c, and main/ramdisk_service.c. Replace the listed dynamic buffers with file-scope bounded storage in PSRAM/EXT_RAM_BSS_ATTR where appropriate. Preserve current 999-track format compatibility instead of shrinking to a smaller playlist cap: size the JBM blob to the current format maximum derived from main/jukeboy_formats.h (1652-byte header plus 999 x 392-byte tracks = 393260 bytes), size playlist/shuffle storage to JUKEBOY_MAX_TRACK_FILES, and document any intentionally lower cap only if memory pressure proves unacceptable. Use compile-time caps for script binary size, argv count/arg length, log rotation buffer, bonded device buffers, ramdisk storage, and console run result storage. This phase depends on the validation guards from steps 2-5 so oversize inputs fail cleanly instead of overrunning the new static storage.
8. Phase 5 - Performance cleanups in main/player_service.c, main/console_service.c, and main/bluetooth_service.c. Replace the pipeline idle poll loop with a single blocking wait, remove the 10-second console scan busy wait in favor of asynchronous status/event feedback, and replace Bluetooth disconnect/SPP-close polling with event-driven waits. This can follow the concurrency work because the same synchronization primitives will likely be reused.
9. Phase 6 - Manual QEMU regression pass. Rebuild firmware, regenerate the merged QEMU flash image and SD image, boot with tools/run_qemu_firmware.py, and run a repeatable console checklist that covers boot stability, SD metadata access, player startup, script listing, hello.wasm, player-control.wasm, net-echo.wasm, malformed JBM/JBA fixtures staged through tools/out plus tools/ensure_sd_image.py, and long-running script/log-truncation behavior. Record any Wi-Fi/BT items that cannot execute in QEMU as hardware-only follow-up.
10. Phase 7 - Hardware-only follow-up, explicitly out of QEMU scope. Validate encrypted NVS credentials on device, verify PSK zeroization/no-SSID logging in live logs, exercise Bluetooth pairing confirmation flow with a real peer, and stress-test A2DP/SPP registration/disconnect races. This phase is required for full closure of the Wi-Fi and Bluetooth items but does not block the QEMU-tested subset.

**Relevant files**
- main/script_service.c — wamr_host_vprintf_hook, log_wrapper, get_track_title_wrapper, script_duplicate_argv, script_load_file, script_prepare_context, script_execute_module, and the active-run/result lifecycle.
- main/script_socket_env.c — reuse script_socket_get_guest_buffer and script_socket_get_guest_string as the guest-address validation pattern.
- main/bluetooth_service.c — SSP confirm event handling, bonded-device buffering, s_pcm_provider synchronization, and disconnect/SPP wait behavior.
- main/bluetooth_service.h — expose pending-confirm accept/reject API for the console layer.
- main/console_service.c — cmd_script argc guards, sd_parse_index, metadata print safety, telemetry locking, Bluetooth confirm command, bonded-device print buffer, and Wi-Fi scan UX.
- main/wifi_service.c — credential storage/logging, copy/termination helpers, queue-full behavior, and temporary password zeroization.
- main/main.c — replace plain NVS init with secure-NVS boot flow and preserve current QEMU branching behavior.
- partitions.csv — add an NVS keys partition needed for encrypted credential storage.
- sdkconfig.defaults — any NVS-encryption-related config needed for the new secure init path.
- main/cartridge_service.c — metadata size/cast checks, safe string accessors/copies, mount/unmount synchronization, and static metadata storage.
- main/player_service.c — lookup table/playlist/shuffle storage, CRC-length validation, resume-state validation, and idle-wait cleanup.
- main/jukeboy_formats.h — source of the current maximum metadata footprint; update comments/constants only if new caps are formalized here.
- main/ramdisk_service.c and main/ramdisk_service.h — replace runtime ramdisk allocation with static storage using RAMDISK_SERVICE_SIZE_BYTES.
- tools/make_album.py — ffmpeg I/O handling and 32-bit offset/range guards when building JBA/JBM artifacts.
- tools/run_qemu_firmware.py — existing manual QEMU launcher used for regression execution.
- tools/ensure_sd_image.py — current SD-image staging path used to inject normal and malformed cartridge fixtures for QEMU.
- docs/scripting.md — remove or replace the stale reference to the missing QEMU smoketest helper and document the manual QEMU smoke procedure that actually exists today.
- flashfs/scripts/hello, flashfs/scripts/player-control, flashfs/scripts/net-echo, flashfs/scripts/google-get — existing staged scripts to use in QEMU smoke tests.
- tools/out — current SD-card staging directory for the default album and any malformed fixture variants used during QEMU regression.

**Verification**
1. Build the firmware with the normal ESP-IDF flow, then regenerate the merged QEMU flash image and SD image before every emulated run.
2. Boot QEMU through tools/run_qemu_firmware.py and confirm clean startup to the console prompt with no panic, watchdog spam, or mount failures after the NVS/partition changes.
3. Run manual console smoke cases: help, sd status, sd meta album_name, sd meta track_name 0, script status, script ls, script run hello, script run player-control status, and script log hello.
4. Stage malformed cartridge fixtures through tools/out and regenerate the SD image with tools/ensure_sd_image.py; verify that oversize metadata, bad lookup lengths, tiny CRC-prefixed chunks, and invalid indices fail cleanly with errors instead of crashes or hangs.
5. Add a runaway or chatty script case and verify bounded logging plus continued console responsiveness during and after the script run.
6. Exercise mount/unmount and playback churn in QEMU to confirm the cartridge queue/mount synchronization and the pipeline idle wait changes do not deadlock the player.
7. Verify that the Bluetooth confirm command is registered and that the firmware still boots under QEMU even though Classic BT is skipped there; perform the actual pairing confirmation flow on hardware afterward.
8. Perform hardware-only checks for encrypted Wi-Fi credentials, PSK zeroization, redacted logging, and Bluetooth MITM confirmation because main/main.c intentionally skips Wi-Fi and Bluetooth init under QEMU.
9. Run any available compiler or lint diagnostics after the changes and treat new warnings in touched files as regressions.

**Decisions**
- Preserve current script host API behavior for now. Pointer validation, time/size limits, and memory safety are in scope; deny-by-default capability gating is explicitly deferred by user choice.
- Add an explicit Bluetooth console confirm/reject flow rather than silently auto-approving SSP confirmations.
- Keep QEMU verification mostly manual rather than building a new automation harness in this pass.
- Preserve the current 999-track media-format compatibility unless memory measurements force a lower documented cap.
- Treat Wi-Fi and Bluetooth runtime security validation as hardware-only where QEMU intentionally bypasses those services.

**Further Considerations**
2. Encrypted NVS is a boot-path change because partitions.csv has no keys partition today. If that migration proves too disruptive for the immediate bugfix batch, split it into its own commit with explicit migration notes.
