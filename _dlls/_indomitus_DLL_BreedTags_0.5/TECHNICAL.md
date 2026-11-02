# GEM BreedTags - technical notes

Everything learned about the GEM engine (Call to Arms: Gates of Hell 1.067.0) while building this.
Addresses are for the 1.067 game exe (`call_to_arms.exe`, PE timestamp `6AB3DCC7`) unless noted;
the DLL never uses them directly, it re-derives everything at startup (see "Derivation").

## 1. The two tag worlds

A breed file's `{tags "soldier" "crew" ...}` is parsed (`eHumanBreed.cpp`) into a **TagContainer**
stored on the breed object. A unit's script tags (`{tags add}`, `{if tagged}`, trigger `{tag}`
selectors) are a **different** TagContainer stored on the entity. The engine never copies one into the
other. Consequences:

| Reader | Sees breed `{tags}`? | Sees entity `{tags add}`? |
|---|---|---|
| `{if tagged}` in interaction_entity | no | yes |
| trigger `{tag ...}` selectors | no (tested: `bt_probe_*` miss) | yes |
| Lua `BotApi.Scene:QueryScene` prop/tag filters | yes | ? |
| editor Alt+T / `v_tags` view | no | yes |

This is why the SC-platform skin system identifies breeds through inventory items + contact scans
(`Skin_Check_Event_Inv_Item`), `Link_weapon` effectors, or 1 s trigger loops: no data-file key
bridges the gap. The breed parser keywords are: `behaviour hero no_inventory portrait field_icon
icon icon_priority nationality armors in_hands skin perks veterancy inventory tags` - nothing else.

## 2. Structures

### TagContainer (size ~0x330)
```
+0x300  byte  (flag)
+0x308  ptr   -> self          } written by every ctor; used as a runtime sanity check
+0x310  ptr   -> self          }
+0x318  Tag*  vector begin     elements are 0x60 bytes
+0x320  Tag*  vector end
+0x328  Tag*  vector capacity
```
Functions (game 1.067): copy ctor `0x140556de0(dst, src)`, add_all `0x140557060(dst, src)`
(dedups), remove_all `0x140557120`, dtor `0x140556f00` (= `add rcx,0x310; jmp vector_dtor`),
Tag equality `0x140556810(Tag*, Tag*)`, to_string with separator `0x140558280(container, &out, sep)`.

### Tag (0x60 bytes)
```
+0x00  ptr   interned/lowercased key (0x14054fdc0(str,len))
+0x38  ptr   small-buffer ptr
+0x40  MSVC std::string (SSO 16 / size +0x50 / cap +0x58)
```
`Tag::Tag(const std::string&)` = `0x140556400`, `~Tag` = `0x1405564e0` (`add rcx,0x38; jmp`).
Tag names are lowercased on construction (`tag::to_lower`), so matching is case-insensitive.

### Entity
```
+0x30   byte  flags: bit1 = alive/in scene (engine checks `test byte [e+0x30],2`)
+0x38   dword flags: bit13 = "notify on tag change" -> vcall [vt+0x1c0]
+0x1d0  TagContainer   (so vector begin/end at +0x4e8/+0x4f0)
+0x578  ptr   actor; [+0x18] -> control, byte +0x174 bit0 = user_control
+0x5a8  ptr   view/model (bone positions)
+0xda8  byte  player id
```
Human (derived):
```
+0x1330 Tag          breed name (string at +0x1370)
+0x1390 eHumanBreed* breed      (editor +0x1390, server +0x1388)
+0x14c0 std::string  skeleton
```
### eHumanBreed (size 0x630)
```
+0x128  TagContainer   {tags}      (editor: +0x130)
+0x599  bool           (hero / no_inventory style flag)
```
### Entity::SetTags `0x14071a3f0(entity, const TagContainer*)`
Compares, assigns (`0x140557200`), then maintains a **global `std::vector<Entity*>` of entities
that have tags** at `0x14156ebc0` (begin) / `+8` (end): pushes when tags become non-empty, erases
when empty. This is the list selectors iterate. Then fires the tag-changed virtual if flag bit13.

### `{tags add|remove}` action eval `0x14074e040`
```
tmp = copy(entity+0x1d0); add_all/remove_all(tmp, action+0x28); SetTags(entity,&tmp); dtor(tmp)
```
action+0x358 = byte add(1)/remove(0). The hook reproduces exactly this with the breed container.

### ApplyBreed `0x14095cad0(human, flags)`  (eHumanBreedUsage.cpp)
Prologue (all builds): `mov [rsp+10],rbx; mov [rsp+20],rsi; push rbp; push rdi; push r12; push r14;
push r15; lea rbp,[rsp-0xa0]; sub rsp,0x1a0`. Looks the breed up by name if `[human+0x1390]` is
null ("Unknown breed \"%s\""), stores it, applies skin/inventory/perks. Called from SetBreed
`0x14095b880(human, Tag* name, flags)` and on reload. Hooking it (14-byte inline jmp, prologue
relocated) and merging `breed+0x128` into the entity afterwards gives breed tags at spawn; the
`{on spawn}` handler already sees them (`bt_probe_spawn_hit`).

## 3. Interaction keyword tables (.data)
16-byte `{const char* name, ctor fn}` entries. Conditions (`eIA_If.cpp`): able tagged prop volume
volume_any material volume_bone_level volume_armored component bone stuff no_stuff rand active
move_forward move_backward turn_left turn_right min_energy min_mass min_speed burned effector
friendly_fire mine_effector can_crush name terrain_pp terrain_fx sunlight rain velocity
effector_velocity contact_velocity altitude air under_water water_level dead kill_flags senseless
personage operatable inactive linked boarding stuff_filled stuff_activated swimming lying standing
squatting in_cover cover_type idle_low_health hold_strap work broken destroyed hit_side mined fallen
user_control user_squad chassis fogged **tex_mod** flora_regenerate gamemode simulating impregnable
place cannon_state crew_in_place has_place weaponry_sealed difficulty true false explosive_amount
season. (No breed / inventory / nationality condition exists; `tex_mod` is the texmod checker BUZZ
notes as unreliable in MP.)

Condition object: vtable[1] = `bool eval(cond*, void** ctx)` (`ctx[0]` = entity), vtable[2] =
parse. `tagged` eval `0x140759ba0` walks entity+0x4e8..0x4f0 comparing with the Tag at cond+8.
Redirecting that vtable slot is a free per-frame tick on the simulation thread (the FSM scripts
evaluate it constantly).

## 4. damage_report
`{damage_report "bone" "<markup>text"}` eval `0x140740da0`: only for the local player's units
(`[e+0xda8] == local player`), then `DamageReport 0x140b8efc0(entity, Tag* bone, std::string* text)`
-> `AddReport 0x140b8cee0`. Reports live in a global vector at `0x1415a3700/+8`, **0xb0-byte**
entries: +0 entity, +0x10 type, +0x18 Tag bone, +0x78 std::string text, +0x98 expiry tick,
+0x9c creation tick, +0xa0 updated flag. A second call for the same entity/bone within 1.0 s
(`comiss 1.0`) **updates the existing entry** (text replaced, expiry extended) instead of adding.
Re-issuing every 0.5 s and rewriting +0x9c keeps a label static. Markup: `<s(12)>`, `<c(RED)>`,
`<c(00ff00)>`, `<f(blackletter)>`, `s(shadow)`.

## 5. Debug view variables (`v_*`)
Static 24-byte records in .data: `{const char* name, type*, 8-byte value}` - `v_tags`, `v_bones`,
`v_mids`, `v_eids`, `v_volumes`, `v_volumeNames`, `v_path`, `v_entityNames`, `v_velocity`, ...
Registry lookup `0x1402415b0(const char*)` returns the record. Per-variable toggle functions exist
in the game exe: `0x1403be2d0` = `toggle v_tags` (`push rbx; sub rsp,0x20; lea rcx,"v_tags";
call Lookup; ...; byte [var+0x10] ^= 1`).

**The editor's Alt+T is exactly this toggle.** The renderer is the per-entity debug draw
`0x140a329d0(ctx, entity)` (called per visible entity from the scene draw loop): with `v_tags`
set and the entity having tags it draws `"<c(00ff00)s(shadow)>" + tags joined by '\n'` via
`DrawText3D 0x140624530`. It is compiled into the game and server exes; only the key is missing.

## 6. Mod manager
Global `std::vector<Mod*>` at `0x1415586b0/+8`. Mod: `+0x18` int source (1 = local, else Steam),
`+0x20` std::string name, `+0x118` byte enabled. `ModManager::Apply` logs `"mod.off"`, walks the
vector disabling all (`Mod: disable mod <name>`), then enables the selected set. Status printer
(`s_mods`): `"Mods loaded:"`, `"[LOCAL] "/"[STEAM] "` + name. Same layout in all three exes.

## 7. Proxy / loading
The three exes import exactly six functions from `dbghelp.dll` (SymInitialize, SymCleanup,
SymFromAddr, SymGetLineFromAddr64, ImageNtHeader, SymSetOptions) and nothing else in the folder
imports it statically, so a local `dbghelp.dll` that forwards those (naked `jmp [ptr]` stubs
resolved from `%SystemRoot%\System32\dbghelp.dll`) loads into game, editor and server before any
engine code runs. `version.dll` is taken by the GEM loader (Editor+/Conquest+). The exe's other
proxyable imports: IPHLPAPI (3 fns, server+game), VERSION (3).

## 8. Derivation (what the DLL does at startup, no fixed addresses)
| Target | Anchor |
|---|---|
| ApplyBreed, human->breed off | `'Unknown breed "%s"'` string -> its `lea` xref -> walk back to the prologue bytes -> first `cmp qword [rcx+disp32],reg` |
| breed container off | `"nationality"` lea in the parser -> nearest earlier `"tags"` lea -> the `lea reg,[reg+disp32]` just before it |
| TagContainer fns, entity container off | `.data` table entry `{"tags", ctor}` -> `lea rax,[rip+vtable]` -> vtable[1] -> `add rdx,imm32` (offset) / calls after it |
| tagged eval slot, Tag ctor/dtor | table entry `{"tagged", ctor}` -> vtable[1]/[2] |
| DamageReport, report vector | table entry `{"damage_report", ctor}` -> vtable[1] -> call after `lea r8,[rsi+0x88]` -> last call before `jmp` -> first two rip loads |
| tagged-entity list | inside SetTags: `mov rdx,[rip]` then `mov rcx,[rip]` |
| v_tags record / toggle | `"v_tags"` string -> .data slot holding its address; first lea xref - 6 = toggle |
| mod list | `"mod.off"` lea -> `mov rbx,[rip]`/`mov rdi,[rip]` -> `lea ?,[r14+0x20]` / `mov byte [r14+0x118],0` |

`gcc -DBT_TEST gem_breedtags.c && ./bt_test call_to_arms.exe` maps the exe at its image base and
runs every finder - the post-update check. Verified on 1.067 game `6AB3DCC7`, editor `6AB3DD83`,
server `6AB3DD21`.

## 9. Dead ends (so nobody repeats them)
- Breed `{tags}` -> `{if tagged}`: no. Confirmed with `bt_probe_spawn/spawn01/link` all missing.
- Trigger `{tag <breed tag>}` selectors: no (same container as `tagged`).
- Perks (`skills.inc`) carry only skill levels, `{barricade}` lists, `{mechanic}` - no tag payload.
- `nationality`, `icon`, `portrait`: not readable from scripts.
- Editor Alt+T has no key-binding data file; it is a hard-coded editor key calling the `v_tags` toggle.
- `GetAsyncKeyState`/`GetKeyState` are not imported by the exes (engine input is message based).
- `localization/` and the profile folder (`Documents\my games\gates of hell\profiles\<id>\`) hold
  `keys.set`, `options_presets.set`; the active mod list is not needed from there since the engine
  keeps it in memory.
