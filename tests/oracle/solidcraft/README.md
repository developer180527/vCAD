# SolidCraft reference parts

Copied from [SolidCraft](https://github.com/tangency-labs/solidcraft) `tests/oracle/` at commit
`0db2abbeca4ae5c82d8d560dafd74feccb3bfbe4`: each case's `recipe.json` (the part, as a list of
sketches and features) and `expected.json` (its volume, area, centre of mass, bounding box, body
and face counts). SolidCraft is MIT OR Apache-2.0; these are taken under the MIT licence, whose
text is in `LICENSE-MIT` beside this file.

Where the expected values come from is SolidCraft's statement, in each `expected.json`: `analytic`
values are closed-form geometry worked by hand, `freecad` values were computed by running FreeCAD
as a separate program. Neither comes from vCAD, which is the point -- these check our modelling
against answers we did not produce.

The `reference.stl` files are not copied: they are large, and the harness compares measured
properties, not surfaces.

`tests/acceptance/oracle_solidcraft.cpp` replays these. To update: copy the two files per case from
a newer SolidCraft checkout, change the commit above, and run the test. A case whose meaning
changed upstream shows up as a failure there, not silently.
