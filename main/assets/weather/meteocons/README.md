Meteocons weather source assets

Source:
- Animated SVGs: `@meteocons/svg@0.1.0`
- Static SVGs: `@meteocons/svg-static@0.1.0`
- Upstream project: Bas Milius Meteocons
- License: MIT, see `LICENSE`

Why this directory exists:
- Keep weather assets out of board-specific code.
- Preserve a neutral source format that can later feed both LVGL and Emote pipelines.
- Avoid modifying third-party component directories under `managed_components/`.

Chosen subset:
- A small, reusable flat-style condition set for weather UI experiments.
- Both animated and static variants are included.

Directory layout:
- `flat/animated/`: animated SVG source assets
- `flat/static/`: static SVG source assets
- `condition-map.json`: normalized weather condition to source filename mapping

Notes:
- Filenames intentionally keep the upstream Meteocons names.
- This is a source asset staging area only. Nothing here is wired into build or display code yet.
