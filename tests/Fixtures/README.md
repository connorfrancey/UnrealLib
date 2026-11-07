# Fixtures

Real game packages are **not** stored in this repository. Integration tests read them from a local, legally obtained
installation configured with `-DUNREALLIB_MIRRORSEDGE_DIR=<.../TdGame/CookedPC>` (or the environment variable of the
same name). Synthetic packages are generated in memory by `tests/Support/SyntheticPackage.cpp`.
