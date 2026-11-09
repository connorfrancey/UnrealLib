# T3D

`UnrealLib::T3D` (`<UnrealLib/T3D/T3D.hpp>`, target `UnrealLib::T3D`) reads, writes, checks, produces and imports
T3D, the line-based text format UnrealEd uses for copy/paste and map export.

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
| `validate(document)` | Warnings: actors without `Class=`/`Name=` (`t3d.actor-without-*`), duplicate sibling names (`t3d.duplicate-name`), a `Map` without `Level` (`t3d.map-without-level`), `Components` entries naming no subobject of the actor (`t3d.unresolved-subobject`), polygons with fewer than 3 `Vertex` lines (`t3d.degenerate-polygon`). |
| `export_object(linker, object, "Actor"/"Object", options)` | One export as a block: `Class`/`Name`/`Archetype` attributes, a nested `Begin Object` per subobject (exports whose outer is the object, depth-limited), then its tagged properties. Brush models are written as `Begin Brush` (below). |
| `export_level(linker, package, options)` | Every `Level` export as `Begin Map / Begin Level / Begin Actor …`; actors are the level's exports deriving from `Engine.Actor`. |
| `import_document(model, linker, package, document, options)` | Applies an edited export to a `PackageModel` of the same package (below); returns counts of matched/modified/skipped objects. |

Values are kept as text. A `=` inside parentheses, double quotes or `Class'…'` names does not split a property;
`Name(i)=` and `Name[i]=` indices are both accepted and the bracket style is preserved.

### Export conventions

* Property values use `format_property_value()` (defaultproperties syntax). Floats are written in their shortest
  round-trip form; NaNs other than the default quiet ones as `nan(0xBITS)`. Decoded dynamic arrays are written as
  `Name(i)=` per element, and an empty one as `Name=()` (so that importing keeps it); static-array elements with a
  non-zero index as `Name(i)=`, element 0 unindexed (as stored in the property tags).
* Object references to subobjects of the same object are bare names (`Components(0)=StaticMeshComponent0`); other
  references are `Class'Package.Path'` (exports are qualified with the package name).
* A `Model` subobject (the geometry of a brush or volume) is written as UnrealEd's model exporter does, from
  `Assets::read_model`/`read_polys`:

  ```text
  Begin Brush Name=Model_3
     Begin PolyList
        Begin Polygon Flags=3584 Link=0 LightingChannels=3
           Origin   -00128.000000,-00128.000000,-00128.000000
           Normal   -00001.000000,+00000.000000,+00000.000000
           TextureU +00000.000000,+00001.000000,+00000.000000
           TextureV +00000.000000,+00000.000000,-00001.000000
           Vertex   -03232.000000,-08560.000000,-00864.000000
           ...
        End Polygon
     End PolyList
  End Brush
  ```

  Polygon attributes are written only when not default: `Item=` (ItemName), `Texture=` (material path name),
  `Flags=`, `Link=` (iLink ≠ -1), `ShadowMapScale=` (≠ 32), `LightingChannels=` (≠ 0). Vector components use
  UnrealEd's `%+013.6f`. A Model that cannot be decoded falls back to `Begin Object` and is reported as
  `t3d.brush-not-decoded`.
* Pass a `ReflectionSchema` in `ExportOptions::schema` so script structs and arrays decode; without it such values
  are written in the raw notation and reported as `t3d.raw-value`.

`UnrealPackageExtractor -t levels <map>.me1 -o out` writes `out/<Package>/<Package>.t3d`.

## Import

`import_document()` is the way back: edit the exported text, apply it to a `PackageModel` made from the same
package, and write the package.

```cpp
const Package& level = linker.load("Subway_Train.me1");
PackageModel model = PackageModel::from_package(level);
T3D::ImportOptions options;
options.schema = &schema; // the ReflectionSchema used for the export
Diagnostics diagnostics;
const T3D::ImportResult r = T3D::import_document(model, linker, level, T3D::parse(text), options, &diagnostics);
save_package(model, "out/Subway_Train.me1");
```

* Blocks are matched to existing exports by name along the `Map → Level → Actor → Object` nesting; `Begin Brush`
  matches the actor's Model. Creating objects is not supported: a block naming a missing object is skipped with
  `t3d.import-new-object`; a block whose `Class=` differs from the export's class with `t3d.import-class-mismatch`.
* Each matched object's tagged properties are rebuilt from its block. Values are parsed by
  `parse_property_value()` (the inverse of `format_property_value()`), with the stored value as the type template:
  existing properties keep their tag and position, properties missing from the block are removed (absent = default,
  as in UnrealEd), new properties are typed by the schema and appended (`t3d.import-unknown-property` if the schema
  does not know them). References are resolved as the export writes them: bare names to the object's subobjects,
  `Class'Package.Path'` to exports and `Class'Path'` to imports; references to objects the package does not import
  yet are errors.
* A value that does not parse, or a reference that does not resolve, leaves that object unchanged
  (`t3d.import-failed`, with the reason).
* Only objects whose property list actually changes are rewritten (`replace_properties()`: UObject header and native
  data are kept byte for byte), so importing an unmodified export writes nothing.
* Brush polygons: a `Polygon` block that equals the export of the stored polygon keeps the stored polygon exactly
  (the 6-decimal text cannot reproduce every float); others are parsed (attributes absent from the text take their
  defaults; Actor, iBrushPoly and LightMapScale come from the stored polygon at that position) and the Polys export
  is rewritten with `Assets::write_polys()`. Polygons can be edited, added and removed; the Model's BSP is **not**
  rebuilt, so the game keeps colliding/rendering with the old BSP until the level is rebuilt in an editor.

`UnrealT3DImporter <map>.me1 <edited>.t3d -o <out>.me1 [--compression none|zlib|lzo] [-n]` applies a file from the
command line (`-n`: report the counts without writing; exit code 1 if anything was skipped).

## Verification

`MirrorsEdgeT3DTests` exports the levels of Mirror's Edge map packages with a `ReflectionSchema`, checks one Actor
block per Actor-derived export, `serialize → parse → serialize` stability, that every brush is exported as a
`Begin Brush` block, and that `validate()` finds no missing names/classes, duplicates, unresolved components or
degenerate polygons. Each exported document is then imported back into a `PackageModel` of its package, which
must change and skip nothing: every value of every object parses back to exactly the stored value. `SampleLevels`
(every 40th map package) runs by default with fixtures; `FullCorpusLevels` needs `UNREALLIB_FIXTURE_SWEEP=1`.
`EditRoundTrip` moves an actor, adds a schema-typed property to another and moves a brush vertex in Subway_Train,
writes the package, reloads it and checks that its export is exactly the edited document.

Result on the Mirror's Edge PC release: 680 levels, 152,965 actors, 185,542 `Begin Object` subobjects, 7,958 brushes
(48,103 polygons), 2,880,354 properties, no value left raw, no validation warnings, every document re-parsed and
re-serialized identically, and all 338,507 actors/subobjects imported back with no change (including NaN-valued
floats, which the game stores in a few spline actors and the export writes losslessly as `nan(0xFFFFFFFF)`).
`FullCorpusLevels` takes about 6 minutes in a Debug build.

## Limitations

* Other native (non-property) data is not exported: the level's BSP tree, terrain heights and other
  serialized-after-properties data.
* Import modifies existing objects only: new actors/subobjects (which need class defaults, archetypes and native
  data) are not created, and references to objects the package does not already import are not added as imports.
* Import does not rebuild the BSP of edited brushes.
* Exported text follows UnrealEd's layout but is not byte-compared against an UnrealEd export (none ships with the
  game).
