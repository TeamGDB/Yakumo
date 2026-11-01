# Adaptive weapon icons

Eighteen original MIT-licensed weapon/action glyphs for Great Sword, Sword and
Shield, Dual Blades, Hammer and Bow. Long Sword and shared controls use the
[common icon pack](../adaptive-controls/README.md). Optional action variants are
source assets, not additional permanent touch buttons.

Glyphs use a transparent 64x64 view box, ivory `currentColor` and the shared
button style. `manifest.json` lists the runtime sources; Hunting Horn, Lance,
Gunlance, Switch Axe and both Bowgun icon sets are deferred and excluded.

The offline tool `profiles/mhp3rd/tools/touch_icons.py` converts both manifests
to bounded ImGui geometry. Run it after source edits, then use `--check` to
verify generated output. The runtime does not load SVG files.

Independent vector drawings under the repository MIT license (included).
No original game artwork, screenshots, models or extracted assets are included.
Glyphs have no embedded images, scripts or external resources. See
[source provenance](../../SOURCE_PROVENANCE.md#adaptive-touch-controls) and
[Adaptive controls](../../ADAPTIVE_CONTROLS.md) for behavior and validation limits.
