# map_stance: GEM MeleeStance 0.7.42 (for the gem_melee merge)

Source: `inputs\meleestance_0.7.42\src\gem_meleestance.c` (one translation unit, **4125** lines, CRLF, sha256 96b0c33e..b474 = MANIFEST),
`src\gem_meleestance.ini`, `README.md`, `build.bat`, `build_log.txt` (ms_test 0.7.42 run on the 1.069 exes, 12:59, ALL OK twice).
`L<n>` = line of the .c file. "inferred" = not proven by code I read. Editor addresses: `ed`; game: `game`; server: `srv` (RVAs).
Address sources: ms_test output in `build_log.txt` (S), the live 0.7.41 editor log of payload a1d6ed3c (L), my read-only re-run of the
finders on the three installed exes (`design\_scratch_stance\overlap.py`, O). Part A = the map; Part B (after it) = the full file-order walk
with every function, global and log line; Part A points into it.

---------------------------------------------------------------------------------------------------------------------------------------
## 1. Start-up (DllMain -> last installer call)

1. `DllMain` L3283-3303, `DLL_PROCESS_ATTACH` only. `DisableThreadLibraryCalls`. Own path (`GetModuleFileNameA(inst)`; failure or length >=
   MAX_PATH-16 -> return TRUE, nothing at all). Extension stripped. ini = `<dll path without ext>.ini`; log = same + `_editor` (exe name
   contains "_ed") / `_server` (contains "_server") / "" + `.log`. **No exe-name gate**: any process that loads it gets the hooks.
   `load_ini(ini)` (L2961-3076) then `install()` (L3105-3281). Everything runs under the loader lock. No DETACH handling, nothing is ever
   unhooked.
2. `install()` order:
   1. `init_tag()` L3109 (tag lists, AutoMeleeTag split, may-melee cache cleared).
   2. Gate `[MeleeStance] Enabled=0` -> log `[GEM MeleeStance 0.7.42] Enabled=0, not installed`, return (no thread, no patch) L3110.
   3. `find_all(base)` L3111. FATAL errors only: no .text, no exception directory, MatchAmmo not found / not unique (L2845-2868) -> log
      `[GEM MeleeStance %s] NOT installed: %s`, return. Everything else is optional and only switches its own feature off.
   4. Linked-flag sites < 20 -> `SkipLinked` forced 0 (log) L3113-3116.
   5. `cfg_ammotags` = offset read from MatchAmmo code; `g_clock` = clock if found L3117-3118.
   6. Charge finder error (`charge_err`, any of: Range call, enemy scan, MakeAttack, UI attack site/AddOrder, MakeRef layout, RefDtor,
      TagDtor, order stamp, AI clock) -> `[Charge]` OFF **and `[AutoDraw]` OFF** (L3119-3120); `g_scan_lo` stays NULL so nothing is ever
      "from the scan", `g_addorder` stays NULL so no order is ever issued (charge, fight, draw, back-to-gun, multi-select draw). What stays:
      the stance ammo filter (ms_decide, incl. the 0.7.42 rule and the [Vehicles] verdicts), [Downed] filter, IgnoreTargetTags, multi-select,
      shield ray, blow reach, direct control. `g_clock` may then be NULL (the clock finder runs last in find_charge), which disables every
      clock-based cache (may_melee, ref_ent) and makes `carries_melee` always 1 (L219).
   7. Charge finders OK -> all engine pointers set (L3121-3139), incl. `g_reflist_off` (0 = 0.7.33 stale-ref test off).
   8. **H1 MatchAmmo detour** (L3141-3154; see section 2).
   9. `if (cfg_log) CreateThread(hb_thread)` L3158 - **the only thread**. It is also the thread that connects to GEM Teleport's
      `gem_order_listen`; with `Log=0` there is no helper thread, no heartbeat AND no Teleport order events (inferred consequence, L1903-1907).
   10. Install lines L3159-3175.
   11. **H5 shield ray** only if `[ShieldMelee] Enabled=1` (default and live 0) L3177-3204; finder error -> that feature off.
   12. **H3 ExecuteInstant vtable slots** if `[MultiSelect] Enabled=1` L3206-3226; finder error (ExecuteInstant / take_item) -> off.
   13. take-item factory + TagContainer pointers L3228-3231 (errors only log; auto-draw "still refuses guns").
   14. order vtables (RTTI) L3237 (NULL allowed: no explicit-order recognition).
   15. Reload cancel pointers L3243-3253 (`rc_err` -> off; **GetCurrentWeapon is only assigned here**, so `ReloadCancel=0` or its finder
       failing also disables held_weapon_name / holds_keep_weapon / back-to-gun memory).
   16. Direct control L3255-3265 (only with SkipDirectControl=1; either of IsManualControlled / manager may be missing).
   17. **H4 blow reach table** if `[BlowReach] Enabled=1` L3268-3280.
   18. NOT at install: **H2 quant hook** is installed lazily on the engine thread by `frame_hook_try()` on the first enemy-scan pick
       (L1987) or the first Teleport order event (L1873). The ClientGate opens on the first MatchAmmo hook call (L2004).
3. Threads: engine thread (hook, tick, button clicks, order events; one id seen in the live log for sim and ui: 32968 / 32708), the
   helper `hb_thread` (log + Teleport lookup; no engine calls except reading `*g_clock` and the tables for counts).

---------------------------------------------------------------------------------------------------------------------------------------
## 2. Hook inventory (every write into engine memory)

| id | engine site, what it is | ed / game / srv | kind | original bytes / check | stub / handler | when, how often | reads / writes | returns | thread, locks |
|---|---|---|---|---|---|---|---|---|---|
| H1 | `MatchAmmo(ctx)`: returns the ammo.set row matching one candidate ammo for (shooter, target), else NULL; called by every weapon choice path (enemy scan x2, order picks, vehicle brains, UI) | ed 0x7AADA0 (S,L,O) / game 0x92C9C0 (S,O) / srv 0x73EE60 (S,O) | prologue detour, **14 bytes** = `FF 25 00000000 <abs64 &ms_hook>` (L3148-3153); trampoline in a fresh `VirtualAlloc` RWX page: the 14 original bytes + `FF 25` abs jmp to fn+14 (L3141-3146) | expected `48 89 5C 24 20 55 56 57 41 54 41 55 41 56` (`mov [rsp+20h],rbx; push rbp rsi rdi r12 r13 r14`); verified only by find_all requiring the 16-byte PROLOG (`... 41 57` push r15) at the function start (L2421, L2858); not re-read at write time; no thread suspension | no asm stub: the jmp lands on the C function `ms_hook` (ms_abi, NON-static) so `__builtin_return_address(0)` = the engine call site (L1972); body `ms_hook_body` L1982-2075; engine continues through `g_orig` = trampoline (`orig_timed` L1888) | per candidate ammo per pick: live ed 0.7.41 sessions 39031 / 96400 calls; Nov 10 MP host 6700/s, peak 24000/s (L1880-1881); Alex's Nov 10 match 2.6 M picks in 57 s (README) | reads ctx +0 ammo, +8 shooter, +0x10 target info (+0x18 target, +0x28 pos), +0x18 "in hand" byte; WRITES ctx[0x18]=1 during the engine call for MELEE (DrawFromBelt) and the target-info point ti+0x28..0x30 (hull contact, `adj_shift`), both restored after (L2058-2062); issues engine orders from inside the pick when the quant hook is not in (charge_flush / autodraw_flush / order_poll on the scan path L2002-2003, L2021) | NULL = candidate refused (REJECT, no-melee, downed target), else the engine's own row (never fabricated) | engine thread (not checked here); re-entrancy counter `g_in_hook` with a cleanup guard (L1985); no lock |
| H2 | `Scene::eQuantExecutor` vtable slot 1 = "run one quant" (frame function of the engine worker thread) | live ed: fn 0x413560, slot 0xF381D0, worker loop 0x9515B, thread start 0xD5A2AA, class `.?AVeQuantExecutor@Scene@@` (L log line 72/475); game / srv: never resolved offline (ms_test does not run this finder) | vtable slot swap (data, 8 bytes) `VirtualProtect(slot,8,RW)`, `*slot = frame_hook`, restore (L1853-1857) | the slot must be the ONLY 8-byte value in all non-exec sections equal to the frame function, with a pointer into the image 16 bytes before it, and RTTI class name containing "QuantExecutor" (L1836-1852) | `frame_hook` (static, ms_abi, 4 pointer args) L1789-1794: `ms_tick()`, then every `gem_quant_listen` callback, then `g_frame_orig(a,b,c,d)` | once per simulation quant (live ed: ~14700 ticks for 403 s sim with TickMs 100) | ms_tick: queues flushed, order poll (orders issued here) | the original's return value | engine thread; installed once per process (`InterlockedCompareExchange(&g_frame_state,2,0)`), lazily from the engine thread, never from DllMain |
| H3 | `Action::ExecuteInstant` in the action vtables ({instant} buttons) | ed 0xAA59F0 in 9/9 vtables, apply_to_all +0x498, GetActor 0xAB8CA0 (+0xF0), CanApply +0xD8, expand 0xAB8640 (S,L); game 0xB7E7B0 / GetActor 0xB91260 / expand 0xB90C40; srv 0x94D7A0 / 0x9603B0 / 0x95FD90 (S) | vtable slot swap x N (up to 32 slots; 9 found in all exes) `*slot = ms_exinst` (L3214-3220) | slot value == ExecuteInstant (live or preferred-base address) at find time (L2686-2692); not re-checked at write time | `ms_exinst` (NON-static, ms_abi) L2335-2371; per-unit run swaps the ACTION OBJECT's vptr to a 64-slot copy whose GetActor slot = `ms_getactor_cur` (NON-static) | per UI click of any {instant} action | calls vanilla ExecuteInstant per eligible selected unit, `draw_melee` (take-item + AddOrder) for the DrawButton action | void | engine thread (button clicks; sets `g_ui_tid`) |
| H4 | `GetBlowDistance` table (paired-melee start distance near/medium/far, vanilla 17/29/35) | ed table 0xF71368 fn 0x7F8910; game 0x1111510 / 0x979C00; srv 0xE0FE70 / 0x78BEC0 (S,L) | data table write, 12 bytes (3 floats) with VirtualProtect (L3271-3274) | table must read exactly 17.0/29.0/35.0 and the reader must be unique (L2911-2919) | none | once at install; affects every paired fight in the process | writes Near/Medium/Far (45/45/45) | - | DllMain |
| H5 | blow picker "first ray hit is the target" test (`[ShieldMelee]`, OFF by default and live) | ed site 0x7F82ED hits 0x1411980 ok 0x7F830B fail 0x7F8834 target r13; game 0x97959E / 0x15B6250 / 0x9795BC / 0x979B57 r12; srv 0x78B85E / 0x11FD740 / 0x78B87C / 0x78BE17 r12 (S) | inline patch 30 bytes = abs jmp (14) + 16 NOP to a stub at trampoline page +512 (L3181-3199) | the 30 bytes must match `48 8B 05 g / 48 3B 05 g+8 / 0F 84 fail / 4C 39 ?? 18 / 0F 85 fail` uniquely (L2896-2907); not re-checked at write | asm stub emitted as bytes: `sub rsp,20h; mov rcx,r<T>; mov rax,&ms_ray_ok; call rax; add rsp,20h; test al,al; je +14; jmp ok; jmp fail`; handler `ms_ray_ok` (NON-static, ms_abi, force_align_arg_pointer) L2391-2414 | per paired-blow start | reads the engine hit list | al = 1 pass / 0 block | engine thread |

Engine functions stance CALLS (no patch; addresses ed, from S/L): Range(stuff) 0x66EE20, MakeAttack 0x77F5D0, AddOrder 0x67E750 (= the
`Entity::GiveOrder` GEM Teleport detours: stance's own orders pass Teleport's lock and its order listeners, L), MakeRef 0xAB9210 (not called,
used to find SetEntity / RefInit / TagCtor / ref vtable), SetEntity 0x6CE3C0, RefInit 0xAA2C0, TagCtor 0x3E4390, TagDtor 0x3E46E0,
RefDtor 0x6CE920 (also pen X's "clear" target), eFight factory 0x77CC00, eMovementChange 0x788C00, MakeMove (eMoveTask) 0x7888F0, UI click
move 0x765FE0, eTakeItem factory 0x77F4F0 ({item} at action+0xC8), TagContainer ctor 0x3E4E60 / FromString 0x3E60D0, InventoryBusy
0x659050, CancelInventoryOps 0xAAD6F0, GetCurrentWeapon 0x695600 (also chase), IsManualControlled 0x67E5C0 (also pen's direct-control test),
action vtable GetActor / CanApply / expand, the trampolines. Globals read: sim clock 0x13DF988 (also pen + chase), order stamp 0x13DF928,
manual-control manager 0x142C678 (+0x3B0 actor; none in the server exe).

Order of filters on one candidate today (load order chase -> stance -> pen): pen's ctx-builder hook W (builder runs before every MatchAmmo
call) -> **H1 ms_hook** (ignored target / downed / fast pass / ms_decide) -> `g_orig` = MatchAmmo body, inside which pen's A2 (MatchAmmo+0x223)
and A1 (+0x28A) run. A candidate stance REJECTs never reaches pen; a MELEE verdict reaches pen with ctx[0x18]=1 and possibly a shifted
target point (pen's ExemptTags `melee_light,melee_heavy,melee` exempt the melee rows there, per pen's live install line).

---------------------------------------------------------------------------------------------------------------------------------------
## 3. Finders and overlap with every patch of the three dlls

All finders run once in DllMain over the exe image (`GetModuleHandleW(NULL)`) except the quant-slot finder (runtime, engine thread). The
patterns use `uint16_t` arrays with `W` (0x100) as wildcard (`match()` L2466). Re-run offline (O) on game / editor / server 1.069 with the
patch ranges of all three dlls (pen: A2 8 bytes, A1 5, builder 15, B 5, P 7, X 5 from pen's selftest.txt + source; chase: the 4-byte rel32 of
its redirected calls: target-evaluation 0x827927, approach 0x82FE14 and every one of the 20 `call Weapon::SetTarget` sites as a superset of
its 4, editor only; chase's vtable-slot hooks are data). Result: **the only overlap is stance with itself** (its 14-byte prologue patch lies
inside the 16 bytes its PROLOG check reads). No pen or chase patch touches a byte any stance finder reads, on any exe that could be checked.

| finder | method | where the bytes are (ed) | overlap |
|---|---|---|---|
| MatchAmmo | `SIG_FLAGS` hit (`0F B6 ?? 94 03 00 00 F6 ?? 1C`) whose function (exception directory) starts with the 16-byte PROLOG, is <= 0x600 bytes and holds `SIG_ADDTAGS` (`48 81 C1 imm32 E8`, imm 0x400..0x3000 = ammo {tag} offset 0xEC8); must be unique L2853-2868 | fn 0x7AADA0..0x7AB09B; FLAGS 0x7AAE9B, ADDTAGS 0x7AAE87, PROLOG fn+0..+16 | SELF: H1 rewrites fn+0..+14 -> a second stance instance (or the old gem_meleestance.dll loaded after gem_melee) fails with "ammo match function not found (unknown exe build, or already patched)" and installs NOTHING. Pen A2 0x7AAFC3 (MatchAmmo+0x223) / A1 0x7AB02A (+0x28A) start 0x53+ bytes past the end of the nearest stance read (SIG_RANGE, ends 0x7AAF70); same layout in game / srv |
| Range(stuff) | first `SIG_RANGE` (`0F 28 F0 E8 rel32 0F 2F C6`) inside MatchAmmo, target = rel32 L2527-2530 | 0x7AAF65 -> 0x66EE20 | none (pen does not patch this call) |
| enemy scan | the one function with exactly two `E8 -> MatchAmmo` calls (8 callers total in each exe) L2532-2544 | calls 0x7EF744 / 0x7EF8AF in 0x7EF410..0x7EFA01 | none: pen only READS the builder returns 0x7EF741 / 0x7EF8AC (3 bytes before these calls). Any future patch that rewrites or adds an `E8 MatchAmmo` breaks it ("enemy scan not unique / not found") |
| MakeAttack | function of the unique `40 88 BB C1 00 00 00` L2546-2551 | 0x77F629 in 0x77F5D0 | none |
| UI attack site + AddOrder | unique `E8 MakeAttack` followed (NOPs allowed) by `48 8B D0 4C 8D 44 24 20 48 8B ?? E8 AddOrder` L2553-2562 | site in Action::e_attack; AddOrder 0x67E750 | none; Teleport later detours AddOrder's PROLOGUE (loads after, and stance reads only the call site) |
| MakeRef / SetEntity / RefInit / TagCtor / ref vtable | call within 0x40 before the site whose body has `SIG_SETENT`; inside it `lea rax,[rip+vt]; mov [rdi],rax`, `SIG_TAG20` + next call, `SIG_REFINIT` L2564-2580 | 0xAB9210 | none |
| RefDtor / ref-list offset / TagDtor | first call within 0x30 after site+5+17; in RefDtor `48 81 C1 imm32(<0x1000) 48 8B D3 E8` = list offset (+0x198); next call = TagDtor L2582-2592 | 0x6CE920, +0x198, 0x3E46E0 | none (pen X redirects ANOTHER call to 0x6CE920, at 0x7A9D49) |
| order stamp | last `8B ?5 rip32` within 0x40 before the site, target past .text L2594-2599 | 0x13DF928 | none |
| sim clock | every `SIG_CLK` (`8B 9F ?? ?? 00 00 E8 rel32 3B C3 0F 82`) whose callee is `mov eax,[rip+g]; ret`; the most named g, >= 5 hits L2601-2614 | 20 hits ed (22 game, 20 srv) -> 0x13DF988 | none. Same global found by pen and chase (and GEM BoardFilter's method) |
| eFight factory | `E8 F` + `4C 8B 44 24 50 48 8B D0 48 8B CB E8 AddOrder`, F allocates 0xE0; all must agree else NULL L2616-2626 | 0x77CC00 | none |
| eMovementChange | every `8B 14 90 E8 X`, X allocates 0x48, all agree L2628-2637 | 0x788C00 | none (a chase-redirected call would point outside .text and simply be skipped) |
| UI click move | `B9 F8 00 00 00 E8` + three stores within 0x40, unique function L2639-2653 | 0x765FE0 | none |
| MakeMove | function of the first `lea r,[rip+"Moving to zero bug!"]`, starting `48 89 5C 24 08/10` L2880-2892 | 0x7888F0 | none (chase's eMoveTask(circle) 0x788A50 / Update 0x78BC80 are different functions) |
| ExecuteInstant + slots | unique fn with `48 39 81 68 08 00 00` within 0x40 of its start and `FF 90 F0 00 00 00` after; slots = 8-byte values after .text equal to it; Apply = slot before; `80 B9 imm32 00` in Apply; layout check slot 19; GetActor slot 0xF0, its first call = expand, first `FF 90 imm32(<0x400)` = CanApply L2660-2714 | see H3 | SELF only: after H3 the slots no longer hold ExecuteInstant (a second instance finds "no vtable holds ExecuteInstant") |
| take_item factory | data pair ("take_item" ptr, factory ptr) -> factory `lea rcx,[rip+vt]` -> vt[3] -> `lea rcx,[rdx+X]` + next call L2719-2753 | 0x77F4F0, +0xC8 | none |
| TagContainer ctor / FromString | data pair ("tags" ptr, factory) unique; ctor holds `C6 81 00 03 00 00 00`; parse vt[5] `41 B0 20` + call; FromString reads +0x318 L2758-2801 | 0x3E4E60 / 0x3E60D0 | none |
| order vtables | RTTI `.?AUeAttack@Order@@`, `.?AUeAttackBasic@Order@@`, `.?AUeFight@Order@@` (type descriptor -> COL -> pointer before vtable) L2806-2835 | 0xF6BF08 / 0xF6BDA8 / 0xF6B798 | none |
| shield ray site | unique 30-byte pattern L2894-2908 | 0x7F82ED | SELF only if enabled |
| blow table | `48 63 C1 48 8D r5 rip32 F3 0F 10 04 (80+r) C3` with 17/29/35 in the table, unique L2911-2919 | 0xF71368 | SELF: after H4 the table holds 45/45/45 and the finder no longer matches |
| InventoryBusy / CancelInventoryOps / GetCurrentWeapon / IsManualControlled | full function-start signatures, rip displacements wildcarded, each unique L2442-2457, L2920-2941 | 0x659050 / 0xAAD6F0 / 0x695600 / 0x67E5C0 | none |
| manual-control manager | `SIG_MGRACT`, both rip loads the same global, second callee exactly `48 8B 81 B0 03 00 00 C3` L2460-2462, L2928-2932 | 0x142C678 (hit at 0x1668D6); srv: 0 hits (expected) | none |
| linked flag count | `8B ?? C0 01 00 00 C1 E8 1C A8 01` count >= 20 L2426, L2869-2870 | 95 ed / 89 game / 86 srv | none |
| quant executor slot (runtime) | engine-thread stack walk from the TEB stack base: 3 outermost .text return addresses with unwind info that follow a call; the worker's must follow `call [reg+d8]`; frame fn = unwind begin of the 3rd; unique pointer slot in non-exec sections with a vtable header; RTTI name contains "QuantExecutor" L1805-1862 | live ed slot 0xF381D0 | none known; it requires EXACTLY ONE pointer to the frame function in the data sections: any other dll that hooks that slot first (or copies the vtable) makes stance's tick fail ("has N pointer slots") |

Install order inside gem_melee: keep chase -> stance -> pen (today's alphabetical order). Since nothing overlaps, any order would find the
same addresses; the two stance self-overlaps (PROLOG, ExecuteInstant slots, blow table) mean the stance module must run its finders exactly
once per process and before its own writes.

---------------------------------------------------------------------------------------------------------------------------------------
## 4. State

All tables are file-static arrays, zero at load, keyed by ENTITY POINTER unless said otherwise (no entity ids anywhere). "Wipe" = the new-mission
wipe `mission_flush()` L2250-2271, called by `log_epoch()` L2272-2282 when the sim clock goes back by more than 5000 ms (`now + 5000 <
g_epoch_last`). log_epoch runs at the top of every hook call (L1988), in scan_note (L1279) and in ms_tick (L1752); NOT from order_event,
ms_exinst or the button path (0.7.37: refs made there before the wipe are released through ref_free first).

| table | size / key | lifetime, eviction | flushed |
|---|---|---|---|
| `g_seen[512]` {unit, ms} L200-215 | ptr hash, 16 probe; "offered melee ammo in the last 20 s" | slot reused when same / empty / >20 s old, else home slot overwritten | wipe (memset) |
| `g_unit[512]` unit_t (23 fields) L304-348 | ptr hash, 16 probe | reusable 60 s after last use (`touch`); when all 16 live, home slot silently evicted (state lost) | wipe (memset) |
| `g_mmc[1024]` may-melee cache L401-415 | direct-mapped ptr hash; key (unit, tag vector begin, end, clock+1) | per sim clock value | `init_tag`, wipe |
| `g_mine[256]` own-order ring L547-550 | ORDER POINTER, ring | newest overwrites oldest; a freed order address reused by a new engine order reads as "mine" (inferred risk) | wipe |
| `g_pend[256]` pend_t (2 weak refs) L734-745 | linear, one per unit | until flushed (tick or scan) | flushed entries ref_free'd; wipe = ref_free all + memset |
| `g_last[1024]` last_t (1 weak ref) L748-774 | ptr hash, 16 probe | reusable 60 s after last ORDER (`ms`); home slot evicted when full (ref freed) | wipe (ref_free + memset) |
| `g_dpend[32]` draw queue (1 ref) L1198-1200 | linear, one per unit | until flushed | wipe (ref_free + memset) |
| `g_known[256]` poll list (1 ref) L1456-1458 | linear search, one per unit | LRU by `ms` (signed age) | dead refs dropped by order_poll; wipe (ref_free + memset) |
| `g_kcache[1024]` {unit, ms} L1469-1471 | direct-mapped ptr hash | 200 sim ms "already noted" | wipe |
| `g_want[32]` (2 refs) L1495-1499 | linear | WANT_MS 12000 | want_poll; wipe (ref_free + memset) |
| `g_hostile[256][32]` owner-pair bitset L1503 | owner byte pairs | grows during a mission | wipe |
| `g_vtn_*[64]` RTTI name cache L1436-1453 | vtable ptr | session (first 64 vtables only) | never |
| `g_vtc[16]` action vtable copies L2092-2110 | vtable ptr; 64-slot VirtualAlloc copies | session | never (never freed) |
| `g_guntc[32]` weapon-name TagContainers L2174-2196; `g_melee_tc` L1196 | name; VirtualAlloc 4 KB each | session; a failed build caches NULL and uses the slot | never (never freed) |
| `g_draw_action` L891/L2283 | the melee_toggle action object | session, from its first click | never |
| `g_quant_cb[4]` L1777 | callbacks | process | never |
| per-ref answer cache inside each weak ref slot (+0xF0 clock+1, +0xF4 answer, +0xF8 entity) L825-839 | per ref | per sim clock value | with its slot |
| scalars | `g_poll_last` (wipe resets), `g_epoch_last`, `g_tick_clock`, `g_tick_wall`, `g_sim_tid`, `g_ui_tid`, `g_frame_state`, every `st_*` counter (process lifetime), log caps (per mission except `g_zlines`, `g_names_logged`) | | |

Weak refs (engine target-ref objects in 0x100-byte, 16-aligned slots): `ref_make` = vtable + TagCtor(+0x20) + RefInit + SetEntity(ref, ent,
temp Tag) (registers the ref in the entity's back-ref list at entity+0x198); `ref_free` = 0.7.33 listed test (2x VirtualQuery) -> if the
entity no longer lists the ref, zero ref+8 / ref+0x18 so RefDtor has nothing to unlink; then RefDtor + TagDtor(+0x20); `ref_ent` = state
`[ref+8]==1` and entity `[ref+0x18]`, trusted only while listed, answer cached per sim clock value (L790-839). 0.7.37: the wipe ref_free's
every live ref before the memsets, but ONLY when the list offset and RefDtor/TagDtor are known; otherwise plain memset (0.7.31 behaviour).

Clocks:
- Sim clock `*g_clock` (u32 ms, the engine's brain-timer clock) drives every decision (holds, cooldowns, probes, polls, caches, stalls).
- `GetTickCount` (wall): ms_tick rate limit `TickMs/2` (L1741-1743) - decides whether a given quant flushes queues / polls, so the
  timing of dll orders depends on frame rate (no MP desync: only the simulating machine acts; the ClientGate idles clients); hb_thread
  pacing (log only); Teleport lookup window (60 s, wall).
- `QueryPerformanceCounter`: HookTiming / tick cost (measurement only).

---------------------------------------------------------------------------------------------------------------------------------------
## 5. Helpers a shared core could replace (exact semantics)

- **Log** `logf_(fmt,...)` L86-98: lazy `fopen(path,"a")`, `vfprintf`, `'\n'`, `fflush` per line; no lock, no timestamp prefix (callers
  write `[%u ms]` sim time), never closed. Caps: per feature counters, most reset per mission by log_epoch. Shared sink must keep line text
  (other notes/tools grep `no-melee:`, `charge:`, `orders:`, `hb:`, `tick:`).
- **ini** `GetPrivateProfileIntA/StringA` on one path; floats via `atof` string; lists via `split_list` (space/comma/tab, lowercase,
  47 chars, max N) L2947-2959. Clamps in section 6.
- **PE / pattern**: `img_t` {base, .text lo/hi, RUNTIME_FUNCTION table} L2499; `func_of` binary search L2501; `in_text`; `next_call` (first
  E8 into .text within n); `rel32(at)` = at+4+int32 (`at` = displacement address); `match`/`MATCH` with uint16 + `W` wildcard; RTTI vtable by
  name (handles live and preferred-base pointers, offline test) L2806; runtime RTTI name `vt_name` with VirtualQuery checks L1438.
- **Patch writer**: `emit`, `emit_absjmp` (`FF 25 00000000 abs64`, 14 bytes) L3096-3103; VirtualProtect RWX + memcpy + restore +
  FlushInstructionCache; no thread suspension (chase suspends), no original-byte re-check at write time, no claim registry, no undo.
- **Memory probe** `readable(p,n)` L466-477: VirtualQuery loop; false unless MEM_COMMIT and not NOACCESS/GUARD/0. Used per weak-ref check
  (2 calls), per poll per known unit (`readable(v,0xDB0)`), per order event, held_weapon_name, vt_name, frame_hook_try, manual_diag (off).
- **Tags = string compare**: `vec_has(vec,list,n)` over the entity's std::vector of 0x60-byte tag records (std::string at +0x40),
  case-insensitive exact match; 0 when begin or end NULL, -1 broken (size not multiple of 0x60 or > 1024 tags). Entity tags vector at
  entity+0x4E8, ammo/weapon {tag} at stuff+0xEC8(+0x318), stuff name at +0x868(+0x318). No interned ids (chase's log shows it uses the
  engine's `HasTag` ed 0x10A3B0; pen uses `TagTest` ed 0x3E4A10).
- **Skip / direct control** `in_manual(ent)` L526-540: non-actor -> 0; SkipTags tag -> 1; then (SkipDirectControl=1) `ismanual_safe`
  (0.7.32: engine IsManualControlled only if [ent+0xDE0] and [ent+0xDD8] are both non-NULL) OR manager actor ([[mgr]+0x3B0] == ent) -> 1.
  NOTE: name says "manual" but it includes SkipTags. Not cached; per call: 1 vec_has + 1 engine call + 1 read.
- **Linked** `is_linked` (honours SkipLinked) / `linked_raw`: dword ent+0x1C0 bit 28; link holder [[ent+0x560]+8].
- **Human / actor / vehicle**: `ent[0x30] & 4` human, `& 2` actor; `is_vehicle` = actor && !human (vehicles AND cannons) L380.
- **Owner / AI-or-player**: owner byte `ent[0xDA8]` (different = enemy, no diplomacy, L611/L997/L1284/L1405); `player_owned` /
  `ad_player_skip` = brain [ent+0xDC8] non-NULL and `(brain[0x398] & 3) == 1` (human player's unit, the `{if user_control}` test).
- **Orders**: `front_order` (brain queue: active when [b+0x39C]&0x1000, deque map +0x70 / size +0x78 / offset +0x80 / count +0x88, two
  pointers per block; else override [b+0xF0]) L551-564; `order_target(ent, own)` (eAttack/eAttackBasic/eFight vtables, target type
  [o+0x48]==1, target [o+0x58]; own=0 excludes `g_mine`) L567-577. No probes.
- **Entity id**: none; pointers + engine weak refs. **Sim clock**: `now_ms()` = `*g_clock` or 0 L205; several sites deref `*g_clock`
  directly (only reachable with the clock found).
- **Engine order issue**: `g_addorder(unit, order, {1, *g_ordstamp})`, then `mine_add(order)` for charge orders. No cease-fire call
  (stance refuses candidates instead; the "weapon stop" ed 0x699CC0 belongs to pen / chase).
- **GetCurrentWeapon** on the holder [ent+0xDD8], params [w+0x98] (same engine function as chase).

---------------------------------------------------------------------------------------------------------------------------------------
## 6. Outside interface

Exports: `gem_quant_listen(void (ms_abi *cb)(void))` L1778-1788 (only export). Contract: up to 4 callbacks, NULL/duplicate ignored,
lock-free CAS registration from any thread at any time, never removed; each runs once per simulation quant on the engine thread from
`frame_hook`, right after ms_tick (whether or not ms_tick did anything: ClientGate / pause / rate limit do not stop callbacks), and only
once H2 is installed (after the first enemy-scan pick or Teleport order event of the session); callbacks must not give orders. Caller today:
GEM Teleport 0.2.13 `tp_quant_register()` (teleport.c L498-510): `GetModuleHandleA("gem_meleestance.dll")` + `GetProcAddress(...,
"gem_quant_listen")`, retried once per wall second from its jump-cursor path, registers `tp_quant` (aim end events + watch tick). Live: the
editor log of a1d6ed3c shows one registration (log line 347) in the first session; none in the 12:16 session (no jump aimed, inferred).

Imports from other dlls: `gem_teleport.dll!gem_order_listen(void (*cb)(void *entity, void *order))`, looked up by module name from
`hb_thread` once a wall second for 60 s (L1903-1907; only when Log=1). Teleport calls the callback from its `Entity::GiveOrder` detour
(teleport.c L565-570) on the engine thread for every order with a non-NULL order, before its lock decides; stance's `order_event` ignores
calls during its own hook / tick (its own AddOrder calls come back through this path), puts human units on the poll list and makes the next
tick poll at once (`g_poll_last=0`), never orders. Live 12:16 session: registered "live now", `order events 0` all session (unexplained;
earlier session: 44).

ini: `<dll dir>\gem_meleestance.ini` (80 keys; sections MeleeStance, Zerg, Charge, MultiSelect, ShieldMelee, Downed, BlowReach, AutoDraw,
Vehicles, ReloadCancel - no name clash with chase's `[meleechase]` or pen's `[PenFilter]`). Code default = default in load_ini; "ini" = snapshot
src ini (= dist ini values); "live" = payload a1d6ed3c (0.7.41) where it differs.

| key | line | code default | snapshot ini | live a1d6ed3c |
|---|---|---|---|---|
| [MeleeStance] Enabled / Log / SkipLinked / DrawFromBelt / SkipDirectControl | L2964-2968 | 1/1/1/1/1 | same | same |
| [MeleeStance] SkipTags | L2969 | tp_jetpack,hidden,melee_disabled,disable_melee,valkyrie,sensor_off (max 8) | same | same |
| [MeleeStance] IgnoreTargetTags | L2971 | hidden,melee_disabled,valkyrie,sensor_off (max 8) | same | same |
| [MeleeStance] DiagManual / HookTiming / FastPass / ClientGate | L2974-2977 | 0/1/1/1 | same | same |
| [MeleeStance] Tag / AlwaysTag | L2978, L3031 | melee_stance / melee_always | same | same |
| [MeleeStance] **AutoMeleeTag** (0.7.42, list max 4, empty = rule off) | L3032 | **melee_infantry** | melee_infantry | **absent -> code default = rule ON** |
| [MeleeStance] MeleeAmmoTags (max 24) | L2979 | melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target | same | same |
| [MeleeStance] DrawOnToggle / DrawCooldownMs / DrawButton | L3015-3017 | 1 / 3000 / melee_toggle | same | same |
| [Zerg] Enabled / Distance (int) / ReorderMs / Tag (one tag; empty = any) | L2986-2990 | 1 / 1600 / 700 / melee_infantry | same | same |
| [Charge] Enabled / Tag / MeleeInHand / PlayerUnits | L2982-2985 | 1 / has_melee_weapon / 1 / 1 | same | same |
| [Charge] **CooldownMs** | L2991 | **4000** | 2000 | 2000 |
| [Charge] RetargetMs / RefreshMs | L2992-2993 | 1500 / 400 | same | same |
| [Charge] SwitchRatio (0.1..1) / ProgressUnits (>=0) / CloseDistance (>=0; static init 150) / RunMode (<=3, -1 none) | L2996-3002 | 0.6 / 10 / 200 / 2 | same | same |
| [Charge] MoveRefreshUnits (>=10) / MoveRefreshFraction (0..1) | L3003-3006 | 40 / 0.4 | same | same |
| [Charge] MaxDistance / PlayerMaxDistance / MinDistance | L3008-3013 | 2000 / 1000 / 60 | same | same |
| [MultiSelect] Enabled | L3014 | 1 | 1 | 1 |
| [ShieldMelee] Enabled / SeeThroughTag | L3018-3020 | 0 / forcefield | same | same |
| [Downed] Enabled / Tag | L3021-3023 | 1 / start_personage | same | same |
| [BlowReach] Enabled / Near / Medium / Far (each >= 2) | L3024-3030 | 1 / 45 / 45 / 45 | same | same |
| [AutoDraw] Enabled / PlayerUnits / HoldMs / Distance (>=0) / Item | L3033-3038 | 1 / 0 / 3000 / 100 / melee | same | same |
| [AutoDraw] Probe / ProbeMs (0->1) / ProbeCooldownMs / NoMeleeMs | L3039-3042 | 1 / 300 / 4000 / 20000 | same | same |
| [AutoDraw] **ProbeAnyRange** | L3043 | 1 | 1 | **0** |
| [AutoDraw] KnownMs / OrderPoll / PollMs / BlindCheckMs / Proximity / EscalateMs | L3044-3049 | 300000 / 1 / 250 / 8000 / 1 / 1500 | same | same |
| [AutoDraw] LeaveFactor (>=1) / Tick / TickMs (>=20) / FightOrder / FightCooldownMs / OrderPicks | L3050-3055, L2973 | 1.5 / 1 / 100 / 1 / 1500 / 1 | same | same |
| [Vehicles] Enabled / InfantryMs / InfantryRange / CloseDistance / CloseAttackOrder | L3057-3063 | 1 / 500 / 400 / 400 / 1 | same | same |
| [Vehicles] VehicleMeleeTags / AdjacentMs / ContactTag (empty = off) / AdjacentMax | L3064-3070 | melee_light,melee_heavy / 1500 / hull_contact / 120 | same | same |
| [ReloadCancel] Enabled / KeepTags (max 24) | L3071-3075 | 1 / mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon | same | same |

Log files: `gem_meleestance.log`, `gem_meleestance_editor.log`, `gem_meleestance_server.log` next to the dll (append, never truncated).
Line families (exact formats in Part B): install block (`[GEM MeleeStance %s] installed: ...`, `  no-melee rule ON/OFF`, `  charge ON:`,
`  charge: ...`, `  refs: ...`, `  multi-unit instant buttons ON ...`, `  auto-draw ON:`, `  fight range (0.7.16):`, `  vehicles ON:`,
`  reload cancel ON:`, `  direct control:`, `  downed targets ignored:`, `  blow reach ON:`, `  melee through shields ON/OFF:`), runtime
`tick:`, `quant listeners:`, `orders: listening ...` / `orders: no gem_order_listen ...`, `hb:` (counters every 5 s wall: hook calls, scan,
humans, human vs enemy human, fast pass, no-melee refused, may-melee cache hits, frame hook, ticks, gated, wrong thread, sim/ui tid, polls,
order events, poll list, tick cost, hook cost, engine MatchAmmo), `--- clock A -> B: new mission ...`, `no-melee:`, `stance:`, `seen:`,
`zerg:`, `probe:`, `adiag:`, `charge:`, `  ^ player unit`, `held:`, `vehicles:`, `auto-draw:`, `draw:`, `instant:`, `reload:`, `ranged:`,
`order:`, `raw:`, `orders:`, `poll:`, `manual:`, `mdiag:`, `skip:`, `ref:`, `melee ray:`. Notes rely on `hb:`, `no-melee:`, `orders:`
(flip diagnosis, known_bugs.md), `charge:`, `tick:`.

Game-side words read (all case-insensitive strings): entity tags `melee_stance`, `melee_always`, `melee_infantry` (AutoMeleeTag and [Zerg]
Tag), `has_melee_weapon` (charge tag = melee weapon in hand, set by skin_weapon.inc), `start_personage` (downed), `hull_contact` (40k_human.inc
{on contact}), SkipTags `tp_jetpack hidden melee_disabled disable_melee valkyrie sensor_off`, IgnoreTargetTags `hidden melee_disabled valkyrie
sensor_off`, `forcefield` (ShieldMelee, off); ammo {tag}s `melee_light melee_heavy medium_inf_melee_target medium_veh_melee_target
heavy_veh_melee_target` (melee ammo), `melee_light melee_heavy` (vehicle melee); held-weapon {tag}s KeepTags list; the `melee_toggle`
button name (action+0x60) and its {item} tags; take-item word `melee` (stuff\melee folder); RTTI order class names. Nothing is SET on
entities (no tags, props or events written); it gives engine orders (eFight, eAttack, eMoveTask + eMovementChange, eTakeItem) and cancels
inventory ops.

---------------------------------------------------------------------------------------------------------------------------------------
## 7. Link-level facts

- Non-static symbols: `DllMain` (L3283), `gem_quant_listen` (dllexport, L1778), `ms_hook` (L1970), `ms_getactor_cur` (L2095), `ms_exinst`
  (L2335), `ms_ray_ok` (L2391), and `main` in the test build (L3350). All four `ms_*` are referenced only by address inside this file (no asm
  references them by name; no `__asm__` block exists in the file): they can be made `static` (keep `used`) or prefixed. Every global
  variable is `static`.
- No `__asm__` blocks, no global asm labels. Machine code is emitted as bytes at install (trampoline, `FF 25` jumps, the ray stub).
- Generic macro names (TU-private, clash only through a shared header or unity build): `W`, `PATCH`, `MATCH`, `R`, `VTP` (never #undef'd),
  `FIXP` (#undef'd), `HITS`, `MAXT`, `TLEN`, `REFSZ`, `NSEEN`, `NUNIT`, `NAMT`, `NMMC`, `NMINE`, `NPEND`, `NLAST`, `NDPEND`, `NKNOWN`, `NKCACHE`,
  `NWANT`, `NQUANTCB`, `VTCOPY`, `NGUNTC`, `NVTNAME`, `PLINES`, `CLINES`, `ADJ_AHEAD`, `TAGS_VEC`, `SEEN_MS`, `WANT_MS`, `KCACHE_MS`,
  `CHARGE_INF_MS`, `PROGRESS_SAMPLE_MS`, `STALL_SAMPLES`, `OFF_BUTTONNAME`, `MS_VERSION`, `T_AMTAG`; enum constants `PASS`, `REJECT`, `MELEE`
  (L196); typedefs `match_t vfn1_t setent_t mkatk_t mkmove_t addord_t range_t fight_t mvchange_t uimove_t tcctor_t tcfromstr_t takeitem_t
  exinst_t getactor_t expand_t canapply_t pickctor_t invbusy_t invcancel_t curweap_t ismanual_t frame_fn_t quant_cb_t sites_t img_t seen_t
  unit_t pend_t last_t dpend_t known_t want_t fake_t`; static function names that the other sources also use (`logf_`, `rel32`, `match`,
  `readable`, `find_all`, `install`, `load_ini`, `func_of`) are fine as separate TUs.
- C, not C++: `g_draw_action` is a tentative definition twice (L891, L2283). GNU extensions: `__attribute__((cleanup))` (L1985),
  `aligned(16)` on stack arrays and struct members, `ms_abi`, `force_align_arg_pointer`, `used`, `noinline`, `__builtin_return_address`,
  `__readgsqword` (intrin via windows.h), `RtlLookupFunctionEntry`. Headers: windows.h (WIN32_LEAN_AND_MEAN), stdint, stdio, string,
  stdarg, math. Libraries: kernel32 + the CRT (msvcrt via zig's mingw target); test also `_strdup`, CreateProcessA.
- Flags (build.bat L24): `zig cc -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off -fno-fast-math`, dll `-shared -s`, test
  `-DMS_TEST`. Distances use float sqrtf / compares: keep the fp flags identical (MP: only the host simulates, but parity tests compare).
- The detour target must be entered by JMP with the engine's return address on the stack (`__builtin_return_address(0)` decides
  `from_scan`, L1972/L1984). A core detour framework that CALLs the handler, or wraps it in a stub that pushes, breaks the scan test.
- `ms_hook` has the engine's signature `void *(ms_abi)(uint8_t *ctx)`; ms_abi is the default on this target but stays explicit.

---------------------------------------------------------------------------------------------------------------------------------------
## 8. Self-test (Part B "L3304-L4125" has the details)

- Built by `build.bat test` = same flags + `-DMS_TEST -o dist\ms_test.exe`; run with the three exe paths (game, editor, server).
- Exe mapping: `LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES)` (image mapped, no DllMain, imports unresolved; finders also accept
  preferred-base pointers through `vt_delta` / `FIXP` / `VTP` in case the image is not relocated). Per exe: `find_all` and a printed check
  of every finder (expects ammo tags 0xEC8, >= 20 linked sites, all features found; manager missing only for a path containing "server").
- Fakes: `fake_t` objects (0x1400-byte entity + tag records as MSVC std::string), fake MatchAmmo (`fake_orig` returns 0x1234 and records
  ctx[0x18]), fake clock / stamp / AddOrder / MakeAttack / SetEntity / Range / inventory / IsManualControlled / take-item; fake brains with
  a one-block order deque; fake ref lists. Real code paths are called directly (`ms_decide`, `ms_hook`, `charge_note`, `charge_flush`,
  `scan_note`, `order_poll`, `ms_tick`, `adj_shift`, `ms_ray_ok`, `reload_cancel`, `ref_ent`/`ref_free`, `log_epoch`, ...).
- Cases (build_log.txt L61-85, one line each): refresh threshold; log caps per mission; charge (13 checks); 0.7.41 ghost; shield ray (5);
  0.7.39 infantry-first; vehicles + auto-draw (10); hull contact (4); direct control (11); reload cancel (6); own tick (3); fight-range
  stance (3); back to the gun (5); order poll (6); order picks (4); fight range (17); stale refs (7); 0.7.35 (11); 0.7.36 (3); 0.7.37 (4);
  0.7.42 (a) 16, (b)/(c) 7, (e) 4, (d) 6; summary `filter: 8 cases + 3 hook cases + charge, %d failed`.
- Second pass: the first run re-executes itself as `"<ms_test.exe>" --amtag` (CreateProcessA, inherited std handles) with AutoMeleeTag
  `"melee_infantry, MM_Test"` and `mm_test` on every fake unit; needs a FRESH PROCESS because all tables / counters are static and the
  cases depend on block order and leftovers.
- To make `main()` callable from a combined test exe: rename to e.g. `int stance_selftest(int argc, char **argv)` (L3350); keep the `#ifdef
  MS_TEST` split (the test needs `install`/DllMain absent); replace the self-re-exec (L4104-4121) by having the combined exe pass a
  distinct flag (e.g. `--stance-amtag`) that it routes to `stance_selftest` in a child process (simplest; no reset function exists), OR
  write a `stance_reset()` that memsets every table / counter and restores every `cfg_*` default and run the second pass in-process
  (larger edit; risky because the defaults live in 60+ initialisers); route `printf` to the shared test output; the finder part must run
  on unpatched images (it does: the exe files are mapped fresh). The test does not cover `frame_hook_try` (runtime stack walk) or any
  install write.

---------------------------------------------------------------------------------------------------------------------------------------
## 9. Port list (smallest edits; snapshot line numbers)

1. L43-49: add `#include "../core/core.h"` (shared API) after the system headers; nothing else in the includes.
2. L81-98 `logf_`: keep the name and signature, replace the body with a call into the shared sink (`core_logv(CORE_STANCE, fmt, ap)`), so
   the 300+ call sites stay untouched; `g_logpath` (L82) and the DllMain path code go away. Keep the line texts (`[%u ms]` prefixes included).
3. L1778-1788 `gem_quant_listen`: keep it the dll's export with the same name and ABI (Teleport looks it up by name). Either it stays in the
   stance TU, or the core owns the quant hook (H2) and `frame_hook` calls `stance_tick()` (= `ms_tick`) first, then the listeners - the
   order "stance tick, then callbacks, then engine" must stay.
4. L1896-1928 `hb_thread`: started from the attach function exactly as L3158 does (only with Log=1, for parity) - flag the Teleport lookup
   being Log-dependent as a decision (section 10); `GetModuleHandleA("gem_teleport.dll")` unchanged.
5. L2961 `load_ini(const char *ini)`: unchanged, called with the shared ini path (all sections keep their names; no clash).
6. L3094-3303: rename `install()` to `int stance_attach(void)` (or `(const char *ini)` doing load_ini + install), `#if 0`/remove `DllMain`
   (L3283-3303); move the log-path logic to the core. Return 0/1 so the core can log "module off".
7. L3141-3154 (H1), L3177-3204 (H5), L3206-3226 (H3), L3268-3280 (H4), L1853-1857 (H2): route each write through the core patch registry
   (claim range, expected original bytes = what the finder matched, then write). H1 must remain a 14-byte `FF 25` jump straight to `ms_hook`
   (no wrapper stub) and the trampoline must keep the 14 copied bytes + abs jump.
8. L1970, L2095, L2335, L2391: make `ms_hook`, `ms_getactor_cur`, `ms_exinst`, `ms_ray_ok` static (keep `used`), or prefix `stance_`.
9. L2272-2282 `log_epoch`: either keep it as the stance mission detector (parity) and add a call-out `core_new_battle(now)`, or let the core
   detect the new battle and call `stance_mission_flush()` (= `mission_flush` L2250 + the cap resets L2276-2279); the reset point must stay
   "before any per-unit state is read in the hook" (L1988).
10. Double-load guard (new, before find_all in the attach): `GetModuleHandleA("gem_meleestance.dll")` loaded -> stance module off with a log
    line (the old dll would otherwise be the one to fail, see section 10).
11. L3350 `main` -> `stance_selftest(argc, argv)` and L4104-4121 re-exec flag (section 8).
12. Expose (non-static wrappers in a small `stance_api` block at the end of the TU, before `#ifndef MS_TEST`): see the list below.

Functions the other two modules could want from this one (all exist today as statics; wrappers only):
- `stance_may_melee(ent)` (= `may_melee` L405, 0.7.42 rule incl. stance tags) and `stance_in_stance(ent)` (= `tag_stance` L382).
- `stance_skip_unit(ent)` (= `in_manual` L526: SkipTags + direct control with the 0.7.32 component guard + manager) - the hardened
  direct-control test the plan wants shared; `stance_is_linked(ent)` (L228 / L301).
- `stance_is_melee_ammo(ammo)` (MeleeAmmoTags on ammo+0xEC8+0x318, L656) and `stance_is_vehicle_melee_ammo(ammo)` (VehicleMeleeTags).
- `stance_front_order(ent)` / `stance_order_target(ent, own)` / `stance_is_own_order(order)` (g_mine) L551-630 - chase's hold and the R4 flip
  logger need "whose order is in front".
- `stance_now()` / the clock pointer, `stance_quant_listen(cb)` (internal callers), `stance_on_order(ent, order)` (fan-out if the core
  takes over the Teleport listener).
- engine pointers already found: GetCurrentWeapon, IsManualControlled, AddOrder + order stamp, MakeAttack, eFight, eMoveTask, eTakeItem,
  RefDtor/TagDtor + weak-ref helpers `ref_make/ref_ent/ref_free` (L778-839).
- the last verdict per unit for R4 (needs a small new record: unit, clock, verdict, ammo) - not in the source today.

---------------------------------------------------------------------------------------------------------------------------------------
## 10. Risks and questions

Risks (port / merge):
1. **dist dll is not the final 0.7.42**: `inputs\meleestance_0.7.42\dist\gem_meleestance.dll` 12:40:23, `src\gem_meleestance.c` 12:58:10,
   `dist\ms_test.exe` 12:59:03 (= the source). Parity tests must use a rebuild from src, not the dist dll (inferred from timestamps).
2. **AutoMeleeTag default**: the code default is `melee_infantry` (rule ON). The live a1d6ed3c ini has no such key, so reusing it turns the
   rule on; README says ship it empty until Alex tags the breeds (only 6 MP breeds carry melee_infantry). Decision needed. Live ini also has
   `ProbeAnyRange=0` (snapshot 1).
3. `[Charge] CooldownMs` code default 4000 vs every ini 2000: a merged ini that drops the key changes behaviour.
4. Teleport finds the export by the module name `gem_meleestance.dll`: with only gem_melee.dll loaded, Teleport's aim-end tick silently
   degrades (its own fallback). If an old gem_meleestance.dll is ALSO loaded (it sorts after gem_melee), it fails its own find_all (MatchAmmo
   already patched) and installs nothing, but still exports gem_quant_listen: Teleport registers with that dead dll and never gets a tick.
5. H1 parity depends on JMP-entry into `ms_hook` (return address = engine call site). H2 depends on a runtime stack walk on the engine
   thread; it is untested offline and depends on exactly one data pointer to the frame function.
6. No write-time original-byte checks and no thread suspension for H1/H3/H4/H5 (safe today because DllMain runs at injection before the
   engine threads work, inferred); a registry that re-checks will need the expected bytes from the finder (PROLOG 14, ray 30, slot values,
   table 17/29/35).
7. Orders are issued from inside the engine's MatchAmmo (another unit's scan pick) whenever H2 is not installed (L2002-2003, L2021,
   `ranged_back` issues directly). With H2 in, only from the quant tick.
8. Wall-clock gate in ms_tick (TickMs/2 via GetTickCount) makes dll order timing frame-rate dependent on the host.
9. Pointer-keyed tables: `g_mine` order-pointer reuse; `unit_of`/`last_of` silent home-slot eviction when 16 neighbours are live; the
   new-mission wipe needs the clock to go back > 5 s.
10. `ms_exinst` swaps the action object's vptr around a vanilla call (not exception-safe); the expand vector leaks per click; vt copies are
    64 slots (any engine call through a higher slot during ExecuteInstant would read past the copy - inferred, slots used are <= 30).
11. `reload_cancel` calls GetCurrentWeapon with `[u+0xDD8]` unchecked (L2144); the other two callers check it.
12. The downed filter returns NULL for EVERY shooter vs a downed human (vehicles, crews and direct-control units too), before ms_decide.
13. ClientGate opens on the first MatchAmmo call of any kind; the header says the UI also calls MatchAmmo locally (L20-21), which on a
    client would open the gate (Nov 10 client log showed 0 calls, per the comment L1737-1739).
14. Log=0 removes the helper thread and with it the Teleport order listener.

Questions (not settled from the files):
- Q1 Should the merged dll keep "no helper thread when Log=0" (parity) or always connect to Teleport?
- Q2 Which AutoMeleeTag ships (empty = 0.7.41, or melee_infantry), and is ProbeAnyRange 1 (snapshot) or 0 (live)?
- Q3 Game / server addresses of the quant-executor slot are unknown (never logged offline); is a live server log available?
- Q4 Why did the 12:16 editor session record `order events 0` while Teleport reported the listener "live now"?
- Q5 Does `draw_melee` from `ms_exinst` on an MP client (AddOrder called locally after the vanilla click) reach the host, or does only the
  host's own path draw? Not visible in the source.
- Q6 Who owns H2 in gem_melee: stance (as today, lazy) or the core (so chase / pen can use per-quant callbacks)? If the core, Teleport's
  callback order relative to ms_tick must stay.

---------------------------------------------------------------------------------------------------------------------------------------
## Extras asked for

### Per-pick fast pass (0.7.35) and costs
- Path of a pick (ms_hook_body L1982-2075): return-address window test, `g_in_hook++` (+cleanup), `frame_hook_try` (one interlocked op per
  SCAN pick, even after install), `log_epoch` (compare), `fast_nomelee` (FastPass && clock: unit slot lookup (hash + <= 16 compares),
  may_melee (cache hit or one vec_has over the unit's tags x 2-6 words), vec_has over the ammo tags x 5 words), ignored-target vec_has
  (target tags x 4), [no tick] queue flushes, [scan] scan_note unless fast, counters, [scan] known_note x2 (200 ms kcache in front of a
  256-slot linear search), hostile_set, [no tick] order_poll, [non-scan] raw log + known_note, order_pick unless fast, downed vec_has, then
  fast -> engine; else `ms_decide` (in_manual = SkipTags vec_has + IsManualControlled engine call + manager read; linked; ammo vec_has;
  stance vec_has; may_melee; vehicle / probe / hold branches) and the engine call; HookTiming adds 4 QPC reads.
- Fast = a human whose unit slot says "no melee weapon" (nomelee_until in the future, no probe / blind draw / hold / melee_until), OR
  (0.7.42) a human that may not melee; and the candidate is not melee ammo. A melee candidate is never fast.
- Numbers: before 0.7.35, 22 us of dll time per pick vs 0.6 us for MatchAmmo (Alex's Nov 10 match, 2.6 M picks / 57 s; README). Live 0.7.41
  editor (a1d6ed3c): 549 ns average over 96400 calls (session 1); 2327 ns over 39031 calls with the engine's MatchAmmo at 61307 us total =
  1571 ns per call (session 2, dread vs Russ test); `fast pass 0` in both (0.7.41 fast only for known no-melee units). Tick: 8-52 us average,
  up to 431 us worst.
### Quant tick and the export contract: section 2 (H2), section 6 (exports), Part B L1734-1862.
### gem_order_listen import: section 6; Part B L1863-1878, L1896-1928.
### Weak-ref rules 0.7.33 / 0.7.37: section 4 "Weak refs"; Part B L790-839, L2237-2271.
### New-mission wipe: section 4; Part B L2232-2282.
### ClientGate: `[MeleeStance] ClientGate=1` (L1730, L2977): `ms_tick` returns (st_tick_gated++) and `order_event` stops after
  `frame_hook_try` while `st_hk_all == 0`, i.e. until this process has made one MatchAmmo hook call; `st_hk_all` is incremented on every
  hook call (L1997, L2004) and never reset, so the gate opens once per process and stays open. Quant callbacks of other dlls are not gated.
### Multi-select button patch (UI side): H3; Part B L2077-2110, L2283-2371.
### The exact 0.7.42 "may melee" rule
`may_melee(ent)` L405-415 = 1 when AutoMeleeTag is empty (`n_amtag == 0`), else 1 when the entity tag vector (ent+0x4E8) holds, case-
insensitively, `[MeleeStance] Tag`, `AlwaysTag` or any AutoMeleeTag entry (`g_mml`, built in init_tag L3088-3090), or the vector is broken
(-1 -> treated as may melee); cached per (entity pointer, tag vector begin, end, sim clock + 1) in `g_mmc[1024]`, no caching without a clock.
Applied at:
1. `ms_decide` L659-663: not in stance and may not melee -> gun candidates PASS (engine's choice), melee candidates REJECT + `no-melee:` log;
   placed AFTER the direct-control / SkipTags (in_manual) and linked exemptions (those units keep PASS) and BEFORE zerg, the vehicle rule
   (so an ordered vehicle is refused too) and the probe.
2. `ms_hook_body` L2046-2047: such a REJECT returns NULL without the `stance:` line (counted in `st_nomm`).
3. `nomm_cand` L1958-1967 via L1998: a melee candidate against an IgnoreTargetTags target (otherwise straight to the engine) is refused too,
   unless linked or in_manual.
4. `fast_nomelee` L1952: every gun candidate of a unit that may not melee takes the fast pass.
5. `scan_note` L1287: no probe, hold, draw, adiag or infantry-seen mark.
6. `order_pick` L1411: no fight-range handling for order picks.
7. `charge_note` L1010: never queued (after all other gates, before the distance math).
8. `charge_flush` L902: a queued charge / fight / draw of a unit that lost the right is dropped (`st_charge_drop`).
9. `autodraw_flush` L1253: a queued take-item draw is dropped.
10. `want_poll` L1562: a wanted fight is dropped (the kind-1 "give the attack order back" entries before it still run, L1554-1561).
11. `order_poll` L1628-1632: no hold / draw / fight order; if the dll drew a melee weapon earlier (`gun[0]`) and the unit is not downed,
    `ranged_back` sends it back to that gun. The `orders:` front-order log still runs for these units.
Not applied (unchanged for such units): `draw_melee` from the melee_toggle click (draws for the unit being switched into stance; that click
adds the stance tag, so it may melee from then on), `ms_exinst`, `reload_cancel`, `ranged_back`, the downed filter, bookkeeping (known_note,
hostile_set, counters), `zerg_unit` itself (its callers are gated), `ms_ray_ok`, blow reach.

---------------------------------------------------------------------------------------------------------------------------------------
# Part B

# Source walk: gem_meleestance.c 0.7.42 (snapshot), in file order

Snapshot: `inputs\meleestance_0.7.42\src\gem_meleestance.c`, 4125 lines (CRLF; the brief said 4064, the snapshot taken 12:58 has 4125),
sha256 96b0c33e...b474 (= MANIFEST.json). All `L<n>` references below are lines of that file.

## L1-L53 header, includes, version
- L1-42 comment header (still says "0.2" / 1.067 offsets; historical, game 1.067 addresses, not current).
- L43-49 includes: `WIN32_LEAN_AND_MEAN`, `<windows.h> <stdint.h> <stdio.h> <string.h> <stdarg.h> <math.h>`.
- L51 `#define MS_VERSION "0.7.42"`; L52 `#define MAXT 24` (max tags in a list); L53 `#define TLEN 48` (tag string buffer).
  Generic macro names MAXT / TLEN / NSEEN / NUNIT / NMINE / PASS / REJECT / MELEE (enum L196) collide only if two modules share a
  header or a unity build; as separate TUs they are private.

## L55-L79 config globals (code defaults; the ini overrides them at start-up)
- `cfg_enabled=1 cfg_log=1 cfg_skip_linked=1 cfg_lift_ready=1` (L56); `cfg_tag="melee_stance"`, `cfg_atag="melee_always"` (L57).
- `cfg_mtags[MAXT][TLEN]` code default = melee_light, melee_heavy, medium_inf_melee_target, medium_veh_melee_target,
  heavy_veh_melee_target; `n_mtags=5` (L58-60).
- `[Charge]`: `cfg_charge=1 cfg_charge_inhand=1 cfg_charge_player=1` (L61), `cfg_ctag="has_melee_weapon"` (L62),
  `cfg_charge_cd=4000` (code default 4000, ini 2000!) `cfg_retarget=1500` (L63), `cfg_refresh_ms=400` (L64), `cfg_charge_max=2000`
  (L67), `cfg_charge_pmax=1000` (L68), `cfg_charge_min=60` (L69).
- `[Downed]`: `cfg_down_on=1`, `cfg_downtag[1]="start_personage"` (L70-71), counter `st_down`.
- `[BlowReach]`: `cfg_blow_on=1`, `cfg_blow[3]={45,45,45}` (L73-74).
- Engine offsets as config: `cfg_enttags=0x4E8` (entity script tag vector) L75, `cfg_ammotags=0xEC8` (stuff {tag} Tags object; REPLACED
  at install by the offset read from MatchAmmo code) L76, `cfg_namtags=0x868` (stuff name Tags) L77, `#define TAGS_VEC 0x318` (vector
  inside a Tags object) L78, `cfg_linkoff=0x1C0 cfg_linkbit=28` (linked flag) L79.

## L81-L98 logging
- `g_logpath[MAX_PATH]`, `FILE *g_log`, line caps `g_lines g_clines g_hlines` (L82-84). `log_epoch(uint32_t now)` forward (L85; resets caps
  per mission, defined later).
- `logf_(fmt,...)` L86-98: lazily `fopen(g_logpath,"a")` once (if path set), `vfprintf`, `fputc('\n')`, `fflush` EVERY line. No lock, no
  timestamp prefix (callers put `[%u ms]` sim time themselves). Called from engine thread (hook / tick) and from the heartbeat thread
  (see later) without a lock: CRT FILE* has its own lock in msvcrt/ucrt (inferred), lines can interleave only at line granularity.

## L100-L153 tag readers (string compare)
- `lc()` ASCII lower (L101).
- `str_get(p,&n)` L104-111: MSVC std::string at p (buf/ptr +0, size +0x10, cap +0x18); returns NULL if `cap<size || cap>0x10000 ||
  size==0`; SSO when cap<=15.
- `eq_ci(s,n,z)` L113-119 case-insensitive exact compare.
- `vec_has(v,list,n)` L123-137: v = std::vector of 0x60-byte tag records (string at +0x40). Returns 0 when begin or end is NULL, -1 when
  end<begin or `len % 0x60` or `len > 0x60*1024` (broken), else 1 if any tag equals any list entry (case-insensitive), else 0.
  Linear scan over all tags x list. NO memory probe (trusts the vector pointers).
- `vec_str(v,out,cap)` L140-153: space-joined names for the log, max 64 tags.

## L155-L184 engine function typedefs and pointers (all static)
- typedefs `match_t vfn1_t setent_t mkatk_t mkmove_t addord_t range_t fight_t mvchange_t uimove_t` (L156-170) - generic names, static
  file scope (typedefs have no linkage; only matter in a unity build).
- Pointers: `g_orig` (MatchAmmo trampoline) L171, `g_fight` (Order::eFight ctor) L172, `g_mvchange` (eMovementChange) L173, `g_uimove`
  (click move order) L174, `cfg_runmode=2` L175, `g_tagctor g_tagdtor g_refinit g_refdtor` L176, `g_setent` L177, `g_mkatk` L178,
  `g_mkmove` L179, `g_addorder` L180, `g_range` L181, `g_refvt` L182, `volatile uint32_t *g_ordstamp` L183 (AddOrder param stamp global),
  `g_scan_lo/g_scan_hi` L184 (return-address window of the enemy scan).

## L186-L228 stance filter state, seen table
- `g_tagl[2][TLEN]` (stance tags: Tag + AlwaysTag), `g_ctagl[1]` (charge tag), `n_tagl=1` (L187-188).
- counters `st_reject st_allow st_charge_q st_charge_go st_charge_drop` (L189); heartbeat counters `volatile LONG st_hk_all st_hk_scan
  st_hk_human st_hk_enemy st_hk_polls` (L193), `st_fast` (L194).
- `enum { PASS=0, REJECT=1, MELEE=2 }` L196 = ms_decide verdicts.
- seen table L200-215: `seen_t{unit,ms}`, `NSEEN 512`, `SEEN_MS 20000`, `g_seen[512]`; key = unit POINTER, hash `((ptr>>4)*2654435761)%512`,
  16-slot linear probe; a slot is replaced when same unit / empty / older than 20 s; if none of the 16 qualifies, the home slot is
  overwritten (L209 default `slot=&g_seen[h]`).
- SIM CLOCK: `volatile uint32_t *g_clock` L204; `now_ms()` L205 = `*g_clock` or 0 when not found. (Sim ms, not wall-clock.)
- `carries_melee(u)` L217-226: returns 1 if no clock found (old behaviour), else 1 if u is in g_seen within 20 s.
- `is_linked(ent)` L228 = `cfg_skip_linked && ((*(u32*)(ent+0x1C0) >> 28) & 1)`. `linked_raw(e)` L301 = same without the ini switch.

## L230-L348 per-unit table (g_unit)
- `cfg_ad_on=1 cfg_ad_player=0` (L241). `ad_player_skip(ent)` L247-252: returns 0 when [AutoDraw] PlayerUnits=1; else 1 when
  `brain=[ent+0xDC8]` non-null and `(brain[0x398]&3)==1` (= unit owned by a human player, the `{if user_control}` test). No probe.
- `cfg_ad_dist=100 cfg_ad_hold=3000` L253-254; probe `cfg_probe_on=1 cfg_probe_any=1 cfg_probe_ms=300 cfg_probe_cd=4000
  cfg_nomelee_ms=20000` (L259-261); counters `st_probe st_probe_ok st_probe_fail st_probe_rej` L262, `g_plines` L263.
- fight order `cfg_fight_on=1 cfg_fight_cd=1500` `st_fight_q` (L267-269).
- vehicles `cfg_veh_on=1`, `cfg_vtags = melee_light, melee_heavy` n=2 (L270-272), `cfg_inf_ms=500` L273, `cfg_inf_range=400` L274,
  `cfg_close_veh=400` L276, `g_vreflines` L279, `cfg_veh_attack=1` L280.
- hull contact: `#define ADJ_AHEAD 10.0f` L290, `cfg_adj_ms=1500` L291, `cfg_adj_extra=120` (ini key AdjacentMax) L292, counters
  `st_adj st_adj_shift g_adjlines` L297, `cfg_contacttag[1]="hull_contact"` `n_contacttag=1` L298-300.
- counters `st_ad_hold st_ad_draw st_veh_reject st_veh_pref` L302.
- `unit_t` L304-327 fields: unit, ad_until, inf_ms, ad_draw_ms, auto_drawn, manual_ms, diag_ms, diag_seen, diag_code, ad_diag, probe_ms,
  probe_last, nomelee_until, probe_rej, melee_until, poll_ms, blind_ms, ovt, touch, gun[72], gun_ms, blind_ord, fr_ms.
  `NUNIT 512`, `g_unit[512]` L328-329. Key = unit POINTER.
- `unit_of(u, make)` L330-348: hash as above, 16-slot probe; hit -> `touch = now?now:1`, return. A slot is reusable when empty or
  `now - touch > 60000` (60 s since last use, 0.7.26). make=0 -> NULL on miss; make=1 -> memset the first reusable slot (or the home
  slot `g_unit[h]` when all 16 are live: a live unit is evicted silently) and set unit/touch.
- `cfg_known_ms=300000` L352; `melee_known(u)` L354-359: `melee_until = now + KnownMs` (0 -> 1), `nomelee_until=0`.
- `has_melee(u)` L360-373: carries_melee -> 1; melee_until in future -> 1; nomelee_until in future -> 0; else asks
  `melee_by_button(u)` (button CanApply, defined later): 1 -> melee_known, return 1; 0 -> nomelee_until = now + NoMeleeMs; -1 -> 0.
- `auto_active(u)` L374-379: cfg_ad_on && slot exists && ad_until in the future (signed compare).
- `is_vehicle(t)` L380: `t && (t[0x30]&2) && !(t[0x30]&4)` (actor, not human: vehicles AND cannons).
- `tag_stance(ent)` L382: vec_has(ent+0x4E8, g_tagl, n_tagl)==1 (Tag or AlwaysTag).

## L383-L425 0.7.42 "may melee" rule (AutoMeleeTag)
- `#define NAMT 4` L396; `cfg_amtag[NAMT*TLEN]="melee_infantry"` (raw ini string) L397; `g_amtag[4][TLEN]`, `n_amtag` (0 = rule off)
  L398; `g_amtag_s` comma-joined for the log L399; `g_mml[2+NAMT][TLEN] n_mml` = stance tags + AutoMeleeTag tags in ONE list (built
  in init_tag, later) L400.
- cache `NMMC 1024`, `g_mmc[1024]{unit, tb, te, clk, mm}` L401-402 (direct-mapped, key = entity pointer + tag-vector begin/end + sim
  clock+1). Counters `st_nomm st_mmc_hit` L403; `g_nmlines g_nm_ref` L404 (g_nm_ref = last ms_decide REJECT was a no-melee refusal).
- `may_melee(ent)` L405-415: `n_amtag==0` -> 1 (rule off). stamp = `*g_clock + 1` (0 when no clock -> no caching). Reads tb/te =
  vector begin/end at ent+0x4E8/+0x4F0. Hit when same unit, same stamp, same tb, same te. Else `vec_has(ent+0x4E8, g_mml, n_mml) != 0`
  (NOTE: a broken vector (-1) counts as "may melee" = 0.7.41 behaviour), stored. Not thread-safe (engine thread only, inferred).
- `nomm_refused(ctx, where)` L416-425: st_nomm++, g_nm_ref=1; logs `no-melee: [%u ms] unit %p refused "%s" vs %p%s: not in melee stance
  and no AutoMeleeTag tag (%s) (refused %ld)` (first 40 per mission, `g_nmlines`).

## L426-L540 direct control, skip tags, ignored targets
- `ismanual_t g_ismanual` L431-432 = eActor::IsManualControlled (found by signature later).
- `ismanual_safe(ent)` L436-441 (0.7.32): 0 if no fn / null ent; 0 if `[ent+0xDE0]==0 || [ent+0xDD8]==0`; else `g_ismanual(ent)!=0`.
- `cfg_manual_skip=1` (SkipDirectControl) L442, `st_manual` L443.
- `g_mgr` (uint8_t** to the eManualControl global) L448; `mgr_actor_is(ent)` L450 = `g_mgr && *g_mgr && [*g_mgr+0x3B0]==ent` (no probe).
- `manual_log` L451-458: per unit once per 3 s, 40 lines/mission: `manual: [%u ms] unit %p under direct control (property %d, manager %d):
  stance filter / charge / auto-draw / reload cancel skipped (skips %u)`. Creates a unit slot (unit_of make=1).
- `cfg_manual_diag=0` L464 (DiagManual), `g_mdiag`.
- `readable(p,n)` L466-477: VirtualQuery loop; 0 when not MEM_COMMIT, PAGE_NOACCESS/PAGE_GUARD or Protect==0. Memory probe #1 (system
  call per region).
- `manual_diag(ent)` L478-509: only when Log && DiagManual; only human-player units (brain[0x398]&3==1, probed). Logs `mdiag:` on state
  change (300/mission); searches the manager's first 0x1000 bytes for the unit pointer. Off by default (0.7.35).
- SkipTags `cfg_skiptags[8][TLEN]` = tp_jetpack, hidden, melee_disabled, disable_melee, valkyrie, sensor_off; n=6 (L513-514).
- IgnoreTargetTags `cfg_tgttags[8][TLEN]` = hidden, melee_disabled, valkyrie, sensor_off; n=4 (L519-520), `st_tgt_ignored` L521.
- `ignored_target(t)` L522-525 = t && n && vec_has(t+0x4E8, tgttags)==1.
- `in_manual(ent)` L526-540 = THE "skip this unit" test: 0 when ent NULL or not actor (`!(ent[0x30]&2)`); 1 when it carries a SkipTags tag
  (log `skip: [%u ms] unit %p carries a SkipTags tag: left alone`, 20 lines); then manual_diag; 0 when SkipDirectControl=0; else
  `prop = ismanual_safe(ent)`, `mgr = mgr_actor_is(ent)`; 1 (st_manual++, manual_log) when either. NOTE the name: in_manual covers
  SkipTags + direct control.

## L541-L593 orders (brain queue reader)
- `g_vt_attack g_vt_attackb g_vt_fight` (void** vtables of Order::eAttack / eAttackBasic / eFight, found by RTTI) L546.
- own-order ring `NMINE 256`, `g_mine[256]`, `g_mine_i`, `g_vlines` L547-549; `mine_add(o)` L550 (ring, newest overwrites oldest). Key =
  ORDER POINTER (an order freed by the engine and a new one allocated at the same address would be taken as "mine": inferred risk).
- `front_order(ent)` L551-564: brain=[ent+0xDC8]; if `[brain+0x39C] & 0x1000` (queue active): count `[b+0x88]` <=0 -> NULL; offset
  `[b+0x80]`, map size `[b+0x78]` (must be a power of two <= 0x100000), map `[b+0x70]`; block = map[(off>>1)&(ms-1)], returns block[off&1]
  (std::deque of 2 pointers per block); else the override order `[b+0xF0]`. No memory probe.
- `order_target(ent, own)` L567-577: front order whose vtable is eAttack/eAttackBasic/eFight; own==0 -> NULL when the order is in g_mine
  (linear 256 scan); target ref type `[o+0x48]==1` (entity) -> `[o+0x58]` target entity.
- `explicit_target(ent)` L578 = order_target(ent,0).
- `fight_running(ent,tgt)` L580-586: front order is eFight on tgt.
- `veh_ordered(u,tgt)` L587-593: explicit_target(u)==tgt; logs `vehicles: unit %p was ordered onto vehicle %p, melee allowed, no retarget`
  (20 lines).

## L595-L643 [Zerg], player_owned
- `cfg_zerg_on=1 cfg_zerg_dist=1600 cfg_zerg_reorder=700 cfg_zerg_tag[1]="melee_infantry"` L601-604; counters `st_zerg st_zerg_reorder`
  (volatile LONG) L605; `g_zlines` L606.
- `zerg_unit(ent, ti)` L607-623 = 1 when: Zerg on, ent and target-info non-null, ent is human (`ent[0x30]&4`); target `tg=[ti+0x18]`
  non-null, not self, human, different OWNER BYTE `tg[0xDA8] != ent[0xDA8]`, target not linked (`linked_raw`); unit carries the charge tag
  (`has_melee_weapon` = melee weapon in hand, L612); unit carries [Zerg] Tag if set (L615); brain non-null and NOT a human player's
  (`(br[0x398]&3)==1` -> 0); unit slot not `auto_drawn` (L618-619); 2D distance unit pos (ent+0x74/0x78) to the target info's recorded
  position (ti+0x28/0x2C) <= Distance. NOTE zerg_unit does NOT test may_melee itself; callers do (ms_decide L659 returns before it; charge
  paths gate with may_melee too) - the [Zerg] Tag and AutoMeleeTag are both melee_infantry by default.
- `front_is_mine(ent)` L624-630: front order is in g_mine.
- 0.7.39 line budgets: `#define PLINES 400` L633, `g_pclines g_xlines g_vfreelines` L634, counters `st_explicit st_veh_free st_redirect
  st_ghost` L635, `g_ghostlines` L636, `cfg_ghost=1` L637 (self-test switch; no ini key).
- `player_owned(ent)` L638-642 = brain non-null && (brain[0x398]&3)==1. (Same test as ad_player_skip without the ini switch, and as the
  inline tests in zerg_unit L617 / charge_note L1006.) Owner byte for friend/foe = `ent[0xDA8]` (L611, L997, L1040).

## L645-L730 ms_decide(ctx) = the per-candidate verdict (PASS / REJECT / MELEE)
ctx layout used: `[ctx+0]` ammo eStuff*, `[ctx+8]` shooter entity, `[ctx+0x10]` target info (ti): `[ti+0x18]` target entity, ti+0x28/0x2C/0x30
recorded target position (float3).
Order of checks (this is the stance half of rule R2):
1. L648 `g_nm_ref=0`. L649 `!cfg_enabled || !ctx` -> PASS. L652 no ammo / no ent -> PASS. L653 not human -> PASS.
2. L654 `in_manual(ent)` (SkipTags + direct control) -> PASS. L655 `is_linked(ent)` (SkipLinked) -> PASS.
3. L656 `m = vec_has(ammo+ammotags+0x318, MeleeAmmoTags)`; L657 broken vector -> PASS.
4. L658 `ts = tag_stance(ent)`. L659-663 (0.7.42) `!ts && !may_melee(ent)`: gun -> PASS; melee -> `nomm_refused(ctx,"")`, REJECT.
   (Applies to EVERY target, incl. vehicles and explicitly ordered targets; ignored-targets / downed are tested before ms_decide in ms_hook
   (see hook section) - verify there.)
5. L664-672 melee candidate: log `seen:` (60/mission, first offer per unit), `melee_seen(ent)` (g_seen), `melee_known(ent)` (unit slot).
6. L673-677 not stance && zerg_unit -> st_zerg++, log `zerg:` (40), return m ? MELEE : REJECT.
7. L678-702 [Vehicles] melee candidate vs `is_vehicle(target)` and ammo carries a VehicleMeleeTags tag:
   - `veh_ordered(ent,tg)` (explicit, non-dll order on THAT vehicle) -> ts ? MELEE : PASS (L682; with 0.7.42 a non-stance unit reaching here
     may melee, so PASS = engine's choice).
   - not stance -> st_veh_reject++, REJECT (L683).
   - `order_target(ent,1)==tg` (own charge order on it) -> MELEE (L684).
   - 0.7.39: unit slot inf_ms within InfantryMs and `!charging_infantry(ent)` -> st_veh_free++, log (40), MELEE (L690-694).
   - inf_ms within InfantryMs (and charging infantry) -> st_veh_pref++, log (20), REJECT ("infantry first") (L695-699).
   - else MELEE (L700).
8. L703-726 probe running (not stance, not auto hold, Probe=1): slot `probe_ms` set and younger than 4 x ProbeMs: melee candidate ->
   probe success (`probe_ms=0 nomelee_until=0 st_probe_ok++`, log `probe:` 60), then if target info present and 2D distance > AutoDraw
   Distance -> REJECT (far probe only learns) else MELEE; gun candidate -> `probe_rej++ st_probe_rej++` REJECT.
9. L727 not stance and no auto hold -> PASS. L728 melee -> MELEE. L729 gun: `has_melee(ent) ? REJECT : PASS` (keep guns if no melee
   weapon known; has_melee may call the button CanApply, see melee_by_button).

## L732-L888 charge tables and weak refs
- `#define REFSZ 0x100` L733. `pend_t` L734-743 (pending order queue): `uref[0x100]`, `tref[0x100]` (16-aligned engine weak refs), unit,
  tgt, dist, clock, ff (fight order even if not in hand), pl (player's target), used, veh, stalled. `NPEND 256`, `g_pend[256]` L744-745.
- `last_t` L748-756 (per-unit charge memory): unit, tgt, ms, draw_ms, chk_ms, lastd, has_ref, held, phase (1 run, 2 attack), ox/oy (enemy
  pos at order), ux/uy (own pos at last stall sample), stalls, vchk_ms, adj_until, vux/vuy, adj_tgt, otgt/ogo (order-path log), pl,
  `tref[0x100]` weak ref.
- `cfg_close=150` (code default; ini CloseDistance=200) L757; `cfg_refresh=40` L758; `cfg_refresh_frac=0.4` L759.
- `goal_moved(d,mx,my)` L763-768: threshold = max(MoveRefreshUnits, d*fraction).
- `PROGRESS_SAMPLE_MS 1000`, `CLINES 200`, `STALL_SAMPLES 2`, `NLAST 1024`, `g_last[1024]` L769-774; `cfg_switch=0.6 cfg_progress=10`
  L775-776.
- WEAK REFS (engine target-ref objects, 0x80 bytes used of a 0x100 slot):
  - `ref_make(ref, ent)` L778-789: memset 0x100; `[ref]=g_refvt`; `g_tagctor(ref+0x20)`; `g_refinit(ref)`; builds a temp 0x80 Tag on the
    stack (tagctor), `g_setent(ref, ent, tag)` (registers the ref on the entity's back-ref list), `g_tagdtor(tag)`. ENGINE CALLS - engine
    thread only.
  - `g_reflist_off` (entity back-ref list offset, read from RefDtor code; 0 = not found = no check) L797; `st_ref_stale g_reflines` L798.
  - `ref_listed(ref)` L799-809 (0.7.33): ent=[ref+0x18]; no ent or no offset -> 1; list = ent+off: `readable(l,0x28)` (VirtualQuery) else 0;
    begin=[l+8], end=[l+0x10]: must be non-null, end>begin, size multiple of 16, <=0x100000, readable (VirtualQuery); scans 16-byte entries
    for `entry+8 == ref` -> 1, else 0.
  - `ref_free(ref)` L810-824: if not listed -> st_ref_stale++, log `ref: weak ref %p names entity %p (state %u) but is not in its ref list:
    entity freed without notice, dropped with no unlink` (20), and zero `[ref+8]` (state) and `[ref+0x18]` (entity) so RefDtor has nothing
    to unlink; then `[ref]=g_refvt`, `g_refdtor(ref)`, `g_tagdtor(ref+0x20)`. Always runs the listed check (2 VirtualQuery).
  - `ref_ent(ref)` L828-839: state `[ref+8]!=1` -> NULL; ent=[ref+0x18]; no ent or no list offset -> ent; per-sim-clock cache in the
    slot's spare bytes: `+0xF0` = clock+1, `+0xF4` = answer, `+0xF8` = entity; hit -> cached answer; else ref_listed and store.
    (0.7.33 rule: a ref is trusted only while its entity still lists it; answer kept per sim clock value.)
- `last_reset(l,unit)` L841-848: frees the held ref (`ref_free`) then clears all fields.
- `last_of(unit, make)` L849-860: hash %1024, 16 probe; hit -> slot; make && (empty || `*g_clock - l->ms > 60000`) -> last_reset + return
  (NOTE: dereferences `*g_clock` unguarded: only reached when the clock was found - verify gating at install); no slot -> home slot
  reset (evicts a live unit; its ref is freed properly).
- `last_order(l,tgt,d,now)` L862-868: frees old ref, ref_make new, sets tgt/lastd/ms/chk_ms, stalls=0, records unit pos (ux/uy).
- `CHARGE_INF_MS 3000` L871; `charging_infantry(ent)` L872-881: own/explicit attack/fight order on a non-vehicle -> 1; else last_t with
  live ref on a non-vehicle and (front order is mine or ordered < 3 s ago).
- `dist3(a,b)` L882-888: 3D distance of entity positions (+0x74/+0x78/+0x7C).

## L890-L962 charge_flush(scanning) = issue queued charge / fight orders (ENGINE ORDER CALLS)
- `g_draw_action` (the melee_toggle action object, found when the button is used) L891; forwards `draw_melee`, `reload_cancel`.
- L897 `now = *g_clock` (unguarded). For each used pend entry with `clock != now` (queued on an earlier sim clock value) and
  `unit != scanning`:
  - L901-902 `u=ref_ent(uref)`, `t=ref_ent(tref)`; proceed only if both still name the same entities and `!is_linked(unit) &&
    !in_manual(unit) && may_melee(unit)` (0.7.11 / 0.7.42: taken over / lost the right since queuing -> dropped, st_charge_drop++).
  - `noweap` = unit lacks the charge tag (melee not in hand) L912; goal xy = target ENTITY position L913; close = veh ? [Vehicles]
    CloseDistance : [Charge] CloseDistance; `mv` (far phase) when a move maker exists, close>0, dist>close and not (veh && stalled) L915;
    goal near (0,0) -> no move (engine assert "Moving to zero bug!") L916.
  - noweap && draw action known && !ff -> `draw_melee(g_draw_action, vtable, unit)` (take-item; kind "draw" / "draw-wait"), no order.
  - else params = `{1, *g_ordstamp}`; far: optional `g_mvchange(-1, RunMode, 0)` + AddOrder (+mine_add) then `g_mkmove(xy,0)` (eMoveTask)
    or `g_uimove(xy,1)`, word order+0x18 = 4, kind run/advance, phase 1; close: `usefight = g_fight && !(veh && CloseAttackOrder)` ->
    `g_fight(tgt)` (eFight) else `g_mkatk(tref, veh?1:0)` (eAttack, double-click flag for vehicles), phase 2. `ok = g_addorder(unit, order,
    params)`; ok -> mine_add(order).
  - always (ok or not) `last_order(last_of(unit,1), tgt, dist, now)`, phase, pl, ox/oy. Counters st_charge_go / st_charge_drop.
  - log `charge: [%u ms] unit %p -> enemy %p at %.0f %s %s (orders %u, refused %u)` (200/mission; player units +400 more, extra line
    `  ^ player unit[, on the enemy the player ordered]`).
  - L959-960 both refs freed (`ref_free`), slot unused.

## L964-L985 pend_push(ent,tgt,d,veh,stalled,now)
- Globals `g_pend_ff`, `g_pend_pl` (L964-965) are implicit extra arguments (set by callers just before).
- Existing entry for the unit: infantry queued beats a vehicle (return); same kind and not nearer -> return; else replace target ref and
  fields. Else first free slot: ref_make both refs, fill, st_charge_q++. Full queue -> silent drop.

## L987-L1180 charge_note(ctx, inhand, stance) (called from the enemy-scan hook path; queues, never orders)
Gates in order: ent/ti/ammo non-null, human, not linked (SkipLinked) L993; not in_manual L994; target non-null, not self, actor L996; owner
byte differs L997; human target not linked L998; vehicle target only for [Vehicles] Enabled && stance L1000; target not downed (Downed
Tag) L1001; target not ignored (IgnoreTargetTags) L1002; non-stance needs the charge tag L1003 and, with MeleeInHand, `inhand` L1004;
PlayerUnits=0 -> not a human player's unit L1005-1008; ammo is melee (MeleeAmmoTags) L1009; 0.7.42 `!ts && !may_melee(ent)` -> return L1010.
- d = 3D distance unit -> recorded target pos (ti+0x28..0x30) L1011-1014.
- 0.7.41 ghost test L1020-1027: entity distance `de=dist3(ent,tgt)`; `cfg_ghost && gap>300 && gap>0.5*d` -> not charged, log (60).
- 0.7.40 player target redirect L1032-1052: `pt = explicit_target(ent)`; else when last_t.pl: if a non-mine front order exists ->
  pl=0, else pt = ref_ent(last.tref). A valid pt (actor, enemy owner, not linked human, not downed, vehicle only for stance) replaces
  tgt (log `charge: ... the scan chose %p, the player ordered %p: charging that one` 80 lines), d = dist3; ptgt=1.
- [Vehicles] L1053-1078: own/explicit order on a vehicle et != tgt: stays unless (infantry tgt AND d <= dist(et)*SwitchRatio). veh && et==tgt:
  hull-contact sample once per 1000 ms: unit moved < ProgressUnits and d < AdjacentMax -> `adj_tgt=tgt, adj_until=now+AdjacentMs` (log
  `vehicles: ... pressed against`), return (never re-orders an attack order on this vehicle).
- `held:` log of the charge-tag change (200) L1079-1086.
- auto-drawn unit (not stance): tag gone and hold over -> auto_drawn=0; else d > AutoDraw Distance -> return L1087-1093.
- 0.7.16 fight range L1095-1107: FightOrder && infantry && d <= AutoDraw Distance: fight_running -> return; explicit target other ->
  return; player-target memory on another -> return; same target ordered < FightCooldownMs ago -> return; st_fight_q++, goto queue.
- 0.7.39 explicit order on another enemy -> return (log `charge: ... has an attack order on %p: no charge at` 60) L1108-1117.
- reach L1118 `g_range(ammo)` (engine Range(stuff)); `d <= reach*1.1+1 || d < MinDistance` -> return.
- distance limits (not for the player's target) L1120-1125: MaxDistance; PlayerMaxDistance for player-owned units.
- re-order logic L1126-1174 on last_t: `drawn` = last phase 0 (draw) >= 200 ms ago and now in hand; zerg re-order (age >= ReorderMs,
  zerg_unit, front not mine) L1139-1142; renew (phase 1 at same target, inside close distance or goal moved) gated by RefreshMs, else
  RetargetMs; same target, not renew: stall test (1 s samples, own movement or distance closing by ProgressUnits; 2 stalled samples and
  age >= CooldownMs -> re-order `stalled=1`, log); other target alive: player target sticks (pl), never infantry -> vehicle, switch only
  when d <= dist(cur)*SwitchRatio.
- `queue:` L1175-1182: `g_pend_pl` = explicit target == tgt OR last_t.pl with live ref == tgt; `pend_push(ent,tgt,d,veh,stalled,now)`;
  g_pend_pl reset. (`g_pend_ff` is NOT set here; callers set it, see order poll.)

## L1185-L1272 auto-draw queue (take-item orders)
- typedefs `tcctor_t tcfromstr_t takeitem_t` L1189-1191; `g_takeitem` (Order::eTakeItem maker, found from the {item} action) L1192;
  `cfg_draw_cd=3000` (DrawCooldownMs) L1193; `g_tcctor g_tcfromstr` (TagContainer ctor / FromString) L1194-1195; `g_melee_tc` (built
  TagContainer, kept for the session) L1196; `cfg_ad_item="melee"` ([AutoDraw] Item) L1197.
- `dpend_t{uref[0x100], unit, clock, used}`, `NDPEND 32`, `g_dpend[32]`, `g_adlines` L1198-1201.
- `melee_tags()` L1203-1221: once: `VirtualAlloc(0x1000)` (never freed; survives missions), `g_tcctor(tc)`, MSVC SSO std::string of Item
  (max 15 chars), `g_tcfromstr(tc, s, ' ')`, logs `auto-draw: item tags built: "%s"`; empty -> NULL (not cached, retried).
- `dpend_push(ent, s, now)` L1225-1239: refuses when the charge tag is on (in hand), within DrawCooldownMs of `s->ad_draw_ms`, no
  takeitem maker, already queued, or queue full (32); else ref_make + sets `s->ad_draw_ms=now` (1 = queued).
- `g_rc_force` L1241 (reload_cancel ignores KeepTags while set; implicit argument).
- `autodraw_flush(scanning)` L1244-1272: `now=*g_clock` (unguarded); each used entry with clock != now and unit != scanning: live ref ==
  unit, not linked, not in_manual, charge tag not on, may_melee (0.7.42) -> `melee_tags()`, remember the held weapon name in unit slot
  `gun` (held_weapon_name), `g_rc_force` when fr_ms within 2000 ms, `reload_cancel(unit)`, `o=g_takeitem(tags)`, `g_addorder(unit, o,
  {1,*g_ordstamp})`; ok -> st_ad_draw++, ad_draw_ms, auto_drawn=1. Log `auto-draw: [%u ms] unit %p take-item order %s (auto draws %u, holds
  %u) | unit now has %s | gun to go back to: "%s"` (40). Ref freed, slot cleared either way.

## L1274-L1338 scan_note(ctx) (every enemy the scan looks at, before the ammo decision)
- L1279 `if (g_clock) log_epoch(*g_clock)` = the NEW-MISSION DETECTOR runs here (and elsewhere, see log_epoch).
- Gates: ent/ti non-null, human, not linked L1280; not in_manual L1281; target human, not self L1283; owner byte differs L1284; target
  not linked L1285; target not downed L1286; 0.7.42 `!may_melee(ent)` -> return L1287 (so "infantry seen" (inf_ms) is NOT recorded for
  units that may not melee either - harmless: those units are refused all melee).
- d2 = squared 3D distance to recorded pos. `in_inf` = Vehicles on && d2 <= InfantryRange^2. `in_ad` = AutoDraw on && d2 <= Distance^2
  && has_melee(ent) (may ask the button) and, with PlayerUnits=0, not a human player's unit L1293-1298.
- `adiag:` log (120/mission) on each state change while an enemy is inside Distance L1299-1311.
- Probe start / end L1312-1330: AutoDraw && Probe && (ProbeAnyRange || inside Distance) && !has_melee && !stance && no charge tag;
  PlayerUnits gate. Running probe: age >= ProbeMs and guns were refused -> fail (nomelee_until = now + NoMeleeMs, log); age >= 4 x ProbeMs ->
  dropped quietly. Not running and no nomelee window and ProbeCooldownMs passed -> start (`probe_ms=now`, log `probe:` 60).
- L1331-1337: in_inf -> `s->inf_ms=now`; in_ad -> hold `ad_until = now + HoldMs` (st_ad_hold on a new hold), `dpend_push`.

## L1340-L1371 adj_shift(ctx, saved) (0.7.3 hull contact; WRITES ENGINE MEMORY: the target info position)
- Needs Vehicles on, AdjacentMs, g_range, g_clock. Target must be a vehicle. dist = 3D unit -> ti+0x28 point; `want = ADJ_AHEAD (10)`.
- marked = last_t adj_tgt == tgt and (adj_until in the future OR (dist < 2*AdjacentMax and own/explicit attack order on tgt)).
- ContactTag (hull_contact) on the unit, dist < AdjacentMax and (order on tgt or dist < AdjacentMax/2) -> mark now (log `vehicles: ...
  touches` 40).
- marked and dist > 10: saves ti+0x28..0x30 into `saved[3]` and writes a point 10 units from the unit toward the target into
  ti+0x28/0x2C/0x30; st_adj_shift++; returns 1 (caller restores after the engine match).

## L1373-L1424 order picks (0.7.17)
- `cfg_order_on=1` ([AutoDraw] OrderPicks) L1376; `g_olines g_rawlines` L1377; `g_exe_lo` (exe base for RVA logging) L1378.
- `order_goes_melee(...)` L1389-1393: always 1 (policy stub).
- `order_pick(ctx, ret)` L1397-1424: needs OrderPicks, AutoDraw, g_clock; unit human, not linked, not in_manual L1402; 0.7.36
  `ad_player_skip(ent) && !tag_stance(ent)` -> 0 L1403; target human enemy, not linked L1405; 3D distance to recorded pos <= Distance
  L1410; 0.7.42 `!may_melee(ent)` -> 0 L1411; po/st/hv (explicit order on it, stance, KeepTags weapon in hand); log `order: [%u ms] unit %p
  weapon pick outside the scan (caller exe+0x%llX) vs enemy %p at %.0f, "%s", player order %d stance %d heavy %d -> %s` (60, per new
  (unit,enemy,decision) via last_t otgt/ogo). Returns 1 = handle like a scan pick in fight range.

## L1426-L1453 vt_name (RTTI class name, diagnostic)
- `NVTNAME 64` cache `g_vtn_vt/g_vtn_nm/g_vtn_n` L1436-1437. `vt_name(vt)` L1438-1453: vt[-1] = COL (signature must be 1), image base
  = col - [col+0x14], name = base + [col+0x0C] + 0x10, must start ".?"; every read through `readable()` (VirtualQuery). Cached up to 64
  vtables (NULL answers cached too).

## L1455-L1459 known units (order poll)
- `g_poll_last` L1455; `NKNOWN 256`; `known_t{ref[0x100], unit, ms, used}`; `g_known[256]` L1456-1458; `cfg_poll_on=1` (OrderPoll) L1459.
- `cfg_poll_ms=250` (PollMs) L1460, `cfg_blind_ms=8000` (BlindCheckMs) L1461; counters `st_poll_fight st_poll_draw st_poll_unknown
  st_poll_player st_poll_nomm` L1462; `g_polllines g_ovtlines` L1463.

## L1464-L1491 known_note (0.7.35 cache in front of the 256-slot known-units table)
- `NKCACHE 1024`, `KCACHE_MS 200`, `g_kcache[1024]{unit, ms}` (direct-mapped by entity pointer) L1469-1471; `cfg_kcache=1` = ini
  [MeleeStance] FastPass (also gates the fast pass) L1472.
- `known_note(ent, now)` L1473-1491: with FastPass, same unit noted < 200 sim ms ago in its kcache slot -> return; else stores (unit,
  now). Then linear search of all 256 `g_known`: hit -> ms=now; else first free slot, else the least recently seen (signed age) is
  evicted (its ref freed with ref_free) and a new weak ref made (ref_make).

## L1492-L1541 wanted fights (0.7.22)
- `NWANT 32`, `WANT_MS 12000`, `want_t{uref, tref, unit, tgt, ms, used, esc, kind}` (kind 1 = give the attack order back once the
  melee weapon is away), `g_want[32]`, counters `st_want_fight st_want_esc` L1495-1500.
- HOSTILITY TABLE `g_hostile[256][32]` (bitset by owner byte pairs) L1503; `hostile_set(a,b)` symmetric L1504; `hostile_is(a,b)` L1505.
  Filled from scan pairs and attack-order targets; never cleared in what was read so far (check the wipe).
- `cfg_prox_on=1` (Proximity) L1506; `cfg_leave=1.5` (LeaveFactor) L1507; `cfg_esc_ms=1500` (EscalateMs) L1509.
- `queue_info(u,buf,cap)` L1511-1519: "N order(s), front <RTTI name>[, heavy weapon in hand]" (uses vt_name -> VirtualQuery).
- `want_drop(u)` L1520-1524; `want_add_k(u,tgt,now,kind)` L1525-1540 (update in place or first free; full -> drop); `want_add` = kind 0.

## L1542-L1585 want_poll(scanning, now)
- counters `st_gun_try st_gun_draw st_atk_back`, `g_rblines` L1542-1543; forward `ranged_back(u, now, atk_tgt)` L1544.
- Per used entry: refs dead / mismatched / older than WANT_MS -> freed L1551. Skip (keep) when unit is the scanning one, linked or
  in_manual L1552. Target downed -> freed L1553.
- kind 1 (L1554-1561): melee still in hand and < 4000 ms -> keep; else re-issue `g_mkatk(tref,0)` + `g_addorder` (the attack order the
  dll took away), log `ranged: [%u ms] unit %p attack order on enemy %p at %.0f given back %s (%u)` (60), freed.
- 0.7.42 L1562: `!may_melee(u)` -> freed (no hold, no fight order).
- L1563-1564 holds the unit (`ad_until = now + HoldMs`).
- not in hand yet L1565-1575: after EscalateMs and within 2 x Distance -> `g_pend_ff=1; pend_push(u,t,d,0,0,now); g_pend_ff=0` (fight
  order without the weapon), log `poll: ... still not in hand` (80). Keep entry.
- in hand L1576-1583: not already fighting it, explicit target none or same, within 3 x Distance -> pend_push (fight order), log; blind
  draw confirmed -> melee_known; entry freed.

## L1586-L1717 order_poll(scanning, now) (0.7.18; runs from the quant tick when Tick is on, else from the hook)
- Needs OrderPoll, AutoDraw, an attack vtable L1588. Interval gate `PollMs` on `g_poll_last` (sim ms) L1589-1590; st_hk_polls++.
- `want_poll` first L1592.
- Resolve all 256 known units once (`static uint8_t *live[256]` - function-static array, not re-entrant) L1593-1601: `ref_ent` mismatch
  -> ref freed, slot unused; `!readable(v, 0xDB0)` (VirtualQuery, 0.7.31: entity freed with its scene) -> slot unused WITHOUT ref_free.
- Per live unit L1602-1716: skip scanning / non-human / linked / in_manual; `ad_player_skip(u) && !tag_stance(u)` -> st_poll_player++, skip
  (0.7.36). Front order + RTTI name (`vt_name`), target type read through `readable(o+0x48,0x18)`, target entity `readable(tgt,0xDB0)`.
  Log `orders: [%u ms] unit %p front order now %s (vt exe+0x%llX)%s, target type %d %p%s, at %.0f` on every change of front-order vtable
  (150/mission; kept also for units that may not melee - the flip diagnostic).
- 0.7.42 L1628-1632: `!may_melee(u)` -> st_poll_nomm++; if the unit slot remembers a gun (`gun[0]`) and the unit is not downed ->
  `ranged_back(u, now, NULL)` (a weapon the dll drew goes back), continue.
- downed unit -> skip L1638. Front eFight -> hold refreshed, fr_ms=now, continue L1639-1642.
- attackish = eAttack / eAttackBasic vtables or RTTI names eAttack, eAttackBasic, eAttackBySelector, eWeaponFire, eWeaponAimTarget
  L1644-1647; `mine` = front order in g_mine L1648; other non-mine order -> ranged_back, continue L1649.
- otgt = the explicit attack target if human enemy, not linked, not downed, not ignored -> `hostile_set` + `known_note(otgt)` L1650-1652.
- range `rng` = Distance x LeaveFactor when the unit holds a dll-drawn melee weapon (`gun[0]` + charge tag), else Distance L1655-1656.
  otgt farther than rng and not mine -> `ranged_back(u, now, otgt)`, continue L1657.
- Proximity: nearest live known unit that is hostile by owner byte (`hostile_is`), human, not linked, within rng, not downed, not ignored
  L1658-1668. None -> ranged_back, continue.
- fr_ms=now; order_goes_melee (always 1).
- in hand L1675-1682: blind draw confirmed -> melee_known; hold; not already fighting; FightCooldownMs gate on last_t; pend_push ->
  st_poll_fight. Carries melee (has_melee) L1683-1688: hold, want_add, dpend_push (else continue). nomelee window L1689-1692 (log every
  5 s). Unknown and no blind draw L1693-1700: dpend_push, want_add, blind_ord, blind_ms. Blind draw pending L1701-1709: judged after
  BlindCheckMs, or after 1200 ms once the front order is no longer eTakeItem: give back the attack order (want kind 1) or drop; set
  nomelee_until.
- Log `poll: [%u ms] unit %p enemy %p in fight range at %.0f (%s): %s (fights %u, draws %u, unknown %u)` (80).

## L1719-L1729 own tick globals
- `cfg_tick_on=1` (Tick), `cfg_tick_ms=100` (TickMs) L1725-1726; `volatile DWORD g_sim_tid, g_ui_tid` L1727; `volatile LONG g_in_hook,
  g_in_tick, st_ticks, st_tick_wrongthread` L1728; `DWORD g_tick_wall` (WALL CLOCK, GetTickCount) and `uint32_t g_tick_clock` L1729.
- `cfg_gate=1` ([MeleeStance] ClientGate) L1730; `st_tick_gated` L1731; `volatile LONG64 st_tick_us`, `st_tick_us_max` L1732-1733.

## L1734-L1760 ms_tick() = the dll's own per-quant work (called from frame_hook, engine thread)
Gates in order: Tick on, clock found, not inside the hook, not re-entered (`g_in_hook || g_in_tick`) L1736; ClientGate: `st_hk_all == 0`
(no MatchAmmo hook call yet in this process) -> st_tick_gated++, return L1740; WALL-CLOCK rate limit `GetTickCount() - g_tick_wall <
TickMs/2` -> return L1741-1743 (TickMs 100 -> at most one tick per 50 wall ms); sim clock `now==0` (no mission) or unchanged since the
last tick (paused) -> return L1744-1745; thread: no engine thread seen yet (`g_sim_tid` / `g_ui_tid` both 0) -> return; current thread is
neither -> st_tick_wrongthread++, return L1746-1748. Then `g_tick_clock=now; g_in_tick=1; st_ticks++`; QPC timing; `log_epoch(now)`;
`charge_flush(NULL)` when [Charge] on && AddOrder && order stamp; `autodraw_flush(NULL)` when (AutoDraw || Vehicles) && AddOrder && stamp;
`order_poll(NULL, now)`; tick cost into st_tick_us / max; `g_in_tick=0`.
- WALL CLOCK EFFECT ON SIMULATION: the GetTickCount gate decides whether THIS quant flushes queued orders / polls; with quants shorter
  than 50 wall ms (fast sim, light load) some quants are skipped, so order timing depends on frame rate (not a desync: orders only matter
  on the machine that simulates, the client is gated off by ClientGate; inferred from the ClientGate comment L1737-1739). The decisions
  inside use sim ms only.

## L1761-L1862 quant hook (Scene::eQuantExecutor vtable slot) + gem_quant_listen export
- `frame_fn_t` = `void *(ms_abi *)(void*,void*,void*,void*)` L1768; `g_frame_orig` L1769; `volatile LONG g_frame_state` (0 not tried, 1
  hooked, 2 failed) L1770.
- EXPORT `__declspec(dllexport) __attribute__((ms_abi)) void gem_quant_listen(quant_cb_t cb)` L1778-1788 (NON-STATIC, exported):
  `quant_cb_t` = `void (ms_abi *)(void)`; up to `NQUANTCB 4` callbacks in `void *volatile g_quant_cb[4]`; NULL and duplicates ignored;
  registration by InterlockedCompareExchangePointer into the first NULL slot (lock-free, any thread, any time); logs `quant listeners:
  callback %p registered (slot %d of %d); live %s` ("now" / "once the quant hook is installed"). A 5th callback is silently dropped.
  Contract (comment L1771-1774): callbacks run once per quant on the engine thread right after the dll's own tick, only once the quant
  hook is installed (after the first enemy scan or order event of the session); must not give orders. No unregister.
- `frame_hook(a,b,c,d)` L1789-1794 (static, `ms_abi, used, noinline`): `ms_tick()`; every registered callback in slot order; then
  `g_frame_orig(a,b,c,d)` (tail result returned). Callbacks run EVEN when ms_tick returned early (gated / paused / ClientGate / wrong
  thread): they run every quant the executor runs. NOT gated by ClientGate (comment L1875 confirms).
- `ret_after_call(r, &indirect_d8)` L1795-1804: return address preceded by `E8 rel32`, `FF 15 rip`, `FF 5x d8` (indirect_d8=1),
  `FF D0..D7 / FF 10..17`, `FF 9x d32`.
- `frame_hook_try()` L1805-1862: once per process (`InterlockedCompareExchange(&g_frame_state, 2, 0)`; Tick=0 -> never). Finder:
  main exe image (GetModuleHandleA(NULL)), `.text` bounds; walks the CURRENT thread's stack from the TEB stack base (`__readgsqword(8)`)
  down to a local: the 3 outermost return addresses inside .text that have unwind info (RtlLookupFunctionEntry) and follow a call
  instruction = thread start / worker loop / frame function; the worker's (ret[1]) must follow `call [reg+d8]`; fn = function start of
  ret[2]'s function (unwind BeginAddress). Then scans every NON-executable section for 8-byte slots equal to fn: exactly one hit and
  `slot[-2]` inside the image (vtable header) required; class name of the vtable (`vt_name(slot-1)`, RTTI) must contain "QuantExecutor".
  PATCH: `VirtualProtect(slot, 8, PAGE_READWRITE)`, `g_frame_orig = fn`, `*slot = frame_hook`, protection restored, state 1, log `tick: %s
  run-quant function exe+0x%llX (worker loop exe+0x%llX, thread start exe+0x%llX), vtable slot exe+0x%llX hooked: the dll ticks at the start of
  each quant on engine thread %lu`. Failures log `tick: ...frame hook off` and leave state 2 (no retry).
  Called from: ms_hook_body when the pick came from the scan (L1987) and order_event (L1873) - i.e. lazily on the ENGINE thread at the
  first scan pick / first Teleport order event, NOT at install. Not a byte patch: a data (vtable slot) write; no claim/registry.

## L1863-L1878 order_event (callback given to GEM Teleport's gem_order_listen)
- Signature `static void order_event(void *entity, void *order)` (default ABI = ms_abi on x86_64-windows-gnu; Teleport calls it as
  `void (*)(void *, void *)`). Ignored when entity NULL, inside tick or hook, no clock, OrderPoll off; `readable(e,0xDB0)` (VirtualQuery),
  human, not linked. First call sets `g_ui_tid` (thread of the GiveOrder hook), tries `frame_hook_try()`, st_order_events++; ClientGate
  closed -> return (the quant hook still went in for the other dlls' callbacks); else `known_note(e, *g_clock)` and `g_poll_last=0` (poll on
  the next tick). Gives no orders.

## L1879-L1895 hook timing
- `cfg_hooktime=1` (HookTiming) L1884; `volatile LONG64 st_hook_qpc st_orig_qpc st_hpoll_qpc`, `st_hook_qpc_max st_hook_timed` L1885-1886.
- `qpc_now()` L1887 (QueryPerformanceCounter). `orig_timed(ctx)` L1888-1895: calls the MatchAmmo trampoline `g_orig(ctx)`, adds its QPC
  time to st_orig_qpc. QPC is measurement only (no decision depends on it).

## L1896-L1928 hb_thread (helper thread, started at install; no engine calls)
- Every 100 ms (Sleep). For the first 60 s (600 tries, every 10th = once a second): `GetModuleHandleA("gem_teleport.dll")` +
  `GetProcAddress(m, "gem_order_listen")` -> `reg(order_event)`, log `orders: listening to GEM Teleport's GiveOrder hook (gem_order_listen)`;
  at try 600 without it: `orders: no gem_order_listen export in gem_teleport.dll (0.2.3+): order poll only`. IMPORT CONTRACT:
  `void gem_order_listen(void (*cb)(void *entity, void *order))`, looked up by MODULE NAME "gem_teleport.dll".
- Every 5 s of wall time (GetTickCount) and only when the hook-call or tick counters changed, up to 400 lines: counts used g_known slots
  (reads the table from this thread, racy but read-only), swaps the max counters with InterlockedExchange, writes the `hb:` line:
  `hb: [%u ms] hook calls %ld (scan %ld, humans %ld, human vs enemy human %ld, fast pass %ld, no-melee refused %ld, may-melee cache hits
  %ld) | frame hook %s, ticks %ld (gated %ld, wrong thread %ld; sim %lu ui %lu) polls %ld, order events %ld, units on the poll list %d |
  tick cost: %lld us total, %ld us average, %ld us worst since the last line | hook cost: %lld us total over %ld timed calls (%lld us of it the
  order poll), %lld ns average, %lld us worst since the last line; engine MatchAmmo %lld us total`. Reads `*g_clock` from this thread.
  Wall-clock only for log pacing.

## L1929-L1967 fast pass helpers
- `hook_leave(int*)` L1929: cleanup handler, `g_in_hook--`.
- `fast_known_none(ent)` L1935-1944 (`*g_clock` direct): slot exists, nomelee_until in the future, no probe running, no blind draw, no
  auto-draw hold, no melee_until -> 1.
- `fast_nomelee(ctx)` L1945-1955: FastPass && Enabled && clock; ammo/ent non-null, human; 0.7.42: `!fast_known_none(ent) && (!n_amtag ||
  may_melee(ent))` -> 0 (so a unit that may NOT melee is fast for every gun candidate); ammo carries no MeleeAmmoTags tag (vec_has != 0 ->
  0, also on a broken vector) -> 1 (= straight to the engine).
- `nomm_cand(ctx)` L1958-1967 (0.7.42): AutoMeleeTag set, Enabled; human, not linked; melee ammo; `!may_melee && !in_manual` ->
  `nomm_refused(ctx, " (an IgnoreTargetTags target)")`, 1 (refuse). Used where the pick would bypass ms_decide (ignored target).

## L1968-L1981 ms_hook (THE MatchAmmo detour target; NON-STATIC symbol `ms_hook`)
- `__attribute__((used, ms_abi, noinline)) void *ms_hook(uint8_t *ctx)` L1970: `ret = __builtin_return_address(0)` (the engine call site,
  because the asm stub JUMPS here, see the stub section); HookTiming off or nested -> `ms_hook_body(ctx, ret)`; else QPC around the
  body minus the engine MatchAmmo share (`st_orig_qpc` delta), into st_hook_qpc / st_hook_timed / st_hook_qpc_max.

## L1982-L2075 ms_hook_body(ctx, ret) = per-candidate hook body (engine thread, inside the engine's weapon choice)
Exact order (this is what the merged dll must keep for rule R2):
1. L1984 `from_scan` = clock found && scan window known && `g_scan_lo <= ret < g_scan_hi` (ret = return address into the engine's enemy
   scan, i.e. the call came from one of the scan's two MatchAmmo call sites).
2. L1985 `g_in_hook++` with a `__attribute__((cleanup(hook_leave)))` guard (decrements on every return path; nesting counter, not a lock).
3. L1986 first scan pick sets `g_sim_tid` (the engine thread id). L1987 scan pick -> `frame_hook_try()` (installs the quant hook once).
4. L1988 `log_epoch(*g_clock)` (new-mission detection + wipe; see later) BEFORE any per-unit state is read.
5. L1991 `ticking = g_frame_state==1 && Tick`. L1992 `fast = fast_nomelee(ctx)`.
6. L1993-2001 IGNORED TARGET: target info's entity is an actor carrying IgnoreTargetTags -> st_hk_all++, st_tgt_ignored++;
   `nomm_cand(ctx)` (0.7.42 no-melee refusal) -> return NULL; else `return orig_timed(ctx)` (engine, untouched). Note: comes BEFORE the
   downed test, the direct-control test and ms_decide; the in_manual exemption is inside nomm_cand.
7. L2002 scan && [Charge] && !ticking -> `charge_flush(scanning unit)` (fallback path when the quant hook is not installed).
8. L2003 scan && (AutoDraw || Vehicles): !ticking -> `autodraw_flush(unit)`; !fast -> `scan_note(ctx)`.
9. L2004-2012 counters st_hk_all, st_hk_scan, st_hk_human, st_hk_enemy (owner byte differs). st_hk_all > 0 OPENS THE CLIENT GATE.
10. L2013-2023 scan: `known_note(scanning unit)` (OrderPoll), `known_note(target)` (human, not linked), `hostile_set(owner bytes)` for
    human vs human; !ticking -> `order_poll(su, now)` (timed into st_hpoll_qpc when HookTiming).
11. L2024-2030 non-scan call: `raw:` log (40/mission): `raw: [%u ms] hook call from exe+0x%llX: unit %p ti %p target %p`.
12. L2031-2034 non-scan call: `known_note(unit)` (human, not linked, OrderPoll).
13. L2035-2036 `ord = !fast && !from_scan && order_pick(ctx, ret)`; ord -> `scan_note(ctx)` (fight-range path for order picks).
14. L2037-2041 [Downed]: target human carrying the Downed Tag -> st_down++, return NULL. No shooter check: ms_hook runs for every
    shooter (vehicles, crews, direct-control units too), so no weapon of anybody matches against a downed human (it sits before ms_decide
    and its in_manual / linked exemptions).
15. L2042 fast -> st_fast++, `return orig_timed(ctx)`.
16. L2043 `ready = ctx[0x18]` (engine's "item in hand" byte). L2044 `d = ms_decide(ctx)`.
17. REJECT L2046-2056: if `g_nm_ref` (no-melee refusal) return NULL silently (already logged); else st_reject++, log `stance: unit %p
    refused "%s" (refused %u, melee picks %u)` (40/mission), return NULL.
18. MELEE L2057-2071: DrawFromBelt -> `ctx[0x18]=1` (lift the ready_for_shot limit: WRITES THE ENGINE'S CTX); `adj_shift(ctx, saved)`
    (may write ti+0x28..0x30); `r = orig_timed(ctx)`; restore ti point if shifted; restore `ctx[0x18]=ready`; r -> st_allow++, log `stance:
    unit %p takes "%s" ...` (40).
19. PASS -> `r = orig_timed(ctx)`.
20. L2073 `(from_scan || ord) && [Charge] && r` -> `charge_note(ctx, ready != 0, d == MELEE)`.
21. return r (the ammo.set row pointer or NULL).
Return contract to the engine: NULL = "this ammo matches no ammo.set row" (candidate refused); non-NULL = the row the engine's own
MatchAmmo returned. The dll never fabricates a row.
PenFilter interplay (inferred from the plan; pen's code not read here): pen's two inner MatchAmmo sites run inside `g_orig` (the
trampoline continues into the original body), so stance is the OUTER filter and pen only sees candidates stance passed or forced (MELEE);
a REJECT never reaches pen. The ctx[0x18] lift and the ti-point shift are visible to pen's inner code during the call.

## L2077-L2110 multi-select (UI side) declarations
- typedefs `exinst_t getactor_t expand_t canapply_t` L2082-2085; `cfg_multi=1` ([MultiSelect] Enabled) L2086; `g_exinst` (vanilla
  ExecuteInstant) L2087; `g_off_apply g_off_getactor g_off_canapply` (vtable / field offsets read from code) L2088; `g_expand` L2089;
  `g_cur_actor` L2090; `st_multi_clicks st_multi_units` L2091; `VTCOPY 64`, `g_vtc[16]{orig, copy}` L2092-2093.
- `ms_getactor_cur(action, list)` L2095: NON-STATIC (`__attribute__((used, ms_abi))`), returns `g_cur_actor`. Link-level name.
- `vt_copy(vt)` L2097-2110: per action vtable (16 max), a 64-slot copy in VirtualAlloc memory (never freed) with the GetActor slot
  (`g_off_getactor/8`) replaced by ms_getactor_cur.

## L2112-L2121 draw on toggle declarations
- `pickctor_t g_pickctor` L2119-2120 (TakeItem order ctor); `g_off_item` ({item} field offset in the action, +0xC8 per the test log) L2121.

## L2122-L2161 reload cancel (0.7.1, ENGINE CALL that cancels inventory ops)
- typedefs `invbusy_t invcancel_t curweap_t`; `g_invbusy` (Human::InventoryBusy), `g_invcancel` (Human::CancelInventoryOps),
  `g_curweap` (GetCurrentWeapon(holder)) L2129-2134; `cfg_rc_on=1 n_keep=12 g_rclines` L2135; `cfg_keep` = mgun, hmg_shell, stubber,
  lascannon, plasmacannon, sm_weapon_plasmacannon, sm_weapon_heavybolter, rocketlauncher, heavy_flamethrower, grenadelauncher, at_rifle,
  ogryn_weapon L2136-2137; `st_rc_cancel st_rc_keep` L2138.
- `reload_cancel(u)` L2140-2161: needs ReloadCancel on, both fns, human; `g_invbusy(u)` false -> 0; held weapon
  `w = g_curweap([u+0xDD8])`, params `[w+0x98]`, reload record `[w+0xB0]` (flags +0x14, clock +0x10; LOG ONLY); unless `g_rc_force`, a
  KeepTags tag on the weapon params' {tag} vector (`prm + ammotags + 0x318`) keeps the reload (log, 40); else `g_invcancel(NULL, u)` (log,
  40). NOTE: GetCurrentWeapon is called with `[u+0xDD8]` (holder component) WITHOUT a null check here (g_curweap(NULL) possible if
  +0xDD8 is NULL; held_weapon_name / holds_keep_weapon do check it).

## L2162-L2230 back to the gun (0.7.26)
- `held_weapon_name(u, out, cap)` L2163-2172: GetCurrentWeapon on `[u+0xDD8]` (null-checked), params +0x98, `readable(prm+0x868+0x318,16)`
  (VirtualQuery), name words of the weapon (`vec_str`).
- `NGUNTC 32`, `g_guntc[32]{name[72], tc}`, `n_guntc` L2174-2176; `gun_tags(name)` L2177-2196: per weapon name, one VirtualAlloc'd
  TagContainer built with the engine's ctor + FromString (MSVC std::string, heap form pointing at the table's own name when > 15 chars),
  kept for the session (also a failed build is cached as NULL and the slot used); log `ranged: item tags built for "%s": "%s"`.
- `ranged_back(u, now, atk_tgt)` L2202-2222: unit slot remembers a gun and is not in stance; melee not in hand -> forget the gun when no
  hold and no draw for 5 s, return 0; no attack target and an enemy in range within HoldMs -> 0; one order per 2500 ms (`gun_ms`);
  `gun_tags(s->gun)`; clears ad_until / auto_drawn; with atk_tgt: `g_invcancel(NULL,u)` and want kind 1 (give the attack order back
  later); `g_takeitem(tags)` + `g_addorder(u, o, {1,*g_ordstamp})`; log `ranged: [%u ms] unit %p nobody in fight range%s: take-item order
  for "%s" %s | unit now has %s (re-draws %u)` (60).
- `holds_keep_weapon(u)` L2224-2230: held weapon params carry a KeepTags tag.

## L2232-L2282 draw config, NEW-MISSION WIPE
- `cfg_drawbtn="melee_toggle"` (DrawButton) L2232; `cfg_draw=1` (DrawOnToggle), `g_names_logged`, `g_dlines` L2233.
- `g_epoch_last` L2236 (last sim clock seen by log_epoch).
- `st_flush_refs` L2248; `flush_ref(ref)` L2249 = count + ref_free.
- `mission_flush()` L2250-2271 (0.7.31 / 0.7.37): ONLY when `g_reflist_off && g_refdtor && g_tagdtor`: ref_free every live weak ref in
  g_pend (uref+tref), g_last (tref when has_ref), g_dpend (uref), g_known (ref), g_want (uref+tref) - each through the 0.7.33 listed test
  (a ref whose entity no longer lists it is zeroed, not unlinked). Then memset: g_seen, g_unit, g_pend, g_last, g_dpend, g_known,
  g_kcache, g_mmc (0.7.42), g_want, g_mine, g_hostile; `g_poll_last = 0`.
  NOT wiped (session lifetime): g_melee_tc, g_guntc (TagContainers, VirtualAlloc), g_vtc (vtable copies), g_vtn cache (RTTI names),
  g_draw_action, the frame hook (g_frame_state / g_frame_orig), g_quant_cb, g_sim_tid / g_ui_tid, g_tick_clock / g_tick_wall, all st_*
  counters, g_pend_ff / g_pend_pl / g_rc_force (transient), ref caches inside wiped slots go with them.
- `log_epoch(now)` L2272-2282: NEW MISSION = `g_epoch_last != 0 && now + 5000 < g_epoch_last` (sim clock went BACKWARDS by more than
  5 s). Then mission_flush, every per-mission log cap reset (g_pclines g_xlines g_vfreelines g_ghostlines g_nmlines g_lines g_clines g_hlines
  g_vlines g_vreflines g_adjlines g_adlines g_rclines g_dlines g_mlines g_mdiag g_seenlines g_addiag g_plines g_skiplines g_olines g_rawlines
  g_polllines g_ovtlines g_rblines g_reflines), log `--- clock %u -> %u: new mission, log line caps reset, unit tables cleared (%u weak refs
  released, %u of them stale so far)`. Always `g_epoch_last = now`.
  NOT reset by log_epoch: `g_zlines` (zerg cap 40 / 80: per PROCESS, L606/L675/L1141 vs L2276-2278) and `g_names_logged` (one-time
  draw-action line); every other cap listed above is per mission.
  Callers: ms_hook_body L1988 (every pick, first thing after the tid / frame hook), scan_note L1279, ms_tick L1752. NOT from
  order_event / ms_exinst / known_note (0.7.37 comment L2244 notes refs can be made on a new mission's entities before log_epoch runs;
  the ref_free-first wipe makes that safe).
  Detection weakness: a new mission whose clock starts within 5 s of where the last one ended is not detected (inferred from the test).

## L2283-L2333 melee_toggle draw
- `g_draw_action` declared AGAIN L2283 (C tentative definition, also at L891: legal C, an error in C++). `st_draws st_draw_drop
  st_draw_wait` L2284; `OFF_BUTTONNAME 0x60` (action's button name std::string) L2285.
- `is_draw_action(action)` L2287-2296: needs DrawOnToggle, takeitem, order stamp, AddOrder; button name == DrawButton (case-insensitive)
  -> remembers `g_draw_action = action` (log once `draw: toggle action %p found (button "%.*s", {item} tags +0x%X)`).
- `melee_by_button(u)` L2299-2306: -1 when the action is not known yet (no click this session) or no CanApply offset; else calls the
  action's CanApply vtable slot `vt[g_off_canapply/8](action, u, NULL)` -> 1/0. ENGINE CALL from wherever has_melee is called (scan_note,
  order_poll, ms_decide via has_melee) - engine thread.
- `wants_draw(u)` L2307-2310: human, not in stance (Tag/AlwaysTag), charge tag not on.
- `draw_melee(action, vt, u)` L2313-2333: needs takeitem + AddOrder, not linked; SkipTags unit -> 0 (0.7.26: the reload cancel would kill a
  jetpack order); per-unit DrawCooldownMs on last_t.draw_ms (only when clock > 0); `reload_cancel(u)`; `o = g_takeitem(action +
  g_off_item)` (the action's own {item} tags); `g_addorder(u, o, {1, *g_ordstamp})`; log `draw: [%u ms] unit %p take-item order %p %s
  (draws %u, refused %u, waited %u)` (40). NOTE: draw_melee has NO direct-control (in_manual) check and NO may_melee check: the toggle
  click draws for the unit it switches into stance (README 0.7.42 "The melee_toggle button still draws"); charge_flush calls it only after
  its own in_manual / may_melee gates.

## L2335-L2371 ms_exinst(action, list) = replacement ExecuteInstant (NON-STATIC, ms_abi; UI thread = engine thread per the 0.7.23 notes)
- First call sets `g_ui_tid`. MultiSelect off / null / action's `apply_to_all` byte (`action[g_off_apply]`, +0x498) 0 -> vanilla
  `g_exinst(action, list)` only (NOTE: no draw on toggle in that case).
- `drawact = is_draw_action(action)`; `one = vt[g_off_getactor/8](action, list)` (vanilla GetActor): a single eligible unit -> vanilla
  ExecuteInstant, then `draw_melee` if it was the toggle and `wants_draw(one)` (evaluated BEFORE the click adds the stance tag).
- Several / none: `g_expand(&units, list)` (selection incl. squad cards; the returned vector is never freed: "leaks a few bytes per
  click"); sanity (non-null, 1..256 entries) else vanilla; `cp = vt_copy(vt)`; per unit with `CanApply(action, u, NULL)`: g_cur_actor=u,
  SWAP THE ACTION OBJECT'S VPTR to the copy (`*(void***)action = cp`), vanilla ExecuteInstant, restore the vptr, draw_melee if wanted.
  Log `instant: button on %d of %d selected units (clicks %u, units %u)` (shares the g_lines 40 cap with stance lines).
  Not exception-safe: an engine exception inside g_exinst would leave the vptr swapped (inferred).

## L2373-L2414 [ShieldMelee] ray filter
- `cfg_ray=0` (Enabled, default OFF since 0.6.1) L2379; `cfg_seetag[1]="forcefield"` (SeeThroughTag) L2380; `g_ray_hits` (engine global
  hit list, found from code) L2381; `st_ray_pass st_ray_skip st_ray_block` L2382.
- `link_holder(e)` L2384-2389: linked bit set -> `[[e+0x560]+8]` (the entity it is linked to).
- `ms_ray_ok(target)` L2391-2414: NON-STATIC, `ms_abi, force_align_arg_pointer`; reads the hit vector (0x48-byte records, entity at +0x18,
  max 256): first hit == target -> pass (counted even when Enabled=0); Enabled=0 -> 0; else walk: skip hits linked to the target or tagged
  SeeThroughTag; reaching the target -> 1 (log `melee ray: target %p reached through %d attached hit(s) ...`), anything else -> 0 (log
  `melee ray: target %p blocked by %p ...`). Called from an asm stub patched into the blow picker (see install; only when Enabled).

## L2416-L2460 finder signatures (wildcard `W 0x100`)
- `#define W 0x100` L2417 (GENERIC ONE-LETTER MACRO NAME); `#define PATCH 14` L2422 (generic).
- `SIG_FLAGS` L2419 `0F B6 ?? 94 03 00 00 F6 ?? 1C` (movzx e?x, byte [r?+394h]; test ?l,1Ch: ammo.set row flags enable_for_*) = the
  MatchAmmo body marker.
- `PROLOG` L2421 (16 bytes) `48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57` = MatchAmmo prologue (mov [rsp+20h],rbx; push rbp rsi rdi
  r12 r13 r14 r15); `PATCH 14` = bytes moved to the trampoline (through push r14).
- `SIG_ADDTAGS` L2424 `48 81 C1 ?? ?? 00 00 E8` (add rcx,imm32; call) -> ammo {tag} Tags offset (expected 0xEC8).
- `SIG_LINK` L2426 `8B ?? C0 01 00 00 C1 E8 1C A8 01` (mov eax,[r?+1C0h]; shr eax,1Ch; test al,1) -> linked bit, counted ("linked-flag
  sites 89/95/86").
- `SIG_RANGE` L2428 `0F 28 F0 E8 ?? ?? ?? ?? 0F 2F C6` (movaps xmm6,xmm0; call Range; comiss xmm0,xmm6).
- `SIG_C1` L2430 `40 88 BB C1 00 00 00` (mov byte [rbx+0C1h],dil, unique) -> MakeAttack.
- `SIG_ADD` L2432 `48 8B D0 4C 8D 44 24 20 48 8B ?? E8` -> call AddOrder after MakeAttack.
- `SIG_SETENT` L2434, `SIG_REFINIT` L2436, `SIG_TAG20` L2438 (inside MakeRef).
- `SIG_INVBUSY` L2442-2444 (Human::InventoryBusy function start, rip displacements wildcarded), `SIG_INVCANCEL` L2447-2449
  (Human::CancelInventoryOps start), `SIG_CURWEAP` L2451-2453 (GetCurrentWeapon(holder); comment: also in GEM MeleeChase),
  `SIG_ISMANUAL` L2456-2457 (eActor::IsManualControlled start).
- `SIG_MGRACT` L2460... (manual-control manager global; continues below).
- `SIG_MGRACT` L2460-2461 `48 8B 0D ?x4 E8 ?x4 84 C0 74 ?? 48 8B 0D ?x4 E8 ?x4 80 78 16 00`; `MGR_GETACTOR` L2462 `48 8B 81 B0 03 00 00 C3`.
- `SIG_CLK` L2464 `8B 9F ?? ?? 00 00 E8 ?x4 3B C3 0F 82` (brain timer `mov ebx,[rdi+X]; call GetClock; cmp eax,ebx; jb`; "from GEM
  BoardFilter") -> sim clock global.
- `match(p, sig, n)` L2466-2471 (W = wildcard); `MATCH(p,s)` macro L2472 (generic name); `rel32(at)` L2473 = at+4+int32.

## L2475-L2499 sites_t / img_t (finder results; static types)
- `sites_t` L2475-2497 fields: fn (MatchAmmo), ammotags, linksites; mkmove; fight, mvchange, uimove; range, scan_lo, scan_hi, mkatk,
  addorder, mkref, setent, refinit, tagctor, refvt, refdtor, tagdtor, ordstamp, clock; reflist; charge_err; blowtab, blowfn; exinst,
  getactor, expand, pickctor, takeitem, off_apply, off_getactor, off_canapply, off_item, exslots[32], nexslots, inst_err, vt_delta;
  ray_site, ray_ok, ray_fail, ray_hits, ray_reg, ray_err; tcctor, tcfromstr, tc_err; vt_attack, vt_attackb, vt_fight; invbusy, invcancel,
  curweap, rc_err; ismanual, manual_err; mgr, mgr_err.
- `img_t{base, lo, hi, rf, nrf}` L2499 (.text bounds + exception directory). `func_of(im,p)` L2501-2509 binary search of the
  RUNTIME_FUNCTION table; `in_text` L2510; `next_call(im,p,n,&at)` L2513-2521 first `E8` within n bytes whose target is in .text.

## L2523-L2655 find_charge (charge finders; return NULL = OK, else an error string -> charge off)
1. Range: inside MatchAmmo's function extent (`func_of(s->fn)`), first `SIG_RANGE` -> `s->range = rel32(p+4)` (target of the `E8`).
   OVERLAP CHECK NEEDED: if another patch rewrites that `call Range` inside MatchAmmo, stance would read a foreign target (see the
   finder table / hazards).
2. Enemy scan: every `E8 rel32` in .text whose target == MatchAmmo; group by function (func_of); the ONE function with exactly 2 such
   calls = the scan; `scan_lo/hi` = its begin/end. More than one -> "enemy scan not unique"; none -> "enemy scan not found". (Up to 16
   calling functions considered.) OVERLAP: a patch that turns either scan call into something else (or adds a third `E8 MatchAmmo` call
   in .text) breaks this.
3. MakeAttack: the function holding the unique `SIG_C1` (mov byte [rbx+0C1h],dil).
4. UI attack site: the unique `E8 MakeAttack` followed (after optional NOPs) by `SIG_ADD`; AddOrder = rel32 at q+12.
5. MakeRef: a call within 0x40 bytes before that site whose body (first 0x70 bytes) has `SIG_SETENT` -> mkref, SetEntity = rel32.
   Inside MakeRef: refvt (`lea rax,[rip+vt]; mov [rdi],rax`), TagCtor (first call after `SIG_TAG20`), RefInit (`SIG_REFINIT` call target
   != TagCtor).
6. After AddOrder at the site: RefDtor = first call within 0x30 bytes from site+5(+NOPs)+17; reflist offset = `add rcx,imm32 (<0x1000);
   mov rdx,rbx; call` inside RefDtor (0 if absent = no 0.7.33 check); TagDtor = next call after RefDtor's call.
7. Order stamp: `mov e?x,[rip+g]` within 0x40 bytes before the site, g beyond .text and below base+0x4000000 (last such match wins).
8. SIM CLOCK: every `SIG_CLK` whose callee is `mov eax,[rip+g]; ret` (8B 05 ?x4 C3); the global g named by most matches (needs >= 5)
   = `s->clock`. Same method as GEM BoardFilter (comment) and found again by the other two modules (plan: "found three times").
9. eFight factory: `E8 F` followed by `4C 8B 44 24 50 48 8B D0 48 8B CB E8 AddOrder`, F allocates 0xE0 (`B9 E0 00 00 00` within 0x40);
   ambiguous -> NULL (not an error; charge falls back to MakeAttack).
10. eMovementChange: every `8B 14 90 E8 X` (mode-table lookup), X allocates 0x48; all must agree else NULL.
11. UI click move: `B9 F8 00 00 00 E8` with three stores within 0x40 bytes; function start; must be unique else NULL.
Only 1-8 can fail with an error (charge_err); 9-11 just leave NULL.

## L2657-L2714 find_instant (multi-select)
- ExecuteInstant = the unique function with `cmp [rcx+0x868],rax` within 0x40 bytes of its start and a `call [rax+0xF0]` within 0x30
  bytes after it. `off_getactor = 0xF0`. pickctor = first direct call within 0x140 bytes.
- Vtable slots holding ExecuteInstant: every 8-byte value after .text equal to its live address or its preferred-base address (offline
  test maps without relocations; `vt_delta`), up to 32 (`exslots`); the slot before the first = Apply.
- `VTP(x)` macro L2695 (rebase for the offline test; never #undef'd - leaks to the rest of the TU).
- apply_to_all offset: Apply's first `80 B9 imm32 00` (cmp byte [rcx+X],0) within 0x30, must be 0x100..0x2000 (0x498 in the logs).
- Layout check: `vt = exslots[0] - 19`, `VTP(vt[19]) == ExecuteInstant`; GetActor = `vt[0xF0/8]`; expand = first direct call in GetActor
  (0x60); CanApply offset = first `FF 90 imm32 (<0x400)` in GetActor (0x90) (0xD8 in the logs).

## L2716-L2753 find_takeitem
- String "take_item" (NUL before it) after .text; an 8-byte pointer to it followed by the factory pointer; factory's `lea rcx,[rip+vt]`;
  `vt[3]` = exec; in exec `lea rcx,[rdx+X]` (X = {item} offset, must be 0x40..0x1000; +0xC8 in logs) then the next call = eTakeItem maker.
  Error "take_item order factory not found" goes into `inst_err` (shares the multi-select error slot).

## L2755-L2801 find_tagcont (TagContainer ctor + FromString)
- `FIXP(x)` macro (rebase; #undef'd at L2790). Non-.text sections: (pointer to "tags\0" with NUL before, factory pointer in .text) pairs,
  exactly one factory; in it `lea r,[rip+vt]` (vt outside .text); next call after it must contain `C6 81 00 03 00 00 00` (mov byte
  [rcx+0x300],0) = TagContainer ctor; vt slot 5 = parse; in parse `41 B0 20` (mov r8b,' ') then next call = FromString, which must read
  `[r+0x318]` (TAGS_VEC) within 0x40 bytes.

## L2803-L2835 find_vtable_rtti(im, name)
- Finds the type-descriptor name string after .text; td = name - 0x10; searches non-.text sections (4-byte steps) for a COL (sig 1,
  offset 0, +0xC = td RVA, +0x14 = own RVA); then an 8-byte pointer to the COL (live or preferred-base) -> vtable = that + 8.

## L2837-L2919 find_all(base, s) part 1
- .text bounds (first ".text" section), exception directory (required: "no exception directory").
- MatchAmmo: every `SIG_FLAGS` hit whose function (func_of) STARTS WITH the 16-byte `PROLOG`, is <= 0x600 bytes, and holds
  `SIG_ADDTAGS` with imm32 in 0x400..0x3000 (= ammo {tag} offset). Exactly one function -> `s->fn`, `s->ammotags`; two -> "ammo match
  function not unique"; none -> "ammo match function not found (unknown exe build, or already patched)" (FATAL: install aborts, see
  install). SELF-OVERLAP: the PROLOG check fails on an already-patched MatchAmmo (a second copy of the module, or the old
  gem_meleestance.dll loaded first) -> that is the double-load guard today.
- Linked sites: count of `SIG_LINK` in .text (informational, logged).
- `charge_err = find_charge`, `inst_err = find_instant` then (if OK) `find_takeitem`, `tc_err = find_tagcont`, the three RTTI vtables
  (eAttack / eAttackBasic / eFight; NULL allowed).
- MakeMove: "Moving to zero bug!" string, the first `lea r,[rip+str]` in .text, its function must start with `48 89 5C 24 08|10` and the
  lea within 0x100 -> `s->mkmove` (else NULL, no error).
- Shield ray site L2894-2908: `48 8B 05 g1 / 48 3B 05 g1+8 / 0F 84 fail / 4C 39 ?? 18 / 0F 85 fail` (30 bytes); exactly one ->
  ray_site, ray_hits=g1, ray_fail, ray_ok=p+30, ray_reg=8+((p[22]>>3)&7) (r8..r15 target register).
- Blow reach L2911-2919: `48 63 C1 48 8D r5 ?x4 F3 0F 10 04 (80|reg) C3` with the table holding exactly 17/29/35 -> blowfn, blowtab;
  not unique -> both NULL. SELF-OVERLAP: once the table is overwritten (45/45/45) the finder no longer matches (a second module instance
  or a re-run finds nothing).

## L2920-L2944 find_all part 2 (function-start signatures, each must be unique)
- One pass over .text (p+80 <= hi): `SIG_INVBUSY`, `SIG_INVCANCEL`, `SIG_CURWEAP`, `SIG_ISMANUAL`, `SIG_MGRACT` (both rip loads must
  name the same global g1, and the second call's target must be exactly `MGR_GETACTOR` = `mov rax,[rcx+3B0h]; ret` -> `s->mgr = g1`).
- IsManualControlled not unique / not found -> `manual_err`, ismanual NULL. Manager not unique / not found -> `mgr_err` (server: none,
  expected). Any of InventoryBusy / CancelInventoryOps / GetCurrentWeapon not exactly once -> `rc_err` and ALL THREE NULL (so
  GetCurrentWeapon-based helpers - held_weapon_name, holds_keep_weapon, ranged_back's gun tags, reload record log - all go off with the
  reload cancel).
- find_all returns NULL (OK) even when every optional finder failed; only "no .text", "no exception directory", MatchAmmo
  not found / not unique are fatal.

## L2946-L3076 ini (load_ini(path)); GetPrivateProfile*A on ONE file path
- `split_list(src, out, max)` L2947-2959: separators space / comma / tab, lowercases, truncates each to TLEN-1 (47) chars, max entries.
- Every key is read with GetPrivateProfileIntA / GetPrivateProfileStringA; float keys go through `atof` of a string (locale "C" assumed:
  the CRT default; inferred). Clamps: SwitchRatio 0.1..1.0; ProgressUnits >= 0; CloseDistance >= 0; RunMode <= 3 (no lower clamp, -1 =
  none); MoveRefreshUnits >= 10; MoveRefreshFraction 0..1; BlowReach each >= 2.0; AutoDraw Distance >= 0; LeaveFactor >= 1; TickMs >= 20;
  ProbeMs 0 -> 1; InfantryRange >= 0; Vehicles CloseDistance >= 0; AdjacentMax >= 0. No clamps on the ms keys.
- Code defaults that DIFFER from what load_ini uses as its default: `[Charge] CooldownMs` load_ini default 4000 (ini file 2000);
  `[Charge] CloseDistance` static 150 vs load_ini default "200"; ini file values override both. (Full key table: see section 6.)
- `[Zerg] Tag`: read into a TLEN buffer and split to ONE tag; empty value -> `cfg_zerg_tag[0][0]=0` = no tag needed.
- `[ShieldMelee] SeeThroughTag` and `[Downed] Tag`: one tag each, the split_list return value is ignored (L3020, L3023). An EMPTY value
  writes nothing (the `while (*p ...)` loop L2950 does not run), so the code default ("forcefield" / "start_personage") stays; a value made
  only of separators (" ," ) runs the loop once with k=0 and writes `out[0][0]=0`, emptying the tag (never matches). Same rule for every
  split_list caller whose count is ignored. `[Vehicles] ContactTag` uses the count (`n_contacttag`): empty = contact-tag path off.
- `[ReloadCancel] KeepTags`, `MeleeAmmoTags`, `VehicleMeleeTags` up to MAXT (24); SkipTags / IgnoreTargetTags up to 8.

## L3078-L3092 init_tag()
- `g_tagl[0]` = Tag (first word), `g_tagl[1]` = AlwaysTag (if non-empty, n_tagl=2); `g_ctagl[0]` = [Charge] Tag; `g_amtag` = AutoMeleeTag
  split, up to NAMT 4 (n_amtag 0 = rule off); `g_amtag_s` comma-joined; `g_mml` = stance tags + AutoMeleeTag tags (one list for
  may_melee); `memset(g_mmc)`. NOTE: if Tag is empty, `split_list(cfg_tag, g_tagl, 1)` leaves g_tagl[0] at its old content (zero-init ""),
  n_tagl still 1 -> an empty tag that never matches (eq_ci needs equal length; str_get rejects size 0).

## L3094-L3103 install helpers (only when !MS_TEST)
- `emit(p,b,n)` L3096; `emit_absjmp(p,to)` L3097-3103 writes `FF 25 00 00 00 00` + 8-byte absolute address (14 bytes).

## L3105-L3159 install() part 1 (called from DllMain; see later)
1. `base = GetModuleHandleW(NULL)` (the exe). `init_tag()`.
2. `[MeleeStance] Enabled=0` -> log `[GEM MeleeStance %s] Enabled=0, not installed`, return (no thread, no hooks).
3. `find_all` error -> log `[GEM MeleeStance %s] NOT installed: %s`, return (no thread, no hooks).
4. linked sites < 20 -> SkipLinked forced off (log).
5. `cfg_ammotags = s.ammotags` (offset from code, replaces 0xEC8). `g_clock = s.clock` when found (L3118).
6. charge_err -> `[Charge]` off (log `  charge OFF: %s`), and AutoDraw off when AutoDraw or Vehicles were on (log `  auto-draw OFF (needs
   the charge finders): %s`). NOTE: [Vehicles] stays on (ms_decide's vehicle rule still runs) but without refs / orders.
7. charge finders OK (L3121-3139): sets g_range, g_scan_lo/hi, g_exe_lo, g_mkatk, g_addorder, g_setent, g_mkmove, g_fight, g_mvchange,
   g_uimove (logs the charge mode lines), g_refinit, g_tagctor, g_refvt, g_refdtor, g_tagdtor, g_reflist_off (log `  refs: entity ref
   list at +0x%X, stale refs are dropped without unlink` or `... stale-ref check OFF`), g_ordstamp, g_clock.
   CONSEQUENCE: if the charge finders fail, g_scan_lo stays NULL (from_scan never true), g_addorder NULL (no orders anywhere), and the
   only remaining features are the stance filter (ms_decide, with g_clock possibly NULL), downed filter, multi-select, shield ray, blow
   reach, direct control.
8. MATCHAMMO PROLOGUE DETOUR L3141-3154: `tr = VirtualAlloc(4096, PAGE_EXECUTE_READWRITE)` (anywhere in the address space; abs jumps so
   no +-2 GB need); trampoline = the 14 original bytes `48 89 5C 24 20 55 56 57 41 54 41 55 41 56` (VERIFIED only indirectly: find_all
   accepted the function only if it starts with the 16-byte PROLOG) + `FF 25 00000000 <fn+14>`; `FlushInstructionCache(tr, 64)`; `g_orig
   = tr`. Patch = `FF 25 00000000 <&ms_hook>` (14 bytes) written over fn[0..13] with VirtualProtect RWX / restore + FlushInstructionCache.
   NO re-check of the bytes right before the write (they were checked during find_all, microseconds earlier, same thread). The detour
   target is the C function `ms_hook` itself (no asm stub): the engine's `call MatchAmmo` pushes its return address, the jmp keeps it,
   so `__builtin_return_address(0)` in ms_hook = the engine call site. A VirtualProtect failure returns AFTER the trampoline was built
   (harmless; logs `[GEM MeleeStance] VirtualProtect failed (%lu)`); a VirtualAlloc failure logs and returns.
9. L3156-3157 builds the MeleeAmmoTags list string; L3158 `if (cfg_log) CreateThread(hb_thread)` - THE HELPER THREAD (heartbeat AND the
   GEM Teleport gem_order_listen connection) ONLY STARTS WHEN Log=1. With Log=0 there are no order events from Teleport (inferred
   consequence; order poll still runs from the tick / scan).

## L3159-L3281 install() part 2
- L3159-3162 install line: `[GEM MeleeStance %s] installed: ammo match exe+0x%llX, ammo tags +0x%X, linked +0x%X bit %u (%d sites) |
  Tag=%s AlwaysTag=%s MeleeAmmoTags=%s SkipLinked=%d DrawFromBelt=%d Log=%d`.
- L3163-3168 `  no-melee rule ON: AutoMeleeTag=%s | ...` / `  no-melee rule OFF (AutoMeleeTag empty): ...`.
- L3169-3175 `  charge ON: Tag=%s MeleeInHand=%d PlayerUnits=%d CooldownMs=%u RetargetMs=%u RefreshMs=%u MinDistance=%.0f MaxDistance=%.0f |
  scan exe+0x%llX, MakeAttack exe+0x%llX, AddOrder exe+0x%llX, ref vt exe+0x%llX, range exe+0x%llX, clock exe+0x%llX, stamp exe+0x%llX`.
- SHIELD RAY PATCH L3177-3204 (only `[ShieldMelee] Enabled=1`; default 0, live 0): ray_err -> `  melee through shields OFF: %s`. Else stub
  at `tr+512` (same RWX page as the MatchAmmo trampoline): `48 83 EC 20` sub rsp,20h; `mov rcx, r<ray_reg>` (4C/48 89 C1|reg<<3);
  `48 B8 <&ms_ray_ok>` mov rax,imm64; `FF D0` call rax; `48 83 C4 20` add rsp,20h; `84 C0 74 0E` test al,al / je +14; abs jmp ray_ok;
  abs jmp ray_fail. Site patch: 30 bytes at ray_site = abs jmp (14) to the stub + 16 NOPs. Original 30 bytes expected = the pattern
  matched in find_all (`48 8B 05 / 48 3B 05 / 0F 84 / 4C 39 ?? 18 / 0F 85`), not re-checked at write time. Comment L3182: the site is
  mid-function, rsp already 16-aligned, so only 0x20 shadow space. Note: the stub does not leave rax = hit-list begin as the replaced
  `mov rax,[hits]` did (clobbered by the call); the ok path at p+30 must not rely on rax (inferred safe since Alex tested 0.7 ok; but
  it is OFF since 0.6.1). Log `  melee through shields ON: blow ray test exe+0x%llX (hits exe+0x%llX, target r%d), skips ...`.
- MULTI-SELECT PATCH L3206-3226 ([MultiSelect] Enabled=1): inst_err -> `  multi-unit instant buttons OFF: %s`. Else sets g_exinst,
  g_expand, g_pickctor, g_ordstamp (again), g_takeitem, g_off_item, g_off_apply, g_off_getactor, g_off_canapply; VTABLE SLOT SWAP: every
  found slot (`exslots`, 9 in all three exes per the test log) `VirtualProtect(slot, 8, PAGE_READWRITE)`, `*slot = ms_exinst`, restore. No
  check of the slot value at write time (it was == ExecuteInstant during find_instant). Log `  multi-unit instant buttons ON
  ({apply_to_all} actions): ExecuteInstant exe+0x%llX in %d/%d vtables, apply_to_all +0x%X, GetActor +0x%X (exe+0x%llX), CanApply +0x%X,
  expand exe+0x%llX`.
- L3228-3231 take-item / TagContainer: inst_err or no takeitem -> log (auto-draw still holds/refuses guns); else g_takeitem + g_ordstamp
  (so g_takeitem is set even with MultiSelect=0). tc_err -> log; else g_tcctor / g_tcfromstr.
- L3232-3236 auto-draw info lines (`  auto-draw ON: Distance=%.0f HoldMs=%u PlayerUnits=%d Item=%s | TagContainer ctor exe+0x%llX
  FromString exe+0x%llX`, `  fight range (0.7.16): Probe=%d ...`).
- L3237 order vtables g_vt_attack / g_vt_attackb / g_vt_fight (set unconditionally). L3238-3241 vehicles lines.
- L3243-3253 reload cancel: rc_err -> `  reload cancel OFF: %s`; else g_invbusy / g_invcancel / g_curweap (GetCurrentWeapon is ONLY
  assigned here, so ReloadCancel=0 also turns off held_weapon_name / holds_keep_weapon / ranged_back gun memory). Log line with KeepTags
  and the three addresses.
- L3255-3265 direct control: only when SkipDirectControl=1; both finders failed -> log, nothing set; else g_ismanual and g_mgr (either may
  be NULL) + log `  direct control: a unit under manual control is left alone ... | IsManualControlled exe+0x%llX | manager exe+0x%llX
  (actor +0x3B0)` (or the error text).
- L3267 downed info line. BLOW REACH DATA WRITE L3268-3280 ([BlowReach] Enabled=1): table not found -> log; else
  `VirtualProtect(blowtab, 12, PAGE_READWRITE)`, writes the three floats (Near/Medium/Far), restores; log `  blow reach ON: near/medium/far
  17/29/35 -> %.1f/%.1f/%.1f units (table exe+0x%llX, GetBlowDistance exe+0x%llX)`. Process-wide engine data change.

## L3283-L3303 DllMain (only when !MS_TEST)
- `DLL_PROCESS_ATTACH` only: `DisableThreadLibraryCalls(inst)`; own path `GetModuleFileNameA(inst)` (fails or >= MAX_PATH-16 -> return
  TRUE silently, nothing installed); strip the extension; exe name from `GetModuleFileNameA(NULL)`; ini = `<dllpath-without-ext>.ini`;
  log = `<dllpath-without-ext>` + `_editor` if the exe name contains "_ed", else `_server` if it contains "_server", else "" + `.log`
  (e.g. gem_meleestance_editor.log, gem_meleestance_server.log, gem_meleestance.log). `load_ini(ini)`, `install()`. Always returns TRUE.
- NO EXE-NAME GATE: it installs into whatever process loads it (the launcher only injects into the game / editor / server; inferred).
- Everything (whole-.text finder scans, VirtualAlloc, patches, CreateThread) runs inside DllMain under the loader lock. The hb_thread
  starts running after DllMain returns (loader lock). No DLL_PROCESS_DETACH handling: hooks are never removed.

## L3304-L4125 offline self-test (`#ifdef MS_TEST`; build.bat test = `zig cc ... -DMS_TEST -o dist\ms_test.exe src\gem_meleestance.c`)
- Helpers: `fake_str(p,s)` L3306-3313 (MSVC std::string; heap form via `_strdup` for > 15 chars, never freed); `g_t_amtag`, `T_AMTAG
  "mm_test"` L3316-3317 (second pass); `fake_vec(at, store, a, b, c)` L3318-3324 (up to 3 tags of 0x60-byte records, +T_AMTAG on entity tag
  vectors in the second pass: detected by `at + 0x1400 - cfg_enttags == store`, i.e. it relies on the fake_t layout obj[0x1400] then tags);
  `fake_t{obj[0x1400], tags[0x60*4], names[0x60*4]}` L3325.
- Fake engine L3327-3348: `fake_orig` (MatchAmmo: records ctx[0x18], returns 0x1234), `f_clock=1000`, `f_stamp=77`, `f_nop`, `f_setent`
  (ref+8=1, ref+0x18=ent), `f_mkatk`, `f_add` (AddOrder: counts, accepts only params {1,77}), `f_range` (3.5), `f_busy`, `f_cancel`,
  `f_curweap`, `f_manual`, `f_take` (take-item factory), `t42_ctx` (builds a 0x20-byte ctx). All `ms_abi` (matches the engine ABI; on
  x86_64-windows-gnu ms_abi is the default anyway).
- `main(argc, argv)` L3350-4124:
  1. `--amtag` as argv[1] -> second pass: `g_t_amtag=1`, argc=1 (skips the exe finder part).
  2. Finder part L3357-3409: for each exe path, `LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES)` (maps the exe image with
     relocations applied? DONT_RESOLVE_DLL_REFERENCES maps it as an image, no DllMain, imports not resolved; the code also handles a
     preferred-base mapping via vt_delta / FIXP / VTP in case relocations were not applied). `find_all(base, &s)` and prints / checks: ammo
     match RVA, `s.ammotags == 0xEC8`, linksites >= 20, shield ray site, take-item, TagContainer, the three order vtables, multi-select,
     blow table, MakeMove, eFight, eMovementChange, UI move, reload cancel trio, IsManualControlled, manager (FAIL unless the path contains
     "server"), charge set (scan, range, MakeAttack, AddOrder, MakeRef, SetEntity, RefInit, TagCtor, refvt, RefDtor, TagDtor, stamp, clock),
     ref list offset. Any missing -> bad. (`#define R(x)` L3401 generic macro name, never #undef'd.) Output = the per-exe address lines in
     build_log.txt (game / editor / server 1.069).
  3. Behaviour cases (no exe needed): `cfg_log=0`; first pass `cfg_amtag=""` (rule off), second pass `"melee_infantry, MM_Test"`;
     `init_tag()`; `g_orig = fake_orig`. Blocks, each printing one line (see build_log.txt L61-85):
     8 ms_decide cases (stance / no stance / crew / non-human) + 3 ms_hook cases; charge (queue / flush / cooldown / reach / ranged / stance /
     always / far / close / 95 m + carried-melee gate); refresh threshold; log caps per mission (log_epoch); 0.7.41 ghost; shield ray (5);
     0.7.39 vehicles + infantry-first + auto-draw; hull contact (4); direct control (11 incl. 0.7.32 component guard and the manager);
     reload cancel (6); probe + fight order + order picks + order poll + own tick (ms_tick with g_sim_tid = current thread, g_tick_wall=0) +
     fight-range stance + back to the gun; stale refs 0.7.33 (7); 0.7.35 fast pass / known cache / client gate / queues; 0.7.36 player units;
     0.7.37 mission wipe; 0.7.42 (a) 16 cases, (b)+(c) 7, (e) 4, (d) 6.
  4. Final line `filter: %zu cases + 3 hook cases + charge, %d failed`.
  5. First pass only: re-runs ITSELF as a child process (`GetModuleFileNameA(NULL)` + `CreateProcessA("\"<me>\" --amtag")`, inherited std
     handles, waits) and fails when the child's exit code is non-zero; prints `second pass (AutoMeleeTag on, every fake unit tagged): %s`.
  6. `ALL OK` / `SOME FAILED`; exit code = bad.
- What the test relies on (for making main() a callable function): (a) all module state is file-static and starts zeroed - the cases
  depend on the ORDER of the blocks and on leftovers (several blocks reset tables explicitly: L3909, L3947-3948, L3994); the second pass
  needs a FRESH PROCESS for clean tables and counters (comment L4104); (b) g_orig / engine pointers / g_clock are overwritten with fakes;
  (c) the finder part needs only the exe paths. To turn it into `int stance_selftest(int argc, char **argv)`: rename `main`; replace the
  self-re-exec (L4104-4121) with either a call of a `stance_reset_all()` (memset every table + counter + cfg back to defaults; does not exist
  today) followed by the second pass in-process, or keep the child-process re-exec with an argument the combined test exe forwards to this
  function (e.g. `--stance-amtag`); route printf to the shared test output; keep `cfg_log=0` (the fakes are not engine objects: logging
  calls vec_str on fake names only, but `readable()`-based helpers are fine).
