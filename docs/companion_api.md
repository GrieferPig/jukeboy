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
| `0x0103` | `OUTPUT_STATUS` | Yes | Current audio output target and A2DP connection state |
| `0x0104` | `OUTPUT_SELECT` | Yes | Select A2DP or local I2S output |
| `0x0110` | `LIBRARY_ALBUM` | Yes | Album/cartridge metadata |
| `0x0111` | `LIBRARY_TRACK_PAGE` | Yes | Paged track metadata, max 8 per response |
| `0x0112` | `LIBRARY_COVER` | Yes | Paged `cover.png` transfer, max 1024 bytes per response |
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
| `0x0502` | `BT_SCAN_START` | Yes | Start A2DP sink discovery |
| `0x0503` | `BT_SCAN_RESULTS` | Yes | Paged discovered sink results |
| `0x0504` | `BT_BONDED_LIST` | Yes | List bonded A2DP devices with display names |
| `0x0505` | `BT_UNBOND` | Yes | Remove a bonded A2DP device and return the refreshed list |

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
| 10 | Set output target | Legacy path; prefer `OUTPUT_SELECT`. Values are `0` A2DP, `1` I2S |

## Audio Output

The preferred output-management surface is separate from playback controls:

- `OUTPUT_STATUS` has no request TLVs. The response includes `OUTPUT_TARGET`
   (`0x030E`) and `BT_A2DP_CONNECTED` (`0x0800`).
- `OUTPUT_SELECT` accepts `OUTPUT_TARGET` (`0x030E`) as one byte: `0` for A2DP
   and `1` for I2S. The response uses the same payload as `OUTPUT_STATUS`.

Selecting A2DP wakes the external A2DP coprocessor if needed. Selecting I2S
suspends A2DP audio and asks the coprocessor to enter deep sleep.

## Bluetooth Bonded Devices

`BT_BONDED_LIST` has no request TLVs. The response includes:

- `BT_BONDED_COUNT` (`0x0801`) for the bonded-device count reported by the A2DP
   coprocessor for this response.
- `RETURNED_COUNT` (`0x040C`) for the number of device records included in this
   frame.
- Repeated device records encoded as `BT_ADDR` (`0x0802`, six raw address bytes)
   followed by `BT_NAME` (`0x0803`, UTF-8 display name). The name can be empty if
   the coprocessor has not cached one for that address yet, and long names may be
   truncated to fit the coprocessor UART payload budget.

`BT_UNBOND` accepts one `BT_ADDR` TLV. On success, the response uses the
refreshed bonded-list payload described above, with opcode `BT_UNBOND`.

## Library Cover Transfer

`LIBRARY_COVER` accepts TLV `OFFSET` (`0x040A`) and optional `COUNT`
(`0x040B`) as a requested byte window. The response includes:

- `OFFSET` (`0x040A`) for the returned chunk start
- `RETURNED_COUNT` (`0x040C`) for bytes included in this frame
- `TOTAL_SIZE` (`0x040D`) for the full PNG length
- `MIME_TYPE` (`0x040F`) currently set to `image/png`
- `BINARY_DATA` (`0x040E`) containing the raw PNG bytes for the chunk

`LIBRARY_ALBUM` also includes `ALBUM_HAS_COVER` (`0x0410`) so clients can
avoid requesting the asset when the cartridge does not provide `cover.png`.

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
