# PrusaSlicer 2.x features missing in 3.x

Inventory of what PrusaSlicer 2.9 users had that this 3.x code base does not provide yet
(from reading the code; `MCR` = `src/slic3r-shared/src/Slic3r/App/MenuCommandRegistrar.cpp`).
Shared code (`slic3r-shared`, `libslic3r`) serves both the desktop and the mobile app.

Status: ✅ done in this branch · ⏳ in progress · ☐ open

## Batch 1: small, high value
| | Feature | Notes |
|---|---|---|
| ✅ | Fill bed with instances | MCR `.todo` menu item, empty handler |
| ✅ | Scale to print volume | MCR `.todo` menu item, empty handler |
| ✅ | Mirror object/part (X, Y, Z) | no command in 3.x |
| ✅ | Edit text / SVG from the context menu | MCR `.todo`; the tools themselves work |
| ✅ | Export whole plate as STL/OBJ | export required a single object |
| ✅ | `automatic_extrusion_widths` | visible checkbox that did nothing |
| ✅ | SLA statistics: material cost and weight | `bottle_*`, `material_density` unused, always 0 |
| ✅ | SLA validation (exposure limits, head penetration, pinhead diameter, tilt) | checks were commented out |
| ✅ | `physical_printer_preset` placeholder | custom G-code using it breaks |
| ✅ | Keyboard shortcuts / About / System info dialogs | hidden TODO menu items |
| ✅ | Export toolpaths as OBJ | File > Export in the preview; writes the .obj and its .mtl |

## Batch 2: medium
| | Feature | Notes |
|---|---|---|
| ✅ | Standalone G-code viewer (open .gcode/.bgcode) | File > Import > G-code preview; on Android: open a G-code file. Settings from the file, .bgcode converted |
| ✅ | Sequential printing: extruder clearance collision check | missing in `Print::validate` |
| ☐ | Sequential arrange (`libseqarrange`) | not built |
| ☐ | Tool changes / extruder sequence on the layer slider (multi-tool) | disabled upstream on purpose (`ysFIXME-spe3494`, an internal Prusa ticket); slicing still supports the modes |
| ✅ | Auto color change | layers slider cog menu > Set auto color changes (2.x rule: slice area drops by > 25 mm² and 10 %, at most 3); heights computed by the slicer and passed with the result |
| ✅ | Object info (size, volume, facets, mesh errors) | object context menu > Object info |
| ✅ | Shape gallery | as object or part/modifier/negative/blocker/enforcer; shapes from `resources/shapes` (not shipped: the 2.x shape files are not in this repo) and the user folder `<data dir>/shapes`, filled by "Add shape from file" |
| ✅ | Import / export config (.ini), load config from G-code | File > Import > Import config (.ini, .gcode, .bgcode, 3.x .json) opens a new project with those presets and a copy of the objects; File > Export > Export config (.ini or .json). On Android, opening an .ini imports it |
| ✅ | Custom G-code editor with placeholder list | "{ }" button next to each custom G-code field: G-code specific placeholders, slicing states, statistics, then all settings; inserts at the end (no caret position available) |
| ☐ | Optimize orientation | algorithm not ported |
| ✅ | AMF import | plain and zipped, units, part/modifier/negative/blocker/enforcer volumes, constellation instances; per-object settings are not read |
| ✅ | Print host upload queue window | main menu > Print host upload queue: uploads of the session with state and progress, cancel an active one, clear finished ones (a list dialog, refreshed on demand) |
| ✅ | Bonjour "Browse" for printers | search button next to the host field (not tested: no printer on the test network) |
| ☐ | 2.x profiles: translate `compatible_*_condition` | pinned to one printer model |
| ✅ | Post-processing scripts on Windows | CreateProcessW like 2.x: .exe/.com directly, .bat/.cmd via cmd.exe, other files via their associated program; SLIC3R_* environment, error output shown. **Not compiled or tested here (no Windows toolchain in this environment)** |
| ✅ | Legacy 3MF projects without a resolvable printer | the objects load with the default printer and a message names the missing printer |
| ✅ | Undo of custom G-code / color changes | serialized, slider edits take a snapshot |
| — | Per-project filament colour | intentionally hidden upstream; the sidebar colour picker (`extruder_colour`) covers it |

## Batch 3: large or desktop only
| | Feature | Notes |
|---|---|---|
| ☐ | Configuration wizard (re-run vendor/filament selection) | replaced by Welcome/Add printer dialogs |
| ☐ | Firmware flashing (avrdude) | desktop only |
| ☐ | Wi-Fi config file | |
| ☐ | Configuration snapshots | |
| ☐ | Check for app updates, hints / tip of the day | |
| ☐ | SLA archive import (SL1 → mesh) | |
| ☐ | Linux "Open in PrusaSlicer" URL registration | |
