# Serialization

## Principles

* **Field by field.** Every structure is read and written one field at a time with explicit byte order
  (`load_unsigned` / `store_unsigned`). Memory is never reinterpreted as a struct; no `memcpy` of serialized layouts.
* **Bounds-checked.** `BinaryReader` checks every read against the remaining bytes and throws `SerializationError`
  with the *absolute* input offset. A failed read does not advance the position.
* **Bounded allocations.** Counts and lengths read from the input are validated twice before anything is allocated:
  against `SerializationLimits` and against the number of bytes that remain (`count × minimum element size`).
* **Version-driven, not game-driven.** `SerializationContext` carries file/licensee version, byte order and limits.
  Version gates use named `LegacyVersion` constants. Game-specific deviations belong in `GameProfile`, never as
  ad-hoc checks inside serializers.
* **Lossless where possible.** Readers keep enough information for writers to reproduce the original bytes
  (string encoding and terminator, raw name numbers, raw object flags). Round-trip tests enforce this.

## Components

| Header | Purpose |
|--------|---------|
| `Serialization/BinaryReader.hpp` | Bounds-checked reader over `std::span<const std::byte>` |
| `Serialization/BinaryWriter.hpp` | Growable writer with seek and back-patching |
| `Serialization/BitStream.hpp` | `BitReader`/`BitWriter` (LSB-first, Unreal `SerializeInt`) |
| `Serialization/Archive.hpp` | `Archive` interface, `MemoryArchive`, `FileArchive`, `read_file` |
| `Serialization/CompactIndex.hpp` | CompactIndex encoding |
| `Serialization/FString.hpp` | FString value type |
| `Serialization/Compression.hpp` | zlib, LZO1X decompression; zlib compression |
| `Serialization/SerializationContext.hpp` | versions, byte order, limits |
| `Serialization/SerializationLimits.hpp` | allocation limits |
| `Serialization/LegacyVersion.hpp` | named version gates (from upstream) |

## Primitive encodings

### CompactIndex (version < 178)

Byte 0: bit 7 sign, bit 6 continuation, bits 0–5 value. Bytes 1–3: bit 7 continuation, bits 0–6 value. Byte 4: all
8 bits (upstream behaviour). Differences from upstream: magnitudes above `INT32_MAX` are rejected instead of
overflowing, and `INT32_MIN` cannot be written. "Negative zero" (`0x80`) decodes to 0.

### Index

`read_index()` is CompactIndex before version 178 and int32 from 178 on.

### FString

`index length`, then `|length|` characters. Positive: Windows-1252 bytes; negative: UTF-16 code units; zero: empty
string without payload. The final character is normally NUL.

UnrealLib stores text as UTF-8 and remembers `encoding` and `null_terminated`. Unpaired UTF-16 surrogates are kept as
WTF-8 so decode → encode is lossless. When writing an `Ansi` string that cannot be represented in Windows-1252 the
writer switches to UTF-16 (as upstream does).

### FName

See [package_format.md](package_format.md#name-reference-fname).

## Default limits

| Limit | Default |
|-------|---------|
| `max_string_length` | 1 Mi characters |
| `max_array_count` | 16 Mi elements |
| `max_allocation_bytes` | 1 GiB |
| `max_decompressed_size` | 2 GiB |
| `max_file_size` | 2 GiB |

The largest Mirror's Edge package is 121 MB, well inside the defaults. Tools and fuzz tests use much smaller limits.

## Compression

* **LZO1X** — UnrealLib contains its own decompressor (`src/Serialization/Lzo1x.cpp`), written from the format
  description. liblzo2 is not used because it is GPL-2.0 licensed. Every input read, output write and back-reference
  is checked; trailing input after the end-of-stream marker is an error (as `lzo1x_decompress_safe` reports
  `LZO_E_INPUT_NOT_CONSUMED`). Verified on all 1553 compressed Mirror's Edge packages. LZO *compression* is not
  implemented (needed only for writing compressed packages, milestone M8).
* **zlib** — via zlib 1.4 (`inflate` with `Z_FINISH`; output size must match exactly).
* **LZX** — not supported (`UnsupportedError`); Xbox 360 only.

## Error model

All failures are exceptions derived from `UnrealLib::Error` (`SerializationError`, `CompressionError`,
`PackageError`, `IoError`, `UnsupportedError`), each with an `ErrorCode` and optional offset. Non-fatal findings are
collected as `Diagnostics` (`Package::diagnostics()`, `validate_package()`).
