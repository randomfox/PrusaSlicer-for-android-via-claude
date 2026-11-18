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
| ☐ | Export toolpaths as OBJ | backend exists (`libvgcode`), no UI |

## Batch 2: medium
| | Feature | Notes |
|---|---|---|
| ⏳ | Standalone G-code viewer (open .gcode/.bgcode) | file loads and is processed (settings from the file, .bgcode converted), opens in a new project's Preview; toolpaths not shown yet (the injected result is probably cleared by the empty project's slicing) |
| ✅ | Sequential printing: extruder clearance collision check | missing in `Print::validate` |
| ☐ | Sequential arrange (`libseqarrange`) | not built |
| ☐ | Tool changes / extruder sequence on the layer slider (multi-tool) | disabled upstream on purpose (`ysFIXME-spe3494`, an internal Prusa ticket); slicing still supports the modes |
| ☐ | Auto color change | stub |
| ☐ | Object info (size, volume, facets, mesh errors) | no UI |
| ☐ | Shape gallery | MCR `.todo` items, `resources/shapes` missing |
| ☐ | Import / export config (.ini), load config from G-code | YAML presets only |
| ☐ | Custom G-code editor with placeholder list | plain text fields |
| ☐ | Optimize orientation | algorithm not ported |
| ☐ | AMF import | |
| ☐ | Print host upload queue window | notifications only |
| ✅ | Bonjour "Browse" for printers | search button next to the host field (not tested: no printer on the test network) |
| ☐ | 2.x profiles: translate `compatible_*_condition` | pinned to one printer model |
| ☐ | Post-processing scripts on Windows | done for Linux, macOS and Android (this branch) |
| ☐ | Legacy 3MF projects without a resolvable printer | load empty |
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
