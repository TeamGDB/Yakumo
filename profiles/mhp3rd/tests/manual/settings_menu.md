# Settings menu and performance overlay regression

Use the `settings-menu` and `performance-overlay` device scenarios on each local
platform. Start with `make device-prepare` for a checklist, or `make device-run
DEVICE=<device> DEVICE_SCENARIO=settings-menu DEVICE_ARGS=--manual` for a bounded
session. These scenarios deliberately require observation: the game has no
reliable log marker for the selected menu row. Fixed navigation scripts and a
successful boot alone must not turn a visual check into PASS.

Keep the exact artifact hash/commit, fixture settings, input method, display
size and captures in the report. Use copied settings/saves and no multiplayer
connection. Complete two bounded sessions when a restart is needed. Record
agent image reviews independently from a person's physical-control assessment.
Set `"performance": "settings"` in the private device configuration so the
harness does not lock the row. Pack Android with `DEVICE_TEST_PERFORMANCE=settings`.

1. Open the menu with Escape, the configured controller menu gesture or the
   touch menu icon. Capture Video and visit every section in both directions.
   Include a narrow-window/touch-sized check of the scrolling tab strip.
2. Capture QoL. Change one reversible setting and test its dependent disabled
   row. Open Layered Armor and return with Back. Visit Mods and System, opening
   and cancelling their browsers without importing anything.
3. Close/reopen on a scrolled page. Capture retained selection/focus. Restore
   QoL defaults with distinctive video/audio/network settings, then compare
   their copied settings values. Keep environment overrides recorded separately.
4. On a stable scene, capture Off, full Overlay, Compact and Compact and Log.
   Let a fresh performance summary appear before judging FPS/speed values.
   Separate log intervals for each mode; old log lines are not fresh evidence.
5. On keyboard-capable desktops, capture Compact before/after F3 twice. Verify
   the saved mode/log choice does not change. On a touch-only device, record the
   keyboard-specific check as unavailable and exercise menu mode changes instead.
6. Repeat a compact capture at a second resolution/aspect, restore the original
   values, and restart the same artifact with the copied settings. Check the
   persisted mode and inspect the log for errors. Bound all runs and confirm
   that only the test-owned process stopped.

Native regression suites cover mode parsing, bounded pixel buffers, glyphs,
real Vulkan framebuffer/swapchain captures, keyboard toggles, CRLF scripts,
virtual controller navigation and settings restoration. Run the full relevant
suites before device review. Android emulator/software rendering is a separate
row from physical GPUs; SSH Windows and Deck headless launches do not certify
visible UI or Game Mode. Audio, hunts and multiplayer remain separate scenarios.
