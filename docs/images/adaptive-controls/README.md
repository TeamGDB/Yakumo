# Adaptive control icons

Twenty-four original MIT-licensed SVG glyphs used by Yakumo's Adaptive controls.
Each glyph uses a transparent 64x64 view box and `currentColor`. Confirm is green
(`#79CC8A`); Cancel is red (`#EB6B67`). Separate ordinary/pressed button frames
are also included.

`manifest.json` lists the runtime glyph sources. The offline tool
`profiles/mhp3rd/tools/touch_icons.py` bakes both icon manifests into
`profiles/mhp3rd/host/ui/touch_icons_data.hpp`. Run it after editing a glyph;
`--check` verifies that generated geometry is current. No SVG parser or image
loader is required at runtime.

These are independently drawn vector assets. No original game artwork was
extracted or traced. See [source provenance](../../SOURCE_PROVENANCE.md#adaptive-touch-controls)
and [Adaptive controls](../../ADAPTIVE_CONTROLS.md).
