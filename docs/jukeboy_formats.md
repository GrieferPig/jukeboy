# Jukeboy File Formats

This document defines the Jukeboy binary formats currently used or reserved by the project.

- `.jba`: Jukeboy Audio
- `.jbm`: Jukeboy Metadata
- `.jbs`: Jukeboy Playback Status

All integer fields are little-endian unless stated otherwise.

## Common String Rules

When this document says `string`, it means:

- UTF-8 encoded
- null terminated
- stored inside a fixed-size byte field
- any unused bytes after the terminating `0x00` should be set to `0x00`

If a string exactly fills the field, the recommended encoding rule is to truncate to `field_size - 1` bytes and append a null terminator.

## `.jba` - Jukeboy Audio

`.jba` stores Opus audio in a custom streaming format optimized for the Jukeboy player.

### `.jbm` Layout

The file layout is:

1. file header
2. lookup table
3. zero padding up to a 512-byte block boundary
4. audio chunks

Each lookup table entry represents exactly 1 second of playback.

### File Header

The `.jba` header is packed and contains the following fields:

| Offset | Size | Type | Name | Description |
| --- | ---: | --- | --- | --- |
| 0 | 1 | `uint8_t` | `version` | Format version. Current value is `0x01`. |
| 1 | 4 | `uint32_t` | `header_len_in_blocks` | Total header area size in 512-byte blocks. This includes the file header, lookup table, and padding. |
| 5 | 4 | `uint32_t` | `lookup_table_len` | Number of lookup-table entries. This is also the number of 1-second chunks in the file. |

Header size before padding is 9 bytes.

### Lookup Table

Immediately after the file header is the lookup table:

- `lookup_table_len` entries
- each entry is a `uint32_t`
- each entry stores the byte offset of a chunk relative to the start of the audio data area

Rules:

- entry `0` must be `0`
- entries must be monotonic increasing
- each entry points to the beginning of a chunk
- the last chunk ends at end-of-file

### Header Padding

The audio data area starts at:

$$
\text{data_offset} = \text{header_len_in_blocks} \times 512
$$

All bytes between the end of the lookup table and `data_offset` are reserved padding and should be zero.

### Audio Chunk Layout

Each chunk represents 1 second of audio and has this structure:

1. `crc32`: `uint32_t`
2. packet payload bytes

The checksum is computed over the packet payload only, not including the 4-byte CRC field itself.

### CRC32

Chunk CRC uses the standard reflected CRC-32 polynomial:

- polynomial: `0xEDB88320`
- initial value: `0xFFFFFFFF`
- final XOR: `0xFFFFFFFF`

This matches the current player implementation.

### Packet Payload

The chunk payload is a sequence of length-prefixed raw Opus packets.

Each packet uses one of two length encodings:

1. short form
   - 1 byte
   - if byte value `< 252`, that value is the packet length in bytes

2. extended form
   - 2 bytes
   - if first byte is `>= 252`, packet length is:

$$
\text{packet_len} = \text{byte0} + 4 \times \text{byte1}
$$

After the length field, exactly `packet_len` bytes of raw Opus packet data follow.

### Encoding Assumptions

The current player and conversion pipeline assume:

- codec: Opus
- sample rate: 48000 Hz
- channels: 2
- target bitrate: 160 kbps
- frame duration: 20 ms
- 50 Opus packets per second
- non-self-delimited Opus packets inside the chunk payload

Because each lookup-table entry is 1 second, each chunk usually contains 50 Opus packets when encoded with 20 ms frames.

### Constraints

Current implementation constraints:

- `version` must be `0x01`
- each chunk must be larger than 4 bytes so that it contains at least a CRC field and some payload
- current player maximum chunk size is 24 KiB
- `.jba` files are typically named `000.jba` through `999.jba`

### `.jbm` Pseudostructures

```c
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint32_t header_len_in_blocks;
    uint32_t lookup_table_len;
} jba_header_t;
```

Chunk payload model:

```c
typedef struct __attribute__((packed)) {
    uint32_t crc32;
    uint8_t packet_stream[];
} jba_chunk_t;
```

## `.jbm` - Jukeboy Metadata

`.jbm` stores album-level and track-level metadata for a Jukeboy album or collection.

### `.jbs` Layout

The file layout is:

1. metadata header fields
2. variable-length track array

There is no separate offset table. Tracks are stored sequentially after the fixed metadata header.

### File Header Fields

| Order | Size | Type | Name | Description |
| --- | ---: | --- | --- | --- |
| 1 | 4 | `uint32_t` | `version` | Metadata format version. Current value is `1`. |
| 2 | 4 | `uint32_t` | `checksum` | CRC32 of the entire `.jbm` file with this field temporarily set to `0` during checksum calculation. |
| 3 | 128 | `string` | `album_name` | Album title. |
| 4 | 1024 | `string` | `album_description` | Album description or notes. |
| 5 | 256 | `string` | `artist` | Primary album artist or album-level artist credit. |
| 6 | 4 | `uint32_t` | `year` | Release year. |
| 7 | 4 | `uint32_t` | `duration_sec` | Total album duration in seconds. |
| 8 | 64 | `string` | `genre` | Primary genre. |
| 9 | 160 | `string[5]` | `tag` | Array of 5 tags, each stored as a 32-byte string. |
| 10 | 4 | `uint32_t` | `track_count` | Number of track entries following the header. Maximum `999`. |

The fixed-size `.jbm` header is 1652 bytes.

### `track_t`

Each track entry has this binary layout:

| Order | Size | Type | Name | Description |
| --- | ---: | --- | --- | --- |
| 1 | 128 | `string` | `track_name` | Track title. |
| 2 | 256 | `string` | `artists` | Track-level artist credit. |
| 3 | 4 | `uint32_t` | `duration_sec` | Track duration in seconds. |
| 4 | 4 | `uint32_t` | `file_num` | Associated `.jba` file number, usually matching `NNN.jba`. |

Each `track_t` entry is 392 bytes.

### `.jbm` Constraints

- `version` must currently be `1`
- `track_count` must be in the range `0..999`
- `file_num` should refer to a corresponding `.jba` asset, for example `0` maps to `000.jba`
- strings should be valid UTF-8 where possible
- unused tag slots should be encoded as empty null-terminated strings

### `.jbm` Size Formula

Total `.jbm` size is:

$$
1652 + 392 \times \text{track_count}
$$

Maximum size at 999 tracks is:

$$
1652 + 392 \times 999 = 393260\ \text{bytes}
$$

### Pseudostructures

```c
typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t checksum;
    char album_name[128];
    char album_description[1024];
    char artist[256];
    uint32_t year;
    uint32_t duration_sec;
    char genre[64];
    char tag[5][32];
    uint32_t track_count;
} jbm_header_t;

typedef struct __attribute__((packed)) {
    char track_name[128];
    char artists[256];
    uint32_t duration_sec;
    uint32_t file_num;
} track_t;
```

## `.jbs` - Jukeboy Playback Status

`.jbs` stores the saved playback position for the current cartridge.

The file is written as `playback.jbs` in the root of the cartridge filesystem.

### High-Level Layout

The file contains a single packed struct with no trailing data.

### File Fields

| Order | Size | Type | Name | Description |
| --- | ---: | --- | --- | --- |
| 1 | 4 | `uint32_t` | `version` | Playback status format version. Current value is `1`. |
| 2 | 4 | `uint32_t` | `current_track_num` | Zero-based playlist index in metadata order. `0` means the first track in `.jbm`. |
| 3 | 4 | `uint32_t` | `current_sec` | Resume position in seconds from the start of the selected track. |

`.jbs` size is always 12 bytes.

### Semantics

- `current_track_num` refers to the metadata playback order, not the `.jba` file number
- `current_sec` maps directly to the `.jba` lookup-table second index
- on cartridge insert, if `playback.jbs` exists and is valid, playback should start from that saved track and second
- implementations should overwrite the whole file and close it after each update so the saved state is durable on media

### `.jbs` Constraints

- `version` must currently be `1`
- `current_track_num` must be less than the number of tracks in `.jbm`
- `current_sec` should be clamped to the available lookup-table range when used for resume

### Pseudostructure

```c
typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t current_track_num;
    uint32_t current_sec;
} jbs_status_t;
```

## Recommended File Pairing

Recommended album bundle layout:

- one `.jbm` metadata file for the album
- up to 999 `.jba` files for audio assets
- one optional `playback.jbs` file for persisted resume state

Recommended mapping:

- track entry with `file_num = 0` maps to `000.jba`
- track entry with `file_num = 1` maps to `001.jba`
- track entry with `file_num = 999` maps to `999.jba`

## Versioning Guidance

If either format changes incompatibly:

- increment its `version`
- preserve little-endian encoding
- document new checksum rules or field additions explicitly
- keep old readers from silently accepting newer unsupported versions
