# Jukeboy Companion API v1

The companion API runs over the existing BLE SPP-style GATT service:

- Service UUID: `0xABF0`
- Write characteristic: `0xABF1`
- Notify/indicate characteristic: `0xABF2`

The `0xABF3` command and `0xABF4` status characteristics remain available for
legacy/debug behavior. Companion API traffic uses only the data write/notify
pair.

## Frame Format

All multi-byte integers are little-endian.

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 1 | Magic byte `J` (`0x4A`) |
| 1 | 1 | Magic byte `C` (`0x43`) |
| 2 | 1 | Protocol version, currently `1` |
| 3 | 1 | Frame type |
| 4 | 2 | Opcode |
| 6 | 4 | Request ID |
| 10 | 2 | Payload length |
| 12 | n | TLV payload |

Frame types:

| Value | Type |
| ---: | --- |
| 1 | Request |
| 2 | Response |
| 3 | Event |
| 4 | Heartbeat |
| 5 | Error |

The firmware accepts frames up to 2048 bytes. BLE writes and notifications may
fragment a frame according to the negotiated MTU; clients should reassemble the
byte stream using the frame header and payload length. v1 uses request IDs and
explicit response/error frames, but does not implement retransmission windows.

## TLV Format

Each payload is a sequence of TLVs:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 2 | TLV type |
| 2 | 2 | Value length |
| 4 | n | Value bytes |

Strings are UTF-8 bytes without a trailing NUL. Boolean values are one byte
where `0` is false and nonzero is true.

## Authentication

Unauthenticated clients may call `HELLO`, `CAPABILITIES`, `PING`, pairing, and
auth opcodes. Other opcodes require app-level authorization.

Pairing flow:

1. The app creates a client ID, app name, 32-byte shared secret, and four-button
   sequence using HID button IDs `0..5`.
2. The app sends `PAIR_BEGIN` with those values.
3. Firmware enters a timed pairing window and waits for the physical sequence.
   The user can press the device buttons or use `companion pair input <button>`
   from the UART console. Development hardware can use `companion pair confirm`.
4. On success, the trusted client record is stored in secure NVS under the
   `companion_api` namespace. Up to four trusted clients are supported.

Reconnect auth flow:

1. App sends `AUTH_CHALLENGE` with its client ID.
2. Firmware returns a 16-byte nonce.
3. App sends `AUTH_PROOF` with `HMAC-SHA256(shared_secret, nonce)`.
4. Firmware marks the BLE session authenticated until disconnect or revocation.

Trusted apps can be inspected and revoked from the console:

- `companion status`
- `companion clients`
- `companion pair status`
- `companion pair input main1`
- `companion pair confirm`
- `companion pair cancel`
- `companion revoke <client_id>`
- `companion revoke all`

## Opcode Registry

| Opcode | Name | Auth | Notes |
| ---: | --- | --- | --- |
| `0x0001` | `HELLO` | No | Device/protocol greeting |
| `0x0002` | `CAPABILITIES` | No | Version, frame size, MTU, feature bits, auth state |
| `0x0003` | `PING` | No | Echoes payload |
| `0x0010` | `PAIR_BEGIN` | No | Begin button-sequence pairing |
| `0x0011` | `PAIR_STATUS` | No | Pairing progress and pending sequence |
| `0x0012` | `PAIR_CANCEL` | No | Cancel pending pairing |
| `0x0013` | `AUTH_CHALLENGE` | No | Request nonce for trusted client |
| `0x0014` | `AUTH_PROOF` | No | Complete HMAC auth |
| `0x0015` | `TRUSTED_LIST` | Yes | List trusted app records |
| `0x0016` | `TRUSTED_REVOKE` | Yes | Revoke by client ID |
| `0x0100` | `SNAPSHOT` | Yes | Composite firmware snapshot |
| `0x0101` | `PLAYBACK_STATUS` | Yes | Same payload as snapshot in first implementation |
| `0x0102` | `PLAYBACK_CONTROL` | Yes | Playback action + optional value |
| `0x0110` | `LIBRARY_ALBUM` | Yes | Album/cartridge metadata |
| `0x0111` | `LIBRARY_TRACK_PAGE` | Yes | Paged track metadata, max 8 per response |
| `0x0200` | `WIFI_STATUS` | Yes | Wi-Fi status snapshot |
| `0x0201` | `WIFI_SCAN_START` | Yes | Starts async scan |
| `0x0202` | `WIFI_SCAN_RESULTS` | Yes | Paged scan results, max 8 per response |
| `0x0203` | `WIFI_CONNECT` | Yes | SSID/password connect |
| `0x0204` | `WIFI_CONNECT_SLOT` | Yes | Connect saved slot |
| `0x0205` | `WIFI_DISCONNECT` | Yes | Disconnect STA |
| `0x0206` | `WIFI_AUTORECONNECT` | Yes | Enable/disable auto reconnect |
| `0x0300` | `LASTFM_STATUS` | Yes | Last.fm status snapshot |
| `0x0301` | `LASTFM_CONTROL` | Yes | Auth URL/token/auth/logout/toggles |
| `0x0400` | `HISTORY_SUMMARY` | Yes | Play-history counts |
| `0x0401` | `HISTORY_ALBUM_PAGE` | Yes | Paged album history, max 4 per response |
| `0x0500` | `BT_AUDIO_STATUS` | Yes | A2DP/bonded device summary |
| `0x0501` | `BT_AUDIO_CONTROL` | Yes | Connect last, pair best sink, disconnect |

## Playback Actions

`PLAYBACK_CONTROL` uses TLV `ACTION` (`0x030C`) and optional `VALUE`
(`0x030D`).

| Value | Action | Value meaning |
| ---: | --- | --- |
| 1 | Next | unused |
| 2 | Previous | unused |
| 3 | Pause toggle | unused |
| 4 | Fast forward | unused |
| 5 | Rewind | unused |
| 6 | Play track by index | track index |
| 7 | Seek absolute seconds | seconds in current track |
| 8 | Set volume percent | `0..100` |
| 9 | Set playback mode | player mode enum |
| 10 | Set output target | `0` Bluetooth, `1` I2S |

## Heartbeat And Resync

The firmware sends heartbeat frames roughly every five seconds when BLE SPP is
connected and notifications are enabled. Heartbeats include uptime, auth state,
queue health, RX/TX counters, and a generation counter. Apps should request a
fresh `SNAPSHOT` whenever the generation changes or after reconnecting.

## Security Notes

- The app-generated shared secret is persisted in secure NVS and is never
  printed to logs or returned by list/status commands.
- Wi-Fi and Last.fm credentials are accepted only after app-level auth.
- Mobile BLE address identity is intentionally ignored because platform BLE
  privacy can rotate addresses.
