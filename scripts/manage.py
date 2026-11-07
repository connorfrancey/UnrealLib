# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "click>=8.1,<9",
#     "questionary>=2.1,<3",
#     "rich>=15,<16",
# ]
#
# [tool.uv]
# exclude-newer = "2026-10-23T00:00:00Z"
# ///
"""UnrealLib project manager.

Run with uv (dependencies are installed automatically into a cached, isolated environment):

    uv run scripts/manage.py               # interactive menu
    uv run scripts/manage.py --help        # all commands
    uv run scripts/manage.py all msvc-x64-debug

On Windows the Visual C++ environment (vcvarsall) is located with vswhere and applied automatically for the
architecture of the selected preset, so no Developer Command Prompt is needed.
"""

from __future__ import annotations

import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import time
import webbrowser
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import click
import questionary
from rich.console import Console
from rich.panel import Panel
from rich.table import Table

ROOT = Path(__file__).resolve().parent.parent
PRESETS_FILE = ROOT / "CMakePresets.json"
SETTINGS_FILE = ROOT / ".manage.json"
ENV_CACHE_DIR = ROOT / "build" / ".manage-cache"
IS_WINDOWS = platform.system() == "Windows"

console = Console(highlight=False)

TOOLS = {
    "inspector": ("tools", "UnrealPackageInspector"),
    "validator": ("tools", "UnrealPackageValidator"),
    "example": ("examples", "UnrealLibExampleLoadPackage"),
}


class ManageError(click.ClickException):
    """An error reported to the user without a traceback."""

    def show(self, file: Any = None) -> None:  # noqa: ARG002 - click signature
        console.print(f"[bold red]error:[/] {self.message}")


# ----------------------------------------------------------------------------------------------------------------------
# Settings
# ----------------------------------------------------------------------------------------------------------------------

SETTING_KEYS = {
    "default_preset": "Preset used when none is given",
    "mirrorsedge_dir": "Mirror's Edge CookedPC directory for fixture tests (UNREALLIB_MIRRORSEDGE_DIR)",
    "jobs": "Parallel jobs for build and test (empty = tool default)",
    "vs_installation": "Visual Studio installation for new build trees (empty = newest)",
}


def load_settings() -> dict[str, Any]:
    if SETTINGS_FILE.exists():
        try:
            return json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            raise ManageError(f"{SETTINGS_FILE.name} is not valid JSON: {e}") from e
    return {}


def save_settings(settings: dict[str, Any]) -> None:
    SETTINGS_FILE.write_text(json.dumps(settings, indent=2, sort_keys=True) + "\n", encoding="utf-8")


# ----------------------------------------------------------------------------------------------------------------------
# CMake presets
# ----------------------------------------------------------------------------------------------------------------------


@dataclass
class Preset:
    name: str
    display_name: str
    description: str
    binary_dir: Path
    architecture: str | None
    cache_variables: dict[str, str] = field(default_factory=dict)
    available: bool = True

    @property
    def configured(self) -> bool:
        return (self.binary_dir / "CMakeCache.txt").exists()

    @property
    def compiler(self) -> str:
        return self.cache_variables.get("CMAKE_CXX_COMPILER", "default")

    @property
    def build_type(self) -> str:
        return self.cache_variables.get("CMAKE_BUILD_TYPE", "?")


def _expand(value: str, preset_name: str) -> str:
    return (
        value.replace("${sourceDir}", str(ROOT))
        .replace("${presetName}", preset_name)
        .replace("${hostSystemName}", platform.system())
    )


def _condition_holds(condition: dict[str, Any] | None, preset_name: str) -> bool:
    if not condition:
        return True
    kind = condition.get("type")
    if kind in ("equals", "notEquals"):
        equal = _expand(condition["lhs"], preset_name) == _expand(condition["rhs"], preset_name)
        return equal if kind == "equals" else not equal
    return True  # other condition types are not used by this project


def load_presets() -> dict[str, Preset]:
    try:
        data = json.loads(PRESETS_FILE.read_text(encoding="utf-8"))
    except FileNotFoundError as e:
        raise ManageError(f"{PRESETS_FILE} not found") from e
    raw = {p["name"]: p for p in data.get("configurePresets", [])}

    def resolve(name: str, seen: tuple[str, ...] = ()) -> dict[str, Any]:
        if name in seen:
            raise ManageError(f"cyclic preset inheritance at '{name}'")
        preset = raw[name]
        merged: dict[str, Any] = {"cacheVariables": {}}
        parents = preset.get("inherits", [])
        for parent in [parents] if isinstance(parents, str) else parents:
            resolved = resolve(parent, (*seen, name))
            merged["cacheVariables"].update(resolved.get("cacheVariables", {}))
            for key, value in resolved.items():
                if key != "cacheVariables":
                    merged[key] = value
        for key, value in preset.items():
            if key == "cacheVariables":
                merged["cacheVariables"].update(value)
            elif key not in ("inherits", "hidden"):
                merged[key] = value
        return merged

    presets: dict[str, Preset] = {}
    for name, entry in raw.items():
        if entry.get("hidden"):
            continue
        resolved = resolve(name)
        arch = resolved.get("architecture")
        arch_value = arch.get("value") if isinstance(arch, dict) else arch
        cache_vars = {
            k: (v["value"] if isinstance(v, dict) else str(v)) for k, v in resolved["cacheVariables"].items()
        }
        presets[name] = Preset(
            name=name,
            display_name=resolved.get("displayName", name),
            description=resolved.get("description", ""),
            binary_dir=Path(_expand(resolved.get("binaryDir", "${sourceDir}/build/${presetName}"), name)),
            architecture=arch_value,
            cache_variables=cache_vars,
            available=_condition_holds(resolved.get("condition"), name),
        )
    return presets


def get_preset(name: str | None) -> Preset:
    presets = load_presets()
    if name is None:
        name = load_settings().get("default_preset")
        if name is None:
            raise ManageError("no preset given and no default set (use `config set default_preset <name>`)")
    if name not in presets:
        raise ManageError(f"unknown preset '{name}'. Available: {', '.join(presets)}")
    preset = presets[name]
    if not preset.available:
        raise ManageError(f"preset '{name}' is not available on {platform.system()}")
    return preset


# ----------------------------------------------------------------------------------------------------------------------
# Visual C++ environment
# ----------------------------------------------------------------------------------------------------------------------


VS_MARKER = ".manage-vs"  # written into a build tree: the Visual Studio installation it was configured with


@dataclass
class VsInstallation:
    name: str
    version: str
    path: Path
    toolset: str

    @property
    def vcvarsall(self) -> Path:
        return self.path / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"


def vs_installations() -> list[VsInstallation]:
    """Visual Studio installations with the C++ x86/x64 tools, newest first."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
        r"Microsoft Visual Studio\Installer\vswhere.exe"
    )
    if not vswhere.exists():
        return []
    result = subprocess.run(
        [str(vswhere), "-all", "-products", "*", "-format", "json", "-utf8",
         "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"],
        capture_output=True, text=True, encoding="utf-8", check=False,
    )
    installs = []
    for entry in json.loads(result.stdout or "[]"):
        path = Path(entry["installationPath"])
        toolset_file = path / "VC" / "Auxiliary" / "Build" / "Microsoft.VCToolsVersion.default.txt"
        toolset = toolset_file.read_text(encoding="utf-8").strip() if toolset_file.exists() else "?"
        install = VsInstallation(entry.get("displayName", path.name), entry.get("installationVersion", "?"), path,
                                 toolset)
        if install.vcvarsall.exists():
            installs.append(install)
    return sorted(installs, key=lambda i: [int(x) for x in re.findall(r"\d+", i.version)], reverse=True)


def _cached_compiler(preset: Preset) -> Path | None:
    cache = preset.binary_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    match = re.search(r"^CMAKE_CXX_COMPILER:\w+=(.+)$", cache.read_text(encoding="utf-8", errors="replace"),
                      re.MULTILINE)
    return Path(match.group(1)) if match else None


def select_vs(preset: Preset | None) -> VsInstallation:
    """The installation to use for `preset`: the one its build tree was configured with, else the
    `vs_installation` setting, else the newest."""
    installs = vs_installations()
    if not installs:
        raise ManageError("no Visual Studio installation with the C++ tools was found (vswhere)")

    def by_path(path: Path) -> VsInstallation | None:
        resolved = path.resolve()
        return next((i for i in installs if i.path.resolve() == resolved or i.path.resolve() in resolved.parents),
                    None)

    if preset is not None:
        marker = preset.binary_dir / VS_MARKER
        if marker.exists() and (found := by_path(Path(marker.read_text(encoding="utf-8").strip()))):
            return found
        if (compiler := _cached_compiler(preset)) and (found := by_path(compiler)):
            return found
    if setting := load_settings().get("vs_installation"):
        if found := by_path(Path(setting)):
            return found
        raise ManageError(f"vs_installation '{setting}' is not a Visual Studio installation with C++ tools")
    return installs[0]


def remember_vs(preset: Preset, install: VsInstallation) -> None:
    preset.binary_dir.mkdir(parents=True, exist_ok=True)
    (preset.binary_dir / VS_MARKER).write_text(str(install.path), encoding="utf-8")


def msvc_environment(arch: str, install: VsInstallation) -> dict[str, str]:
    """Environment with vcvarsall applied for `arch` (cached on disk, keyed by installation, mtime and PATH)."""
    vcvars = install.vcvarsall
    ENV_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    slug = re.sub(r"[^A-Za-z0-9]+", "-", f"{install.name}-{install.toolset}-{arch}").strip("-").lower()
    cache = ENV_CACHE_DIR / f"vcvars-{slug}.json"
    key = f"{vcvars}|{vcvars.stat().st_mtime_ns}|{os.environ.get('PATH', '')}"
    if cache.exists():
        try:
            cached = json.loads(cache.read_text(encoding="utf-8"))
            if cached.get("key") == key:
                return cached["env"]
        except (json.JSONDecodeError, KeyError):
            pass

    with console.status(f"Loading Visual C++ environment ({install.name}, MSVC {install.toolset}, {arch})..."):
        command = f'cmd /d /s /c ""{vcvars}" {arch} >nul && set"'
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise ManageError(f"vcvarsall.bat {arch} failed:\n{result.stdout}{result.stderr}")
    env = {}
    for line in result.stdout.splitlines():
        name, sep, value = line.partition("=")
        if sep and name:
            env[name] = value
    cache.write_text(json.dumps({"key": key, "env": env}), encoding="utf-8")
    return env


def environment_for(preset: Preset | None, extra: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ)
    if IS_WINDOWS and preset is not None and preset.architecture:
        env = msvc_environment(preset.architecture, select_vs(preset))
    if extra:
        env.update(extra)
    return env


# ----------------------------------------------------------------------------------------------------------------------
# Process execution
# ----------------------------------------------------------------------------------------------------------------------


def resolve_executable(command: list[str], env: dict[str, str] | None) -> list[str]:
    """Look the program up in `env`'s PATH: on Windows subprocess would otherwise search the parent's PATH, which
    lacks the Visual C++ tools."""
    found = shutil.which(command[0], path=(env or os.environ).get("PATH"))
    return [found, *command[1:]] if found else command


def run(command: list[str], env: dict[str, str] | None = None, cwd: Path = ROOT, check: bool = True) -> int:
    console.print(f"[dim]$ {shlex.join(command)}[/]")
    command = resolve_executable(command, env)
    started = time.perf_counter()
    try:
        code = subprocess.run(command, env=env, cwd=cwd, check=False).returncode
    except FileNotFoundError as e:
        raise ManageError(f"command not found: {command[0]}") from e
    except KeyboardInterrupt:
        raise ManageError("interrupted") from None
    elapsed = time.perf_counter() - started
    if code != 0 and check:
        raise ManageError(f"`{command[0]}` failed with exit code {code} after {elapsed:.1f}s")
    return code


def step(title: str) -> None:
    console.rule(f"[bold cyan]{title}")


def jobs_arg(jobs: int | None) -> list[str]:
    if jobs is None:
        configured = load_settings().get("jobs")
        jobs = int(configured) if configured else None
    return ["-j", str(jobs)] if jobs else []


def fixture_dir() -> str | None:
    value = load_settings().get("mirrorsedge_dir")
    return value or None


# ----------------------------------------------------------------------------------------------------------------------
# Actions (shared by the CLI and the interactive menu)
# ----------------------------------------------------------------------------------------------------------------------


def do_configure(preset: Preset, defines: tuple[str, ...] = (), fresh: bool = False,
                 differential: bool | None = None, docs: bool | None = None) -> None:
    step(f"Configure {preset.name}")
    command = ["cmake", "--preset", preset.name]
    if fresh:
        command.append("--fresh")
    if fixtures := fixture_dir():
        command.append(f"-DUNREALLIB_MIRRORSEDGE_DIR={fixtures}")
    if differential is not None:
        command.append(f"-DUNREALLIB_BUILD_DIFFERENTIAL={'ON' if differential else 'OFF'}")
    if docs is not None:
        command.append(f"-DUNREALLIB_BUILD_DOCS={'ON' if docs else 'OFF'}")
    command += [f"-D{d}" for d in defines]
    env = environment_for(preset)
    if IS_WINDOWS and preset.architecture:
        install = select_vs(preset)
        console.print(f"[dim]Visual Studio: {install.name} (MSVC {install.toolset})[/]")
        remember_vs(preset, install)
    run(command, env=env)


def ensure_configured(preset: Preset) -> None:
    if not preset.configured:
        console.print(f"[yellow]{preset.name} is not configured yet; configuring first.[/]")
        do_configure(preset)


def do_build(preset: Preset, targets: tuple[str, ...] = (), clean_first: bool = False, jobs: int | None = None) -> None:
    ensure_configured(preset)
    step(f"Build {preset.name}" + (f" ({', '.join(targets)})" if targets else ""))
    command = ["cmake", "--build", "--preset", preset.name, *jobs_arg(jobs)]
    for target in targets:
        command += ["--target", target]
    if clean_first:
        command.append("--clean-first")
    run(command, env=environment_for(preset))


def do_test(preset: Preset, regex: str | None = None, exclude: str | None = None, jobs: int | None = None,
            sweep: bool = False, verbose: bool = False, build: bool = True) -> None:
    if build:
        do_build(preset, jobs=jobs)
    else:
        ensure_configured(preset)
    step(f"Test {preset.name}" + (" (full corpus sweep)" if sweep else ""))
    command = ["ctest", "--preset", preset.name, *jobs_arg(jobs)]
    if regex:
        command += ["-R", regex]
    if exclude:
        command += ["-E", exclude]
    if verbose:
        command.append("--verbose")
    extra = {"UNREALLIB_FIXTURE_SWEEP": "1"} if sweep else {}
    if fixtures := fixture_dir():
        extra["UNREALLIB_MIRRORSEDGE_DIR"] = fixtures
    run(command, env=environment_for(preset, extra))


def do_clean(presets: list[Preset], assume_yes: bool = False) -> None:
    existing = [p for p in presets if p.binary_dir.exists()]
    if not existing:
        console.print("[green]Nothing to clean.[/]")
        return
    table = Table(title="Build directories to delete")
    table.add_column("Preset")
    table.add_column("Directory")
    for preset in existing:
        table.add_row(preset.name, str(preset.binary_dir.relative_to(ROOT)))
    console.print(table)
    if not assume_yes and not click.confirm("Delete these directories?", default=False):
        console.print("Cancelled.")
        return
    build_root = (ROOT / "build").resolve()
    for preset in existing:
        target = preset.binary_dir.resolve()
        # Safety: only ever delete directories strictly inside build/.
        if build_root not in target.parents:
            raise ManageError(f"refusing to delete {target}: not inside {build_root}")
        shutil.rmtree(target)
        console.print(f"[green]deleted[/] {target.relative_to(ROOT)}")


def do_docs(preset: Preset, open_browser: bool = False) -> None:
    do_configure(preset, docs=True)
    do_build(preset, targets=("unreallib_docs",))
    index = preset.binary_dir / "docs" / "html" / "index.html"
    console.print(f"[green]Documentation:[/] {index}")
    if open_browser and index.exists():
        webbrowser.open(index.as_uri())


def tool_path(preset: Preset, tool: str) -> Path:
    folder, stem = TOOLS[tool]
    return preset.binary_dir / folder / (stem + (".exe" if IS_WINDOWS else ""))


def do_run(preset: Preset, tool: str, args: tuple[str, ...]) -> int:
    executable = tool_path(preset, tool)
    if not executable.exists():
        target = {"inspector": "unreallib_package_inspector", "validator": "unreallib_package_validator",
                  "example": "unreallib_example_load_package"}[tool]
        do_build(preset, targets=(target,))
    # Run from the caller's directory so relative package paths work as typed.
    return run([str(executable), *args], env=environment_for(preset), cwd=Path.cwd(), check=False)


def tool_version(command: list[str], env: dict[str, str] | None = None, stderr_first: bool = False) -> str | None:
    try:
        result = subprocess.run(resolve_executable(command, env), capture_output=True, text=True, env=env,
                                check=False, timeout=30)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    output = ((result.stderr or result.stdout) if stderr_first else (result.stdout or result.stderr)).strip()
    return output.splitlines()[0] if output else None


def do_doctor() -> None:
    table = Table(title="Development environment")
    table.add_column("Tool")
    table.add_column("Status")
    table.add_column("Details", overflow="fold")

    def add(name: str, found: str | None, required: bool, note: str = "") -> None:
        if found:
            table.add_row(name, "[green]ok[/]", found + (f"  [dim]{note}[/]" if note else ""))
        else:
            table.add_row(name, "[red]missing[/]" if required else "[yellow]optional[/]", note)

    add("uv", tool_version(["uv", "--version"]), True)
    add("cmake", tool_version(["cmake", "--version"]), True)
    add("ninja", tool_version(["ninja", "--version"]), True)

    msvc_env = None
    if IS_WINDOWS:
        installs = vs_installations()
        if not installs:
            add("Visual Studio", None, True, "C++ tools not found with vswhere")
        for install in installs:
            add("Visual Studio", f"{install.name} {install.version}", True, f"MSVC {install.toolset}, {install.path}")
        if installs:
            default = select_vs(None)
            msvc_env = msvc_environment("x64", default)
            add("cl (MSVC)", tool_version(["cl"], env=msvc_env, stderr_first=True), True, f"default for new build trees: {default.name}")
    add("clang-cl", tool_version(["clang-cl", "--version"], env=msvc_env), False, "clang-cl presets")
    add("ccache", tool_version(["ccache", "--version"]), False, "used for MSVC builds when found")
    add("doxygen", tool_version(["doxygen", "--version"]), False, "`docs` command")
    add("dotnet", tool_version(["dotnet", "--version"]), False, "differential tests vs. upstream UELib")
    add("git", tool_version(["git", "--version"]), False)
    console.print(table)

    settings = load_settings()
    fixtures = settings.get("mirrorsedge_dir")
    if fixtures and Path(fixtures).is_dir():
        console.print(f"[green]Fixtures:[/] {fixtures}")
    elif fixtures:
        console.print(f"[red]Fixtures directory does not exist:[/] {fixtures}")
    else:
        console.print("[yellow]No fixture directory configured[/] (fixture tests will be skipped); "
                      "set it with `config set mirrorsedge_dir <.../TdGame/CookedPC>`")


def print_presets() -> None:
    settings = load_settings()
    table = Table(title="CMake presets")
    for column in ("Preset", "Compiler", "Arch", "Type", "State", "Description"):
        table.add_column(column, overflow="fold")
    for preset in load_presets().values():
        marker = " [bold](default)[/]" if preset.name == settings.get("default_preset") else ""
        if not preset.available:
            state = "[dim]unavailable[/]"
        elif preset.configured:
            state = "[green]configured[/]"
        else:
            state = "[dim]-[/]"
        table.add_row(preset.name + marker, preset.compiler, preset.architecture or "-", preset.build_type,
                      state, preset.description or preset.display_name)
    console.print(table)


def print_settings() -> None:
    settings = load_settings()
    table = Table(title=f"Settings ({SETTINGS_FILE.name})")
    table.add_column("Key")
    table.add_column("Value", overflow="fold")
    table.add_column("Meaning", style="dim")
    for key, meaning in SETTING_KEYS.items():
        table.add_row(key, str(settings.get(key, "")), meaning)
    console.print(table)


# ----------------------------------------------------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------------------------------------------------

preset_argument = click.argument("preset", required=False)
jobs_option = click.option("-j", "--jobs", type=int, help="Parallel jobs.")


@click.group(invoke_without_command=True, context_settings={"help_option_names": ["-h", "--help"]})
@click.pass_context
def cli(ctx: click.Context) -> None:
    """Build, test and maintain UnrealLib. Without a command, an interactive menu starts."""
    if ctx.invoked_subcommand is None:
        if sys.stdin.isatty() and sys.stdout.isatty():
            interactive_menu()
        else:
            click.echo(ctx.get_help())


@cli.command("presets")
def presets_cmd() -> None:
    """List the CMake presets."""
    print_presets()


@cli.command()
@preset_argument
@click.option("-D", "defines", multiple=True, metavar="VAR=VALUE", help="Extra CMake cache variable.")
@click.option("--fresh", is_flag=True, help="Discard the existing CMake cache.")
@click.option("--differential/--no-differential", default=None, help="Toggle differential tests (needs dotnet).")
@click.option("--docs/--no-docs", default=None, help="Toggle the Doxygen target.")
def configure(preset: str | None, defines: tuple[str, ...], fresh: bool, differential: bool | None,
              docs: bool | None) -> None:
    """Configure PRESET (default preset if omitted)."""
    do_configure(get_preset(preset), defines, fresh, differential, docs)


@cli.command()
@preset_argument
@click.option("-t", "--target", "targets", multiple=True, help="Build only this target (repeatable).")
@click.option("--clean-first", is_flag=True, help="Clean before building.")
@jobs_option
def build(preset: str | None, targets: tuple[str, ...], clean_first: bool, jobs: int | None) -> None:
    """Build PRESET (configures first if needed)."""
    do_build(get_preset(preset), targets, clean_first, jobs)


@cli.command()
@preset_argument
@click.option("-R", "--regex", help="Only run tests matching this regular expression.")
@click.option("-E", "--exclude", help="Exclude tests matching this regular expression.")
@click.option("--sweep", is_flag=True, help="Enable the full Mirror's Edge corpus sweeps (slow).")
@click.option("--no-build", is_flag=True, help="Do not build before testing.")
@click.option("-v", "--verbose", is_flag=True, help="Show test output.")
@jobs_option
def test(preset: str | None, regex: str | None, exclude: str | None, sweep: bool, no_build: bool, verbose: bool,
         jobs: int | None) -> None:
    """Build and run the tests of PRESET with CTest."""
    do_test(get_preset(preset), regex, exclude, jobs, sweep, verbose, build=not no_build)


@cli.command("all")
@preset_argument
@click.option("--fresh", is_flag=True, help="Discard the existing CMake cache first.")
@jobs_option
def all_cmd(preset: str | None, fresh: bool, jobs: int | None) -> None:
    """Configure, build and test PRESET."""
    selected = get_preset(preset)
    started = time.perf_counter()
    do_configure(selected, fresh=fresh)
    do_test(selected, jobs=jobs)
    console.print(Panel.fit(f"[bold green]{selected.name}: configure, build and test succeeded[/] "
                            f"({time.perf_counter() - started:.0f}s)"))


@cli.command()
@click.argument("presets", nargs=-1)
@click.option("--all", "all_presets", is_flag=True, help="Clean every preset's build directory.")
@click.option("-y", "--yes", is_flag=True, help="Do not ask for confirmation.")
def clean(presets: tuple[str, ...], all_presets: bool, yes: bool) -> None:
    """Delete the build directories of PRESETS (only build/<preset>, nothing else)."""
    available = load_presets()
    if all_presets:
        selected = list(available.values())
    elif presets:
        selected = [get_preset(name) for name in presets]
    else:
        selected = [get_preset(None)]
    do_clean(selected, yes)


@cli.command()
@preset_argument
@click.option("--open", "open_browser", is_flag=True, help="Open the generated documentation.")
def docs(preset: str | None, open_browser: bool) -> None:
    """Generate the Doxygen API documentation."""
    do_docs(get_preset(preset), open_browser)


@cli.command(context_settings={"ignore_unknown_options": True, "allow_extra_args": True})
@click.argument("tool", type=click.Choice(sorted(TOOLS)))
@click.argument("args", nargs=-1, type=click.UNPROCESSED)
@click.option("-p", "--preset", "preset_name", help="Preset whose build to use (default preset if omitted).")
def tool(tool: str, args: tuple[str, ...], preset_name: str | None) -> None:
    """Run a built tool: inspector, validator or example (arguments are passed through).

    Example: manage.py tool inspector Core.u --exports --limit 10
    """
    raise SystemExit(do_run(get_preset(preset_name), tool, args))


@cli.command()
def doctor() -> None:
    """Check the development environment."""
    do_doctor()


@cli.group(invoke_without_command=True)
@click.pass_context
def config(ctx: click.Context) -> None:
    """Show or change settings stored in .manage.json."""
    if ctx.invoked_subcommand is None:
        print_settings()


@config.command("set")
@click.argument("key", type=click.Choice(sorted(SETTING_KEYS)))
@click.argument("value")
def config_set(key: str, value: str) -> None:
    """Set KEY to VALUE."""
    settings = load_settings()
    if key == "default_preset":
        get_preset(value)  # validates
    elif key == "mirrorsedge_dir":
        path = Path(value).expanduser()
        if not path.is_dir():
            raise ManageError(f"'{value}' is not a directory")
        value = path.resolve().as_posix()
    elif key == "vs_installation" and value:
        if not any(i.path.resolve() == Path(value).resolve() for i in vs_installations()):
            raise ManageError(f"'{value}' is not a Visual Studio installation with C++ tools "
                              "(see `doctor` for the list)")
    elif key == "jobs" and value and not re.fullmatch(r"\d+", value):
        raise ManageError("jobs must be a positive integer")
    settings[key] = value
    save_settings(settings)
    print_settings()


@config.command("unset")
@click.argument("key", type=click.Choice(sorted(SETTING_KEYS)))
def config_unset(key: str) -> None:
    """Remove KEY."""
    settings = load_settings()
    settings.pop(key, None)
    save_settings(settings)
    print_settings()


# ----------------------------------------------------------------------------------------------------------------------
# Interactive menu
# ----------------------------------------------------------------------------------------------------------------------


def ask(question: questionary.Question) -> Any:
    answer = question.ask()  # returns None on Ctrl+C
    if answer is None:
        raise KeyboardInterrupt
    return answer


def choose_preset(message: str = "Preset") -> Preset:
    presets = [p for p in load_presets().values() if p.available]
    default = load_settings().get("default_preset")
    choices = [
        questionary.Choice(f"{p.name}  ({'configured' if p.configured else 'not configured'})", value=p.name)
        for p in presets
    ]
    default_choice = next((c for c in choices if c.value == default), None)
    return get_preset(ask(questionary.select(message, choices=choices, default=default_choice)))


def interactive_settings() -> None:
    print_settings()
    key = ask(questionary.select("Change which setting?",
                                 choices=[*SETTING_KEYS, questionary.Choice("(back)", value=None)]))
    if key is None:
        return
    settings = load_settings()
    if key == "default_preset":
        settings[key] = choose_preset("Default preset").name
    elif key == "mirrorsedge_dir":
        value = ask(questionary.path("CookedPC directory:", only_directories=True,
                                     default=settings.get(key, "")))
        if value and not Path(value).is_dir():
            console.print(f"[red]'{value}' is not a directory[/]")
            return
        settings[key] = Path(value).resolve().as_posix() if value else ""
    elif key == "vs_installation":
        choices = [questionary.Choice(f"{i.name} (MSVC {i.toolset})", value=str(i.path)) for i in vs_installations()]
        settings[key] = ask(questionary.select("Visual Studio for new build trees:",
                                               choices=[questionary.Choice("(newest)", value=""), *choices]))
    else:
        settings[key] = ask(questionary.text("Parallel jobs (empty = default):", default=str(settings.get(key, "")),
                                             validate=lambda v: v == "" or v.isdigit()))
    save_settings(settings)
    print_settings()


def split_arguments(raw: str) -> list[str]:
    """Shell-like splitting that keeps backslashes (Windows paths) intact."""
    lexer = shlex.shlex(raw, posix=True)
    lexer.whitespace_split = True
    lexer.escape = ""
    return list(lexer)


def interactive_menu() -> None:
    console.print(Panel.fit("[bold]UnrealLib project manager[/]\n[dim]Ctrl+C to go back / quit[/]"))
    actions = {
        "Configure + build + test": "all",
        "Build": "build",
        "Test": "test",
        "Configure": "configure",
        "Clean build directories": "clean",
        "Generate documentation": "docs",
        "Run a tool": "tool",
        "List presets": "presets",
        "Check environment (doctor)": "doctor",
        "Settings": "settings",
        "Quit": "quit",
    }
    while True:
        try:
            action = actions[ask(questionary.select("What do you want to do?", choices=list(actions)))]
        except KeyboardInterrupt:
            return  # Ctrl+C at the top level quits
        if action == "quit":
            return
        try:
            if action == "presets":
                print_presets()
            elif action == "doctor":
                do_doctor()
            elif action == "settings":
                interactive_settings()
            elif action == "clean":
                names = ask(questionary.checkbox(
                    "Clean which build directories?",
                    choices=[questionary.Choice(p.name, checked=False) for p in load_presets().values()
                             if p.binary_dir.exists()]))
                do_clean([get_preset(n) for n in names])
            else:
                preset = choose_preset()
                if action == "all":
                    do_configure(preset)
                    do_test(preset)
                elif action == "build":
                    do_build(preset)
                elif action == "configure":
                    fresh = ask(questionary.confirm("Discard the existing cache (--fresh)?", default=False))
                    do_configure(preset, fresh=fresh)
                elif action == "test":
                    regex = ask(questionary.text("Test filter regex (empty = all):"))
                    sweep = ask(questionary.confirm("Include the full corpus sweeps (slow)?", default=False))
                    do_test(preset, regex=regex or None, sweep=sweep)
                elif action == "docs":
                    do_docs(preset, open_browser=ask(questionary.confirm("Open in browser?", default=True)))
                elif action == "tool":
                    which = ask(questionary.select("Tool", choices=sorted(TOOLS)))
                    raw = ask(questionary.text("Arguments:"))
                    do_run(preset, which, tuple(split_arguments(raw)))
            console.print()
        except KeyboardInterrupt:
            console.print("[dim]back[/]")
            continue
        except ManageError as e:
            e.show()


def main() -> None:
    try:
        cli()
    except KeyboardInterrupt:
        console.print("[dim]interrupted[/]")
        sys.exit(130)


if __name__ == "__main__":
    main()
