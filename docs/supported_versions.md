# Supported versions

## Summary

| Profile id       | Game                | Platform | File version | Licensee | Engine build | Status |
|------------------|---------------------|----------|--------------|----------|--------------|--------|
| `mirrorsedge-pc` | Mirror's Edge (2008)| PC       | 536          | 43       | 3716         | **Verified** (package header & tables) |
| `ue3-generic`    | any UE3             | any (LE) | 249 – 766    | any      | any          | Fallback, unverified beyond Mirror's Edge |

Not supported (rejected with `UnsupportedError`): UE1/UE2 (file version < 249), late UE3 (file version ≥ 767,
summary contains texture allocations that are not parsed yet), UE4/UE5 (negative legacy version).

## Mirror's Edge PC — verification record

### Candidate values vs. evidence

| Value                 | Candidate | Upstream UELib                                   | Real packages (Steam release)                     |
|-----------------------|-----------|--------------------------------------------------|---------------------------------------------------|
| File version          | 536       | `[Build(536, 43)] MirrorsEdge` (UnrealPackage.cs) | 2426 / 2426 packages                               |
| Licensee version      | 43        | same                                             | 2425 / 2426 (the exception is a modded file)       |
| Engine build          | 3716      | not used for detection                           | 2426 / 2426                                         |
| Cooker version        | —         | —                                                | 60 for 2399 packages, 0 for 27 (editor-saved/modded) |
| Compression           | —         | —                                                | LZO (flag `0x2`) for 1553 packages, none for 873   |
| Generations           | —         | —                                                | exactly 1 in every package                          |
| Byte order            | —         | —                                                | little-endian everywhere                            |

Corpus: `TdGame/CookedPC` of a Steam installation — 20 `.u`, 1727 `.upk`, 679 `.me1` files, 6.5 GB. The
installation used during development contains a few modified packages (`Engine.u`, `TdGame.u` are stored uncompressed;
`MirrorsEdgeConsole.u` has licensee 0 and is detected as `ue3-generic`). This is useful: it exercises both the
compressed and uncompressed code paths.

### What is verified

| Feature                                   | Evidence                                                                 |
|-------------------------------------------|--------------------------------------------------------------------------|
| Package summary layout                    | All 54 top-level packages: re-serialized summary is byte-identical (`SummaryRewriteIsByteIdentical`); Core.u header bytes are pinned in a unit test |
| Compressed chunk table order              | Core.u: `UncompressedOffset == NameOffset`, `CompressedOffset == end of summary` |
| LZO1X decompression                       | All 1553 compressed packages decompress to exactly the declared sizes and their tables parse (`FullCorpusSweep`) |
| Name / import / export tables             | All 2426 packages load and validate with zero errors; Core.u tables re-serialize byte-identically |
| Depends table                             | Parsed for all packages; all indices valid                                |
| Object paths / outer chains               | Every export path in all 2426 packages resolves without cycles or invalid references |
| Game profile detection                    | 2425 packages → `mirrorsedge-pc`, 1 → `ue3-generic`                        |

| Import resolution | All 2426 packages: 179,461 imports → exports, 6,536 intrinsic classes, 8,895 packages; the 101 unresolved imports are all in the three modded packages (references to content that is not installed) (`FullCorpusLinking`) |
| UObject header | 2,525,148 exports: state frame / component template / NetIndex parsed and followed by a well-formed property tag (`FullCorpusLinking`) |
| Agreement with upstream UELib (`8236c39`) | Differential tests over all 2426 packages: every summary identical; names, imports, exports and depends identical for all 873 uncompressed packages (upstream cannot read compressed tables); upstream's build detection (`MirrorsEdge`) agrees with profile detection for every package |

Commands used:

```powershell
UnrealPackageValidator.exe "<install>/TdGame/CookedPC" -r -q
$env:UNREALLIB_FIXTURE_SWEEP=1; ctest --preset msvc-x64-debug -R FullCorpusSweep
cmake --preset msvc-x64-release -DUNREALLIB_BUILD_DIFFERENTIAL=ON -DUNREALLIB_DIFFERENTIAL_RECURSIVE=ON
ctest --preset msvc-x64-release -R Differential
```

### Mirror's Edge specifics known from upstream (not yet used)

* Bytecode: upstream maps token `0x4F` to `UnresolvedToken` for Mirror's Edge (`DefaultEngineBranch.cs`).
* Bytecode: Mirror's Edge (536) is the first build where the struct-member context token serializes a 16-bit property
  type instead of 8-bit (`ContextTokens.cs`: "first attested with Mirrors Edge (536)").

These belong to milestone M5 (bytecode) and will be added to the profile together with tests.

### Not yet verified

Script properties, UField/UStruct/UClass data, bytecode, assets. See
[development.md](development.md) for the milestone plan.
