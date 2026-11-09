#!/usr/bin/env sh
# UnrealLib project manager wrapper (Linux, macOS, Git Bash / MSYS2 on Windows).
#
#   ./build.sh                 interactive menu
#   ./build.sh all             configure + build + test the default preset
#   ./build.sh --help          all commands
#
# Runs uelib.py with uv; all arguments are passed through unchanged and the working directory is kept,
# so relative paths (e.g. `./build.sh tool inspector Core.u`) refer to where you are.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

if ! command -v uv >/dev/null 2>&1; then
    echo "error: uv is not installed or not on PATH." >&2
    echo "Install it from https://docs.astral.sh/uv/getting-started/installation/ :" >&2
    echo "  curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
    exit 127
fi

exec uv run --script "$script_dir/uelib.py" "$@"
