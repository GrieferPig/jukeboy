from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import secrets
import socket
import struct
import sys
import uuid
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Any

try:
    from bleak import BleakClient, BleakScanner
    from bleak.backends.characteristic import BleakGATTCharacteristic
    from bleak.backends.device import BLEDevice
    from bleak.backends.scanner import AdvertisementData
except ModuleNotFoundError as exc:
    raise SystemExit(
        "The bleak package is required for tools/companion.py. "
        "Install it into the Python environment you will use for BLE testing with "
        "`python -m pip install bleak`."
    ) from exc


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_STATE_FILE = REPO_ROOT / "tools" / "out" / "companion_state.json"

BLE_BASE_UUID = "0000{short:04x}-0000-1000-8000-00805f9b34fb"
SERVICE_UUID = BLE_BASE_UUID.format(short=0xABF0)
WRITE_UUID = BLE_BASE_UUID.format(short=0xABF1)
NOTIFY_UUID = BLE_BASE_UUID.format(short=0xABF2)
DEFAULT_DEVICE_NAME = "ESP_SPP_SERVER"

MAGIC = b"JC"
VERSION = 1
FRAME_HEADER_LEN = 12
TLV_HEADER_LEN = 4
DEFAULT_TIMEOUT = 10.0
DEFAULT_SCAN_TIMEOUT = 5.0
DEFAULT_CHUNK_SIZE = 20
FRAME_MAX_LEN = 2048
PAIR_SECRET_LEN = 32
AUTH_NONCE_LEN = 16
HMAC_LEN = 32

BUTTON_NAME_TO_ID = {
    "main1": 0,
    "main2": 1,
    "main3": 2,
    "misc1": 3,
    "misc2": 4,
    "misc3": 5,
}
BUTTON_ID_TO_NAME = {value: key for key, value in BUTTON_NAME_TO_ID.items()}

PLAYBACK_MODE_TO_ID = {
    "sequential": 0,
    "single_repeat": 1,
    "shuffle": 2,
}
PLAYBACK_MODE_FROM_ID = {value: key for key, value in PLAYBACK_MODE_TO_ID.items()}

OUTPUT_TARGET_TO_ID = {
    "bluetooth": 0,
    "i2s": 1,
}
OUTPUT_TARGET_FROM_ID = {value: key for key, value in OUTPUT_TARGET_TO_ID.items()}

WIFI_STATE_FROM_ID = {
    0: "idle",
    1: "scanning",
    2: "connecting",
    3: "connected",
    4: "disconnected",
}

CARTRIDGE_STATUS_FROM_ID = {
    0: "empty",
    1: "ready",
    2: "invalid",
}

TLV_NAME = {
    0x0001: "status",
    0x0002: "error_code",
    0x0003: "error_message",
    0x0004: "protocol_version",
    0x0005: "feature_bits",
    0x0006: "max_frame",
    0x0007: "mtu",
    0x0008: "max_payload",
    0x0009: "request_id",
    0x0100: "authenticated",
    0x0101: "client_id",
    0x0102: "app_name",
    0x0103: "shared_secret",
    0x0104: "button_sequence",
    0x0105: "pairing_pending",
    0x0106: "pairing_progress",
    0x0107: "pairing_required",
    0x0108: "auth_nonce",
    0x0109: "auth_hmac",
    0x010A: "trusted_count",
    0x010B: "created_at",
    0x0200: "generation",
    0x0201: "uptime_ms",
    0x0202: "queue_free",
    0x0203: "rx_frames",
    0x0204: "tx_frames",
    0x0205: "rx_errors",
    0x0300: "playing",
    0x0301: "paused",
    0x0302: "track_index",
    0x0303: "track_count",
    0x0304: "position_sec",
    0x0305: "started_at",
    0x0306: "duration_sec",
    0x0307: "volume_percent",
    0x0308: "playback_mode",
    0x0309: "track_title",
    0x030A: "track_artist",
    0x030B: "track_file",
    0x030C: "action",
    0x030D: "value",
    0x030E: "output_target",
    0x0400: "cartridge_status",
    0x0401: "cartridge_mounted",
    0x0402: "cartridge_checksum",
    0x0403: "metadata_version",
    0x0404: "album_name",
    0x0405: "album_artist",
    0x0406: "album_description",
    0x0407: "album_year",
    0x0408: "album_duration",
    0x0409: "album_genre",
    0x040A: "offset",
    0x040B: "count",
    0x040C: "returned_count",
    0x0500: "wifi_state",
    0x0501: "wifi_internet",
    0x0502: "wifi_autoreconnect",
    0x0503: "wifi_active_slot",
    0x0504: "wifi_preferred_slot",
    0x0505: "wifi_ip",
    0x0506: "wifi_ssid",
    0x0507: "wifi_password",
    0x0508: "wifi_slot",
    0x0509: "wifi_rssi",
    0x050A: "wifi_channel",
    0x050B: "wifi_authmode",
    0x0600: "lastfm_has_auth_url",
    0x0601: "lastfm_has_token",
    0x0602: "lastfm_has_session",
    0x0603: "lastfm_busy",
    0x0604: "lastfm_scrobbling",
    0x0605: "lastfm_now_playing",
    0x0606: "lastfm_pending_commands",
    0x0607: "lastfm_pending_scrobbles",
    0x0608: "lastfm_successful",
    0x0609: "lastfm_failed",
    0x060A: "lastfm_auth_url",
    0x060B: "lastfm_username",
    0x0700: "history_album_count",
    0x0701: "history_track_count",
    0x0702: "history_play_count",
    0x0703: "history_first_seen",
    0x0704: "history_last_seen",
    0x0800: "bt_a2dp_connected",
    0x0801: "bt_bonded_count",
}


class FrameType(IntEnum):
    REQUEST = 1
    RESPONSE = 2
    EVENT = 3
    HEARTBEAT = 4
    ERROR = 5


class Opcode(IntEnum):
    HELLO = 0x0001
    CAPABILITIES = 0x0002
    PING = 0x0003
    PAIR_BEGIN = 0x0010
    PAIR_STATUS = 0x0011
    PAIR_CANCEL = 0x0012
    AUTH_CHALLENGE = 0x0013
    AUTH_PROOF = 0x0014
    TRUSTED_LIST = 0x0015
    TRUSTED_REVOKE = 0x0016
    SNAPSHOT = 0x0100
    PLAYBACK_STATUS = 0x0101
    PLAYBACK_CONTROL = 0x0102
    LIBRARY_ALBUM = 0x0110
    LIBRARY_TRACK_PAGE = 0x0111
    WIFI_STATUS = 0x0200
    WIFI_SCAN_START = 0x0201
    WIFI_SCAN_RESULTS = 0x0202
    WIFI_CONNECT = 0x0203
    WIFI_CONNECT_SLOT = 0x0204
    WIFI_DISCONNECT = 0x0205
    WIFI_AUTORECONNECT = 0x0206
    LASTFM_STATUS = 0x0300
    LASTFM_CONTROL = 0x0301
    HISTORY_SUMMARY = 0x0400
    HISTORY_ALBUM_PAGE = 0x0401
    BT_AUDIO_STATUS = 0x0500
    BT_AUDIO_CONTROL = 0x0501


class TlvType(IntEnum):
    ERROR_CODE = 0x0002
    ERROR_MESSAGE = 0x0003
    PROTOCOL_VERSION = 0x0004
    FEATURE_BITS = 0x0005
    MAX_FRAME = 0x0006
    MTU = 0x0007
    MAX_PAYLOAD = 0x0008
    AUTHENTICATED = 0x0100
    CLIENT_ID = 0x0101
    APP_NAME = 0x0102
    SHARED_SECRET = 0x0103
    BUTTON_SEQUENCE = 0x0104
    PAIRING_PENDING = 0x0105
    PAIRING_PROGRESS = 0x0106
    PAIRING_REQUIRED = 0x0107
    AUTH_NONCE = 0x0108
    AUTH_HMAC = 0x0109
    TRUSTED_COUNT = 0x010A
    CREATED_AT = 0x010B
    GENERATION = 0x0200
    UPTIME_MS = 0x0201
    QUEUE_FREE = 0x0202
    RX_FRAMES = 0x0203
    TX_FRAMES = 0x0204
    RX_ERRORS = 0x0205
    PLAYING = 0x0300
    PAUSED = 0x0301
    TRACK_INDEX = 0x0302
    TRACK_COUNT = 0x0303
    POSITION_SEC = 0x0304
    STARTED_AT = 0x0305
    DURATION_SEC = 0x0306
    VOLUME_PERCENT = 0x0307
    PLAYBACK_MODE = 0x0308
    TRACK_TITLE = 0x0309
    TRACK_ARTIST = 0x030A
    TRACK_FILE = 0x030B
    ACTION = 0x030C
    VALUE = 0x030D
    OUTPUT_TARGET = 0x030E
    CARTRIDGE_STATUS = 0x0400
    CARTRIDGE_MOUNTED = 0x0401
    CARTRIDGE_CHECKSUM = 0x0402
    METADATA_VERSION = 0x0403
    ALBUM_NAME = 0x0404
    ALBUM_ARTIST = 0x0405
    ALBUM_DESCRIPTION = 0x0406
    ALBUM_YEAR = 0x0407
    ALBUM_DURATION = 0x0408
    ALBUM_GENRE = 0x0409
    OFFSET = 0x040A
    COUNT = 0x040B
    RETURNED_COUNT = 0x040C
    WIFI_STATE = 0x0500
    WIFI_INTERNET = 0x0501
    WIFI_AUTORECONNECT = 0x0502
    WIFI_ACTIVE_SLOT = 0x0503
    WIFI_PREFERRED_SLOT = 0x0504
    WIFI_IP = 0x0505
    WIFI_SSID = 0x0506
    WIFI_PASSWORD = 0x0507
    WIFI_SLOT = 0x0508
    WIFI_RSSI = 0x0509
    WIFI_CHANNEL = 0x050A
    WIFI_AUTHMODE = 0x050B
    LASTFM_HAS_AUTH_URL = 0x0600
    LASTFM_HAS_TOKEN = 0x0601
    LASTFM_HAS_SESSION = 0x0602
    LASTFM_BUSY = 0x0603
    LASTFM_SCROBBLING = 0x0604
    LASTFM_NOW_PLAYING = 0x0605
    LASTFM_PENDING_COMMANDS = 0x0606
    LASTFM_PENDING_SCROBBLES = 0x0607
    LASTFM_SUCCESSFUL = 0x0608
    LASTFM_FAILED = 0x0609
    LASTFM_AUTH_URL = 0x060A
    LASTFM_USERNAME = 0x060B
    HISTORY_ALBUM_COUNT = 0x0700
    HISTORY_TRACK_COUNT = 0x0701
    HISTORY_PLAY_COUNT = 0x0702
    HISTORY_FIRST_SEEN = 0x0703
    HISTORY_LAST_SEEN = 0x0704
    BT_A2DP_CONNECTED = 0x0800
    BT_BONDED_COUNT = 0x0801


class PlaybackAction(IntEnum):
    NEXT = 1
    PREVIOUS = 2
    PAUSE_TOGGLE = 3
    FAST_FORWARD = 4
    REWIND = 5
    PLAY_INDEX = 6
    SEEK_SECONDS = 7
    SET_VOLUME_PERCENT = 8
    SET_MODE = 9
    SET_OUTPUT_TARGET = 10


class LastfmAction(IntEnum):
    SET_AUTH_URL = 1
    REQUEST_TOKEN = 2
    AUTH = 3
    LOGOUT = 4
    SET_SCROBBLING = 5
    SET_NOW_PLAYING = 6


class BtAction(IntEnum):
    CONNECT_LAST = 1
    PAIR_BEST = 2
    DISCONNECT = 3


@dataclass(slots=True)
class Tlv:
    tlv_type: int
    value: bytes

    @property
    def name(self) -> str:
        return TLV_NAME.get(self.tlv_type, f"0x{self.tlv_type:04x}")


@dataclass(slots=True)
class Frame:
    frame_type: FrameType
    opcode: int
    request_id: int
    payload: bytes
    tlvs: list[Tlv] | None = None


@dataclass(slots=True)
class CompanionCredentials:
    client_id: str
    app_name: str
    secret_hex: str

    @property
    def secret(self) -> bytes:
        return bytes.fromhex(self.secret_hex)


class CompanionApiError(RuntimeError):
    def __init__(
        self, opcode: int, request_id: int, error_code: int, message: str
    ) -> None:
        self.opcode = opcode
        self.request_id = request_id
        self.error_code = error_code
        self.error_message = message
        super().__init__(
            f"Companion API error opcode=0x{opcode:04x} request_id={request_id}: "
            f"{message} ({error_code})"
        )


class StateStore:
    def __init__(self, path: Path) -> None:
        self.path = path

    def load(self) -> dict[str, Any]:
        if not self.path.exists():
            return {"profiles": {}}
        with self.path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        if not isinstance(data, dict):
            return {"profiles": {}}
        if "profiles" not in data or not isinstance(data["profiles"], dict):
            data["profiles"] = {}
        return data

    def save(self, data: dict[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("w", encoding="utf-8") as handle:
            json.dump(data, handle, indent=2, sort_keys=True)
            handle.write("\n")

    def get_credentials(self, profile: str) -> CompanionCredentials | None:
        data = self.load()
        raw = data.get("profiles", {}).get(profile)
        if not isinstance(raw, dict):
            return None
        client_id = raw.get("client_id")
        app_name = raw.get("app_name")
        secret_hex = raw.get("secret_hex")
        if not all(
            isinstance(value, str) and value
            for value in (client_id, app_name, secret_hex)
        ):
            return None
        return CompanionCredentials(
            client_id=client_id, app_name=app_name, secret_hex=secret_hex
        )

    def put_credentials(self, profile: str, credentials: CompanionCredentials) -> None:
        data = self.load()
        data.setdefault("profiles", {})[profile] = {
            "client_id": credentials.client_id,
            "app_name": credentials.app_name,
            "secret_hex": credentials.secret_hex,
        }
        self.save(data)


def write_u16(value: int) -> bytes:
    return struct.pack("<H", value)


def write_u32(value: int) -> bytes:
    return struct.pack("<I", value)


def read_u16(data: bytes) -> int:
    return struct.unpack_from("<H", data)[0]


def read_u32(data: bytes) -> int:
    return struct.unpack_from("<I", data)[0]


def read_i32_from_u32(value: int) -> int:
    return struct.unpack("<i", struct.pack("<I", value & 0xFFFFFFFF))[0]


def tlv_bytes(tlv_type: int, value: bytes) -> bytes:
    return write_u16(tlv_type) + write_u16(len(value)) + value


def tlv_u8(tlv_type: int, value: int) -> bytes:
    return tlv_bytes(tlv_type, bytes([value & 0xFF]))


def tlv_u16(tlv_type: int, value: int) -> bytes:
    return tlv_bytes(tlv_type, write_u16(value))


def tlv_u32(tlv_type: int, value: int) -> bytes:
    return tlv_bytes(tlv_type, write_u32(value))


def tlv_string(tlv_type: int, value: str) -> bytes:
    return tlv_bytes(tlv_type, value.encode("utf-8"))


def parse_tlvs(payload: bytes) -> list[Tlv]:
    tlvs: list[Tlv] = []
    offset = 0
    while offset + TLV_HEADER_LEN <= len(payload):
        tlv_type = read_u16(payload[offset : offset + 2])
        tlv_len = read_u16(payload[offset + 2 : offset + 4])
        offset += TLV_HEADER_LEN
        if offset + tlv_len > len(payload):
            raise ValueError("TLV exceeds payload length")
        tlvs.append(Tlv(tlv_type=tlv_type, value=payload[offset : offset + tlv_len]))
        offset += tlv_len
    if offset != len(payload):
        raise ValueError("Trailing bytes after TLV parse")
    return tlvs


def tlv_value_u8(tlv: Tlv) -> int:
    if len(tlv.value) != 1:
        raise ValueError(f"TLV {tlv.name} is not u8")
    return tlv.value[0]


def tlv_value_u16(tlv: Tlv) -> int:
    if len(tlv.value) != 2:
        raise ValueError(f"TLV {tlv.name} is not u16")
    return read_u16(tlv.value)


def tlv_value_u32(tlv: Tlv) -> int:
    if len(tlv.value) != 4:
        raise ValueError(f"TLV {tlv.name} is not u32")
    return read_u32(tlv.value)


def tlv_value_bool(tlv: Tlv) -> bool:
    return bool(tlv_value_u8(tlv))


def tlv_value_string(tlv: Tlv) -> str:
    return tlv.value.decode("utf-8", errors="replace")


def tlv_first(tlvs: list[Tlv], tlv_type: int) -> Tlv | None:
    for tlv in tlvs:
        if tlv.tlv_type == tlv_type:
            return tlv
    return None


def decode_slot(value: int) -> int | None:
    return None if value == 0 else value - 1


def decode_ip_address(value: int) -> str:
    try:
        return socket.inet_ntoa(struct.pack("<I", value))
    except OSError:
        return f"0x{value:08x}"


def ensure_track_index(value: int) -> int | None:
    return None if value == 0xFFFFFFFF else value


def button_sequence_names(sequence: bytes) -> list[str]:
    return [BUTTON_ID_TO_NAME.get(value, f"unknown:{value}") for value in sequence]


def json_dump(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=False))


def frame_type_name(frame_type: FrameType) -> str:
    return frame_type.name.lower()


def opcode_name(opcode: int) -> str:
    try:
        return Opcode(opcode).name.lower()
    except ValueError:
        return f"0x{opcode:04x}"


def discovered_devices_with_advertisements(
    discovered: Any,
) -> list[tuple[BLEDevice, AdvertisementData | None]]:
    if isinstance(discovered, dict):
        entries: list[tuple[BLEDevice, AdvertisementData | None]] = []
        for value in discovered.values():
            if isinstance(value, tuple) and len(value) == 2:
                device, advertisement = value
                entries.append((device, advertisement))
            else:
                entries.append((value, None))
        return entries
    return [(device, None) for device in discovered]


def advertised_service_uuids(
    device: BLEDevice, advertisement: AdvertisementData | None
) -> list[str]:
    if advertisement is not None:
        return [uuid_value.lower() for uuid_value in advertisement.service_uuids or []]

    metadata = getattr(device, "metadata", None)
    if isinstance(metadata, dict):
        return [uuid_value.lower() for uuid_value in metadata.get("uuids", []) or []]

    return []


class CompanionBleClient:
    def __init__(
        self,
        *,
        address: str | None,
        name: str | None,
        scan_timeout: float,
        timeout: float,
        verbose: bool,
    ) -> None:
        self.address = address
        self.name = name
        self.scan_timeout = scan_timeout
        self.timeout = timeout
        self.verbose = verbose
        self.device: BLEDevice | None = None
        self.client: BleakClient | None = None
        self.write_char: BleakGATTCharacteristic | None = None
        self.notify_char: BleakGATTCharacteristic | None = None
        self.max_chunk_size = DEFAULT_CHUNK_SIZE
        self._loop: asyncio.AbstractEventLoop | None = None
        self._next_request_id = 1
        self._pending: dict[int, asyncio.Future[Frame]] = {}
        self._rx_buffer = bytearray()
        self._event_queue: asyncio.Queue[Frame] = asyncio.Queue()

    async def __aenter__(self) -> CompanionBleClient:
        await self.connect()
        return self

    async def __aexit__(self, exc_type, exc, tb) -> None:
        await self.disconnect()

    async def connect(self) -> None:
        self._loop = asyncio.get_running_loop()
        self.device = await self._resolve_device()
        self.client = BleakClient(
            self.device, timeout=self.timeout, disconnected_callback=self._on_disconnect
        )
        await self.client.connect()

        self.write_char = self.client.services.get_characteristic(WRITE_UUID)
        self.notify_char = self.client.services.get_characteristic(NOTIFY_UUID)
        if self.write_char is None or self.notify_char is None:
            raise RuntimeError("Failed to resolve companion API GATT characteristics")

        chunk_size = getattr(self.write_char, "max_write_without_response_size", None)
        if isinstance(chunk_size, int) and chunk_size > 0:
            self.max_chunk_size = chunk_size

        await self.client.start_notify(self.notify_char, self._notification_callback)

        if self.verbose:
            print(
                f"Connected to {self.device.name or 'unknown'} @ {self.device.address}, "
                f"write chunk size {self.max_chunk_size}"
            )

    async def disconnect(self) -> None:
        if self.client is not None:
            try:
                if self.notify_char is not None:
                    await self.client.stop_notify(self.notify_char)
            except Exception:
                pass
            await self.client.disconnect()
        self._fail_pending(RuntimeError("BLE disconnected"))
        self.client = None
        self.write_char = None
        self.notify_char = None

    async def _resolve_device(self) -> BLEDevice:
        discovered = await BleakScanner.discover(
            timeout=self.scan_timeout, return_adv=True
        )
        filtered: list[BLEDevice] = []

        for device, advertisement in discovered_devices_with_advertisements(discovered):
            uuids = advertised_service_uuids(device, advertisement)
            matches_service = SERVICE_UUID.lower() in uuids
            matches_name = bool(device.name and device.name == DEFAULT_DEVICE_NAME)
            if self.address and device.address.lower() != self.address.lower():
                continue
            if self.name and (device.name or "") != self.name:
                continue
            if self.address or self.name:
                filtered.append(device)
            elif matches_service or matches_name:
                filtered.append(device)

        if not filtered:
            if self.address or self.name:
                target = self.address or self.name or "<unknown>"
                raise RuntimeError(f"No BLE device matched {target!r}")
            raise RuntimeError(
                "No Jukeboy companion BLE device found. Pass --address or --name if it is not advertising as ESP_SPP_SERVER."
            )

        if len(filtered) > 1 and not (self.address or self.name):
            print(
                "Multiple candidate devices found; using the first. Pass --address to select explicitly."
            )
            for device in filtered:
                print(f"  {device.address:>18}  {device.name or '<unnamed>'}")

        return filtered[0]

    def _on_disconnect(self, _client: BleakClient) -> None:
        self._fail_pending(RuntimeError("BLE disconnected"))

    def _notification_callback(
        self, _characteristic: BleakGATTCharacteristic, data: bytearray
    ) -> None:
        if self._loop is None:
            return
        self._loop.call_soon_threadsafe(self._process_notification, bytes(data))

    def _process_notification(self, data: bytes) -> None:
        self._rx_buffer.extend(data)

        while len(self._rx_buffer) >= FRAME_HEADER_LEN:
            if self._rx_buffer[0:2] != MAGIC:
                del self._rx_buffer[0]
                continue

            version = self._rx_buffer[2]
            if version != VERSION:
                del self._rx_buffer[:FRAME_HEADER_LEN]
                continue

            payload_len = read_u16(self._rx_buffer[10:12])
            frame_len = FRAME_HEADER_LEN + payload_len
            if frame_len > FRAME_MAX_LEN:
                del self._rx_buffer[:FRAME_HEADER_LEN]
                continue
            if len(self._rx_buffer) < frame_len:
                return

            raw = bytes(self._rx_buffer[:frame_len])
            del self._rx_buffer[:frame_len]
            frame = self._decode_frame(raw)

            if (
                frame.frame_type in (FrameType.RESPONSE, FrameType.ERROR)
                and frame.request_id in self._pending
            ):
                future = self._pending.pop(frame.request_id)
                if future.done():
                    continue
                if frame.frame_type is FrameType.ERROR:
                    error_code_tlv = tlv_first(frame.tlvs or [], TlvType.ERROR_CODE)
                    error_message_tlv = tlv_first(
                        frame.tlvs or [], TlvType.ERROR_MESSAGE
                    )
                    error_code = tlv_value_u16(error_code_tlv) if error_code_tlv else -1
                    error_message = (
                        tlv_value_string(error_message_tlv)
                        if error_message_tlv
                        else "error"
                    )
                    future.set_exception(
                        CompanionApiError(
                            frame.opcode, frame.request_id, error_code, error_message
                        )
                    )
                else:
                    future.set_result(frame)
                continue

            self._event_queue.put_nowait(frame)

    def _decode_frame(self, raw: bytes) -> Frame:
        frame_type = FrameType(raw[3])
        opcode = read_u16(raw[4:6])
        request_id = read_u32(raw[6:10])
        payload_len = read_u16(raw[10:12])
        payload = raw[12 : 12 + payload_len]
        tlvs: list[Tlv] | None = None
        if frame_type is not FrameType.RESPONSE or opcode != Opcode.PING:
            try:
                tlvs = parse_tlvs(payload)
            except ValueError:
                tlvs = None
        return Frame(
            frame_type=frame_type,
            opcode=opcode,
            request_id=request_id,
            payload=payload,
            tlvs=tlvs,
        )

    def _fail_pending(self, exc: Exception) -> None:
        pending = list(self._pending.values())
        self._pending.clear()
        for future in pending:
            if not future.done():
                future.set_exception(exc)

    async def _write(self, data: bytes) -> None:
        if self.client is None or self.write_char is None:
            raise RuntimeError("BLE client is not connected")

        for offset in range(0, len(data), self.max_chunk_size):
            chunk = data[offset : offset + self.max_chunk_size]
            await self.client.write_gatt_char(self.write_char, chunk, response=False)

    def _next_request(self) -> int:
        request_id = self._next_request_id
        self._next_request_id += 1
        if self._next_request_id == 0:
            self._next_request_id = 1
        return request_id

    async def request(
        self,
        opcode: int,
        *,
        tlvs: list[bytes] | None = None,
        payload: bytes | None = None,
        timeout: float | None = None,
    ) -> Frame:
        if payload is None:
            payload = b"".join(tlvs or [])
        request_id = self._next_request()
        frame = (
            MAGIC
            + bytes([VERSION, FrameType.REQUEST])
            + write_u16(opcode)
            + write_u32(request_id)
            + write_u16(len(payload))
            + payload
        )

        future: asyncio.Future[Frame] = asyncio.get_running_loop().create_future()
        self._pending[request_id] = future
        try:
            await self._write(frame)
        except Exception:
            self._pending.pop(request_id, None)
            raise
        return await asyncio.wait_for(future, timeout=timeout or self.timeout)

    async def next_event(self, timeout: float | None = None) -> Frame:
        if timeout is None:
            return await self._event_queue.get()
        return await asyncio.wait_for(self._event_queue.get(), timeout=timeout)

    async def hello(self) -> dict[str, Any]:
        return decode_hello(await self.request(Opcode.HELLO))

    async def capabilities(self) -> dict[str, Any]:
        return decode_capabilities(await self.request(Opcode.CAPABILITIES))

    async def ping(self, text: str) -> dict[str, Any]:
        frame = await self.request(Opcode.PING, payload=text.encode("utf-8"))
        return {
            "opcode": opcode_name(frame.opcode),
            "request_id": frame.request_id,
            "echo": frame.payload.decode("utf-8", errors="replace"),
            "echo_hex": frame.payload.hex(),
        }

    async def pair_begin(
        self, client_id: str, app_name: str, secret: bytes, sequence: list[int]
    ) -> dict[str, Any]:
        return decode_pair_status(
            await self.request(
                Opcode.PAIR_BEGIN,
                tlvs=[
                    tlv_string(TlvType.CLIENT_ID, client_id),
                    tlv_string(TlvType.APP_NAME, app_name),
                    tlv_bytes(TlvType.SHARED_SECRET, secret),
                    tlv_bytes(TlvType.BUTTON_SEQUENCE, bytes(sequence)),
                ],
            )
        )

    async def pair_status(self) -> dict[str, Any]:
        return decode_pair_status(await self.request(Opcode.PAIR_STATUS))

    async def pair_cancel(self) -> dict[str, Any]:
        return decode_pair_status(await self.request(Opcode.PAIR_CANCEL))

    async def auth_challenge(self, client_id: str) -> dict[str, Any]:
        return decode_auth_challenge(
            await self.request(
                Opcode.AUTH_CHALLENGE, tlvs=[tlv_string(TlvType.CLIENT_ID, client_id)]
            )
        )

    async def auth_proof(
        self, client_id: str, secret: bytes, nonce: bytes
    ) -> dict[str, Any]:
        proof = hmac.new(secret, nonce, hashlib.sha256).digest()
        return decode_auth_status(
            await self.request(
                Opcode.AUTH_PROOF,
                tlvs=[
                    tlv_string(TlvType.CLIENT_ID, client_id),
                    tlv_bytes(TlvType.AUTH_HMAC, proof),
                ],
            )
        )

    async def trusted_list(self) -> dict[str, Any]:
        return decode_trusted_list(await self.request(Opcode.TRUSTED_LIST))

    async def trusted_revoke(self, client_id: str) -> dict[str, Any]:
        return decode_trusted_list(
            await self.request(
                Opcode.TRUSTED_REVOKE, tlvs=[tlv_string(TlvType.CLIENT_ID, client_id)]
            )
        )

    async def snapshot(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.SNAPSHOT))

    async def playback_status(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.PLAYBACK_STATUS))

    async def playback_control(
        self, action: PlaybackAction, value: int | None = None
    ) -> dict[str, Any]:
        tlvs = [tlv_u8(TlvType.ACTION, int(action))]
        if value is not None:
            tlvs.append(tlv_u32(TlvType.VALUE, value))
        return decode_snapshot(await self.request(Opcode.PLAYBACK_CONTROL, tlvs=tlvs))

    async def library_album(self) -> dict[str, Any]:
        return decode_album(await self.request(Opcode.LIBRARY_ALBUM))

    async def library_track_page(self, offset: int, count: int) -> dict[str, Any]:
        return decode_track_page(
            await self.request(
                Opcode.LIBRARY_TRACK_PAGE,
                tlvs=[tlv_u32(TlvType.OFFSET, offset), tlv_u32(TlvType.COUNT, count)],
            )
        )

    async def wifi_status(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.WIFI_STATUS))

    async def wifi_scan_start(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.WIFI_SCAN_START))

    async def wifi_scan_results(self, offset: int, count: int) -> dict[str, Any]:
        return decode_wifi_scan_results(
            await self.request(
                Opcode.WIFI_SCAN_RESULTS,
                tlvs=[tlv_u32(TlvType.OFFSET, offset), tlv_u32(TlvType.COUNT, count)],
            )
        )

    async def wifi_connect(self, ssid: str, password: str) -> dict[str, Any]:
        tlvs = [
            tlv_string(TlvType.WIFI_SSID, ssid),
            tlv_string(TlvType.WIFI_PASSWORD, password),
        ]
        return decode_snapshot(await self.request(Opcode.WIFI_CONNECT, tlvs=tlvs))

    async def wifi_connect_slot(self, slot: int) -> dict[str, Any]:
        return decode_snapshot(
            await self.request(
                Opcode.WIFI_CONNECT_SLOT, tlvs=[tlv_u32(TlvType.WIFI_SLOT, slot)]
            )
        )

    async def wifi_disconnect(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.WIFI_DISCONNECT))

    async def wifi_autoreconnect(self, enabled: bool) -> dict[str, Any]:
        return decode_snapshot(
            await self.request(
                Opcode.WIFI_AUTORECONNECT,
                tlvs=[tlv_u32(TlvType.VALUE, 1 if enabled else 0)],
            )
        )

    async def lastfm_status(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.LASTFM_STATUS))

    async def lastfm_control(
        self,
        action: LastfmAction,
        *,
        auth_url: str | None = None,
        username: str | None = None,
        password: str | None = None,
        enabled: bool | None = None,
    ) -> dict[str, Any]:
        tlvs = [tlv_u8(TlvType.ACTION, int(action))]
        if auth_url is not None:
            tlvs.append(tlv_string(TlvType.LASTFM_AUTH_URL, auth_url))
        if username is not None:
            tlvs.append(tlv_string(TlvType.LASTFM_USERNAME, username))
        if password is not None:
            tlvs.append(tlv_string(TlvType.WIFI_PASSWORD, password))
        if enabled is not None:
            tlvs.append(tlv_u32(TlvType.VALUE, 1 if enabled else 0))
        return decode_snapshot(await self.request(Opcode.LASTFM_CONTROL, tlvs=tlvs))

    async def history_summary(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.HISTORY_SUMMARY))

    async def history_album_page(self, offset: int, count: int) -> dict[str, Any]:
        return decode_history_album_page(
            await self.request(
                Opcode.HISTORY_ALBUM_PAGE,
                tlvs=[tlv_u32(TlvType.OFFSET, offset), tlv_u32(TlvType.COUNT, count)],
            )
        )

    async def bt_audio_status(self) -> dict[str, Any]:
        return decode_snapshot(await self.request(Opcode.BT_AUDIO_STATUS))

    async def bt_audio_control(self, action: BtAction) -> dict[str, Any]:
        return decode_snapshot(
            await self.request(
                Opcode.BT_AUDIO_CONTROL, tlvs=[tlv_u8(TlvType.ACTION, int(action))]
            )
        )


def decode_auth_status(frame: Frame) -> dict[str, Any]:
    tlvs = frame.tlvs or []
    result: dict[str, Any] = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "authenticated": False,
        "client_id": "",
        "trusted_client_count": 0,
    }
    for tlv in tlvs:
        if tlv.tlv_type == TlvType.AUTHENTICATED:
            result["authenticated"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.CLIENT_ID:
            result["client_id"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.TRUSTED_COUNT:
            result["trusted_client_count"] = tlv_value_u8(tlv)
    return result


def decode_hello(frame: Frame) -> dict[str, Any]:
    result = decode_auth_status(frame)
    result["app_name"] = ""
    result["protocol_version"] = None
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.APP_NAME:
            result["app_name"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.PROTOCOL_VERSION:
            result["protocol_version"] = tlv_value_u16(tlv)
    return result


def decode_pair_status(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "pairing_pending": False,
        "pairing_progress": 0,
        "pairing_required": 0,
        "pending_client_id": "",
        "pending_app_name": "",
        "button_sequence": [],
    }
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.PAIRING_PENDING:
            result["pairing_pending"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_PROGRESS:
            result["pairing_progress"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_REQUIRED:
            result["pairing_required"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.CLIENT_ID:
            result["pending_client_id"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.APP_NAME:
            result["pending_app_name"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.BUTTON_SEQUENCE:
            result["button_sequence"] = button_sequence_names(tlv.value)
    return result


def decode_capabilities(frame: Frame) -> dict[str, Any]:
    result = decode_auth_status(frame)
    result.update(
        {
            "protocol_version": None,
            "max_frame": None,
            "mtu": None,
            "max_payload": None,
            "feature_bits": None,
            "pairing": {
                "pairing_pending": False,
                "pairing_progress": 0,
                "pairing_required": 0,
                "pending_client_id": "",
                "pending_app_name": "",
                "button_sequence": [],
            },
        }
    )

    client_id_seen = 0
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.PROTOCOL_VERSION:
            result["protocol_version"] = tlv_value_u16(tlv)
        elif tlv.tlv_type == TlvType.MAX_FRAME:
            result["max_frame"] = tlv_value_u16(tlv)
        elif tlv.tlv_type == TlvType.MTU:
            result["mtu"] = tlv_value_u16(tlv)
        elif tlv.tlv_type == TlvType.MAX_PAYLOAD:
            result["max_payload"] = tlv_value_u16(tlv)
        elif tlv.tlv_type == TlvType.FEATURE_BITS:
            result["feature_bits"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.CLIENT_ID:
            client_id_seen += 1
            if client_id_seen == 1:
                result["client_id"] = tlv_value_string(tlv)
            else:
                result["pairing"]["pending_client_id"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_PENDING:
            result["pairing"]["pairing_pending"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_PROGRESS:
            result["pairing"]["pairing_progress"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_REQUIRED:
            result["pairing"]["pairing_required"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.APP_NAME:
            result["pairing"]["pending_app_name"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.BUTTON_SEQUENCE:
            result["pairing"]["button_sequence"] = button_sequence_names(tlv.value)

    return result


def decode_auth_challenge(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "client_id": "",
        "nonce_hex": "",
    }
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.CLIENT_ID:
            result["client_id"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.AUTH_NONCE:
            result["nonce_hex"] = tlv.value.hex()
    return result


def decode_trusted_list(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "trusted_count": 0,
        "clients": [],
    }
    current: dict[str, Any] | None = None
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.TRUSTED_COUNT:
            result["trusted_count"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.CLIENT_ID:
            if current is not None:
                result["clients"].append(current)
            current = {
                "client_id": tlv_value_string(tlv),
                "app_name": "",
                "created_at": 0,
            }
        elif tlv.tlv_type == TlvType.APP_NAME and current is not None:
            current["app_name"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.CREATED_AT and current is not None:
            current["created_at"] = tlv_value_u32(tlv)
    if current is not None:
        result["clients"].append(current)
    return result


def decode_snapshot(frame: Frame) -> dict[str, Any]:
    result: dict[str, Any] = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "generation": None,
        "uptime_ms": None,
        "auth": {
            "authenticated": False,
            "client_id": "",
            "trusted_client_count": 0,
        },
        "pairing": {
            "pairing_pending": False,
            "pairing_progress": 0,
            "pairing_required": 0,
            "pending_client_id": "",
            "pending_app_name": "",
            "button_sequence": [],
        },
        "playback": {
            "playing": False,
            "paused": False,
            "cartridge_checksum": None,
            "track_index": None,
            "track_count": None,
            "position_sec": None,
            "started_at": None,
            "duration_sec": None,
            "volume_percent": None,
            "playback_mode": None,
            "track_title": "",
            "track_file": "",
            "output_target": None,
        },
        "cartridge": {
            "status": None,
            "mounted": False,
            "checksum": None,
            "metadata_version": None,
            "track_count": None,
        },
        "wifi": {
            "state": None,
            "internet": False,
            "autoreconnect": False,
            "active_slot": None,
            "preferred_slot": None,
            "ip": None,
        },
        "lastfm": {
            "has_auth_url": False,
            "has_token": False,
            "has_session": False,
            "busy": False,
            "scrobbling": False,
            "now_playing": False,
            "pending_commands": 0,
            "pending_scrobbles": 0,
            "successful": 0,
            "failed": 0,
            "auth_url": "",
            "username": "",
        },
        "history": {
            "album_count": 0,
            "track_count": 0,
        },
        "bluetooth": {
            "a2dp_connected": False,
            "bonded_count": 0,
        },
    }

    client_id_seen = 0
    cartridge_checksum_seen = 0
    track_count_seen = 0
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.GENERATION:
            result["generation"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.UPTIME_MS:
            result["uptime_ms"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.AUTHENTICATED:
            result["auth"]["authenticated"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.CLIENT_ID:
            client_id_seen += 1
            if client_id_seen == 1:
                result["auth"]["client_id"] = tlv_value_string(tlv)
            else:
                result["pairing"]["pending_client_id"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.TRUSTED_COUNT:
            result["auth"]["trusted_client_count"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_PENDING:
            result["pairing"]["pairing_pending"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_PROGRESS:
            result["pairing"]["pairing_progress"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.PAIRING_REQUIRED:
            result["pairing"]["pairing_required"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.APP_NAME:
            result["pairing"]["pending_app_name"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.BUTTON_SEQUENCE:
            result["pairing"]["button_sequence"] = button_sequence_names(tlv.value)
        elif tlv.tlv_type == TlvType.PLAYING:
            result["playback"]["playing"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.PAUSED:
            result["playback"]["paused"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.CARTRIDGE_CHECKSUM:
            cartridge_checksum_seen += 1
            if cartridge_checksum_seen == 1:
                result["playback"]["cartridge_checksum"] = tlv_value_u32(tlv)
            else:
                result["cartridge"]["checksum"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.TRACK_INDEX:
            result["playback"]["track_index"] = ensure_track_index(tlv_value_u32(tlv))
        elif tlv.tlv_type == TlvType.TRACK_COUNT:
            track_count_seen += 1
            if track_count_seen == 1:
                result["playback"]["track_count"] = tlv_value_u32(tlv)
            else:
                result["cartridge"]["track_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.POSITION_SEC:
            result["playback"]["position_sec"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.STARTED_AT:
            result["playback"]["started_at"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.DURATION_SEC:
            result["playback"]["duration_sec"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.VOLUME_PERCENT:
            result["playback"]["volume_percent"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.PLAYBACK_MODE:
            mode_value = tlv_value_u8(tlv)
            result["playback"]["playback_mode"] = PLAYBACK_MODE_FROM_ID.get(
                mode_value, mode_value
            )
        elif tlv.tlv_type == TlvType.TRACK_TITLE:
            result["playback"]["track_title"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.TRACK_FILE:
            result["playback"]["track_file"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.OUTPUT_TARGET:
            target_value = tlv_value_u8(tlv)
            result["playback"]["output_target"] = OUTPUT_TARGET_FROM_ID.get(
                target_value, target_value
            )
        elif tlv.tlv_type == TlvType.CARTRIDGE_STATUS:
            status_value = tlv_value_u8(tlv)
            result["cartridge"]["status"] = CARTRIDGE_STATUS_FROM_ID.get(
                status_value, status_value
            )
        elif tlv.tlv_type == TlvType.CARTRIDGE_MOUNTED:
            result["cartridge"]["mounted"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.METADATA_VERSION:
            result["cartridge"]["metadata_version"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.WIFI_STATE:
            state_value = tlv_value_u8(tlv)
            result["wifi"]["state"] = WIFI_STATE_FROM_ID.get(state_value, state_value)
        elif tlv.tlv_type == TlvType.WIFI_INTERNET:
            result["wifi"]["internet"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.WIFI_AUTORECONNECT:
            result["wifi"]["autoreconnect"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.WIFI_ACTIVE_SLOT:
            result["wifi"]["active_slot"] = decode_slot(tlv_value_u8(tlv))
        elif tlv.tlv_type == TlvType.WIFI_PREFERRED_SLOT:
            result["wifi"]["preferred_slot"] = decode_slot(tlv_value_u8(tlv))
        elif tlv.tlv_type == TlvType.WIFI_IP:
            result["wifi"]["ip"] = decode_ip_address(tlv_value_u32(tlv))
        elif tlv.tlv_type == TlvType.LASTFM_HAS_AUTH_URL:
            result["lastfm"]["has_auth_url"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_HAS_TOKEN:
            result["lastfm"]["has_token"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_HAS_SESSION:
            result["lastfm"]["has_session"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_BUSY:
            result["lastfm"]["busy"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_SCROBBLING:
            result["lastfm"]["scrobbling"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_NOW_PLAYING:
            result["lastfm"]["now_playing"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_PENDING_COMMANDS:
            result["lastfm"]["pending_commands"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_PENDING_SCROBBLES:
            result["lastfm"]["pending_scrobbles"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_SUCCESSFUL:
            result["lastfm"]["successful"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_FAILED:
            result["lastfm"]["failed"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_AUTH_URL:
            result["lastfm"]["auth_url"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.LASTFM_USERNAME:
            result["lastfm"]["username"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.HISTORY_ALBUM_COUNT:
            result["history"]["album_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.HISTORY_TRACK_COUNT:
            result["history"]["track_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.BT_A2DP_CONNECTED:
            result["bluetooth"]["a2dp_connected"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.BT_BONDED_COUNT:
            result["bluetooth"]["bonded_count"] = tlv_value_u32(tlv)
    return result


def decode_album(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "cartridge": {
            "status": None,
            "mounted": False,
            "checksum": None,
            "metadata_version": None,
            "track_count": None,
        },
        "album": {
            "name": "",
            "artist": "",
            "description": "",
            "year": None,
            "duration_sec": None,
            "genre": "",
        },
    }
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.CARTRIDGE_STATUS:
            value = tlv_value_u8(tlv)
            result["cartridge"]["status"] = CARTRIDGE_STATUS_FROM_ID.get(value, value)
        elif tlv.tlv_type == TlvType.CARTRIDGE_MOUNTED:
            result["cartridge"]["mounted"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.CARTRIDGE_CHECKSUM:
            result["cartridge"]["checksum"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.METADATA_VERSION:
            result["cartridge"]["metadata_version"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.TRACK_COUNT:
            result["cartridge"]["track_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_NAME:
            result["album"]["name"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_ARTIST:
            result["album"]["artist"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_DESCRIPTION:
            result["album"]["description"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_YEAR:
            result["album"]["year"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_DURATION:
            result["album"]["duration_sec"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_GENRE:
            result["album"]["genre"] = tlv_value_string(tlv)
    return result


def decode_track_page(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "offset": 0,
        "track_count": 0,
        "returned_count": 0,
        "tracks": [],
    }
    current: dict[str, Any] | None = None
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.OFFSET:
            result["offset"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.TRACK_COUNT:
            result["track_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.TRACK_INDEX:
            if current is not None:
                result["tracks"].append(current)
            current = {
                "track_index": tlv_value_u32(tlv),
                "title": "",
                "artist": "",
                "duration_sec": None,
                "file_num": None,
            }
        elif tlv.tlv_type == TlvType.TRACK_TITLE and current is not None:
            current["title"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.TRACK_ARTIST and current is not None:
            current["artist"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.DURATION_SEC and current is not None:
            current["duration_sec"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.TRACK_FILE and current is not None:
            current["file_num"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.RETURNED_COUNT:
            result["returned_count"] = tlv_value_u32(tlv)
    if current is not None:
        result["tracks"].append(current)
    return result


def decode_wifi_scan_results(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "offset": 0,
        "total_count": 0,
        "returned_count": 0,
        "results": [],
    }
    current: dict[str, Any] | None = None
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.OFFSET:
            result["offset"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.COUNT:
            result["total_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.WIFI_SSID:
            if current is not None:
                result["results"].append(current)
            current = {
                "ssid": tlv_value_string(tlv),
                "rssi": None,
                "channel": None,
                "authmode": None,
            }
        elif tlv.tlv_type == TlvType.WIFI_RSSI and current is not None:
            current["rssi"] = read_i32_from_u32(tlv_value_u32(tlv))
        elif tlv.tlv_type == TlvType.WIFI_CHANNEL and current is not None:
            current["channel"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.WIFI_AUTHMODE and current is not None:
            current["authmode"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.RETURNED_COUNT:
            result["returned_count"] = tlv_value_u32(tlv)
    if current is not None:
        result["results"].append(current)
    return result


def decode_history_album_page(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "request_id": frame.request_id,
        "offset": 0,
        "album_count": 0,
        "returned_count": 0,
        "albums": [],
    }
    current: dict[str, Any] | None = None
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.OFFSET:
            result["offset"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.HISTORY_ALBUM_COUNT:
            result["album_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.CARTRIDGE_CHECKSUM:
            if current is not None:
                result["albums"].append(current)
            current = {
                "checksum": tlv_value_u32(tlv),
                "track_count": None,
                "first_seen_sequence": None,
                "last_seen_sequence": None,
                "album_name": "",
                "album_artist": "",
            }
        elif tlv.tlv_type == TlvType.TRACK_COUNT and current is not None:
            current["track_count"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.HISTORY_FIRST_SEEN and current is not None:
            current["first_seen_sequence"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.HISTORY_LAST_SEEN and current is not None:
            current["last_seen_sequence"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_NAME and current is not None:
            current["album_name"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.ALBUM_ARTIST and current is not None:
            current["album_artist"] = tlv_value_string(tlv)
        elif tlv.tlv_type == TlvType.RETURNED_COUNT:
            result["returned_count"] = tlv_value_u32(tlv)
    if current is not None:
        result["albums"].append(current)
    return result


def decode_heartbeat(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "frame_type": frame_type_name(frame.frame_type),
        "request_id": frame.request_id,
        "uptime_ms": None,
        "generation": None,
        "authenticated": False,
        "queue_free": None,
        "rx_frames": None,
        "tx_frames": None,
        "rx_errors": None,
    }
    for tlv in frame.tlvs or []:
        if tlv.tlv_type == TlvType.UPTIME_MS:
            result["uptime_ms"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.GENERATION:
            result["generation"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.AUTHENTICATED:
            result["authenticated"] = tlv_value_bool(tlv)
        elif tlv.tlv_type == TlvType.QUEUE_FREE:
            result["queue_free"] = tlv_value_u8(tlv)
        elif tlv.tlv_type == TlvType.RX_FRAMES:
            result["rx_frames"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.TX_FRAMES:
            result["tx_frames"] = tlv_value_u32(tlv)
        elif tlv.tlv_type == TlvType.RX_ERRORS:
            result["rx_errors"] = tlv_value_u32(tlv)
    return result


def decode_generic_tlvs(frame: Frame) -> dict[str, Any]:
    result = {
        "opcode": opcode_name(frame.opcode),
        "frame_type": frame_type_name(frame.frame_type),
        "request_id": frame.request_id,
        "tlvs": [],
    }
    for tlv in frame.tlvs or []:
        item: dict[str, Any] = {"type": tlv.name, "type_id": tlv.tlv_type}
        if len(tlv.value) == 1:
            item["value"] = tlv.value[0]
        elif len(tlv.value) == 2:
            item["value"] = read_u16(tlv.value)
        elif len(tlv.value) == 4:
            item["value"] = read_u32(tlv.value)
        else:
            try:
                item["value"] = tlv.value.decode("utf-8")
            except UnicodeDecodeError:
                item["value_hex"] = tlv.value.hex()
        result["tlvs"].append(item)
    return result


def decode_frame(frame: Frame) -> dict[str, Any]:
    if frame.frame_type is FrameType.HEARTBEAT:
        return decode_heartbeat(frame)
    if frame.frame_type is FrameType.EVENT and frame.opcode == Opcode.PAIR_STATUS:
        decoded = decode_pair_status(frame)
        decoded["frame_type"] = frame_type_name(frame.frame_type)
        return decoded
    if frame.frame_type is FrameType.EVENT:
        decoded = decode_generic_tlvs(frame)
        decoded["frame_type"] = frame_type_name(frame.frame_type)
        return decoded
    return decode_generic_tlvs(frame)


async def ensure_authenticated(
    client: CompanionBleClient,
    state_store: StateStore,
    profile: str,
    args: argparse.Namespace,
) -> dict[str, Any]:
    capabilities = await client.capabilities()
    if capabilities.get("authenticated"):
        return capabilities

    credentials = resolve_credentials(args, state_store, profile)
    if credentials is None:
        raise SystemExit(
            "This command requires auth, but no credentials were found. "
            "Pair first with `pair begin` or pass --client-id and --secret-hex."
        )

    challenge = await client.auth_challenge(credentials.client_id)
    nonce_hex = challenge.get("nonce_hex")
    if not isinstance(nonce_hex, str) or len(nonce_hex) != AUTH_NONCE_LEN * 2:
        raise RuntimeError("Auth challenge returned an invalid nonce")
    return await client.auth_proof(
        credentials.client_id, credentials.secret, bytes.fromhex(nonce_hex)
    )


def resolve_profile(args: argparse.Namespace) -> str:
    if args.profile:
        return args.profile
    if args.address:
        return args.address.lower()
    if args.name:
        return args.name
    return "default"


def resolve_credentials(
    args: argparse.Namespace,
    state_store: StateStore,
    profile: str,
) -> CompanionCredentials | None:
    if args.client_id and args.secret_hex:
        app_name = args.app_name or DEFAULT_DEVICE_NAME.lower()
        return CompanionCredentials(
            client_id=args.client_id, app_name=app_name, secret_hex=args.secret_hex
        )
    return state_store.get_credentials(profile)


def generate_pairing_credentials(args: argparse.Namespace) -> CompanionCredentials:
    client_id = args.client_id or str(uuid.uuid4())
    app_name = args.app_name or "jukeboy-companion-test"
    secret_hex = args.secret_hex or secrets.token_hex(PAIR_SECRET_LEN)
    return CompanionCredentials(
        client_id=client_id, app_name=app_name, secret_hex=secret_hex
    )


def parse_button_sequence(value: str | None) -> list[int]:
    if not value:
        return [secrets.randbelow(len(BUTTON_NAME_TO_ID)) for _ in range(4)]
    names = [part.strip().lower() for part in value.split(",") if part.strip()]
    if len(names) != 4:
        raise SystemExit(
            "Button sequence must contain exactly four comma-separated button names"
        )
    try:
        return [BUTTON_NAME_TO_ID[name] for name in names]
    except KeyError as exc:
        raise SystemExit(f"Unknown button in sequence: {exc.args[0]}") from exc


async def print_scan(timeout: float) -> None:
    discovered = await BleakScanner.discover(timeout=timeout, return_adv=True)
    results = []
    for device, advertisement in discovered_devices_with_advertisements(discovered):
        uuids = sorted(advertised_service_uuids(device, advertisement))
        results.append(
            {
                "address": device.address,
                "name": device.name or "",
                "service_match": SERVICE_UUID.lower()
                in [uuid_value.lower() for uuid_value in uuids],
                "uuids": uuids,
            }
        )
    json_dump(results)


def command_requires_auth(args: argparse.Namespace) -> bool:
    if args.command in {"scan", "hello", "capabilities", "ping", "auth", "watch"}:
        return False
    if args.command == "pair":
        return False
    return True


def command_should_auto_auth(args: argparse.Namespace) -> bool:
    return command_requires_auth(args) and not args.no_auto_auth


async def handle_pair_command(
    client: CompanionBleClient,
    args: argparse.Namespace,
    state_store: StateStore,
    profile: str,
) -> Any:
    if args.pair_command == "begin":
        credentials = generate_pairing_credentials(args)
        sequence = parse_button_sequence(args.sequence)
        response = await client.pair_begin(
            credentials.client_id,
            credentials.app_name,
            credentials.secret,
            sequence,
        )
        state_store.put_credentials(profile, credentials)

        result = {
            "credentials_saved_to": str(state_store.path),
            "profile": profile,
            "client_id": credentials.client_id,
            "app_name": credentials.app_name,
            "secret_hex": credentials.secret_hex,
            "button_sequence": [BUTTON_ID_TO_NAME[value] for value in sequence],
            "pair_status": response,
        }

        if args.wait:
            deadline = asyncio.get_running_loop().time() + args.wait_timeout
            while True:
                capabilities = await client.capabilities()
                if capabilities.get("authenticated"):
                    result["authenticated"] = True
                    result["capabilities"] = capabilities
                    break
                if asyncio.get_running_loop().time() >= deadline:
                    result["authenticated"] = False
                    result["timeout"] = True
                    break
                try:
                    event = await client.next_event(timeout=1.0)
                except asyncio.TimeoutError:
                    continue
                result["last_event"] = decode_frame(event)
        return result

    if args.pair_command == "status":
        return await client.pair_status()

    if args.pair_command == "cancel":
        return await client.pair_cancel()

    raise AssertionError(f"unexpected pair command {args.pair_command!r}")


async def handle_trusted_command(
    client: CompanionBleClient, args: argparse.Namespace
) -> Any:
    if args.trusted_command == "list":
        return await client.trusted_list()
    if args.trusted_command == "revoke":
        return await client.trusted_revoke(args.client_id_to_revoke)
    raise AssertionError(f"unexpected trusted command {args.trusted_command!r}")


async def handle_playback_command(
    client: CompanionBleClient, args: argparse.Namespace
) -> Any:
    if args.playback_command == "status":
        return await client.playback_status()
    if args.playback_command == "next":
        return await client.playback_control(PlaybackAction.NEXT)
    if args.playback_command == "prev":
        return await client.playback_control(PlaybackAction.PREVIOUS)
    if args.playback_command == "pause":
        return await client.playback_control(PlaybackAction.PAUSE_TOGGLE)
    if args.playback_command == "ff":
        return await client.playback_control(PlaybackAction.FAST_FORWARD)
    if args.playback_command == "rewind":
        return await client.playback_control(PlaybackAction.REWIND)
    if args.playback_command == "play-index":
        return await client.playback_control(
            PlaybackAction.PLAY_INDEX, args.track_index
        )
    if args.playback_command == "seek":
        return await client.playback_control(PlaybackAction.SEEK_SECONDS, args.seconds)
    if args.playback_command == "volume":
        return await client.playback_control(
            PlaybackAction.SET_VOLUME_PERCENT, args.percent
        )
    if args.playback_command == "mode":
        return await client.playback_control(
            PlaybackAction.SET_MODE,
            PLAYBACK_MODE_TO_ID[args.mode_value],
        )
    if args.playback_command == "output":
        return await client.playback_control(
            PlaybackAction.SET_OUTPUT_TARGET,
            OUTPUT_TARGET_TO_ID[args.output_target],
        )
    raise AssertionError(f"unexpected playback command {args.playback_command!r}")


async def handle_library_command(
    client: CompanionBleClient, args: argparse.Namespace
) -> Any:
    if args.library_command == "album":
        return await client.library_album()
    if args.library_command == "tracks":
        return await client.library_track_page(args.offset, args.count)
    raise AssertionError(f"unexpected library command {args.library_command!r}")


async def handle_wifi_command(
    client: CompanionBleClient, args: argparse.Namespace
) -> Any:
    if args.wifi_command == "status":
        return await client.wifi_status()
    if args.wifi_command == "scan-start":
        return await client.wifi_scan_start()
    if args.wifi_command == "scan-results":
        return await client.wifi_scan_results(args.offset, args.count)
    if args.wifi_command == "connect":
        return await client.wifi_connect(args.ssid, args.password)
    if args.wifi_command == "connect-slot":
        return await client.wifi_connect_slot(args.slot)
    if args.wifi_command == "disconnect":
        return await client.wifi_disconnect()
    if args.wifi_command == "autoreconnect":
        return await client.wifi_autoreconnect(args.enabled)
    raise AssertionError(f"unexpected wifi command {args.wifi_command!r}")


async def handle_lastfm_command(
    client: CompanionBleClient, args: argparse.Namespace
) -> Any:
    if args.lastfm_command == "status":
        return await client.lastfm_status()
    if args.lastfm_command == "set-auth-url":
        return await client.lastfm_control(LastfmAction.SET_AUTH_URL, auth_url=args.url)
    if args.lastfm_command == "token":
        return await client.lastfm_control(LastfmAction.REQUEST_TOKEN)
    if args.lastfm_command == "auth":
        return await client.lastfm_control(
            LastfmAction.AUTH,
            username=args.username,
            password=args.password,
        )
    if args.lastfm_command == "logout":
        return await client.lastfm_control(LastfmAction.LOGOUT)
    if args.lastfm_command == "scrobble":
        return await client.lastfm_control(
            LastfmAction.SET_SCROBBLING, enabled=args.enabled
        )
    if args.lastfm_command == "now-playing":
        return await client.lastfm_control(
            LastfmAction.SET_NOW_PLAYING, enabled=args.enabled
        )
    raise AssertionError(f"unexpected lastfm command {args.lastfm_command!r}")


async def handle_history_command(
    client: CompanionBleClient, args: argparse.Namespace
) -> Any:
    if args.history_command == "summary":
        return await client.history_summary()
    if args.history_command == "albums":
        return await client.history_album_page(args.offset, args.count)
    raise AssertionError(f"unexpected history command {args.history_command!r}")


async def handle_bt_command(
    client: CompanionBleClient, args: argparse.Namespace
) -> Any:
    if args.bt_command == "status":
        return await client.bt_audio_status()
    if args.bt_command == "connect-last":
        return await client.bt_audio_control(BtAction.CONNECT_LAST)
    if args.bt_command == "pair-best":
        return await client.bt_audio_control(BtAction.PAIR_BEST)
    if args.bt_command == "disconnect":
        return await client.bt_audio_control(BtAction.DISCONNECT)
    raise AssertionError(f"unexpected bt command {args.bt_command!r}")


async def watch_events(client: CompanionBleClient) -> None:
    print("Watching companion API events and heartbeats. Press Ctrl+C to stop.")
    while True:
        frame = await client.next_event()
        json_dump(decode_frame(frame))


def add_common_connection_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--address", help="BLE address to connect to")
    parser.add_argument("--name", help="BLE name to connect to")
    parser.add_argument("--profile", help="Credential profile key in the state file")
    parser.add_argument("--state-file", type=Path, default=DEFAULT_STATE_FILE)
    parser.add_argument("--client-id", help="Override stored client ID")
    parser.add_argument(
        "--app-name", help="App name for pairing or credential override"
    )
    parser.add_argument("--secret-hex", help="Override stored 32-byte secret in hex")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--scan-timeout", type=float, default=DEFAULT_SCAN_TIMEOUT)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument(
        "--no-auto-auth",
        action="store_true",
        help="Disable automatic auth using stored credentials for auth-required commands",
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="BLE test client for the Jukeboy companion API v1"
    )
    add_common_connection_args(parser)
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan_parser = subparsers.add_parser(
        "scan", help="Scan for BLE devices and print candidates"
    )
    scan_parser.add_argument("--scan-timeout", type=float, default=DEFAULT_SCAN_TIMEOUT)

    subparsers.add_parser("hello", help="Call HELLO")
    subparsers.add_parser("capabilities", help="Call CAPABILITIES")

    ping_parser = subparsers.add_parser("ping", help="Call PING")
    ping_parser.add_argument("text", nargs="?", default="ping")

    pair_parser = subparsers.add_parser("pair", help="Pairing commands")
    pair_subparsers = pair_parser.add_subparsers(dest="pair_command", required=True)
    pair_begin = pair_subparsers.add_parser("begin", help="Begin pairing")
    pair_begin.add_argument(
        "--sequence",
        help="Comma-separated button sequence, e.g. main1,main2,misc1,misc3. Defaults to a random sequence.",
    )
    pair_begin.add_argument("--wait", action="store_true", default=True)
    pair_begin.add_argument("--no-wait", dest="wait", action="store_false")
    pair_begin.add_argument("--wait-timeout", type=float, default=120.0)
    pair_subparsers.add_parser("status", help="Get pairing status")
    pair_subparsers.add_parser("cancel", help="Cancel pairing")

    subparsers.add_parser(
        "auth", help="Authenticate using stored or supplied credentials"
    )

    trusted_parser = subparsers.add_parser("trusted", help="Trusted client commands")
    trusted_subparsers = trusted_parser.add_subparsers(
        dest="trusted_command", required=True
    )
    trusted_subparsers.add_parser("list", help="List trusted clients")
    trusted_revoke = trusted_subparsers.add_parser(
        "revoke", help="Revoke a trusted client"
    )
    trusted_revoke.add_argument("client_id_to_revoke")

    subparsers.add_parser("snapshot", help="Get a composite snapshot")

    watch_parser = subparsers.add_parser(
        "watch", help="Print unsolicited events and heartbeats"
    )
    watch_parser.add_argument(
        "--auth",
        action="store_true",
        help="Authenticate before watching if credentials exist",
    )

    playback_parser = subparsers.add_parser("playback", help="Playback commands")
    playback_subparsers = playback_parser.add_subparsers(
        dest="playback_command", required=True
    )
    playback_subparsers.add_parser("status")
    playback_subparsers.add_parser("next")
    playback_subparsers.add_parser("prev")
    playback_subparsers.add_parser("pause")
    playback_subparsers.add_parser("ff")
    playback_subparsers.add_parser("rewind")
    playback_play_index = playback_subparsers.add_parser("play-index")
    playback_play_index.add_argument("track_index", type=int)
    playback_seek = playback_subparsers.add_parser("seek")
    playback_seek.add_argument("seconds", type=int)
    playback_volume = playback_subparsers.add_parser("volume")
    playback_volume.add_argument("percent", type=int)
    playback_mode = playback_subparsers.add_parser("mode")
    playback_mode.add_argument("mode_value", choices=sorted(PLAYBACK_MODE_TO_ID))
    playback_output = playback_subparsers.add_parser("output")
    playback_output.add_argument("output_target", choices=sorted(OUTPUT_TARGET_TO_ID))

    library_parser = subparsers.add_parser("library", help="Album and track metadata")
    library_subparsers = library_parser.add_subparsers(
        dest="library_command", required=True
    )
    library_subparsers.add_parser("album")
    library_tracks = library_subparsers.add_parser("tracks")
    library_tracks.add_argument("--offset", type=int, default=0)
    library_tracks.add_argument("--count", type=int, default=8)

    wifi_parser = subparsers.add_parser("wifi", help="Wi-Fi commands")
    wifi_subparsers = wifi_parser.add_subparsers(dest="wifi_command", required=True)
    wifi_subparsers.add_parser("status")
    wifi_subparsers.add_parser("scan-start")
    wifi_scan_results = wifi_subparsers.add_parser("scan-results")
    wifi_scan_results.add_argument("--offset", type=int, default=0)
    wifi_scan_results.add_argument("--count", type=int, default=8)
    wifi_connect = wifi_subparsers.add_parser("connect")
    wifi_connect.add_argument("ssid")
    wifi_connect.add_argument("password", nargs="?", default="")
    wifi_connect_slot = wifi_subparsers.add_parser("connect-slot")
    wifi_connect_slot.add_argument("slot", type=int)
    wifi_subparsers.add_parser("disconnect")
    wifi_autoreconnect = wifi_subparsers.add_parser("autoreconnect")
    wifi_autoreconnect.add_argument("enabled", choices=["on", "off"])

    lastfm_parser = subparsers.add_parser("lastfm", help="Last.fm commands")
    lastfm_subparsers = lastfm_parser.add_subparsers(
        dest="lastfm_command", required=True
    )
    lastfm_subparsers.add_parser("status")
    lastfm_set_auth_url = lastfm_subparsers.add_parser("set-auth-url")
    lastfm_set_auth_url.add_argument("url")
    lastfm_subparsers.add_parser("token")
    lastfm_auth = lastfm_subparsers.add_parser("auth")
    lastfm_auth.add_argument("username")
    lastfm_auth.add_argument("password")
    lastfm_subparsers.add_parser("logout")
    lastfm_scrobble = lastfm_subparsers.add_parser("scrobble")
    lastfm_scrobble.add_argument("enabled", choices=["on", "off"])
    lastfm_now_playing = lastfm_subparsers.add_parser("now-playing")
    lastfm_now_playing.add_argument("enabled", choices=["on", "off"])

    history_parser = subparsers.add_parser("history", help="Play history commands")
    history_subparsers = history_parser.add_subparsers(
        dest="history_command", required=True
    )
    history_subparsers.add_parser("summary")
    history_albums = history_subparsers.add_parser("albums")
    history_albums.add_argument("--offset", type=int, default=0)
    history_albums.add_argument("--count", type=int, default=4)

    bt_parser = subparsers.add_parser("bt", help="Bluetooth audio commands")
    bt_subparsers = bt_parser.add_subparsers(dest="bt_command", required=True)
    bt_subparsers.add_parser("status")
    bt_subparsers.add_parser("connect-last")
    bt_subparsers.add_parser("pair-best")
    bt_subparsers.add_parser("disconnect")

    return parser.parse_args()


async def main_async(args: argparse.Namespace) -> int:
    if args.command == "scan":
        await print_scan(args.scan_timeout)
        return 0

    state_store = StateStore(args.state_file)
    profile = resolve_profile(args)

    async with CompanionBleClient(
        address=args.address,
        name=args.name,
        scan_timeout=args.scan_timeout,
        timeout=args.timeout,
        verbose=args.verbose,
    ) as client:
        if args.command == "hello":
            json_dump(await client.hello())
            return 0

        if args.command == "capabilities":
            json_dump(await client.capabilities())
            return 0

        if args.command == "ping":
            json_dump(await client.ping(args.text))
            return 0

        if args.command == "pair":
            json_dump(await handle_pair_command(client, args, state_store, profile))
            return 0

        if args.command == "auth":
            credentials = resolve_credentials(args, state_store, profile)
            if credentials is None:
                raise SystemExit(
                    "No credentials were found. Pair first or supply --client-id and --secret-hex."
                )
            challenge = await client.auth_challenge(credentials.client_id)
            nonce_hex = challenge.get("nonce_hex")
            if not isinstance(nonce_hex, str):
                raise RuntimeError("Auth challenge response did not include a nonce")
            json_dump(
                await client.auth_proof(
                    credentials.client_id, credentials.secret, bytes.fromhex(nonce_hex)
                )
            )
            return 0

        if args.command == "watch":
            if args.auth and not args.no_auto_auth:
                await ensure_authenticated(client, state_store, profile, args)
            await watch_events(client)
            return 0

        if command_should_auto_auth(args):
            await ensure_authenticated(client, state_store, profile, args)

        if args.command == "trusted":
            json_dump(await handle_trusted_command(client, args))
            return 0

        if args.command == "snapshot":
            json_dump(await client.snapshot())
            return 0

        if args.command == "playback":
            json_dump(await handle_playback_command(client, args))
            return 0

        if args.command == "library":
            json_dump(await handle_library_command(client, args))
            return 0

        if args.command == "wifi":
            if hasattr(args, "enabled") and isinstance(args.enabled, str):
                args.enabled = args.enabled == "on"
            json_dump(await handle_wifi_command(client, args))
            return 0

        if args.command == "lastfm":
            if hasattr(args, "enabled") and isinstance(args.enabled, str):
                args.enabled = args.enabled == "on"
            json_dump(await handle_lastfm_command(client, args))
            return 0

        if args.command == "history":
            json_dump(await handle_history_command(client, args))
            return 0

        if args.command == "bt":
            json_dump(await handle_bt_command(client, args))
            return 0

    raise AssertionError(f"Unhandled command {args.command!r}")


def main() -> int:
    args = parse_args()
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        print("Interrupted", file=sys.stderr)
        return 130
    except CompanionApiError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
