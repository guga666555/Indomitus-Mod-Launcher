# GEM ResearchPath 0.4.0

DLL for Call to Arms: Gates of Hell 1.067, loaded through the Indomitus Launcher payload. It adds two things to the
Dynamic Campaign research tree:

1. **Path highlight.** Hovering a unit highlights it and every prerequisite you still have to research to get it. The
   walk follows prerequisites transitively and stops at researched units.
   - The icons on the path use another frame of their own button (`highlight_frame`, the hover frame by default).
   - Their glow widget is shown: a hidden picture `rp_glow` in the icon layout, with the texture
     `/interface/scene/portrait_squad/frame_glow`, next to the portrait frame. You draw that texture.
   - The research lines along the path are drawn with the texture `line_texture` and the color `line_color`. With
     `line_to_researched=1` this includes the links back to the researched units the path starts from.
2. **"Requires" tooltip.** The tooltip of a locked unit ("Research previous unit to unlock") gets the missing units
   appended on a new line: `Requires: A, B, C`. They are listed deepest prerequisite first, which is the order to
   research them in, and each display name appears once. At most 4 names are listed by default, then `+N more`
   (`tooltip_max_names`).

It's UI only, so it doesn't touch the simulation and is safe in co-op campaigns.

## Install

1. Run `build.bat`. It produces `dist\gem_researchpath.dll` and copies the ini next to it.
2. Copy `dist\gem_researchpath.dll` and `gem_researchpath.ini` into `_indomitus_mod_launcher\launcher\payload\`, then
   rebuild the launcher.
3. Mod side (already done in Indomitus_001):
   - `mod_side\progress_line_selected.tga` goes to `resource\interface\pages\main\dynamic_campaign\`.
   - `mod_side\UnitPortraitView.xml` goes to `resource\interface\layout\views\`. It is the mod's own layout plus the
     hidden `rp_glow` picture; the original is in `_bckp\mod_side_orig\`.
   - Your glow texture goes to `resource\interface\scene\portrait_squad\frame_glow.tga` (or .dds/.png).
4. The log is `gem_researchpath.log` in `%LocalAppData%\Indomitus Launcher\payload\<hash>\`.

Expected log (with GEM ConquestUI loaded too):

```
GEM ResearchPath 0.4.0 loaded into call_to_arms.exe (timestamp 6AB3DCC7)
already hooked by another DLL (originals read from the exe file, hooks chained): tree[62], tree[0], tree[69], tree[19], icon[86]
selected-line texture /interface/pages/main/dynamic_campaign/progress_line_selected = mods\Indomitus_001\resource\interface\pages\main\dynamic_campaign\progress_line_selected.tga
path highlight         ON (hover frame)
path glow              ON (widget "rp_glow")
path lines             ON (color FFFFFFFF, + links to researched units)
path line texture      ON
requires tooltip       ON
tree bound: 481 icons (481 with a glow widget), 493 prerequisite links (0 to units without an icon), 493 lines (493 between two icons)
tooltip <key>: \nRequires: ...
hover <key> (state 3): N unit(s) on the path - icons N on / 0 off, lines L on / 0 off
```

## The path line texture

The vanilla line is `/interface/pages/main/dynamic_campaign/progress_line.tga`: 1x10 pixels, a 2-pixel white stripe
at 25% alpha, so it looks faint. `mod_side\progress_line_selected.tga` is the same stripe fully opaque, as an
uncompressed 32-bit TGA. Any texture works under that name (or under another `line_texture`) in the same folder.
Keep it 1x10 so the line keeps its geometry; a taller stripe makes it thicker. `line_color` is multiplied with the
texture (`FFFFFFFF` = as drawn).

At startup the DLL looks for `<game>\mods\*\resource\[40k_]interface\pages\main\dynamic_campaign\<line_texture>.*`. If
nothing is found, path lines keep the vanilla texture and the log says so, so a missing file never shows a broken
texture. `line_texture_check=0` skips the check, for example when the file is inside a .pak.

## The glow

The icon layout (`UnitPortraitView.xml`, used by every unit portrait of the campaign pages) gets one extra element.
It sits right after `frame`, so it is drawn over the portrait and the frame and under the texts:

```xml
<PictureDialog id="rp_glow" resource="/interface/scene/portrait_squad/frame_glow" zoomMode="fit">
    <Active value="false"/>
    <Ables mouse="false"/>
</PictureDialog>
```

- It starts hidden, and the DLL only toggles it, with the engine's own `SetActive` (the same call the icon uses for
  `is_locked` and its other overlays). Everywhere else it simply stays hidden.
- The look is entirely the texture. Move the element above `frame` to draw it under the frame instead, or change
  `resource` to use another file.
- `Ables mouse="false"` keeps the hover and the clicks on the icon.
- Without the element there is no glow; the log says how many icons have it.

## Together with GEM ConquestUI

Both DLLs swap some of the same vtable slots: the tree's dtor, LayoutInit and Close, and the icon frame map. Each one
stores whatever is in a slot as its "original", so the hooks chain. ConquestUI's finders read those slots directly, so
ConquestUI must load first. The launcher injects the payload in alphabetical order, which already does this.
ResearchPath reads an already-hooked slot's original from the exe file on disk, so it works in either order.
ConquestUI's zoom rebuilds the lines (LineSetup) as vanilla ones; path lines are switched again on the next frame.

## How it works (engine 1.067)

- **Tree.** LayoutInit (tree vt[19]) creates one icon per research node and one line per prerequisite. The hook
  records them once the original returns.
  - The icon's node `+0x2E0` is the research map value. Its prerequisite keys are `vector<std::string>` at `+0x20`;
    LayoutInit's line loop and `GetState` both confirm this.
  - The icon state is `+0x2E8`: 1 researched, 2 researchable, 3 locked, 4 DLC.
- **Hover.** The GUI keeps the hovered dialog in a weak reference, cleared when that dialog dies. The tree's draw
  (vt[30], every frame while shown) compares it before the icons are drawn.
  - When the hovered icon changes, the icons and lines whose state changed are marked for update (vt[58]).
  - An icon's refresh (vt[59]) re-reads the frame map (vt[86]); the hook returns `highlight_frame` as the normal
    frame for icons on the path.
- **Lines.** Each `eGUI_CampaginResearchLine` keeps its dependent icon (`+0x1A0`) and prerequisite icon (`+0x1A8`).
  - Its sprite is FX entry 0 of the FX holder (`+0x148`): `{sprite, id, Sprite::eFXAnimator}`.
  - The animator's color `+0x60` (`0xAARRGGBB`) is baked into the sprite's cached quad. The quad is rebuilt only while
    the sprite's "built" byte `+0x14` is 0, so the DLL clears it after every change.
  - The texture is loaded the way LineSetup does it: a resource path (ctor, assign, Normalize),
    `Sprite::Load(sprite, &path, 0x11, 0)`, `Material::SetState(mat, StateBits(0x40000))`, `Sprite::Finalize(sprite)`.
  - Leaving the path restores the vanilla texture and the old color the same way.
- **Glow.** At bind, each icon's `rp_glow` child is looked up once with the icon's `FindChild` (vt[60]). The DLL calls
  `SetActive(glow, on)` when the icon joins or leaves the path; that is flag bit 1 + vt[5] and bit 2 + vt[6], exactly
  what `<Active value=".."/>` does.
- **Tooltip.** Icon vt[74](icon, std::string*) fills the locked hint for state 3. The hook appends the list with the
  engine's own `std::string::assign`, and the refresh copies it into the button tooltip.
  - Names are what the icon labels show: units use `GetUnitName(ConfigLookup(Tag(key)), &s, 1)` (that string is freed
    with the engine's operator delete), other nodes use `Localize("main_menu2/dynamic_campaign/" + key)`.
  - Names are cached per tree.
- **Finders.** Every address and offset is found at startup from RTTI, vtable slots and byte patterns, with
  cross-checks. If something is not recognised, that feature is OFF and the log says why.

## Tests (Linux/WSL)

`sh tests/run_offline_check.sh <call_to_arms.exe> <call_to_arms_ed.exe> <call_to_arms_server.exe>`

- **Finders:** the game must print `ALL FOUND`, also with 6 slots pre-hooked as ConquestUI does. The editor and the
  server must print `INERT`.
- **Logic test:** a fake tree with icons, nodes, lines, sprites and engine functions. It checks:
  - the path order;
  - the highlight and line switching, including a line rebuilt while on the path;
  - texture and color restore;
  - the tooltip text (`+N more`, non-unit nodes, prerequisites without an icon, duplicate names);
  - that every engine string is freed.
- A clang Windows-target syntax check, run with -Wall -Wextra.

## Limits / ideas

- With this skin the icons' hover frame looks much like the normal one; the glow is the visible icon highlight.
- Units already selected for research in the current batch still count as missing.
