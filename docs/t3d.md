# T3D

`UnrealLib::T3D` (`<UnrealLib/T3D/T3D.hpp>`, target `UnrealLib::T3D`) reads, writes, checks and produces T3D, the
line-based text format UnrealEd uses for copy/paste and map export.

```text
Begin Map Name=Edge_Pt1
   Begin Level NAME=PersistentLevel
      Begin Actor Class=StaticMeshActor Name=StaticMeshActor_12 Archetype=StaticMeshActor'Engine.Default__StaticMeshActor'
         Begin Object Class=StaticMeshComponent Name=StaticMeshComponent0 Archetype=StaticMeshComponent'Engine.Default__StaticMeshActor:StaticMeshComponent0'
            StaticMesh=StaticMesh'Pkg.Group.Mesh'
         End Object
         Components(0)=StaticMeshComponent0
         Location=(X=1024.000000,Y=-512.000000,Z=96.000000)
      End Actor
   End Level
End Map
```

## API

| Function | Purpose |
|----------|---------|
| `parse(text)` | Text → `T3DDocument` (blocks, `Begin` attributes, properties, other lines such as polygon `Vertex` data). Throws `Error(InvalidData)` naming the line for unbalanced/mismatched blocks, content outside a block, malformed `Begin` lines, invalid indices and unterminated strings. |
| `serialize(document, indent)` | Document → text, 3-space indentation by default (UnrealEd's). The parsed order of properties, lines and nested blocks is kept, so UnrealEd-formatted text re-serializes byte-identically; `parse(serialize(d)) == d` for every parsable document (property-based test). |
| `validate(document)` | Warnings: actors without `Class=`/`Name=` (`t3d.actor-without-*`), duplicate sibling names (`t3d.duplicate-name`), a `Map` without `Level` (`t3d.map-without-level`), `Components` entries naming no subobject of the actor (`t3d.unresolved-subobject`). |
| `export_object(linker, object, "Actor"/"Object", options)` | One export as a block: `Class`/`Name`/`Archetype` attributes, a nested `Begin Object` per subobject (exports whose outer is the object, depth-limited), then its tagged properties. |
| `export_level(linker, package, options)` | Every `Level` export as `Begin Map / Begin Level / Begin Actor …`; actors are the level's exports deriving from `Engine.Actor`. |

Values are kept as text. A `=` inside parentheses, double quotes or `Class'…'` names does not split a property;
`Name(i)=` and `Name[i]=` indices are both accepted and the bracket style is preserved.

### Export conventions

* Property values use `format_property_value()` (defaultproperties syntax). Decoded dynamic arrays are written as
  `Name(i)=` per element; static-array elements with a non-zero index as `Name(i)=`, element 0 unindexed (as stored
  in the property tags).
* Object references to subobjects of the same object are bare names (`Components(0)=StaticMeshComponent0`); other
  references are `Class'Package.Path'` (exports are qualified with the package name).
* Pass a `ReflectionSchema` in `ExportOptions::schema` so script structs and arrays decode; without it such values
  are written in the raw notation and reported as `t3d.raw-value`.

`UnrealPackageExtractor -t levels <map>.me1 -o out` writes `out/<Package>/<Package>.t3d`.

## Verification

`MirrorsEdgeT3DTests` exports the levels of Mirror's Edge map packages with a `ReflectionSchema`, checks one Actor
block per Actor-derived export, `serialize → parse → serialize` stability, and that `validate()` finds no missing
names/classes, duplicates or unresolved components. `SampleLevels` (every 40th map package) runs by default with
fixtures; `FullCorpusLevels` needs `UNREALLIB_FIXTURE_SWEEP=1`.

Result on the Mirror's Edge PC release: 680 levels, 152,965 actors, 201,458 subobjects, 2,880,268 properties, no
value left raw, no validation warnings, every document re-parsed and re-serialized identically.

## Limitations

* Native (non-property) data is not exported: brush geometry (`UModel`/`UPolys` → `Begin Brush`/`PolyList`), BSP,
  terrain heights and other serialized-after-properties data. Brush actors are exported with their properties only.
* Importing T3D into a package (creating exports from blocks) is not implemented; T3D documents can be parsed,
  edited and re-written as text, and packages can be modified through `PackageModel` (see the writer).
* Exported text follows UnrealEd's layout but is not byte-compared against an UnrealEd export (none ships with the
  game).
