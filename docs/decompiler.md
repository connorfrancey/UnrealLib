# Decompiler

`UnrealLib::Decompiler` turns UnrealScript bytecode back into UnrealScript-style source. Status: **functions and
states** are decompiled (declaration, parameters, locals, body); class-level output (class declaration, variables,
structs, enums, consts, `defaultproperties`) is the next step of milestone M6.

## Pipeline

```text
bytecode ──decode_script()──▶ token tree ──structure()──▶ statement tree ──Printer──▶ source
          (Bytecode module)                (Statements.hpp)                (Decompiler.hpp)
```

Each stage is usable on its own. `structure()` works on memory offsets only and knows nothing about text; the
printer never changes the structure.

```cpp
UnrealLib::Linker linker;
linker.add_search_path("…/TdGame/CookedPC");
std::vector<const UnrealLib::Package*> scripts = /* Core, Engine, TdGame, … */;

auto natives = UnrealLib::Decompiler::NativeTable::from_packages(linker, scripts);
UnrealLib::Decompiler::ScriptDecompiler decompiler(linker, std::move(natives));

auto result = decompiler.decompile(function_ref);   // a Function or State export
std::cout << result.source;
bool exact = result.clean();   // no placeholders, gotos or structure problems
```

## Native functions

Native calls (opcode ≥ 0x60) are printed through a `NativeTable`: native index → name, call syntax (function,
operator, pre-/post-operator) and operator precedence. Upstream UELib reads it from a separate `.NTL` file;
UnrealLib builds it from the native `UFunction` exports of the game's own script packages, so it always matches the
game (Mirror's Edge: 199 native functions). A native missing from the table prints as `__NFUN_<index>__` and is
counted in `DecompiledScript::unknown_natives`.

## Control flow

| Source                  | Bytecode pattern (memory offsets)                                          |
|-------------------------|-----------------------------------------------------------------------------|
| `if (c) A`              | `JumpIfNot(c) → E; A; E:`                                                   |
| `if (c) A else B`       | `JumpIfNot(c) → L; A; Jump → E; L: B; E:`                                   |
| `while (c) A`           | `T: JumpIfNot(c) → E; A; Jump → T; E:`                                      |
| `for (i; c; n) A`       | `i; T: JumpIfNot(c) → E; A; C: n; Jump → T; E:` (when a `continue` jumps to C) |
| `while (true) A`        | `T: A; Jump → T; E:`                                                        |
| `do A until (c)`        | `T: A; JumpIfNot(c) → T`                                                    |
| `foreach F(…) A`        | `Iterator(F) → E; A; IteratorNext; IteratorPop; E:`                         |
| `switch (x)`            | `Switch(x); Case(v) → next; A; Jump → E; …; Case(default); E:`              |
| `break` / `continue`    | `Jump` to the innermost loop's (or switch's) end / continue point           |

Choices where the bytecode allows several readings (all of them equivalent in behaviour):

* A jump to where the enclosing loop continues or ends is printed as `continue`/`break` when the alternative would be
  an empty `if` branch (`if (c) {} else B` → `if (c) continue; B`) or would leave gotos inside an `if`/`else`.
* A loop is printed as `for` only when some `continue` jumps to its last statement; the increment is then lifted
  into the header, and the statement before the loop becomes the initialization when it assigns the same variable.
* The compiler ends every `switch` with a `default:` case; an empty one is not printed.
* The compiler's trailing `return;` (or implicit `return ReturnValue;`) is not printed.

Jumps that match no pattern are kept as `goto J0x<offset>;` with a label at the target (`if (!(c)) goto …;` for
conditional ones) and counted in `StructuredScript::gotos`. Functions have no label names on disk, so their labels
are generated; state code labels come from the state's `LabelTable`.

## Verification (Mirror's Edge PC)

`MirrorsEdgeDecompilerTests` decompiles every function and state of the script packages:

* 13,244 scripts (13,051 functions, 193 states): 0 failures, 0 unknown natives, 0 placeholders, 0 structure problems;
* 13,241 scripts are fully structured. The remaining 27 gotos are in 3 versions of `TestCombatTransitions`, which
  jump to a shared `TdGotoState(...)` call and to the function end from many places: written with labels in the
  original source, not a structuring failure.

Set `UNREALLIB_DECOMPILE_DUMP=<dir>` to write every decompiled script as `.uc` for review (game-derived output: never
commit it), and `UNREALLIB_DECOMPILE_TRACE=<function name>` to print a script's token tree with offsets.

Unit tests (`tests/Unit/Decompiler`) cover each control-flow pattern on hand-assembled bytecode.

## Differences from upstream UELib

| Topic | UELib | UnrealLib |
|-------|-------|-----------|
| Native table | external `.NTL` file | built from the game's native functions |
| Floats | 7 fixed decimals (`1.0000000`) | shortest exact text (`1.0`, `0.1`) |
| Pre-/post-operators | post-operator test (`precedence == 0`) first | `PreOperator` flag first, so a zero-precedence pre-operator is not printed as postfix |
| Operator parentheses | right operand of equal precedence unparenthesized (`a - (b - c)` printed as `a - b - c`) | left-associative: equal-precedence right operands are parenthesized |
| `DynamicArrayAdd` | always reads a trailing `EndFunctionParms` | only from version 649, like the other array intrinsics: Mirror's Edge (536) has none, and reading one swallowed the next statement. Every expected terminator is now checked |
| Parameter modifiers | — | `optional const out coerce`, matching the original source order |
| Control flow | nest manager interleaved with printing | separate statement IR (`structure()`), explicit gotos counted |
