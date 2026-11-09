# Reference metadata

Normalized metadata of the Mirror's Edge PC release (Steam, `TdGame/CookedPC`, 2,426 packages), used as a regression
baseline by `MirrorsEdgeReferenceTests.FullCorpusMatchesReference` (`UNREALLIB_FIXTURE_SWEEP=1`). It holds no game
content — only paths, sizes, versions, profile ids, compression flags, table sizes, total export bytes and an FNV-1a
hash of the re-serialized name/import/export/depends tables — so any change in how a package's summary or tables are
read shows up as a one-line diff.

Regenerate after an intended change (and review the diff):

```powershell
$env:UNREALLIB_FIXTURE_SWEEP=1; $env:UNREALLIB_UPDATE_REFERENCE=1
build/msvc-x64-debug/tests/UnrealLibTests.exe --gtest_filter=*FullCorpusMatchesReference*
```

The installation it was generated from contains a few modified packages (see docs/supported_versions.md); other
installations may differ in those lines.
