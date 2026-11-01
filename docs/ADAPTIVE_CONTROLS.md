# Adaptive touch controls

Status: experimental. Six weapon profiles and forty-two original SVG glyphs
are integrated; full weapon/device validation is still open.

The design goal is to adapt MHFU iOS's thumb-zone arrangement and contextual,
weapon-specific action surfaces to Yakumo. Floating movement, the switchable
D-pad, layout editing and the movable item HUD are part of that adaptation.
This document describes implemented behavior and its validation limits. See
[source provenance](SOURCE_PROVENANCE.md#adaptive-touch-controls) for attribution
and public design references.

## Current contexts and controls

| Context | Current arrangement |
| --- | --- |
| Village | Movement, sprint, camera, NPC/context Circle, Square with interaction-hand icon, optional D-pad and Commands. Ordinary X is hidden, including in older saved layouts. Deliberately rebound X entries are preserved. |
| Quest, weapon sheathed | Immediate draw/basic attack, separate sprint and evade buttons, accessible Circle fallback and item tap/swipe. |
| Quest, weapon drawn | Weapon-specific main selector and special action, evade, item/sheath surface, R3 and camera. Unsupported weapons retain ordinary button input. |
| Interface | D-pad, four PSP face buttons, L/R paging and Commands. Combat gestures, item swipes and camera drags are disabled. Also used before a character is loaded. |

Positions stay relative to the safe area. The user can independently edit Quest,
Village and Interface layouts. Floating/fixed movement, the on-screen D-pad
switch and existing HUD placement are deliberate Yakumo features.

The context reader uses observed guest task/menu state, and weapon identity uses
the actual equipped record. A separate validated local quest actor supplies
sheathed/drawn posture. A validated Long Sword transition reader now supplies main-selector follow-ups
and their input-buffer windows. Other weapon combo state, nearby interaction
prompts and Demon mode remain unverified. Unknown context has a safe
Interface fallback and an explicit Interface/Auto override. Unknown posture
retains the earlier attack behavior.

## Compact movement iteration

Sprint and evade are separate buttons in quests, including while the weapon is
sheathed. Hold sprint for immediate R; press evade separately for X. Holding both
sends R + X. Swiping off sprint only releases it and cannot turn into an evade.
Each button releases independently, without a delayed gesture pulse. Existing
placements and custom visibility/bindings are preserved. Village X stays hidden;
interface X remains Cancel. There is no combined sprint/evade setting.

A short free-space tap in village/quest sends native L to recenter the camera.
Dragging still rotates it. Motion beyond the tap tolerance, even if it returns
to its origin, cancels tap recognition. Movement, buttons, item swipes, interface
backgrounds and Commands dismissal cannot also trigger recentering. Active or
queued item input takes priority, so reset cannot change item use into L+Square
selection. The 200 ms
limit and height-relative slop are Yakumo choices, not measured iOS values.
`MHP3RD_ADAPTIVE_COMPACT=0` disables the camera-tap shortcut for comparison.
Sprint and evade remain separate in either mode.

This iteration uses the existing posture and contact ownership. It does not add
combo-state guesses, new guest hooks, settings, timers or permanent buttons.
The sprint control has no gesture arrow.
Phone ergonomics and game action timing still need manual verification.

## Input ownership and cancellation

A contact owns one role: movement, attack, guard, ordinary button, D-pad, item
surface or free-space camera. Sliding outside an ordinary button does not convert
it into another action. Posture changes cancel old attack/special contacts while
preserving movement, camera, evade and item selection. Context, weapon, layout,
input-source and focus changes cancel stale contacts and pending item input.

The attack selector waits up to 110 ms before selecting a held primary action.
A released tap uses an 85 ms pulse. Direction recognition latches one action for
that contact. These are Yakumo timing choices, not measured MHFU constants.
Great Sword charge and Sword and Shield guard have immediate release semantics
for held actions; the guard selector may recognize a later flick from a block.

The [profile README](../profiles/mhp3rd/README.md#adaptive-touch-layout) lists the
exact experimental PSP input maps. Long Sword lateral fade requires an earlier
attack, as confirmed by the tester; it must not be forced from idle. The four
newer weapon profiles need real-game verification. They are not claimed to
reproduce every MHFU move or P3rd animation rule.

## Long Sword combo availability

The local hunter's current transition table supplies primary strike, thrust,
backward fade and lateral fade choices. Each original flip reads its animation
position, step and duration, plus the transition's start/end and input-buffer
window. Arrow directions are not inferred from the last pressed button.

At idle, the normal main-selector actions remain available without side arrows.
During an eligible combo window the corresponding directions appear, including
side fades. Recovery, draw/sheath and evade transitions suppress main-selector
input. Unknown metadata or unsupported predicates preserve ordinary gestures and
show no speculative arrows. Great Sword, Hammer and Dual Blades instead show
fixed gesture legends; these are not live combo availability.
The separate special/guard control is unchanged.

Unavailable directional gestures are canceled without becoming Triangle or moving
the analog stick. A canceled direction cannot activate when the window changes;
lift to start a new gesture. Held output and release pulses stop when their route
closes. Context, posture and weapon changes retain the existing release rules.
The selector is dimmed when a verified state has no main action.

`MHP3RD_ADAPTIVE_COMBOS=0` restores unrestricted gestures and removes availability
arrows for comparison. The reader is bounded to 48 rows, validates code/layout,
actor identity and all memory ranges, and writes no guest state. The initial
manual recording covered Long Sword idle, normal attack, thrust, backward/side
fades, evade and sheathe; unobserved states fail back to ordinary input. Full
weapon/device/multiplayer validation is still required.

## Compact Commands

The small three-dot root opens five named commands: Yakumo, Game Menu,
Select, Camera/L and Interface/Auto. Select uses an **SEL** glyph and sends the
native PSP Select input; its action depends on the current game context. In Interfaces, the L command is
labelled Previous Page. Ordinary interface L/R paging remains directly available.

Opening the panel releases gameplay contacts. A command highlights while held
and commits only on release over its target. Cancellation or dragging away sends
nothing; Start, Select and L cannot leak into the game while the panel is open.
Committed PSP commands use a bounded pulse. Tapping outside dismisses the panel
and discards the dismissal contact, including later movement of that finger.

Expanded targets aim for a 44-pixel diameter while fitting small/portrait safe
areas and their labels. Keep R3 separate: lock-on is a combat action and should
not need the utility panel. `MHP3RD_ADAPTIVE_COMPACT=0` restores the preceding
separate controls for comparison without overwriting saved arrangements.

## Item HUD and fallback

The item touch surface follows the player's movable/scalable quest HUD block.
Horizontal swipes select one item per contact; lift before selecting again.
Recognized swipes retain L while held. A bounded FIFO serializes rapid swipes;
L preparation advances only on pad reads consumed by the game. Vertical motion
becomes camera input. Canceled contacts, recognized swipes and long holds do not
also use an item on release. Explicit buttons win where areas overlap.

An item tap sends ordinary Square: use while sheathed, sheathe while drawn.
The exception is a confirmed drawn Sword and Shield: the tap sends native
R + Square to use the item without sheathing. All six supported weapon layouts
keep a separate Square/sheath button while drawn, using the existing editable
Interact / Sheathe placement. Its icon is the sheathing symbol. Explicitly hidden
or rebound placements remain respected; sheathed/unknown states use the item
surface unless it is disabled, in which case ordinary Square remains available.
Unknown posture keeps ordinary Square. Horizontal selection never adds R.
There is no automatic use queued after sheathing and no direct inventory write.

If the game HUD is hidden/unavailable, an editable strip supplies the same input.
Its glyph changes to sheathing when drawn, except SnS retains the item symbol. If the player
hides this touch surface in the editor, the enabled ordinary Square control
becomes visible again. Outside known drawn weapon layouts, compact mode hides
a duplicate only while its replacement is enabled; a deliberately rebound Square entry remains visible.

The real game-HUD image is not repainted by this fallback glyph. Distinguishing
its drawn/guarded item availability is still a presentation task.

## D-pad appearance

Adaptive uses a single bevelled cross with a dark teal body, pale rim and muted
gold inset. Ivory chevrons show the four directions; a pressed arm and chevron
turn gold, including both arms for diagonals. The centre is connected visually
but retains the existing input dead zone. Touch geometry, diagonal recognition,
switchable visibility and editor placement/scale are unchanged. The game and
layout editor use the same drawing routine.

## Visual assets

- [Common glyph sources](images/adaptive-controls/README.md) and
  [manifest](images/adaptive-controls/manifest.json): 24 original glyphs.
- [Weapon glyph sources](images/adaptive-weapons/README.md) and
  [manifest](images/adaptive-weapons/manifest.json): 18 weapon/action glyphs.

The original SVGs are the authoring sources. The offline stdlib-only tool
`profiles/mhp3rd/tools/touch_icons.py` bakes both manifests into bounded constexpr
geometry in `host/ui/touch_icons_data.hpp`. Run it after asset edits; `--check`
rejects stale output. There is no runtime SVG parser or new image dependency.
The renderer and editor use the same geometry, palette and button frames.

Confirm is green, Cancel red; held controls use restrained feedback. The main
selector cap moves a bounded distance toward the selected direction. Long Sword arrows show the currently accepted directional choices; unknown
weapon/state readers retain neutral controls. Hammer's
ordinary side input does not display a charged-spin symbol.

The primary attack surface has a persistent inner cap, visually distinct from
ordinary buttons. A chosen direction moves that cap. Direction hints are larger
outlined chevrons outside the circle, with a gold, slightly larger selected hint;
their minimum opacity keeps them readable without expanding the touch target.
Long Sword uses verified availability; the other basic profiles use fixed gesture legends;
unverified controls do not advertise combo routes. Sword and Shield's guard
surface uses the same gesture presentation. Unsupported weapon profiles display
ordinary Triangle/R controls rather than Long Sword/Spirit symbols. This changes
presentation only; input mapping and saved placement are unchanged.

All SVGs are independently constructed from simple geometry. Reference images
were not traced/extracted into these assets. See [source provenance](SOURCE_PROVENANCE.md#adaptive-touch-controls)
for the original PR #342 prototype credit and implementation boundaries.

## Sword and Shield mapping update

The main selector sends Triangle on tap/hold, Triangle + Circle upward, Select
rightward (native spin), and neutral-touch Circle downward (horizontal chain).
The left flick retains Circle with the player's movement. The explicit forward
shield-strike adapter in the mapping matrix still needs in-game verification.
Only the downward action suppresses touch-stick output, including its 85 ms
release pulse; it preserves the movement contact and restores its current value
on expiry/cancellation. Physical keyboard/gamepad movement remains independent.
The shield surface still provides immediate R guard, R + Circle sideways,
R + Triangle downward. Upward motion keeps guarding and leaves other shield
gestures available; evasion uses the separate button. The shield circle and its
touch target are 35% larger while the SnS selector is active, with proportional
editor scaling and unchanged saved coordinates. Sprint, interface R and other
weapons retain their existing sizes. A drawn SnS item tap supplies R + Square
without requiring a second finger on guard; the separate sheath action sends Square.
These mappings have synthetic coverage; gameplay timing and shield direction
remain manual checks. Unknown SnS combo state does not invent availability arrows.

## Great Sword layout

The main selector uses tap/hold or up for Triangle, right for Circle (sweep),
down for Triangle + Circle (rise) and left for Select (native kick). Left is a
P3rd adaptation, not a claim about every contextual FU iOS gesture. Charge
releases immediately on lift, including a short upward flick; quick ordinary
taps retain their sampleable pulse. Player movement remains independent: after
a side slap, move the left stick and hold the main selector to request the
native strong-charge continuation. No guessed direction, charge level or
slap-window state is injected.

The separate guard control sends R immediately and retains its finger outside
the circle until lift/cancel. Guard plus a main tap can request native R + Triangle
kick. Evade remains separate. The left-kick icon stays a neutral weapon symbol
until a dedicated original glyph is available. Main arrows are fixed gesture
legends, not a promise that the current animation accepts each action.

## Hammer and Dual Blades layouts

Hammer uses tap/hold Triangle for its native chain and charge/spin follow-ups,
and a right flick for Circle (side swing). Up/down/left are unassigned; lifting
such a flick never turns it into a delayed Triangle. The separate charge control
sends R immediately, captures its finger through movement outside the circle,
and releases R immediately on lift, even after a short hold. Movement stays live
so the game chooses the stationary or moving charge result. The separate evade
button sends native X during charge; no duplicate upward evade is added.

Dual Blades uses tap/hold Triangle, up for Triangle + Circle and sides for Circle
(left also supplies left analog). Down is unassigned. The game itself selects
Normal, Demon or Archdemon actions from those requests; Yakumo does not infer
mode from the last R press. Its separate R mode button is immediate and retains
an 85 ms release pulse so a quick tap can be sampled by the native game tick.
Cancellation, posture/weapon/context changes clear captured secondary input.
Sprinting with sheathed weapons and custom secondary bindings remain ordinary
buttons. Charge cancellation from an OS focus/touch loss only releases host
input; it cannot guarantee which animation the game chooses on that release.

External arrows for these two main selectors are fixed gesture legends, not
live combo-window availability. No charge meter or active-Demon indicator is
invented. Side-swing/double-slash icons remain neutral about charged spins and
mode-specific follow-ups. All attack timing and resource gates belong to the
game; one gesture requests one native input, not a timed automatic combo.

## Supported weapons and deferred classes

Quest Adaptive layouts are available for Long Sword, Great Sword, Sword and
Shield, Dual Blades, Hammer and Bow. Hunting Horn, Lance, Gunlance, Switch Axe,
Light Bowgun and Heavy Bowgun are not supported yet. Their class names remain
recognizable for the warning, but they have no gesture routes or weapon icons.

Equipping a deferred class immediately opens a centered **Adaptive Controls**
notice, including in the village or the native equipment menu. It identifies the
weapon, explains that its Quest controls will be hidden, and offers a gold
**Close** button matching Yakumo's menu. During quests, every Adaptive control is
hidden, including movement, D-pad and Commands, and previous touch input is
canceled. Dismissing the notice keeps it closed across scene transitions, brief
loading gaps and same-class equipment changes. Changing to another deferred
class shows a new notice. Village/NPC and native-interface controls remain
usable. Switching to PSP/Action or a supported weapon restores those controls;
physical keyboard/gamepad mappings are not disabled.

Bow retains charge/release, melee and the late arc-shot input. Its separate
coating surface uses L + Triangle/Cross with the existing actual-read-counted
lead-in. One swipe selects one entry; a tap requests attach/remove coating.
Selection cannot interrupt an active charge. Custom bindings and hidden
placements remain respected. See the aim-toggle behavior below.

## Bow aim-toggle candidate

The combined charge/aim-stick experiment was reverted. Outside aim, Bow keeps
its original main gestures: hold/release Triangle, down Circle melee and a late
right flick for the native arc input. The secondary scope button now toggles aim
on a deliberate tap; entering aim holds R without charging or owning the camera.
Free screen drags remain camera motion, including while the other hand fires.

During aim the main selector becomes a plain charge/fire button. An editable
**Left Fire Button** appears on the left and duplicates it. Hold either to charge;
release to shoot. A short tap is retained for three actual guest reads if needed.
Both surfaces share one owner: another finger cannot release or repeat the first
finger's shot. Fire contacts never become camera or melee gestures. Free-space
taps do not send L recentering while aiming. Tap the scope button again to exit.

The left duplicate is shown only while aiming with a drawn Bow and standard
main/secondary bindings. Its position, size and visibility are editable in the
Quest layout with Weapon Preview set to Bow. Existing placements are preserved.
A custom main or secondary binding keeps the previous ordinary/gesture behavior.
`MHP3RD_ADAPTIVE_BOW_TOGGLE=0` restores the previous held-R comparison layout.
Bowgun Adaptive layouts are deferred.

Exit during charge, canceled touch, Evade and host-menu entry retain Triangle
while requesting native X cancellation for three guest reads, then release it.
Host-menu entry also clears aim; canceled fire alone and Evade keep the intended
aim toggle. Weapon/posture/context changes discard combat state before exposing
interface controls. This ordering is covered synthetically, but actual no-shot
cancellation, stamina exhaustion and animation gates require device testing.
No synthetic test proves an arrow already accepted by the game can be undone.

Item/coating interaction exits idle aim first so its chords do not inherit R;
selection cannot interrupt an active charge. Melee and arc gestures are available
outside aim, rather than competing with free-screen camera motion inside it.

## Touch aim axis arbitration

Android Bow traces confirmed that the native aim services only pitch when shown
both axes together. Turning tiny vertical finger noise into a full diagonal
therefore blocked horizontal motion; accumulated yaw was later spent in a jump.

Touch aim now requests the axis with the larger outstanding angle, using the
same cardinal request for native input and advance step correction. The other
axis retains its distance for a later update, including fractional angle units.
Deliberate deferral is not counted as a request rejected by a game animation.
Blocked requests still expire, and the game retains authority over when it may
turn. Real mouse input retains its existing direction path.

`MHP3RD_TOUCH_AIM_CARDINAL=0` restores the previous touch direction conversion for
comparison. An Android tester confirmed responsive Bow aiming after this fix
on 2026-11-01. Other weapons and broader device coverage still require testing. Aim remains governed by the game's update
rate, so this does not promise continuous simultaneous updates of both axes.

## Touch targets and editor previews

Small circular controls have a little extra invisible padding: at least the
existing 15%, up to 30% of their radius, limited by the safe-area height. A
visible button wins over a neighbour's padding; overlapping visible buttons
still choose the smaller control. Saved positions, sizes and HUD placement are
unchanged. The selected editor circle shows its actual touch margin.

Only one free-space finger controls the camera. A second contact cannot add
motion or take over when the first lifts; lift and touch again to acquire it.
Vertical item swipes yield to an existing camera/aim owner without using an
item. Movement, attack and secondary controls retain their independent owners.

In the Quest layout editor, **Weapon Preview** cycles the six supported weapons without
changing equipment or saved placements. Selecting an attack/special control
shows its current gesture guide. Preview arrows are fixed mapping legends,
not claims of live combo availability. Village and Interface previews stay
separate. Rebinding a control replaces its gesture guide with ordinary-input
help.

While an Adaptive element is dragged, the options panel stays on its initial
side, becomes translucent and stops receiving pointer input. On release it
becomes interactive again and chooses the opposite side of the selected control.

## Validation and remaining work

Public synthetic suites cover input timing, direction selection, cancellation,
short pulses, independent owners, posture/context changes, item FIFO sampling,
custom layouts, hidden-HUD fallback, Commands commitment and rendered geometry.
Actual SDL/Vulkan/ImGui captures exercise the controls, editor, command panel,
weapon symbols and item/sheath strip. Asset tests validate SVG bounds, restricted
syntax, semantic colors and generated-data freshness.

Windows tests and cross-compilation do not establish Android multi-touch,
haptics, ergonomics or gameplay. Native changed-line coverage is not measured by
the Windows test build. See [TESTING.md](TESTING.md) for automated contracts and
manual verification scenarios.

The next implementation steps are:

1. Verify the six supported profiles in quests, including hold/release,
   guard/item interactions and multiplayer. Preserve working Long Sword gestures.
2. Extend verified transition reading to the other weapon profiles and trace
   interaction prompts. Do not infer availability from recent touch history.
3. Add truthful context icons and only the legal direction hints. Keep a
   reachable Circle fallback until prompt detection covers all interactions.
4. Verify ranged aim/fire ownership and ammunition on actual devices. Keep
   Switch Axe and Hunting Horn adaptations specific to P3rd, not FU mechanics.
5. Validate title, dialogues, vendors, chests and results on actual phones and
   tablets, including moved/hidden HUD blocks and alternate aspect ratios.