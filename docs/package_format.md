# UE3 package format (as implemented)

All multi-byte values are little-endian unless the signature is byte-swapped (big-endian console packages; supported
by the archive layer but not verified). Version gates refer to the package **file version**; their names and values
come from upstream UELib `PackageObjectLegacyVersion` and are mirrored in `UnrealLib/Serialization/LegacyVersion.hpp`.

## File layout

```
+--------------------------+  0
| FPackageFileSummary      |
+--------------------------+  NameOffset          ┐
| Name table               |                      │
| Import table             |                      │  "header"  (HeaderSize bytes from 0)
| Export table             |                      │
| Depends table            |                      │
+--------------------------+  HeaderSize          ┘
| Export serial data       |  ExportEntry.SerialOffset / SerialSize
+--------------------------+
```

For fully compressed packages (`CompressedChunks` non-empty) the file contains the summary followed by compressed
chunks. The *decompressed image* is: file bytes `[0, Chunks[0].UncompressedOffset)` (the summary) followed by every
chunk decompressed to its `UncompressedOffset`. **All table and serial offsets refer to the decompressed image.**

## FPackageFileSummary

| Field                       | Type      | Present when      | Notes |
|-----------------------------|-----------|-------------------|-------|
| Tag                         | uint32    | always            | `0x9E2A83C1`; `0xC1832A9E` means big-endian |
| FileVersion / Licensee      | int32     | always            | low 16 bits / high 16 bits; negative ⇒ UE4 (unsupported) |
| HeaderSize                  | int32     | ≥ 249             | |
| FolderName                  | FString   | ≥ 269             | `"None"` in Mirror's Edge |
| PackageFlags                | uint32    | always            | see `PackageFlag` |
| NameCount, NameOffset       | int32 × 2 | always            | |
| ExportCount, ExportOffset   | int32 × 2 | always            | note: exports **before** imports |
| ImportCount, ImportOffset   | int32 × 2 | always            | |
| DependsOffset               | int32     | ≥ 415             | |
| ImportExportGuidsOffset, ImportGuidsCount, ExportGuidsCount | int32 × 3 | ≥ 623 | not parsed |
| ThumbnailTableOffset        | int32     | ≥ 584             | not parsed |
| Guid                        | FGuid     | always            | |
| Generations                 | TArray<FGenerationInfo> | always | {ExportCount, NameCount, NetObjectCount (≥ 322)} |
| EngineVersion               | int32     | ≥ 245             | 3716 for Mirror's Edge |
| CookerVersion               | int32     | ≥ 277             | |
| CompressionFlags            | uint32    | ≥ 334             | 1 zlib, 2 LZO, 4 LZX |
| CompressedChunks            | TArray<FCompressedChunk> | ≥ 334 | see below |
| PackageSource               | uint32    | ≥ 482             | |
| AdditionalPackagesToCook    | TArray<FString> | ≥ 516       | |
| TextureAllocations          | …         | ≥ 767             | **not implemented** → versions ≥ 767 rejected |

Mirror's Edge summary size: 125 bytes (with one compressed chunk) / 109 bytes (uncompressed).

### FCompressedChunk

`int32 UncompressedOffset, int32 UncompressedSize, int32 CompressedOffset, int32 CompressedSize` — in that order.

> Upstream UELib's `CompressedChunk.Deserialize` reads `UncompressedOffset, CompressedOffset, UncompressedSize,
> CompressedSize`, which disagrees with its own `Serialize` and with real data. UnrealLib follows the order verified on
> Mirror's Edge `Core.u`.

### Compressed chunk payload

At `CompressedOffset`:

```
uint32 Tag            (package signature)
int32  BlockSize      (if == signature: 0x20000)
int32  TotalCompressed, TotalUncompressed
{ int32 Compressed, int32 Uncompressed } × ceil(TotalUncompressed / BlockSize)
block payloads, concatenated
```

UnrealLib validates that block sizes sum to the totals, that no block exceeds `BlockSize`, and that each block
decodes to exactly its declared size.

## Name table entry

| Version | Layout |
|---------|--------|
| ≥ 64    | FString, then flags |
| < 64    | NUL-terminated ANSI, then flags |
| flags   | uint64 (≥ 195) / uint32 (< 195) |

## Name reference (FName)

`index` (CompactIndex < 178, int32 ≥ 178) followed by `int32 number` (≥ 343). `number` is stored as-is; 0 means no
suffix, N > 0 displays as `Name_<N-1>`.

## Import entry (FObjectImport)

`FName ClassPackage, FName ClassName, int32 Outer, FName ObjectName` (Outer is always 32-bit).

## Export entry (FObjectExport, default UE3 branch)

| Field                     | Type        | Present when |
|---------------------------|-------------|--------------|
| Class, Super              | index       | always |
| Outer                     | int32       | always |
| ObjectName                | FName       | always |
| Archetype                 | int32       | ≥ 220 |
| ObjectFlags               | uint64 / uint32 | ≥ 195 / < 195 |
| SerialSize                | index       | always |
| SerialOffset              | index       | SerialSize > 0 or ≥ 249 |
| ComponentMap              | TArray<{FName, int32}> | 178 ≤ v < 543 (present in Mirror's Edge) |
| ExportFlags               | uint32      | ≥ 247 |
| GenerationNetObjectCount  | TArray<int32> | ≥ 322 |
| PackageGuid               | FGuid       | ≥ 322 |
| PackageFlags              | uint32      | ≥ 475 |

ObjectFlags are kept exactly as stored; they match UE3's `RF_*` constants (e.g. `RF_Public = 0x0000000400000000`).
Upstream UELib swaps the two 32-bit halves to map them onto its own enum.

## Object index (PACKAGE_INDEX)

`0` = null, `N > 0` = export `N-1`, `N < 0` = import `-N-1`.

## Depends table

At `DependsOffset`: one `TArray<int32>` of object indices per export, in export order.

## Normalized JSON

`UnrealPackageInspector --json [-a]` emits a key-ordered, normalized document (`tools/Common/PackageJson.cpp`):
`summary` (versions, offsets, flags as `0x%08X`, chunks, profile), `names` (`name`, `flags`), `imports` (`index`,
`path`, `class_package`, `class`, `outer`), `exports` (`index`, `path`, `class`, `super`, `outer`, `archetype`,
flags, serial range, optional `component_map`/`depends`) and `diagnostics`. Object references are emitted as dotted
paths so that the document can be compared semantically with output from other implementations (e.g. upstream
UELib) regardless of internal index conventions.
