# Decompiler

`UnrealLib::Decompiler` turns UnrealScript bytecode back into UnrealScript-style source. Status: complete for
Mirror's Edge: **functions, states and whole classes** (class declaration, constants, enums, structs with
`structdefaultproperties`, variables, replication, functions, states with their functions and labels,
`defaultproperties` with `Begin Object` subobject blocks).

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

## Encoding

`encode_script(writer, script, token_map)` is the inverse of `decode_script()`: it writes a token tree back as
bytecode in the writer's file version and returns the in-memory size (ScriptSize units). Operands are written as
they are, jump targets and skip sizes included, so a script decoded and encoded unchanged is byte-identical: every
script of Mirror's Edge (13,770 scripts, 477,343 tokens over the whole corpus) round-trips
(`MirrorsEdgeReflectionTests`). Terminators the decoder doesn't keep as children (EndFunctionParms, EndParmValue,
the LabelTable's "None" entry, whose name and offset the LabelTable token keeps) are written from the kinds, and a
token built from scratch only needs its kind: the opcode comes from the map.

## Classes

`ScriptDecompiler::decompile_class(ref)` produces a whole `.uc` file. Pass a `ReflectionSchema` over the game's script
packages with `set_schema()` so arrays and script-declared structs in default properties decode (without one they
are printed as comments and counted as placeholders).

* **Declaration.** Only what differs from the parent class is declared: inheritable flags (`transient`,
  `config(...)`, `placeable`, `editinlinenew`, `collapsecategories`, …), `within`, `hidecategories`,
  `autoexpandcategories` and `implements(...)` lists. A class whose parent is placeable but which is not prints
  `notplaceable`.
* **Members.** Constants, enums, structs, functions and states are printed in declaration order (the children list
  stores them in reverse); variables in children-list order. The compiler-generated `<Enum>_MAX` value is dropped,
  and the hidden `__X__Delegate` variables are printed as comments.
* **Replication.** Every replicated variable stores the offset of its condition in the class script (`RepOffset`);
  variables sharing a condition are grouped under one `if (...)`.
* **Default properties.** The class default object's properties; exports whose outer is that object become
  `Begin Object Class=… Name=…` blocks (nested up to 8 levels). Arrays print one `Name(i)=…` line per element, or
  `Name.Empty` when empty.

The `UnrealScriptDecompiler` tool writes one `<output>/<Package>/Classes/<Class>.uc` per class:

```powershell
UnrealScriptDecompiler.exe "…/CookedPC/Engine.u" --output scripts/
UnrealScriptDecompiler.exe "…/CookedPC/Core.u" --class Object --stdout
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

`MirrorsEdgeDecompilerTests.ScriptPackageClasses` decompiles every class of the script packages: 2,338 classes,
about 220,000 lines, 0 failures, 0 unknown natives, 0 placeholders (every default property decodes with the
`ReflectionSchema`).

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
| Class declaration | prints the class's own flags | prints only flags and lists that differ from the parent class (what the source declared) |
| Control flow | nest manager interleaved with printing | separate statement IR (`structure()`), explicit gotos counted |

## Limitations

* Comments, `cpptext`/`structcpptext` blocks and `#exec` directives are not stored in cooked packages and cannot be
  recovered. Function-local labels are generated (`J0x…`).
* Variable declaration order within a class follows the children list, which matches the source for compiled
  scripts but is not guaranteed by the format.
* Default values equal to the parent's are not stored by the cooker, so `defaultproperties` holds only overrides.
* Some modifiers have no flag on disk (e.g. `dependson`, `showcategories`) and are not printed.
