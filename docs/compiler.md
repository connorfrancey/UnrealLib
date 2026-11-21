# Compiler

`UnrealLib::Compiler` compiles UnrealScript function and state bodies to bytecode: the inverse of the
[decompiler](decompiler.md).

```text
source ──parse──▶ syntax tree ──bind + generate──▶ token tree (Script) ──assemble_script()──▶ bytecode
```

```cpp
UnrealLib::Compiler::ScriptCompiler compiler(linker, script_packages);   // Core, Engine, ... (operators, natives)
auto result = compiler.compile_body(source, function_ref, default_values);
if (result.ok())
{
    BinaryWriter writer(package.context());
    assemble_script(writer, result.script, token_map);
}
```

**Scope.** Bodies: statements, expressions, labels and state code, and the default values of optional parameters.
Declarations (classes, variables, function signatures) are read from the packages, not compiled: a body is compiled
against an existing Function or State export. Object operands are indices of the owner's package (its export, or the
import that refers to the object), so the result can go back into that package.

**Binding.** Names resolve as the UE3 compiler resolves them: parameters and locals, then the state's and the class's
properties (and supers), `Outer` typed as the class's `within` class. Calls are `FinalFunction` for final functions and
super calls, `NativeFunction` for functions with a native index, `VirtualFunction` otherwise; a delegate held in a
variable is called through it (`DelegateFunction`). Operators are chosen among every operator the script packages
declare by their operand types; the parser takes their text and precedence from the same declarations (an operator's
precedence for parsing is the one most of its overloads declare).

## Verification (Mirror's Edge PC)

`MirrorsEdgeCompilerTests.DecompileCompileRoundTrip` decompiles every function and state of the script packages,
compiles the source back, assembles it in the package's format and compares the bytes with the original:

* 13,244 scripts; 13,231 compile (99.9%); **12,866 byte-identical (97.1%)**;
* the game's own packages: **11,706 of 11,992 (97.6%)** (Core 99.2%, Engine 97.8%, TdGame 97.1%, Tp 99.7%, Editor,
  GameFramework, IpDrv, UnrealEd and the content packages 100%);
* `MirrorsEdgeTweaksScripts` and `MirrorsEdgeConsole` (third-party mods, built with another compiler) 92.7% / 89.5%.

The test reports the first differing token of every other script, grouped, with an example each;
`UNREALLIB_COMPILE_TRACE=<name>[,<name>...]` prints the source and both token streams.

### What the UE3 compiler does that the source doesn't say

Found from the corpus, all reproduced:

| Where | What |
|-------|------|
| End of a function | `return;` as `Return(Nothing)`, or `Return(ReturnNothing(ReturnValue))` with a return value; none in state code |
| Native functions | their script is a `NativeParameter` per parameter, `Nothing`, `EndOfScript` |
| Optional parameters | one statement each at the head: `DefaultParameter(value)` or `Nothing` |
| Omitted optional arguments | an `EmptyParm` each, trailing ones too |
| Context | the value size (`word2`): element size × array dimension; 0 for strings and arrays; the skip of a context on `Outer` is 5 (the size of the `Outer` variable); a context around an iterator (or array) a `foreach` loops over skips the whole loop |
| Struct members | the member nearest the root variable of an assigned path (through contexts too) has IsModification set; so do the arrays `AddItem`/`RemoveItem`/`InsertItem` change, not `Insert`/`Remove`/`Add` |
| Discarded results | `EatReturnValue(ReturnValue)` before a call whose result needs destroying (strings, arrays) |
| foreach | the iterator's jump goes to its `IteratorPop`; `continue` is `IteratorNext`; `return` pops every enclosing iterator first |
| Casts | to an interface: `InterfaceCast`, `DynamicCast` from a plain `Object`; `int(1)` of a literal casts a byte constant |
| Delegates | `none` assigned is `DelegateProperty None`, compared is `EmptyDelegate` |
| Label tables | `Nothing` up to where the table's entries start on a 4-byte boundary; labels newest first |
| Array intrinsics | the skip is the span after the field (assemble_script's formula for new tokens was 2 more) |

### What stays different, and why

* **`return;` for a jump to the function's end** (96 scripts): the decompiler prints a jump to the trailing return as
  `return;`; compiled back it is a `Return`. The same behaviour, other bytes.
* **`assert` line numbers** (13): the line is in the bytecode, not in the decompiled source.
* **Structuring choices** (about 30: `Jump` read as another statement): the decompiler's reading of some jumps.
* **Third-party packages:** their compiler stores real context skips on `Outer` and leaves out trailing optional
  arguments in places.
* A few single cases (a final call the source doesn't mark, literal casts, `-1.0` as a negation).

## The class package builder

`ScriptCompiler::build_class_package(name, source)` turns a `.uc` class (or interface) into a script package laid out
like the game's: the reflection is written and loaded first, the bodies are compiled against it, then the package is
written again with the bytecode and the default values.

Supported: consts (inlined where used, as the UE3 compiler does), enums (`<Prefix>_MAX` added), structs (modifiers,
`extends`, `structdefaultproperties`), variables (every modifier, static arrays sized by a number, an enum or a const,
component, interface and delegate types), functions and events (modifiers, `native(N)` bound to native table slot N,
optional / out / coerce / const / static-array parameters, defaults), delegates (and their hidden `__X__Delegate`
variables), states (`auto`, `simulated`, `state()`, `extends`, `ignores`, state functions, state code with labels and
latent calls), `defaultproperties` (dynamic arrays with `Arr(i)=`, `.Empty`, `.Add()`, `.Remove()` on the inherited
value; `Begin Object` subobjects, nested ones too, overriding the parent's templates by name), class modifiers
(`within`, `config`, `placeable`, `abstract`, `transient`, `perobjectconfig`, `editinlinenew`, `collapsecategories`,
`hidecategories`, `autoexpandcategories`, `implements` of script interfaces), and `` `log`` / `` `warn``.

Rules found in the game's data and reproduced:

- A class lists its fields as states, functions, consts and structs newest first, then its variables in declaration
  order, then its enums newest first. A state lists its functions newest first; a function its parameters in order.
- Probe masks: bit `index - 300` of the engine's probe names (FName indices 300..363, read from the executable) for each
  probe event a state or class defines, over its super state's (or parent class's) mask. `ignores` clears ignore-mask
  bits. A state without code is `Stop, EndOfScript` with label table `0xFFFF`; state code ends with the compiler's
  `stop`; the label table offset is the memory offset of the LabelTable token.
- A state function's super is the super state's function, else the class-level function of its name (even
  `Core.Object.BeginState`). An override takes only the replication flags and `exec` from what it overrides (not
  `event`, not `simulated`); every function is `public` unless `private`/`protected`.
- Inherited class flags: Config, Transient, Localized, SafeReplace, Placeable, PerObjectConfig, EditInlineNew,
  CollapseCategories, HasInstancedProps, HasComponents.
- Every component the parent class has (its component map) gets the class's own template under its default object,
  archetype the parent's template, as the game's subclasses have; a `Begin Object` of the same name is that template.
  Inherited defaults referring to the parent's templates (`Components`, `CollisionComponent`) are written again,
  pointing at the class's own. Without its own templates, the engine attaches the parent's template itself to the first
  actor spawned (shared, and unusable for the next ones). The cooker leaves some editor-only components (Sprite,
  Arrow) out of a few subclasses; the builder copies them all (9 classes of the round trip differ by that).
- Hand-written code needs UE3's automatic conversions, which the cooked code shows as explicit casts: numbers among
  themselves (widening ranked before narrowing) and numbers, bools and names to strings. There is no byte `==`: bytes
  and enums compare as ints (`int(M) == int(2)`).

Not built (C++ only): replication blocks, native interfaces (their vtable property), `map{}` variables, native classes
and structs (a built class is a script class). Round trip over the game's 2,338 classes (`UNREALLIB_CLASS_ROUNDTRIP=1`:
decompile, build, decompile again, compare): 2,220 build, 2,196 identical (93.9%); the rest differ by a decompiler
notation for some consts (`Profile.400`), a few jump layouts, and the position of hidden delegate variables. (2,187 identical, 93.5%, since every inherited component is copied.)
