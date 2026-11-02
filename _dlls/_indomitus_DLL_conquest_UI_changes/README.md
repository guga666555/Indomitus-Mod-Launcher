# GEM ConquestUI 0.4.2

**0.4.2:** the zoom needs Ctrl by default (`zoom_modifier=ctrl`). The plain wheel scrolls the research tree as in
vanilla again; Ctrl+wheel zooms. Set `zoom_modifier=` (empty) for the old behaviour.

**0.4.1:** review and cleanup, no change in behaviour. Two defensive fixes from an independent code review: a
`zoom_modifier` value of 16+ characters no longer leaves an uninitialised string behind, and a reveal queue without
its list sentinel (never seen in game) is left alone instead of being walked. Pattern scans are clamped to the end of
the code section. The portrait toggle log line is capped at 16 entries, and reopening the tree at a remembered zoom
logs it. Slot and offset names, the header's engine facts and a few comments were brought in line with the code.

**0.4.0:** the SHOW LOCKED / HIDE LOCKED button is localized. Its texts are the keys
`main_menu2/dynamic_campaign/gem_show_locked` and `main_menu2/dynamic_campaign/gem_hide_locked`. The DLL looks them up
with the engine's own localization calls, the ones the research tree uses for "Unselect All". The mod ships the English
texts in `mod_side\localizations\default\interface\text\dlg_gem_conquestui.pot`; install it into
`mods\Indomitus_002\localizations\default\interface\text\`. A key with no text shows the English default.
`mod_side\DynamicCampaignOverview.xml` uses the key too (`<Text key=.../>`).

**0.3.3:** wheel notches within `zoom_delay` ms (150 by default) of the last zoom level are ignored, so a fast wheel
spin cannot pile up re-layouts of the whole tree. Ignored notches are swallowed; they don't scroll the tree. Code
review and cleanup: the zoom's leftover anchor state is gone, and its ini reading has its own function.

**0.3.2:** the smooth zoom of 0.3.0/0.3.1 is removed again. Gliding between levels re-laid out every icon and line on
each frame and lagged in game. Each wheel notch jumps straight to the next level, as in 0.2.x, and keeps the point
under the mouse in place. The `zoom_smooth` / `zoom_interval` ini keys are gone. Kept from 0.3.0: 20% per notch and
the faster icon lookup.

**0.2.1:** code cleanup with no change in behaviour. The log keeps only the first few zoom steps.

**0.2.0:** research tree zoom (feature 2 below). The build now also links `user32` (for the optional zoom modifier
key). No layout file changes are needed.

**0.1.3:** code cleanup with no change in behaviour. The optional launcher-only layout rename is removed, because the
mod uses the vanilla layout names. The `research_tree_layout` / `overview_layout` ini keys are gone.

**0.1.2:**
* The mod layouts use the vanilla names again: `DynamicCampaignResearchTree.xml` and `DynamicCampaignOverview.xml`.
  The 0.1.1 layout rename is off by default. Delete the 0.1.1 `40k_DynCampaign*.xml` files from the mod and
  from `mod_side\`.
* The log is one file per exe. The game writes `gem_conquestui.log`, the editor `gem_conquestui_editor.log`, and the
  server `gem_conquestui_server.log`. Before this, starting the editor overwrote the game's log.

**0.1.1:** This release makes the research tree inspector safe; see "Why the developers commented the inspector out"
below. It adds a library guard, and a launcher-only layout rename that 0.1.3 removed again.

Dynamic Campaign ("Conquest") UI extension for Call to Arms - Gates of Hell 1.067, injected by the Indomitus
Launcher (`launcher\payload\`). It only touches the UI, never the simulation, so it is safe in co-op campaigns.
Only `call_to_arms.exe` has this UI: in the editor and the dedicated server the DLL writes one log line and does nothing.

It re-implements the two DLLs in `_for_claude_to_study\` (`goh_research_tree_preview.dll` 1.0.3 and
`goh_dynamic_campaign_unlocked_portraits.dll`). They were studied, not copied: this is new code in the style of
BreedTags / ScriptExt (zig cc, no MinHook, no MSVC runtime).

## Features

**1. Research tree unit preview.** When you click a unit node in the campaign research tree, that unit appears in an
inspector under the tree. The inspector is the engine's own unit card from the multiplayer library, the same one the
campaign page uses for reinforcements, with the 3D model and stats. Researched nodes can also be clicked to preview
them, but they still look researched and cannot be selected for research again. The developers had already placed
this inspector in the vanilla layout and then commented it out. `mod_side\DynamicCampaignResearchTree.xml` turns it
back on.

**2. Research tree zoom.** The mouse wheel over the research tree zooms it in and out, keeping the point under the
mouse pointer in place. Dragging still pans the tree, as in vanilla.
* By default the range is 40% to 150%, one wheel notch is 20%, and 100% is always one of the steps.
* The icons move and grow or shrink, and their pictures scale with them. The connecting lines are rebuilt by the
  engine's own line code. Text on the icons keeps its size.
* The tree reopens at the last zoom for the rest of the game session (`zoom_remember=1`).
* In vanilla the wheel scrolls the tree up and down; with the default settings it zooms instead. To keep the vanilla
  scrolling, set `zoom_modifier=ctrl` (or `shift` / `alt`): then the wheel zooms only while that key is held.

**3. Unlocked portraits.** This covers the Reinforcements, Airstrikes and Emplacements tabs.
* `portrait_mode=1` (the default) shows only the units you have unlocked, in the game's own order.
* `portrait_mode=0` shows every unit, with the unlocked ones first.
* The **SHOW LOCKED / HIDE LOCKED** button next to "Open research" switches between the two modes during play
  (`mod_side\DynamicCampaignOverview.xml`). The DLL updates the button text to match the mode.
* Tabs stay exactly as in vanilla. Airstrikes and Emplacements still appear when every unit in them is locked, so you
  can switch the mode and see those units.

## Install

1. Run `build.bat`. It finds zig in `zig\`, in the launcher project (`..\..\zig\`), in the ScriptExt repo, on PATH, or
   as `python -m ziglang`. The output is `dist\gem_conquestui.dll` plus the ini.
2. Copy `dist\gem_conquestui.dll` and `gem_conquestui.ini` into `..\..\launcher\payload\`, then rebuild the launcher.
   The payload fingerprint changes automatically.
3. Copy both files from `mod_side\` into `mods\Indomitus_002\resource\interface\layout\views\`. They have the
   vanilla names, so whenever the mod is enabled they replace the game's own layouts.
   Also copy `mod_side\localizations\` into `mods\Indomitus_002\localizations\` (the button texts).
   * `DynamicCampaignOverview.xml` is the vanilla overview layout with one change: `btn_open_research` now sits in a
     FlowPanel together with the new `btn_portraits`.
   * `DynamicCampaignResearchTree.xml` is the vanilla research tree layout with the `<DialogRef id="inspector" .../>`
     line uncommented.

**Known risk of the vanilla names:** if the game starts from Steam without the launcher, with the mod enabled,
nothing starts the research tree inspector, and **opening research crashes the game**. The "Why" section below
explains this. With the launcher, the DLL starts the inspector, and its guard covers every other case.

If the inspector or the button is missing from the layouts, or the feature is off or not recognised, that part does
nothing:
* With no button, the mode stays at the ini's `portrait_mode`.
* With no inspector, the research tree behaves as in vanilla.

The log says which case applies.

## Why the developers commented the inspector out

The vanilla research tree layout already contains the inspector
(`<DialogRef id="inspector" refid="gui_multiplayer_library"/>`), commented out. Un-commenting it **without code that
drives it crashes the game**:
* The inspector is an `eGUI_MultiplayerLibrary`. Its 3D render engine (`eLibraryRenderEngine`, at library `+0x280`)
  only exists after `Start` (vt[73]). The campaign page calls `Start` for its own inspector, but nothing in the
  research tree code ever calls it for this one.
* The library's `OnEvent` uses that engine **without a null check** on four events:
  * `0x469` and `0x101D` (3D scene ready) write to `engine+0x12`.
  * `0xFB6` (scene unload) calls the engine's Clear.
  * `0xFD2` (the per-frame tick, while the library is shown) calls the engine's Quant.
* So the first frame the research tree is open, or the first scene load or unload, dereferences a null pointer and
  crashes the game.

How 0.1.1 fixes it:
* **Library guard.** Library `OnEvent` (vt[1]) is hooked: when a multiplayer library has no render engine, those four
  events go only to `eDialog::OnEvent`, the generic handler the library itself forwards every event to. Its own
  engine code is skipped. The event ids and the forwarding target are read from the engine's code at startup. This
  covers every case where nothing started the inspector: preview off, a tree whose owner is not the campaign page, a
  finder failing after a game update. In those cases the inspector just stays empty.
* **The research preview starts the inspector itself.** At tree Bind it calls `Start`, then `SetContext` with the
  faction of the page's inspector, before the first tick.
* **Two 3D previews at once is supported by the engine.** All library render engines draw into one shared "library"
  scene. The engine keeps a global list of them: the first one is "primary", the primary role passes to the next when
  it closes, and the scene is unloaded only when the last engine goes. The studied DLL used the same setup.
  **Still to check in game:** the campaign page's inspector stays behind the tree. If its unit ever shows up in the
  tree's preview as well (one shared scene), the fix is to deselect the page portrait while the tree is open.


## ini (`[conquestui]`)

| key | default | meaning |
|---|---|---|
| `enabled` | 1 | 0 = nothing is patched |
| `log` / `console` / `verbose` | 1 / 0 / 1 | `gem_conquestui.log` next to the DLL (`_editor` / `_server` for the other exes); verbose = first previews / portrait passes (capped) |
| `research_preview` | 1 | feature 1 |
| `research_zoom` | 1 | feature 2 |
| `zoom_modifier` | ctrl | key to hold while zooming (`ctrl` / `shift` / `alt`): the wheel scrolls as in vanilla unless it is held; empty = the plain wheel zooms |
| `zoom_min` / `zoom_max` / `zoom_step` | 40 / 150 / 20 | zoom range and step per wheel notch, in percent (clamped to 20-100 / 100-300 / 2-100) |
| `zoom_delay` | 150 | least time between two zoom levels, in ms (0-1000); sooner notches are ignored; 0 = every notch counts |
| `zoom_remember` | 1 | 1 = the tree reopens at the last zoom during the session, 0 = always at 100% |
| `inspector` | inspector | id of the inspector in the research tree layout |
| `portrait_filter` | 1 | feature 3 |
| `portrait_mode` | 1 | start mode: 1 = unlocked only, 0 = all, unlocked first |
| `portrait_toggle` | 1 | bind the mode button |
| `toggle_button` | btn_portraits | its id in the overview layout |
| `toggle_label_unlocked_only` / `toggle_label_all` | `main_menu2/dynamic_campaign/gem_show_locked` / `..._hide_locked` | button text in each mode (what a click will do). A value with a `/` and no space is a localization key (a key with no text shows SHOW LOCKED / HIDE LOCKED); other text is shown as written; empty = keep the XML text |

## What to check in game (first run)

The log `gem_conquestui.log` sits in the launcher's payload folder under `%LocalAppData%\Indomitus Launcher\payload\<fp>\`.
It should start like this:
```
GEM ConquestUI 0.4.2 loaded into call_to_arms.exe (timestamp 6AB3DCC7)
library guard                  ON
research tree preview          ON
research tree zoom             ON (ctrl+wheel, 40%-150%, 150 ms between levels)
unlocked portraits             ON (start mode 1 = unlocked only)
mode switch button "btn_portraits" ON
```
1. Open a Conquest campaign. Check that locked units are missing from the three tabs, and that the log shows
   `portraits (reinforcements): N portraits, M unlocked ...`.
2. Click SHOW LOCKED. Locked units should slide in after the unlocked ones and the button should read HIDE LOCKED.
   Click again to go back.
3. Open research. Click a locked unit: its card appears under the tree and the node is still selected for research
   as in vanilla. Click a researched unit: you get the card and no selection. Close the tree: the log shows
   `inspector detached (tree closed)`.
4. Research something, close the tree, and check that the newly unlocked unit appears in its tab.
5. In the research tree, turn the wheel up and down over a few different places. The point under the pointer should
   stay put, the lines should still join the icons, and the scroll bars should fit the new size. Zoom out fully and back
   in until 100%, then close and reopen the tree: it opens at the same zoom. The log shows
   `research zoom: N% (... icons moved, ... lines rebuilt)` for the first steps.

## How it works

Every address and offset is derived again at startup from RTTI class names, vtable slots and byte patterns. If any
of them is not recognised, that feature stays OFF and the log names the reason. Only vtable slots are swapped (15,
including the guard). No game code or data is patched.

* **Portraits.** The panels' `Refresh` (vt[73] of `eCampaign{Reinforcements,Airstrikes,Emplacements}Panel`) creates
  one portrait per eligible unit and never removes any. After each Refresh the DLL does three things:
  * It reorders the wrap panel's child vector so unlocked portraits come first. This is a pure permutation of the
    vector, followed by the layout invalidation that `eDialog::AddChild` itself does.
  * It hides or shows each portrait with the engine's own flag. Bit 1 of `+0x48` plus vt[5] is the pattern the
    engine uses inline everywhere, and `eWrapLayout` skips invisible children.
  * It keeps the wrap panel's reveal-animation queue consistent. The queue only advances when its head finishes
    sliding in, and a hidden portrait is never drawn, so a hidden head would stall it. Hidden portraits are therefore
    removed, starting from the back so that removing one never starts another hidden one. Visible portraits that were
    never revealed are queued again in display order. The queue is read first and only rebuilt when it disagrees.

  When a mode switch hides the selected portrait, the DLL also deselects it, which clears the page's inspector.
  The unlocked flag is the one the engine's own `UpdatePortrait` stores in the portrait (`+0x2E0` = research state 1).
  The button is bound the way the engine binds `btn_open_research`, and its click arrives in the view's Notify.
* **Zoom.** The tree's LayoutInit (vt[19]) places each icon at (icon size + spacing) x grid position and builds each
  line once from the centres of its two icons. After it returns, the DLL records every icon's position and size (and its
  preferred/min/max size). A zoom step sets each icon to those values times the zoom (SetSize vt[16], position +
  vt[14], the way LayoutInit does it), then calls the engine's LineSetup again for every line. The wheel arrives in
  the tree scroll pane's `OnMouseWheel` (vt[51] of `eScrollPane`, shared by all scroll panes; only the research
  tree's pane zooms). The scroll position that keeps the point under the mouse in place is set right away and again
  after the scroll pane's next layout passes (`eScrollPaneLayout` vt[4] Arrange), once the scroll ranges fit the new
  size. The tree's dtor and Close release the binding.
* **Research preview.** The DLL hooks these research tree slots: `Bind` (vt[69]), `Notify` (vt[34], icon clicks 7/8),
  `Close` (vt[62]) and the destructor. It also hooks `OnEvent` of the inspector library (vt[1]: scene reset and scene
  ready), and two research icon slots: vt[73] "disabled" (researched icons stay clickable) and vt[86] state frames
  (they keep the researched look). A click turns the node name into a unit template with
  `ConfigLookup(Tag(name))`, then calls `ShowTemplate(inspector, template)` once the inspector's 3D scene is ready.
  The inspector gets the faction context of the campaign page's own inspector.

Engine notes with every 1.067 address are in the header comment of `src\gem_conquestui.c`.

## Differences from the studied DLLs

* No inline hooks (they used MinHook on 8 + 6 functions). Here only vtable slots are swapped (15 slots), so no
  prologue gets relocated and nothing breaks if another tool hooks the same functions.
* No hard-coded RVAs or SHA-256 pin of the exe. Everything is found by pattern, and the offline test runs every
  finder against the exes.
* Locked portraits are hidden, not destroyed and recreated. The original removed native portrait objects and carried
  a lot of rollback code for when that failed. Hiding needs none of it, and the "keep empty support tab" hook becomes
  unnecessary.
* The button text follows the mode, and the toggle no longer reruns the whole page refresh.
* A research tree that is destroyed without Close unbinds itself, so it cannot leave a stale binding.
* While the DLL runs, the inspector cannot crash the game, thanks to the library guard. Like the studied DLL's
  companion XML, the mod uses the vanilla file name, so a Steam start without the DLL still crashes when research is
  opened.

## Offline check (after a game update)

`tests/run_offline_check.sh <game exe> [<editor exe> <server exe>]` uses gcc/clang on Linux or WSL. It maps each exe
and runs every finder. The game must print `ALL FOUND` and the editor/server print `INERT`. Then it syntax-checks the
Windows build against the fake headers in `tests/stub/`.

`tests/logic_test.c` runs `pf_apply` against a fake panel, wrap panel and reveal queue that behave like the engine's.
It checks order, visibility and the queue across the first page open, both toggles, and a panel that starts in
mode 0. `tests/zoom_test.c` runs the zoom against fake icons, lines and a scroll pane: geometry at each zoom, the point
under the mouse staying put, the limits, the exact return to 100%, the modifier key, other scroll panes left alone,
a reopened tree keeping its zoom, and the delay (a notch 60 ms after a level is ignored, one 160 ms after applies). An independent review of the hooks against the disassembly found no crash risks; its fixes are included.

Known cosmetic: the engine starts the first portrait it creates sliding in immediately. If that portrait is locked,
it gets hidden mid-animation and later pops in instead of sliding.

Status: on game `6AB3DCC7` every finder passes and every value matches the addresses the studied DLLs hard-code.
0.1.2 was tested in game on 2026-10-28 and works. 0.1.3 resolves exactly the same engine addresses offline.
The 0.2.0 zoom was tested in game on 2026-10-29 and works (481 icons and 493 lines per step, 40%-150%, back to exactly
100%). 0.2.1 resolves exactly the same engine addresses offline. The 0.3.x smooth zoom lagged in game and was removed in 0.3.2.
