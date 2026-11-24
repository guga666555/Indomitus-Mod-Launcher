# map_pen: GEM PenFilter 0.1.1 (module "pen")

Source snapshot: `inputs\penfilter_0.1.1\gem_penfilter.c` (2476 lines, sha256 460dd416... per MANIFEST.json), `gem_penfilter.ini`,
`README.md`, `build.bat`, `out\selftest.txt`. All `L<n>` references below are lines of `gem_penfilter.c` unless another file is named.
Address ground truth: `out\selftest.txt` (game / editor / server, offline finder run) and `inputs\live_payload_a1d6ed3c\gem_penfilter_editor.log`
(editor run 12:16-12:34 Nov 20 2026). The editor addresses of the live log equal the selftest editor block line for line (checked).
`out\gem_penfilter.dll` md5 5bfbd47d... == `live_payload_a1d6ed3c\gem_penfilter.dll` (same build); snapshot ini == live ini == out ini (diff empty).

The file is in two parts: Part I = reading log written chunk by chunk while reading the source in order (raw facts with line numbers);
Part II = the structured map (sections 1-10 the task asked for), written after the whole source was read.

# Part I: reading log

## L1-L200: header comment, includes, config globals, log, string / tag helpers, AimPriority parser (start)
- L1-61 header comment = the engine model the code relies on (game RVAs): MatchAmmo 0x92C9C0 + ctx layout (L5-6: ctx +0 ammo eStuff,
  +8 shooter, +0x10 target info (+0x18 target entity, +0x28 target position); row flags byte +0x394 bit 2 = `{volume "x"}`); A1 / A2 sites
  (L7-8); ctx builder 0x92C8E0 (L9-10: called right before MatchAmmo in 8 pairs; enemy scan 0x970990 passes weapon NULL, r14 = weapon stuff);
  params 0x81EC40 = PLIST(weapon+0xA0, ammo+0xEC8), range 0x83F210 = PLIST(wstuff+0x1600, ammo+0xEC8)+0x20 (L11-12); pen 0x841650 (L13,
  distance in edx as float bits); armour chain (L14-17: angle 0x7FC090, block 0x7FC310, evaluate 0x7FC3A0, bs.flags bit1 pierce / bit2 full
  pen, threshold 0x7FCDE0, effective 0x7FCA60, thickness 0x7FC5B0, typed value get/set 0xA8BE80 / 0xA8BE10); volume picker 0x92B6F0
  (L18-24: static candidate vector begin 0x15B3630 / end +8 / cap +0x10, ebp = previous-aim index or -1, up to min(n,5) tries, set volume
  0x852DC0, range check 0x81F710, solver 0x833C30, status 3 keep); aim record layout (L25-28); weapon layout (L29-30: +0x90 shooter,
  +0x98 wstuff, +0xA8 firer, +0xB0 ammo slot, aim slots +0xC8 / +0x168; stop 0x81F480(weapon,1)); solver hit record 0x48 bytes (L31);
  eBox face index (L32-33); sim clock = dword read by 0x66E7B0 (L34); IsManualControlled 0x804650 (L35).
- L37-58 behaviour summary of A / P / B / X and keys (entity id +0xC0, ammo name-tag hash mixed with weapon-stuff name-tag hash, volume index).
- L62-70 headers: `WIN32_LEAN_AND_MEAN`, windows.h, stdint, stdio, stdlib, string, stdarg, math, limits. L72 `#define PF_VERSION "0.1.1"`.
- L75-78 generic macro names: `MAXT 16`, `TLEN 64`, `MAXG 8`, `MAXGT 64` (collision risk in a unity build; harmless in separate TUs).
- L79-82 config statics with defaults: cfg_enabled 1, cfg_log 1, cfg_logverdicts 400, cfg_rowfilter 1, cfg_aimcheck 1, cfg_approach 0,
  cfg_cachems 60000, cfg_rule 1, cfg_player 1, cfg_facing 1, cfg_prio 1, cfg_exhaust 1, cfg_exstop 1, cfg_holdms 8000, cfg_holddist 15,
  cfg_oldstreak -1, cfg_unrms 3000, cfg_dcskip 1. L83 `#define BLK_COS 0.9063078f` (cos 25 deg). L84 cfg_pierce "pierce";
  L85-86 cfg_exempt {melee_light, melee_heavy, melee}, n_exempt 3. L87-92 DEF_PRIO string (= ini default). L93-94 g_ptag[64][64],
  g_ptag_grp[64], n_ptag, n_grp. L95 g_holdunits = 300.0f (HoldDistanceM x 20 units/m; <= 0 = no distance gate).
- L98-113 log: g_logpath[MAX_PATH], FILE *g_log, `volatile LONG g_vlines` (verdict line budget counter). `logf_(fmt,...)`: lazily
  fopen(g_logpath,"a") on first call (no-op while g_logpath empty), vfprintf + '\n' + fflush per line. NO lock, NO timestamp, NO prefix
  (prefixes like `A [ms]` are written by the callers). Not thread-safe by itself (CRT FILE has its own lock, so lines do not interleave
  mid-call in practice; inferred from MSVCRT/ucrt FILE locking, not read).
- L116-127 `eq_ci(s,n,t)`: length-exact ASCII case-insensitive compare.
- L129-137 `str_get(p,&n)`: MSVC std::string reader (buf/ptr +0, size +0x10, cap +0x18); guards cap<size, cap>0x10000, size>0x1000 ->
  NULL; SSO when cap <= 15; NULL data -> NULL. No VirtualQuery: trusts the pointer.
- L139-150 `vec_has(v,list,n)`: std::vector of 0x60-byte Tag (name std::string at Tag+0x40). 1 = any name equals any list entry (eq_ci),
  0 = none or begin/end NULL, -1 = broken (e<b, size not multiple of 0x60, > 1024 tags).
- L151-165 `vec_str`: names joined by ' ' into out (cap, maxn), log only; > 256 tags = empty.
- L167-179 `vec_hash`: FNV-1a 32 over lower-cased names each followed by ' ' ; 0 on broken / NULL vector (> 256 tags). Deterministic
  (content only) = MP-safe key part.
- L180-184 `mix64`: murmur3 fmix64 finaliser (cache slot index).
- L187-200 `parse_prio(s)` (continues): groups by ';', alternatives by ','; spaces / tabs dropped; tokens truncated to TLEN-1; at most
  MAXG (8) groups and MAXGT (64) tags; an empty group (";;") does not advance g (`any` gate).

## L200-L419: priority group lookup, sites_t, pattern engine, all byte patterns, image helpers, find_all start
- L203-216 `vol_group(vol)`: info = [vol+0x30] (volume info); tag vector at info+0x398 / +0x3A0 (begin / end, 0x60-byte Tags, name at
  +0x40); returns the LOWEST group index whose tag list matches any of the volume's tags; n_grp (= "last") when info NULL, no ptags,
  broken vector (> 256 tags). Pure function of the volume's tags + ini.
- L219-241 `typedef struct sites_t` (generic name `sites_t`; static use only): every found address / offset + an error string per finder
  (err_ma, err_bld, err_ai, err_arm, err_b, err_clk, err_box, err_p, err_x, err_gv, err_stop, err_man, scan_err).
- L244-268 pattern engine: `pat_t {b[512], m[512], n}` (generic type name), `pat_parse` ("??" wildcard, hex pairs, spaces), `pat_at`,
  `pat_find(lo,hi,p,&count)` = first hit + total count (linear scan of .text; no SIMD). L269 `rel32(at)` = at+4+*(int32*)at.
- Patterns (L271 comment: cut from the 1.068 game exe, rel32 / rip displacements wildcarded, identical in editor and server). Byte
  lengths counted from the source:
  | name | line | len | anchors at | used for |
  |---|---|---|---|---|
  | P_MA | L272-276 | 130 | A2 site (`A8 02 0F 84`) .. end of MatchAmmo's volume loop | A1 = +0x67 (MA_A1, L277), MA_JNE 0x6E (L278), MA_NEXT_REL 0x14 (L279) |
  | P_BLD | L280-281 | 37 | ctx builder prologue | BLD_COPY 15 (L282) = bytes moved to the trampoline |
  | P_PAIR | L283 | 13 | `call builder; mov rcx,rax; call MatchAmmo` | builder / MatchAmmo pairs |
  | P_SCANW | L284-285 | 30 | after the scan's MatchAmmo: weapon-stuff cast + range calls | wcast, range |
  | P_WCAST | L286-287 | 38 | weapon-stuff dynamic_cast thunk | verify |
  | P_RANGE | L288 | 19 | range(): `mov rcx,[rdi+wlist]; lea rdx,[rbx+ammotags]; call plist` | wlistoff, plist |
  | P_P | L289-291 | 73 | params(weapon) | loaded getter, ilistoff, plist |
  | P_PEN | L292-293 | 30 | pen(dist) prologue | pen |
  | P_C090 | L294-295 | 32 | angle function prologue | c090 |
  | P_FABS | L296 | 8 | `andps xmm0,[rip]; ret` | fabs |
  | P_C310 | L297-301 | 135 | armour block builder (whole function) | c310 + callees |
  | P_C3A0 | L302-312 | 350 | armour evaluator (start) | c3a0 + callees |
  | P_BSITE | L313-314 | 36 | picker code before the solver call, ends `cmp eax,3` | B site = +0x1C (BSITE_CALL, L315) |
  | P_SOLV | L316-318 | 56 | solver prologue | verify (weapon+0x10/+0xA8/+0x90 layout) |
  | P_PICKP | L320-321 | 40 | picker try-loop head `mov rdi,[end]; mov eax,5 ...` | P site (PICKP_LEN 7, L322), PICKP_JS 0x1F (L323) |
  | P_PUSH | L325-326 | 32 | candidate push in the collection loop | verify vector |
  | P_GETV | L328 | 24 | previous-aim test `call AimVolume` | getvol |
  | P_GETVB | L329 | 28 | AimVolume body | verify |
  | P_CLR | L330 | 23 | aim Clear prologue | verify |
  | P_FIRE0 | L332 | 19 | fire function's mode-0 `call Stop` | wstop |
  | P_WSLOT | L334 | 21 | Stop: `lea rdi,[rbx+slot]; ... call Clear` | ws1 / ws2 |
  | P_CLK | L335 | 18 | `mov ebx,[r9+64h]; call; sub eax,ebx; ...` | sim clock |
  | P_MANUAL | L338-339 | 40 | IsManualControlled prologue | ismanual |
  | P_PROPON | L340 | 8 | `cmp dword [rcx+18h],0; setne al; ret` | verify PropOn callee |
  | P_PLAYER | L341 | 14 | `mov rcx,[rax+0DC8h]; test byte [rcx+398h],3` in MatchAmmo | verify |
- L344-356 `sec_range(base,name,&lo,&hi)` PE section by 8-char name (VirtualSize). L357-373 `.pdata`: `g_pdata`, `g_npdata` (static
  globals, set by `pdata_init`), `is_func_start` = binary search of RUNTIME_FUNCTION.BeginAddress == rva.
- L375-382 `find_leas(from,to,&r8,&r9)`: LAST rip-relative `lea r8,[rip]` / `lea r9,[rip]` in a range (dynamic_cast type descriptors).
- L384-402 `rtti_vtable(base,name)`: finds the decorated type name in .data, TypeDescriptor = name-0x10, then a COL in .rdata
  (signature 1, offset 0, pTypeDescriptor, pSelf == own RVA), then the vtable slot -8 holding the COL VA (either live address or
  ImageBase-relative = works on a mapped file and on the loaded image). Returns vtable (v+8).

## L405-L618: find_all (part 1)
Signature `static void find_all(uint8_t *base, sites_t *s)` (L405). memset, pdata_init, .text range (L410: no .text -> err_ma = err_b =
err_p = err_x = "no .text section", return). `#define IN_TEXT(x)` (L413, never #undef'd in what I read so far: see later).
1. MatchAmmo sites (L416-442): P_MA must match exactly once in .text (count != 1 -> err_ma "not unique" / "not found (unknown build or
   already patched)"). a2 = hit, a1 = hit+0x67, match = rel32(hit+4) (target of `je`), next = je-NEXT rel8 target, tagtest = rel32(a1+1).
   Checks: the `jne` at +0x6E lands on match (L424-425); match bytes `48 8B C7` (mov rax,rdi) (L426); next bytes `48 81 C7 98 03 00 00`
   (add rdi,398h = row stride 0x398) (L427); tagtest in .text (L428). Ammo tag offset (L431-432) = imm32 of the LAST `add rcx,imm32(<0x10000);
   call` in [a2-0x200, a2) (game/editor/server 0xEC8). P_PLAYER must occur in [a2-0x200, a2) (L433-435). At least one other `call tagtest`
   in [a2-0x200, a2) (L437-439: "already redirected?" check: if A1 were already redirected the class loop's call would still hit the original
   tag test... the check is that the class loop calls the same function the A1 site calls).
   Window note: [a2-0x200, a2) = game [0x92C9E3, 0x92CBE3); MatchAmmo starts 0x223 before a2 (game 0x92C9C0, editor 0x7AADA0 / a2 0x7AAFC3,
   server 0x73EE60 / a2 0x73F083), so the first 0x23 = 35 bytes of MatchAmmo are OUTSIDE every pen scan window. MeleeStance's prologue
   hook rewrites 14 bytes (its source L36) -> no overlap with any pen pattern or scan.
2. ctx builder + enemy scan (L445-498; needs no err_ma, else err_bld "needs MatchAmmo"): linear scan of .text for P_PAIR; the second
   call's target must lie within 0x400 BEFORE a2 and be a .pdata function start (= MatchAmmo `ma_fn`); every pair must agree on builder
   and MatchAmmo (else err_bld "disagree"); builder prologue must equal P_BLD (L463 "differs (unknown build or already hooked)" - this is
   W's own patch area, so a second W on the same bytes fails here). npairs = count (8 on all three exes). Enemy scan (L467-497): pairs with
   `xor edx,edx` (33 D2) within 0x18 bytes before the builder call: exactly 2 (else scan_err). Sorted; r14 = [rdx+40h] (`4C 8B 72 40`)
   within 0x400 before the first; `mov r8,r14` (`4D 8B C6`) within 0x20 before the second; distance <= 0x300; P_SCANW within
   [sc0+13, sc0+0x80) -> wcast = rel32(w+4), range = rel32(w+0x1A); wcast prologue == P_WCAST; P_RANGE within 0x80 of range(); the
   ammo-tag disp in range() == ammotags; wlistoff = disp at rq+3 (0x1600), plist = rel32(rq+15); scan_ret[i] = sc[i]+5 (the return
   addresses of the two builder calls: game 0x970CC3 / 0x970E2F, editor 0x7EF741 / 0x7EF8AC, server 0x782F53 / 0x7830BF). Scan errors
   only set scan_err (not err_bld): W still installs, the scan's weapon fallback (r14) is then off (see W handler).
3. Armour chain (L501-516): P_C310 unique -> a79530 = rel32(+0x3E), armorof = rel32(+0x46), matof = rel32(+0x55), relof = rel32(+0x6D);
   P_C3A0 unique -> a8be80 = rel32(+0x25), cde0 = rel32(+0x40), a8be10 = rel32(+0x93), c5b0 = rel32(+0xA2), ca60 = rel32(+0xAD); cross
   checks rel32(+0x139) == cde0 and rel32(+0xC8) == a8be80. Errors -> err_arm.
4. AimInfo chain (L520-564; needs armour): every `call c3a0` in .text; collect up to 256 calls in the 0x700 bytes before it whose target is
   a .pdata start or follows CC/90/C3; need n >= 10 and the tail ct[n-1]=c310, ct[n-3]=a8be10, ct[n-4] matches P_FABS, ct[n-2] matches
   P_C090; rt = ct[n-5] (dynamic_cast); then search back for i with ct[i] = pen (P_PEN), ct[i+1] = c310, ct[i-1] = tagtest, ct[i-3] = rt,
   ct[i-4] = params (P_P). Takes aiminfo_call, c090, rtdc, fabs_, pen, loaded = ct[j-2], P = ct[j-4]; the two dynamic_cast type-descriptor
   pairs from the last `lea r8` / `lea r9` in the 0x28 bytes before the casts (pc_src / pc_dst = params casts, wc_src / wc_dst = weapon
   stuff casts); wstuffoff = disp of the last `mov rcx,[rXX+disp32]` before the weapon-stuff cast (must be 0x98); caliboff = disp of the
   first `movss xmm,[r+disp32]` between that cast and the angle call (0x1628). Checks L552-562: params calls loaded at +0x11 and +0x22;
   params ammo-tag disp at +0x28 == ammotags; ilistoff = disp at P+0x38 (0xA0); plist = rel32(P+0x45) (overrides range()'s, mismatch ->
   scan_err); wcast must call the same dynamic_cast at +0x1D (else scan_err).
5. B site (L567-574): P_BSITE unique (36 bytes incl. the `E8 rel32` of the solver call at +0x1C, wildcarded rel32) -> bsite = hit+0x1C,
   solver = rel32(bsite+1); solver prologue == P_SOLV. NOTE: the rel32 is wildcarded and the opcode E8 kept, so P_BSITE STILL MATCHES after
   B's own call redirect (a second copy of B would then find the site and fail later only on the solver prologue check: the redirected
   target is the dll stub, not the solver, so err_b "aim solver prologue differs"). Inferred from the pattern; B's patch kind is read below.
6. P / X / getvol (L577-606; need B): P_PICKP unique in [bsite-0x200, bsite) -> vend = rel32(ps+3), vbeg = rel32(ps+15) (game candidates
   0x15B3630); T = js target (ps+0x1F, `0F 88 rel32`) = exhausted exit. vend must be vbeg+8; P_PUSH in [ps-0x100, ps) must use the same
   vector (end, cap = beg+16, `add qword [end],8` at +0x1B). X: T in (bsite, bsite+0x100), bytes at T = `49 8B CE E8` (mov rcx,r14; call),
   T[-9..-5] = `83 EF 01 0F 89` (sub edi,1; jns loop), clr = rel32(T+4) must match P_CLR, loop = T + rel32(T-4) between ps and bsite ->
   xsite = T+3 (the 5-byte call), clr. getvol: P_GETV in [ps-0x80, ps) and its call target matches P_GETVB. err_p and err_x are separate:
   a P pattern failure sets both (L580); later P-only failures leave X's finder result alone but X needs P at install (README).
7. Weapon stop (L609-...): P_FIRE0 unique in .text -> st = rel32(f0+15); needs s->clr (X finder) else err_stop "needs the aim clear";
   inside st's first 0x30 bytes a `cmp qword [rcx+98h],0` (`48 83 B9 98 00 00 00 00`) must exist (chk) ...

## L618-L1016: find_all end, engine call typedefs, globals, engine structs, entity readers, stats, change-only log, memo tables, volume frame, sim caches (start)
- Weapon stop finder end (L614-625): st = rel32(f0+15) (P_FIRE0 = `cmp dword [rXX+8],0; jne +12; mov edx,1; mov rcx,rbx; call Stop`);
  `chk` = `cmp qword [rcx+98h],0` in st's first 0x30 bytes (L616-617, the weapon-stuff test); P_WSLOT occurrences in st..st+0x120 whose
  call goes to s->clr: exactly two with different slot displacements -> ws1 / ws2 (0xC8 / 0x168 on all three exes, selftest L10/21/32).
  err_stop "no weapon-stuff (+0x98) test" / "not exactly two aim slots cleared". Requires the X finder's clr (L612).
- Sim clock (L628-635): P_CLK unique (`mov ebx,[r9+64h]; call Clock; sub eax,ebx; mov ecx,eax; call MsToSec`, the picker wrapper); f =
  rel32(ck+5) must be `8B 05 rel32 C3` (mov eax,[rip+x]; ret) -> clk = rel32(f+2) = the dword global (game 0x1584568, editor 0x13DF988,
  server 0x11CBA58). err_clk "sim clock read not found" / "sim clock getter differs". NOTE: this is the same global MeleeStance and
  MeleeChase find (merge plan: "Sim clock (ed 0x13DF988) is found three times"); pen's finder is P_CLK via the picker wrapper.
- Direct control (L638-647): P_MANUAL unique -> PropOn = rel32(mn+17) must equal P_PROPON (`cmp dword [rcx+18h],0; setne al; ret`);
  mn must be a .pdata start -> ismanual (game 0x804650, editor 0x67E5C0, server 0x617E70). Offsets 0xDE0 / 0xDD8 / 0x70 / 0x18 are FIXED
  BYTES of the pattern (so a layout change makes the finder fail instead of reading wrong fields). The function is never called: is_manual()
  re-implements it (L739-746); the finder only proves the layout.
- eBox / ePolyhedron vtables (L650-652): rtti_vtable(".?AVeBox@TraceVolume@@") / (".?AVePolyhedron@TraceVolume@@") (game 0x10E3420 /
  0x10E3540, editor 0xF3C210 / 0xF3C330, server 0xDE1B50 / 0xDE1C70); either missing -> err_box (facing off, see install).
- L653 `#undef IN_TEXT`. find_all writes ONLY into *s; no engine memory is written by any finder; all finders read .text / .data / .rdata
  of the main module (base) and .pdata.
- L657-672 function-pointer typedefs (generic names: fp_p, fp_pp, fp_pen, fp_rtdc, fp_angle, fp_blk, fp_eval, fp_thick, fp_eff, fp_thr,
  fp_dvget, fp_mult, fp_rel, fp_solve, fp_v, fp_wstop). Engine signatures as the dll calls them (Win64 ABI):
  loaded(weapon)->ammo; params P(weapon)->entry; wcast(x)->weapon stuff; armorof(entity)->armour; getvol(aimrec)->volume;
  plist(list, ammo+ammotags)->raw entry; matof(armour, rec)->material; pen(params, dist float bits in edx)->float;
  rtdc(obj, 0, src, dst, 0) = __RTDynamicCast; angle(bs, params, cos); block(blk, entity, shooter, rec, bs); eval(blk);
  thick(blk, 0)->float; eff(blk, thickness)->float; thr(blk)->float; dvget(dv, unit)->float; mult(int, ?, float*);
  rel(a,b)->uint8 (relation); solve(6 args)->status; clear(aimrec); wstop(weapon, int).
- L674 `static sites_t S;` L675 `static volatile uint32_t *g_clock;` L676 `now_ms()` = *g_clock or 0 if not found (sim ms; 0 forever when the
  clock finder failed -> holds never age: see install gate).
- L677-692 static engine function pointers e_loaded, e_P, e_wcast, e_armorof, e_getvol, e_plist, e_matof, e_pen, e_rtdc, e_angle, e_block,
  e_eval, e_thick, e_eff, e_thr, e_dvget, e_mult, e_rel, e_solve, e_clear, e_wstop.
- **L693 `__attribute__((used)) void *g_tagtest;` = NON-STATIC global** (read by the asm stubs by name; generic name, link collision risk).
- L695 statics hookA_on, hookB_on, hookP_on, hookX_on, prio_on, facing_on, solve_on. L696 `static SRWLOCK g_lock = SRWLOCK_INIT;`
  (the one lock; where taken: below).
- L699-728 engine struct mirrors (generic typedef names): `bstate_t` 0x34 "bullet state" (+0 flags bit1 pierce bit2 full pen, +4 calibre,
  +8 angle, +0x24 slope, +0x28 frac, +0x2C dv_type, +0x30 dv_val); `blk_t` armour block 0x28 (+0 armour, +8 rec, +0x10 mat, +0x18 bs,
  +0x20 mult, +0x24 enemy); `rec_t` hit record the chain reads 0x34 (+0 pt, +0xC nrm, +0x18 dir, +0x24 face, +0x28 vol, +0x30 flag).
- L730 `is_vehicle(t)` = t && (t[0x30]&2) && !(t[0x30]&4) (entity type byte: actor and not human). No pointer probe.
- L731-735 `player_unit(e)`: brain = [e+0xDC8]; brain && ([brain+0x398]&3)==1 (MatchAmmo's own enable_for_user test, P_PLAYER).
- L738 `static int dc_on;` L739-746 `is_manual(e)`: 0 if !dc_on, e NULL, or not actor (e[0x30]&2 == 0); 1 if [e+0xDE0] non-NULL and
  [[e+0xDE0]+0x18] != 0; else bit 1 of [[e+0xDD8]+0x70] (null-checked). No VirtualQuery / SEH; no caching (read every call). Compare with
  MeleeStance's "0.7.32 component guard" version (merge plan design 4).
- L747 `ent_id(e)` = dword [e+0xC0]. L748-753 `edist(a,b)` = 3D Euclid of positions at +0x74 (engine units; 20 units per metre assumed).
- L754-760 `ent_desc` = "0x<id>(<first 3 def tags>)" from def = [e+0x548], tag vector def+0x330 (log only).
- L761-767 `vol_index(target, vol)`: linear search in the target's volume pointer vector [target+0x510 .. +0x518) (<= 4096); UINT32_MAX
  if absent. This index (not a pointer) is the cache key part.
- L768-776 `vol_desc` "vol#i(tags)" (log only; tags from [vol+0x30]+0x398).
- L779-787 `stats_t ST` (log only): counters; x_clear / x_exh / x_stop are `volatile LONG` (Interlocked use expected), the rest plain.
- L790-806 change-only verdict log: `V_NONE INT32_MIN`, `vl_t g_vl[4096]` direct-mapped by mix64(sid,tid,akey,hook). vl_change returns 1 =
  print (verdict changed or new key, budget g_vlines < cfg_logverdicts and cfg_log), 0 = repeat (reps++ / ST.log_rep++) or over budget
  (ST.log_supp++). Writes *pv / *preps = previous verdict and repeat count. Log only (sim never reads it). Budget counter g_vlines is
  `volatile LONG` but incremented with plain `g_vlines++` (L804).
- L807-821 was_str / nm_allow / nm_pierce / nm_group (`static __thread char b[24]`: thread-local static buffer = TLS in a dll) / nm_exh.
- Memo tables (L824-919), all direct-mapped, keyed by POINTERS (engine object addresses), values are pure functions of the pointed-to data:
  | memo | line | size | key | value | engine call on miss |
  |---|---|---|---|---|---|
  | m_ammo (am_memo_t) | L824-839 | 512 | ammo eStuff ptr | filtered = has PierceTag in the ammo `{tag}` vector (ammo+ammotags+0x318) and no ExemptTags; key = vec_hash(ammo name tags at ammo+0x868+0x318) ^ 0x9E3779B9; name (first 2 name tags) | none |
  | m_par (pm_memo_t) | L840-851 | 1024 | ({parameters} list ptr, ammo ptr) | params = rtdc(plist(list, ammo+ammotags), 0, pc_src, pc_dst, 0) or NULL | e_plist, e_rtdc |
  | m_bs (bs_memo_t) | L852-875 | 1024 | (params ptr, calibre float bits, cos bucket) | bullet state after e_angle(bs, params, cos_rep(b)) with dv_type 0 | e_angle |
  | m_mat (mat_memo_t) | L876-887 | 2048 | (armour ptr, volume info ptr) | material = matof(armour, rec{vol, nrm +Z, dir -Z}) | e_matof |
  | m_thr (thr_memo_t) | L888-919 | 8192 | (info, armour, mat, params, factor bits, mult bits, calibre bits, face, cosb, PierceRule) | pen6 threshold (INF_THR 1e30 when calibre < material min (mat+0x10) or mat flag bit0 (mat+0xB0)); rule 1 = thr(blk), rule 3 = max(thr, eff(thick)); NaN -> 0 (do not filter) | e_thick, e_eff, e_thr |
  NCOSB 32 (L854): cos_bucket(c) = floor(c*32) clamped 0..31; cos_rep(b) = (b+1)/32 (rounded towards head-on, README). face_threshold
  (L892-919) builds bs from m_bs, slope = 1 when mat flags (mat+0xB0) & 6, rec{vol, face, nrm +Z, dir -Z}, blk{armour, &rec, mat, &bs,
  mult, enemy 1} and calls the engine's thickness / effective / threshold sub-functions directly (not the block builder c310 / evaluate
  c3a0). Purity caveat: keys are pointers; a freed-and-reused engine object at the same address would hit a stale entry (whether the
  memos are flushed at a new battle: see flush below). Value purity also assumes the engine functions are pure (no RNG; inferred).
- L924-941 `vol_frame(vol, ax, ay, az, c)`: trace volume = [vol+0x50]; vtable == box_vt -> 1, == poly_vt -> 2, else 0; matrix floats at
  vol+0x58+0x30 (3x3 rows + translation m[9..11]); columns normalised (length in (1e-6, 1e6) else 0); centre = -(t . axes).
- L944-961 `vol_dir(vol, target, eye, u)`: unit vector volume-centre -> eye, in the volume frame (returns 1) or in world axes from the
  target origin +0x74 when no frame (returns 2); 0 = degenerate (< 1e-3 units).
- L967-969 `aged(t, now, ms)` = (uint32)(now - t) >= ms (wrap-safe); `nearer(was, now)` = g_holdunits > 0 && was - now > g_holdunits;
  `farther` symmetric.
- L972-976 `wa_key(akey, ws)` = akey ^ (vec_hash(ws+0x868+0x318) * 0x9E3779B9) (weapon-stuff name tags; ws NULL -> akey ^ 0).
- Blocked cache (L980-1002): `blk_cache_t c_blk[8192]` {sid, tid, key, vidx, t, dist, u[3], fr, set}, slot = mix64(sid<<32|tid ^ key<<7 ^
  vidx*golden) & 8191. blk_hit: miss unless all four keys match; expired when aged(CacheMs) or shooter came > HoldDistance nearer; dropped
  when both old and new frame kinds are non-zero and (kind differs or dot(u_old, u_new) < BLK_COS = cos 25 deg); a zero frame kind on either
  side keeps it. blk_put overwrites the slot (collision eviction is deterministic given the keys).
- Hold cache (L1005-1016...): `hold_t c_hold[2048]` {sid, tid, key, t, dist, verdict, rs, set}, slot = mix64(sid<<32|tid ^ key*golden) &
  2047. hold_get: miss unless keys match, not aged(VerdictHoldMs), rs (range state) equal; ALLOW (verdict 1) dropped when farther, REJECT
  (0) dropped when nearer ...

## L1016-L1360: hold / unreachable caches, epoch flush, per-thread records, shared pierce test, hook A handlers, hook P (start)
- L1019-1023 `hold_put` overwrites the slot (sid, tid, key, t = now, dist = gdist, rs, verdict). hold_get (L1011-1018) also returns the
  hold start time (`since`).
- Unreachable cache (L1025-1040): `unr_t c_unr[2048]` {sid, tid, key, vidx, t, set}, slot = mix64(sid<<32|tid ^ key<<9 ^ vidx*0xC2B2AE3D27D4EB4F)
  & 2047; unr_hit = keys match && !aged(UnreachableMs); unr_put overwrites. Written only by B's handler on solver failure of a P-cut list,
  read only by P.
- **New-battle flush** (L1044-1057): statics `g_last_now`, `g_epoch`. `epoch_check(now)`: if now < g_last_now -> memset c_blk, c_hold, c_unr,
  m_ammo, m_par, m_bs, m_mat, m_thr, g_vl; g_epoch++; ST.flushes++; g_vlines = 0 (log budget restarts); log line `new battle [%u ms]: sim
  clock went back from %u ms, caches cleared (LogVerdicts budget restarts)`. Always g_last_now = now. Caller must hold g_lock (comment
  L1043). Called from prep() (A, L1246), pf_pick (L1329), pf_solve (L1393, L1413), pf_exhaust (L1518). ALL memos are flushed too, so the
  pointer-keyed memos cannot carry across battles. t_prep (thread-local A memo) is invalidated by the g_epoch compare (L1250).
  The flush happens on the first hook call that sees a smaller clock: a hook that never runs in the new battle never flushes; a test
  stopped within its first seconds is missed (README risk).
- Builder record (L1060-1065): `brec_t {ctx, weapon, ret, r14}` (generic name), `static __thread brec_t t_rec`; **`pf_record(ctx, weapon,
  ret, r14)` NON-STATIC, `__attribute__((used))`** (called from pf_stub_w). Thread-local: MatchAmmo on another thread cannot see it.
- Per-pick state (L1068-1072): `static __thread struct {...} t_pick` {open, tries, maxtries, bblocks, haskey, cut, target, weapon, sid, tid,
  key, gdist}; P opens it, B counts tries / blocks, X closes. Thread-local, no lock needed.
- prep_t (L1075-1082): state 0 = do not filter, 1 = filter, 2 = reject every volume; ammo, shooter, target, am, params, calibre, dist (the
  distance pen() is looked up at), gdist (shooter-target origin distance for hold / block gates), pen, pen6, factor, mult, armour, spos (eye),
  sid, tid, key, rs (pen >= 1e-5), verdict, held, since. vinfo_t (L1083) {vidx, face, nvol, ncached, cos, thr} (log only).
- `prep_fill` (L1086-1107): key = wa_key(am->key, weapon stuff); calibre = [cw + caliboff] (cw = weapon stuff dynamic_cast to the
  calibre-bearing class); pen = e_pen(params, dist bits); pen6 = e_dvget({0, pen}, 6) (typed value conversion to unit 6); armour =
  e_armorof(target); factor = *(float*)armour (1.0 if NULL); mult = 1 then e_mult(0xB, target, &mult) (actor multiplier); eye = shooter
  origin (+0x74) + 30 units z ("~1.5 m", 20 units/m assumed); sid / tid = entity ids; gdist = edist; rs = pen >= 1e-5; state = rs ? 1 : 2.
- `box_faces(vol, eye, cs[6])` (L1112-1127): 0 when Facing off (facing_on) or no frame; else cos per face index between the face's outward
  axis and the eye->centre direction (0 +X, 1 +Z, 2 +Y, 3 -Y, 4 -X, 5 -Z); returns 1 box / 2 polyhedron.
- `vol_ok(p, vol, vidx, now, vi)` (L1130-1156) = THE pierce test (A1, A2-verdict via any_volume_ok, P, X):
  1. blk_hit(sid, tid, key, vidx, now, gdist, vol, target, eye) -> not pierceable (vi face -2).
  2. no armour -> pierceable. 3. material = mat_for(armour, vol); NULL or computed thickness (mat+0xB0 & 4) -> pierceable (left to B).
  4. faces: box -> faces with cos > 0.1, each at its own bucket; polyhedron -> faces with cos > -0.2, head-on (bucket of 1.0); other -> all 6
     head-on; pierceable on the first face with pen6 >= face_threshold(...). 5. box with no face passing the cos gate -> all faces at bucket
     31. Returns 0 with the best (lowest-threshold) miss in vi.
- `vol_candidate(vol)` (L1158-1166): the picker's own rule re-implemented: vol non-NULL, state [vol+0x38] NULL or byte +0x24 == 0
  (unbroken), and info tag vector (+0x398) has no `no_target` (`g_notarget`).
- `any_volume_ok(p, now, vi)` (L1168-1182): over the target's volume list [target+0x510..+0x518) (> 4096 or broken -> returns 1 = ALLOW):
  first candidate volume that vol_ok accepts -> 1; counts nvol / ncached (B-blocked), keeps the nearest miss. Order = list order.
- Hook A thread memo (L1185): `static __thread struct { ctx, ammo, shooter, target, weapon, r14, now, epoch, prep_t p } t_prep`.
- `a_verdict(p, now)` (L1187-1211): hold_get hit -> held (ST.a_held); else v = state 1 ? any_volume_ok : 0, hold_put(v), since = now;
  p->verdict = v; state = v ? 1 : 2. Change-only log line (hook 0) `A [ms] shooter <desc> target <desc> ammo "<name>" d=<dist> (<m> m)
  pen=<pen> (<pen6> armour units) -> ALLOW|REJECT (<why>)<was>`; why = "held since N ms" / "no penetration at this range" /
  "vol#i face f cos c needs t" / "no candidate pierceable (n volumes, k blocked by B); nearest vol#i face f needs [no-pierce
  material/calibre ]t" / "no candidate pierceable (n volumes, k blocked by B)".
- `prep_ctx(ctx, p, now)` (L1213-1242): ammo = [ctx], shooter = [ctx+8], ti = [ctx+0x10], target = [ti+0x18]. Gates in order: all non-NULL,
  is_vehicle(target), target != shooter; ammo filtered (PierceTag and not Exempt); PlayerUnits=0 and player_unit(shooter) -> skip;
  is_manual(shooter) -> skip (ST.dc_skip); t_rec.ctx must equal ctx (else ST.a_noweap, skip); weapon known: list = [weapon+ilistoff (0xA0)],
  ws = [weapon+wstuffoff (0x98)]; else enemy-scan fallback when t_rec.ret is scan_ret[0] / [1] and t_rec.r14: ws = e_wcast(r14), list =
  [ws+wlistoff (0x1600)]; neither -> a_noweap skip. params = params_for(list, ammo); cw = rtdc(ws, wc_src, wc_dst); NULL -> skip;
  e_rel(shooter, target) == 0 (friendly) -> skip. Distance for pen = shooter origin to target-info position (ti+0x28), NOT the target
  origin; then prep_fill + a_verdict. (state stays 0 on every skip = "do not filter".)
- `prep(ctx)` (L1243-1255, caller holds g_lock): now = now_ms(); epoch_check(now); thread memo hit when ctx, ammo, shooter, target, now,
  epoch, t_rec.ctx == ctx, weapon and r14 all equal -> reuse t_prep.p (so the A1 per-volume calls of ONE MatchAmmo row loop and the A2 call
  share one verdict per sim ms); else prep_ctx and store.
- **`pf_vol(ctx, slot)`** (L1257-1271, NON-STATIC, used by pf_stub_a1): hookA_on off -> 1; exclusive g_lock; p = prep(ctx); state 2 -> 0;
  state 1 -> vol_ok(p, *slot, slot - [target+0x510], now_ms(), NULL); counters a_vpass / a_vblock; returns 1 = volume counts.
- **`pf_novol(ctx, row)`** (L1273-1283, NON-STATIC, used by pf_stub_a2): hookA_on off -> 1; exclusive g_lock; p = prep(ctx); returns
  state != 2 (counters a_rpass / a_rblock). row unused.
- `prep_weapon(p, weapon, shooter, target, ammo)` (L1286-1301) = prep for P / X: is_vehicle(target), target != shooter, filtered,
  PlayerUnits, is_manual (ST.dc_skip) gates; params = rtdc(e_P(weapon)) (the LOADED ammo's {parameters}: engine params() = PLIST(weapon+0xA0,
  loaded ammo)), cw = rtdc([weapon+0x98]); e_rel; prep_fill with pendist = edist(shooter, target) (origin to origin, unlike A).
- `pick_compact(b, n, keep, cur)` (L1303-1309): stable in-place compaction of the engine candidate array; *cur follows its volume or -1.
- L1310 `#define PICK_MAX 512` (generic-ish name). **L1311 `uint8_t ***g_vb, ***g_ve` NON-STATIC** (stub reads g_ve).
- **`pf_pick(aim, weapon, target, cur64, noaim)`** (L1313-1376, NON-STATIC, from pf_stub_p): n = *g_ve - *g_vb (<= 4096); opens t_pick
  (open 1, target, weapon, tries 0, bblocks 0, haskey 0, cut 0, maxtries = min(n,5)) BEFORE any gate, so X works with HookAimPriority=0.
  Gates: prio_on, weapon, target, 0 < n <= PICK_MAX (512), is_vehicle(target) -> else return cur unchanged. shooter = [weapon+0x90], ammo =
  e_loaded([weapon+0xB0]) (loaded ammo); exclusive g_lock; epoch_check; prep_weapon; state 0 -> return cur. haskey + keys into t_pick.
  For each candidate b[i] find its index k in the target's volume list (walk forward from the last match, else restart search); skip when
  !vol_ok or unr_hit (nunr++); grp[i] = vol_group; best = lowest group. best found -> keep[] = grp == best, pick_compact, **writes the
  engine's static vector end `*g_ve = b + kept`** (and the array contents), maxtries = min(kept,5), cut = 1, counters. Change-only log (hook
  2) `P [ms] ... -> aim group N (tags): k of n candidates kept[, previous volume dropped][, u pierceable but unreachable skipped]` or
  `-> no reachable pierceable candidate of n[, ...], list left as it is`. Returns new ebp (nc). Note state 2 (pen < 1e-5) gives all grp
  INT_MAX -> list untouched.

## L1360-L1666: hook P end, hook B (pf_solve), hook X (pf_exhaust), the asm stubs
- **`pf_solve(firer, aim, a3, a4, outrec, a6)`** (L1379-1496, NON-STATIC; same 6-argument shape as the engine solver, so the B call site can
  call it directly: install below). local[0x60] 16-aligned stack buffer; r = outrec if non-NULL or B off, else local (B asks the solver
  for its hit record; at the picker call outrec is NULL, P_BSITE `mov [rsp+20h],rax(0)` / `[rsp+28h]`). target = [aim+0x18];
  inpick = t_pick.open && t_pick.target == target -> t_pick.tries++. st = e_solve(...) (the real solver, always called first).
  - st != 3: if inpick && t_pick.cut && haskey && e_getvol && target -> lock, epoch_check, av = e_getvol(aim), unr_put(sid, tid, key,
    vol_index(target, av)) (ST.s_unr). Return st unchanged. (Runs even with HookAimCheck=0: the redirect stays for P, README.)
  - st == 3 and B on: weapon = [firer+0x10] (the firer's weapon), shooter = [weapon+0x90], hitent = [r+0x18], vol = [r+0x20]; gates:
    shooter, target, hitent == target, vol, is_vehicle(target), PlayerUnits; ammo = e_loaded([weapon+0xB0]); lock + epoch_check;
    am filtered && is_manual(shooter) -> ST.dc_skip, no change; filtered && e_rel(shooter, target) -> params = rtdc(e_P(weapon)), cw =
    rtdc([weapon+0x98]); dist = shooter origin -> hit point (r+8); pen = e_pen; rec R from the solver record (pt r+8, nrm r+0x28,
    dir r+0x34, face r+0x40, vol); cosv = |nrm . dir| clamped >= 0.01; bs = bs_init(calibre), dv {0, pen}; if pen >= 1e-5:
    e_angle(&bs, params, cosv); e_block(&b, target, shooter, &R, &bs) (engine block builder c310: fills armour, material, enemy flag);
    material NULL or computed (mat+0xB0 & 4) -> pierced; else slope = 1 when mat & 6, e_eval(&b) (engine evaluator c3a0), pierced =
    rule 3 ? (flags&3)==3 : flags&1; !b.enemy -> pierced. Not pierced -> blk_put(hit volume) and blk_put(aimed volume = e_getvol(aim))
    when different (ST.b_aimed); **res = 5** (returned to the picker instead of 3: the candidate is erased and the next try rolls);
    inpick -> t_pick.bblocks++, keys refreshed. Change-only log (hook 1) `B [ms] ... pen=.. pen6=.. vs flag-1 threshold .. (full-penetration
    armour ..) calibre .. min .. mat flags 0x.. -> flags 0x..[ (not an enemy)]; hit vol#i(tags) face f cos c, aimed <vol> "<name>" ->
    PIERCE|BLOCK` (log-only extra engine calls e_thr / e_eff / e_thick on copies, L1471-1476), or the short form when not evaluated.
    Aim name = std::string at aim+0x60 (volume Tag name).
  - Returns res (3 or 5, or the solver's own failure status). Lock held around the body; e_solve is called WITHOUT the lock.
- X (L1499-1563): `slot_on(slot, target)` = [slot+8] (mode) != 0 && [slot+0x18] == target.
  **`pf_exhaust(aim, weapon, target)`** (NON-STATIC, from pf_stub_x): 1. e_clear(aim) FIRST, always (the engine call it replaces);
  InterlockedIncrement(ST.x_clear). 2. mine = t_pick.open && same target && same weapon; full = mine && maxtries > 0 && tries >= maxtries;
  bc = full && bblocks > 0 && haskey; t_pick.open = 0 (closed on every exit through this clear). 3. !bc -> return. 4. lock, epoch_check,
  ST.x_bc; shooter under direct control -> log (hook 4, change-only) `X [ms] shooter 0x.. is under direct control: left alone (no verdict,
  no stop)`, return. 5. loaded ammo -> prep_weapon -> v = any_volume_ok (state 1) / 0 (state 2) -> **hold_put** (rewrites A's verdict for
  the LOADED ammo + weapon key); v stays -1 when not filtered. 6. if ExhaustStop && e_wstop && v >= 0 && weapon stuff present: for the two
  aim slots (weapon+ws1 0xC8, weapon+ws2 0x168) that hold this target: aimed = 1; REJECT -> stop; else slot volume (e_getvol(slot))
  known and !vol_ok -> stop (unknown volume = keep). 7. change-only log (hook 3) `X [ms] shooter 0x.. target 0x..: pick ran out after n
  tries, k blocked by B -> ammo verdict ALLOW|REJECT|unchanged (ammo not filtered here) held (n volumes, k blocked by B)[, weapon stopped
  (its aim at this target cannot pierce) | , weapon kept (aimed at a pierceable volume)]`. 8. unlock, THEN `e_wstop(weapon, 1)` (engine
  cease-fire 0x81F480 / ed 0x699CC0 / srv 0x631D00) outside the lock; ST.x_stop.
- Stub globals (L1566) **NON-STATIC `uint64_t g_a2_back, g_a2_match, g_a2_next, g_w_tramp, g_p_back`** (`__attribute__((used))`).
- Asm block (L1567-1660), one top-level `__asm__` in `.intel_syntax noprefix`, `.text`, five `.globl` labels (link-global):
  | stub | how reached | engine registers it relies on | does | returns / continues |
  |---|---|---|---|---|
  | pf_stub_a1 (L1571-1586) | `call` (A1 rel32 redirect replaces `call TagTest`) | rcx / rdx = TagTest args (unchanged), rbx = &volume slot, r13 = ctx (callee-saved, MatchAmmo's) | push rbp / align 16 / shadow 0x20; `call [g_tagtest]` (original TagTest); al == 0 -> return 0; else pf_vol(r13, rbx) | `ret` with eax = pf_vol (0/1); engine does `test al,al; jne MATCH` |
  | pf_stub_a2 (L1588-1607) | `jmp` (replaces `test al,2; je MATCH`, 8 bytes) | al = row flags byte (+0x394), r13 = ctx, rdi = row | al&2 (row has `{volume}`) -> `jmp [g_a2_back]` (site+8, the volume loop); else aligned call pf_novol(r13, rdi) | eax != 0 -> `jmp [g_a2_match]` (MATCH: `mov rax,rdi`), else `jmp [g_a2_next]` (NEXT: `add rdi,398h`) |
  | pf_stub_w (L1610-1624) | `jmp` from the builder prologue (detour, 15 bytes moved to a trampoline) | rcx = ctx, rdx = weapon or NULL, [rsp] = return address, r14 = caller's r14 | push rcx/rdx/r8/r9, sub 0x28 (aligned), pf_record(rcx, rdx, [rsp+0x48] = return address, r14); restore | `jmp [g_w_tramp]` (moved prologue bytes + jmp back to builder+15) |
  | pf_stub_p (L1627-1645) | `jmp` (replaces the 7-byte `mov rdi,[rip+end]` at the try-loop head) | r14 = aim, r13 = weapon, r15 = target, ebp = previous-volume index, r12b = noAimCheck; comment: rax, rcx, rdx, r8-r11, flags dead | push rbp; aligned frame 0x30; pf_pick(r14, r13, r15, [rbp] (= the caller's rbp), 5th arg = movzx r12b) | ebp = eax (new index), rdi = [[g_ve]] (re-reads the possibly cut end), `jmp [g_p_back]` (site+7) |
  | pf_stub_x (L1648-1658) | `call` (X rel32 redirect replaces `call Clear` at T+3) | rcx = aim (`mov rcx,r14` before the call), r13 = weapon, r15 = target | aligned frame; pf_exhaust(rcx, r13, r15) | `ret` (rax = whatever pf_exhaust leaves; Clear's own return value is not forwarded) |
  B has NO stub: pf_solve is a plain C function with the solver's arguments (see install). XMM registers: no stub saves xmm0-5; at the
  A1 / X sites they are dead across a call anyway (the engine made a call there); at the A2 / P jmp sites the comment only lists GPRs and
  flags as dead (xmm0-5 not mentioned: inferred dead, check the bytes - see Part II).
- L1661-1665 extern declarations of the five stubs.

## L1667-L1955: ini, site log, patch writer, install, stats thread, DllMain
- `load_ini(ini)` (L1669-1703): section **`[PenFilter]`**, `GetPrivateProfileIntA` / `GetPrivateProfileStringA` on the given path. Keys and
  defaults: Enabled 1, Log 1, LogVerdicts 400, HookRowFilter 1, HookAimCheck 1, HookAimPriority 1, HookExhaust 1, ExhaustStop 1,
  HookApproach 0, CacheMs 60000, VerdictHoldMs 8000, HoldDistanceM 15, UnreachableMs 3000, PierceRule 1 (anything but 3 -> 1), PlayerUnits 1,
  SkipDirectControl 1, Facing 1, BlockStreak -1 (read only to warn), PierceTag "pierce" (into cfg_pierce[64]), ExemptTags
  "melee_light,melee_heavy,melee" (strtok on ", ", <= 16 tags, each < 64 chars), AimPriority DEF_PRIO (into g_prio_str[2048], parse_prio).
  Clamps: CacheMs / VerdictHoldMs / UnreachableMs < 0 -> 0; g_holdunits = HoldDistanceM > 0 ? x20 : 0. `#define GI` local (#undef L1690).
  strtok is not reentrant (only DllMain uses it).
- `prio_desc` (L1704-1713) rebuilds the AimPriority string for the log.
- L1715 `#define OFF(x)` (exe-relative offset for logs; generic name, never #undef'd). `log_sites()` (L1716-1740) = the 9 finder lines
  (the selftest / live log lines 2-10 per exe), each with " | ERROR <msg>" / " ERROR <msg>" / " | scan OFF: <msg>" on failure.
- Patch writer (L1743-1779): `write_code(at, bytes, n)` = VirtualProtect RWX, memcpy, restore old protection, FlushInstructionCache.
  **No compare of the bytes being replaced at write time** (the finders' patterns are the only verification; they run just before, in
  the same DllMain call), no thread suspension, no atomic write (memcpy of 5 / 7 / 8 / 15 bytes while other engine threads may run:
  inferred harmless because DllMain runs at injection before the sim starts; not proven). `absjmp(p, to)` = `FF 25 00 00 00 00` + 8-byte
  address (14 bytes). `alloc_near(site)` = VirtualQuery + VirtualAlloc(MEM_RESERVE|COMMIT, 0x1000, RWX) at allocation-granularity steps
  within +-0x70000000 of the site (never freed). `rel_ok`, `put_rel(patch, op, site, to)` (E8 / E9 rel32).
  Thunk slots in the near page (L1780-1784): TH_A1 0, TH_A2 16, TH_B 32, TH_P 48, TH_X 64 (each an absjmp to the stub / handler).
- **`install_at(base)`** (L1786-1904), called once from DllMain:
  1. Enabled=0 -> log `[GEM PenFilter 0.1.1] Enabled=0, not installed`, return (no finders run).
  2. find_all; log `[GEM PenFilter 0.1.1] finders:` + log_sites.
  3. core = !err_ai && !err_arm && !err_clk (L1792). core -> bind g_clock and every e_* pointer (L1794-1801); e_getvol only without err_gv,
     e_wstop only without err_stop, g_tagtest = S.tagtest (even if err_ma). NOT core -> no engine pointer bound, every hook "NOT installed".
  4. facing_on = Facing && box_vt && poly_vt (L1803).
  5. near page: alloc_near(a2 or bsite or .text start) if core and any hook wanted; none -> "no executable page near the exe: nothing installed",
     core = 0.
  6. **A** (L1809-1839) needs Enabled hook, core, !err_ma, !err_bld, rel32 range: VirtualAlloc ANY-address RWX page `tr` (trampoline) =
     the builder's first 15 bytes + absjmp(builder+15); g_w_tramp = tr; g_a2_back = a2+8; g_a2_match = match; g_a2_next = next; thunks A1 /
     A2; patches: builder 15 bytes = absjmp(pf_stub_w) (14) + 1 NOP; A1 5 bytes = `E8 rel32` -> thunk A1; A2 8 bytes = `E9 rel32` -> thunk A2
     + 3 NOP. Written in that order; any write failing -> "VirtualProtect failed" and hookA_on stays 0 (earlier writes NOT undone).
     Installed log: `  row filter (hook A) installed: builder exe+0x.., volume site exe+0x.., no-volume site exe+0x..[ (enemy scan calls
     unfiltered: no weapon known there)]`. scan_err only removes the enemy-scan fallback.
  7. **B** (L1841-1855) when HookAimCheck or HookAimPriority, core, !err_b, range: thunk TH_B = absjmp(pf_solve) (pf_solve is the C
     function itself, no asm stub), B site 5 bytes `E8 rel32` -> thunk. hookB_on = HookAimCheck; solve_on = 1. Log `  aim check (hook B)
     installed: picker solver call exe+0x..[ (aim-volume getter missing: ...)]` or `... OFF by ini; picker solver call exe+0x.. redirected
     for P's unreachable fallback only`.
  8. **P** (L1857-1872) when HookAimPriority or HookExhaust, core, !err_b, !err_p, range: g_vb / g_ve = the engine vector's begin / end
     globals, g_p_back = psite+7; thunk TH_P; patch 7 bytes = `E9 rel32` + 2 NOP. hookP_on = 1; prio_on = HookAimPriority && solve_on.
  9. **X** (L1874-1890) when HookExhaust, hookP_on, !err_x, range: e_clear = clr; thunk TH_X; patch 5 bytes at xsite = `E8 rel32`.
     hookX_on = 1. Notes ExhaustStop=0 / weapon stop missing / B off.
  10. Direct control (L1891-1893): dc_on = 1 when SkipDirectControl and the IsManualControlled finder succeeded (independent of core).
  11. HookApproach=1 / BlockStreak present -> warning lines. Summary line `[GEM PenFilter 0.1.1] active|NOTHING installed | PierceTag=..
      ExemptTags=.. PierceRule=.. PlayerUnits=.. SkipDirectControl=.. CacheMs=.. VerdictHoldMs=.. HoldDistanceM=.. UnreachableMs=..
      ExhaustStop=.. Facing=..[ (trace volume vtables not found: all faces head-on)] LogVerdicts=.. | AimPriority (N groups) ..` ("active"
      = A or B or P on; X alone cannot be on without P).
  Failure fan-out: err_clk / err_ai / err_arm -> nothing installed. err_ma or err_bld -> A off only. err_b -> B, P and (through P) X off.
  err_p -> P and X off. err_x -> X off. err_gv -> no unreachable fallback, B caches only the volume hit, X cannot judge slot volumes
  (keeps firing unless REJECT). err_stop -> X never stops. err_man -> direct-control skip off (units filtered). err_box -> facing off.
  scan_err -> A skips enemy-scan MatchAmmo calls (no weapon known: a_noweap).
- `stats_thread` (L1907-1931, only in the non-test build): `Sleep(10000)` loop (wall clock, LOG ONLY), copies ST, skips when unchanged,
  logs `stats [<sim ms>]: A ammo verdicts ALLOW n (+d) ...` (the README's stats line). Reads ST without the lock (torn reads possible,
  log only). Never exits.
- **DllMain** (L1933-1954): DLL_PROCESS_ATTACH only: DisableThreadLibraryCalls; module path (n == 0 or >= MAX_PATH-16 -> return TRUE,
  nothing done, no log); strip extension; exe name -> log suffix `_editor` when the exe name contains "_ed", `_server` when it contains
  "_server", else none; ini = `<dll path without ext>.ini`; g_logpath = `<dll path without ext>[_editor|_server].log`; load_ini;
  install_at(GetModuleHandleW(NULL)) **inside DllMain (loader lock held)**; stats thread CreateThread when Log and A / B / P installed.
  No exe-name gate (any process that loads it runs the finders on its main module and logs). No DLL_PROCESS_DETACH handling (patches and
  pages are never removed). Log name: game `gem_penfilter.log`, editor `gem_penfilter_editor.log`, server `gem_penfilter_server.log`.

## L1956-L2476: offline self-test (PF_TEST build)
- Build: `build.bat test` = same compiler + flags with `-DPF_TEST -o out\pf_test.exe gem_penfilter.c` (no -shared, no -s), run with the
  three exe paths, output to `out\selftest.txt`, success = a line exactly `ALL OK` (findstr /x).
- Helpers: `fake_str` (MSVC std::string, SSO <= 15, else _strdup heap), `fake_tags` (0x60-byte Tag vector). **Non-static** `t_p_rdi`
  (L1974) and a second top-level `__asm__` (L1976-2019) with `.globl` labels `t_call_a2`, `ta2_back`, `ta2_match`, `ta2_next`,
  `t_call_a1`, `t_call_w`, `tw_ret`, `t_call_p`, `tp_back`, `t_call_x` = harnesses that set the engine registers and jump / call into the
  real stubs (A2: al, r13, rdi; A1: r13, rbx, rcx = rdx = 0; W: r14 and a real return address `tw_ret`; P: r12 from the 6th stack arg,
  r14 / r13 / r15 / ebp, rdi = 0, returns at `tp_back` with t_p_rdi = rdi; X: r13, r15).
- `stub_test()` (L2029-2046): A2 volume row -> back (1), A2 no-volume row (no target -> state 0) -> match (2), A1 tag hit -> 1, miss -> 0,
  W records ctx / weapon / r14 / return address and reaches the trampoline (fake_builder returns ctx).
- Faked engine (L2048-2107): fk_* replacements for every e_* pointer; per-volume threshold at volume+0x80 read by fk_thr / fk_eval;
  fk_getvol reads aim+0x90; fk_rotz writes a box matrix at vol+0x58+0x30; 4 volumes (body, turret, mover+track, body).
- `logic_test()` (L2109-2405): saves S, fakes offsets (ammotags 0xEC8, wstuffoff 0x98, caliboff 0x1628, ws1 0xC8, ws2 0x168, ilistoff
  0xA0), g_clock = &fk_clock, facing off, cfg defaults, prio_on / hookB_on / dc_on = 1. Cases (= selftest.txt lines 36-61): AimPriority
  parser (custom + default 6 groups 33 tags); ammo + weapon key; P cases 0-4 (body pierceable / body not, turret kept as current / turret
  blocked by B -> tracks / nothing pierceable -> untouched / infantry target -> untouched); P unreachable fallback (solver status 1 recorded,
  next pick turret, 2.999 s still skipped, 3 s back, uncut list not recorded); B (plate hit first while aimed at body -> 5, both cached,
  other weapon key unaffected, counters, next pick turret -> 3); blocked-cache gates (59.999 s kept, 60 s dropped, 20 m farther kept, 20 m
  nearer dropped, other volume, shooter 30 deg round dropped / 10 deg kept, volume frame used, target turned 10 deg kept / 30 deg dropped);
  verdict hold (time, one-way distance both verdicts, range state); change-only log (first, repeats, change with was/count, other hook,
  over budget); X cases 0-7 (B-caused + aimed + nothing pierceable -> stop; no B block -> no hold; aimed elsewhere; out-of-range exit;
  aimed at pierceable tracks -> keep; aimed at B-blocked turret -> stop; aim volume unknown -> keep; direct control -> nothing); A through
  the stubs (hold, expiry at 8 s), A range state, A weapon key, direct control (A, P, the DD8 bit), new battle (flush, epoch, caches
  empty), pick_compact. Restores S and globals at the end (L2403).
- `patch_test(base)` (L2408-2436): install_at on the mapped image with logging off (g_logpath empty), then checks every written byte:
  A1 `E8` -> thunk -> pf_stub_a1; A2 `E9` + NOPs at +5 / +7 -> pf_stub_a2; B `E8` -> thunk -> pf_solve; P `E9` + NOP NOP, byte +7 = `B8`
  (the next `mov eax,5`); X `E8`, byte -3 = `49`; builder = absjmp pf_stub_w + NOP at +14; trampoline = P_BLD's first 15 bytes + absjmp
  builder+15; every bound pointer equals the finder result; solve_on && prio_on && dc_on. `#define THUNK` (L2415, never #undef'd).
- `main(argc, argv)` (L2437-2475): parse_prio(DEF_PRIO); per exe: `LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES)` (maps the exe
  as an image in the test process; same path twice returns the same mapping), find_all, log_sites to stdout (g_log = stdout), the
  "=> hook A .. OK" verdict line (fails also on scan_err / err_gv / err_stop / err_man), patch_test (patches the mapped image in place;
  never freed); then stub_test, logic_test, tag reader / stable hash check; prints `ALL OK` or `SOME FAILED`, returns bad.

# Part II: structured map

Evidence tags: `L<n>` = gem_penfilter.c line; `ST` = out\selftest.txt; `LIVE` = inputs\live_payload_a1d6ed3c\gem_penfilter_editor.log;
`DIS` = capstone disassembly of the installed 1.069 exes done for this map (script `work\pen\pen_sites.py`, output `work\pen\pen_sites.txt`,
plus the MatchAmmo / picker / Clear / weapon-stop listings quoted below, run from the same script's Img class). "inferred" = not proven by
code I read.

## 1. Start-up (DllMain to the last installer call)
Order (non-test build, `#ifndef PF_TEST` L1906):
1. `DllMain(inst, DLL_PROCESS_ATTACH)` L1933-1954. Other reasons ignored; no detach handling.
2. `DisableThreadLibraryCalls(inst)` L1937.
3. `GetModuleFileNameA(inst)`; n == 0 or n >= MAX_PATH-16 -> **return TRUE silently** (no log, nothing installed) L1939-1940.
4. Strip the extension; exe name of the process (L1943-1945). **The exe name is NOT a gate**: it only picks the log suffix (`_editor` if the
   exe name contains "_ed", `_server` if it contains "_server", else none) L1947-1948. ini = `<dll dir>\gem_penfilter.ini` L1946.
5. `load_ini(ini)` L1949 (section 6 for keys).
6. `install_at(GetModuleHandleW(NULL))` L1950 = everything below, **inside DllMain under the loader lock, on the injecting thread, before
   the game's simulation runs** (the launcher injects at start; inferred from the launcher design, see launcher.md).
   6.1 `Enabled=0` -> log `[GEM PenFilter 0.1.1] Enabled=0, not installed`, return (no finder runs) L1788.
   6.2 `find_all(base, &S)` L1789 -> finders in this order: MatchAmmo sites (err_ma) -> ctx builder + enemy scan (err_bld / scan_err; needs
       MatchAmmo) -> armour chain (err_arm) -> AimInfo chain (err_ai; needs armour) -> B site + solver (err_b) -> P / X / getvol (err_p,
       err_x, err_gv; need B) -> weapon stop (err_stop; needs X's clear) -> sim clock (err_clk) -> IsManualControlled (err_man) ->
       eBox / ePolyhedron vtables (err_box). Finders only read the main module image (.text/.data/.rdata/.pdata); no engine writes.
   6.3 log `[GEM PenFilter 0.1.1] finders:` + 9 site lines (log_sites L1716-1740).
   6.4 Gate `core = !err_ai && !err_arm && !err_clk` (L1792). core -> bind the clock and every engine function pointer (L1794-1801).
   6.5 `facing_on = Facing && box_vt && poly_vt` (L1803).
   6.6 near thunk page `alloc_near(a2 | bsite | .text start)` when core and any of HookRowFilter / HookAimCheck / HookAimPriority /
       HookExhaust; none -> "no executable page near the exe: nothing installed", core = 0 (L1805-1806).
   6.7 Hook A (W + A1 + A2) L1809-1839; 6.8 hook B L1841-1855; 6.9 hook P L1857-1872; 6.10 hook X L1874-1890 (order fixed: A, B, P, X).
   6.11 direct-control skip on/off (dc_on) L1891-1893 (independent of core); warnings for HookApproach=1 / BlockStreak (L1894-1895).
   6.12 summary line `[GEM PenFilter 0.1.1] active | ...` or `NOTHING installed | ...` (L1899-1903) = the LAST installer action.
7. `CreateThread(stats_thread)` only when Log=1 and (A or B or P) installed (L1951). The only thread pen starts. Never exits.

Failure fan-out (what else goes off when one finder fails):
| finder failure | effect |
|---|---|
| no .text | err_ma = err_b = err_p = err_x set and find_all returns at once (L410). err_ai / err_arm / err_clk stay NULL, so `core` is 1 with every engine pointer NULL, but A, B, P, X are all refused by their own errors; dc and facing finders never ran (off). Cannot happen on the game exes |
| err_clk, err_ai or err_arm | core = 0: **nothing** installed (A, B, P, X all "NOT installed"), engine pointers unbound; dc_on still set if IsManualControlled was found (no effect) |
| err_ma | A (and W) off; also err_bld = "needs MatchAmmo" |
| err_bld | A (and W) off |
| scan_err | A on, but MatchAmmo calls from the enemy scan carry no weapon (`a_noweap`, unfiltered); install log adds "(enemy scan calls unfiltered ...)" |
| err_b | B, P (needs !err_b) and X (needs hookP_on) off; err_p / err_x / err_gv also set "needs the picker solver call" |
| err_p | P and X off (err_x set too when the P pattern itself fails, L580) |
| err_x | X off; also err_stop "needs the aim clear" (L612) |
| err_gv | e_getvol NULL: B caches only the volume hit (not the aimed one), no unreachable fallback for P (pf_solve L1390 needs e_getvol), X cannot read slot volumes (keeps firing unless REJECT) |
| err_stop | e_wstop NULL: X never stops a weapon |
| err_man | dc_on = 0: units under direct control are filtered like the others |
| err_box | facing off: every face head-on (optimistic) |
| alloc_near / VirtualAlloc / VirtualProtect failure | that hook off; earlier writes of the same hook are NOT undone (e.g. builder written, A1 failed: W stays live, harmless) |

## 2. Hook inventory
Addresses: game / editor / server from ST (lines 3-34); the editor column equals LIVE (lines 2-14 of the log). Patch bytes from L1827-1882,
original bytes from DIS (identical instruction sequences on all three exes).

| id | engine site and what the engine does there | game / editor / server | patch kind, bytes | original bytes expected; how verified | stub -> C handler | when it runs, how often | reads / writes in engine memory | returns to the engine | lock / thread / re-entrancy |
|---|---|---|---|---|---|---|---|---|---|
| W | ctx builder prologue: fills the MatchAmmo ctx (ammo, shooter, ti, flags) from (ctx, weapon or NULL, ammo, shooter, ti, flag); called right before MatchAmmo in 8 pairs | 0x92C8E0 / 0x7AACC0 / 0x73ED80 | prologue detour, 15 bytes = `FF 25 00000000 <abs pf_stub_w>` + `90`; original 15 bytes copied to a VirtualAlloc'd (any address) RWX trampoline + absjmp builder+15 | `48 89 5C 24 08 57 48 83 EC 20 80 7C 24 58 00` (mov [rsp+8],rbx; push rdi; sub rsp,20h; cmp byte [rsp+58h],0: position independent, no rip-relative; the moved `cmp` sets flags that survive the `jmp`); verified by P_BLD (37 fixed bytes, L280-281, L463) just before the write | pf_stub_w (L1610-1624) -> pf_record (L1062) | every builder call (= every MatchAmmo call from the 8 pairs; stance's LIVE counter of MatchAmmo calls: 12045 between 3110 and 7916 sim ms, ~2.5 k/s, 2 tanks) | none (stores ctx, weapon, return address, r14 in thread-local t_rec) | jumps to the trampoline with rcx, rdx, r8, r9 and the stack restored | no lock; TLS; re-entrant |
| A1 | MatchAmmo volume loop: `call TagTest(volumeTag=r14, rowTag=rdi+0x330)` for rows with `{volume "x"}`; al != 0 -> MATCH | 0x92CC4A / 0x7AB02A / 0x73F0EA (TagTest 0x565340 / 0x3E4A10 / 0x37D9B0) | call-site rel32 redirect, 5 bytes `E8 rel32` -> near-page thunk +0 (absjmp pf_stub_a1) | `E8 <rel32 TagTest>`; P_MA (130 bytes from A2, rel32 wildcarded) + the class loop must call the same TagTest (L437-439) | pf_stub_a1 (L1571-1586) calls the original TagTest via `g_tagtest`, then pf_vol(r13 ctx, rbx &volume slot) (L1257) | per (row with {volume}, unbroken volume, volume tag) until a match; rare since the Nov 19 ammo.set change (LIVE stats: "volumes passed 0 blocked 0") | reads ctx, ammo, shooter, ti, target, volume list (+0x510/+0x518), volume info / state / matrix, armour, material, params via engine calls | al = 1 (volume counts) / 0 | g_lock exclusive inside pf_vol; engine functions called under it (section 3 extra); TLS memo t_prep |
| A2 | MatchAmmo: `test al,2; je MATCH` (al = row flags byte +0x394; bit 2 = row has `{volume}`); rows without `{volume}` match at once | 0x92CBE3 / 0x7AAFC3 / 0x73F083 (MATCH 0x92CC97 / 0x7AB077 / 0x73F137 = `mov rax,rdi`; NEXT 0x92CC65 / 0x7AB045 / 0x73F105 = `add rdi,398h`) | inline replace, 8 bytes = `E9 rel32` -> thunk +16 (absjmp pf_stub_a2) + `90 90 90` | `A8 02 0F 84 <rel32 MATCH>`; P_MA + jne-target == je-target, MATCH bytes `48 8B C7`, NEXT bytes `48 81 C7 98 03 00 00` (L424-427) | pf_stub_a2 (L1588-1607): al&2 -> `jmp [g_a2_back]` (= site+8); else pf_novol(r13 ctx, rdi row) (L1273) -> `jmp [g_a2_match]` / `jmp [g_a2_next]` | every row that passes the engine's earlier row tests (tag set, enable_for_user, range, LOS ...): LIVE ~1.1-2.2 k/s with 2 tanks | as A1 (verdict needs all volumes: any_volume_ok) | control flow: MATCH (row accepted) or NEXT (next row); volume rows continue the engine loop | g_lock exclusive inside pf_novol; DIS: at this site xmm6 holds the range value (callee-saved), xmm0-5 / rax / rcx / rdx / r8-r11 / flags are dead on all three continuations (MATCH writes rax; NEXT reloads at the loop head 0x92CAA0; the volume path reads only r13) |
| B | volume picker: `call Solver(firer=[weapon+0xA8], aim=r14, 0, 0, outRec=NULL, 0)`; status 3 = keep, else erase the candidate and roll again | 0x92B9CD / 0x7A9D0D / 0x73DE6D (solver 0x833C30 / 0x6AEB80 / 0x6467C0) | call-site rel32 redirect, 5 bytes `E8 rel32` -> thunk +32 (absjmp **pf_solve itself**, no asm stub: same 6-arg Win64 signature) | `E8 <rel32 solver>`; P_BSITE (36 bytes ending in `cmp eax,3`, rel32 wildcarded) + solver prologue P_SOLV (56 bytes) | pf_solve (L1379-1496) | every picker try that passed the range check (<= min(n,5) per pick): LIVE ~100-160/s; installed when HookAimCheck OR HookAimPriority | substitutes a 0x60-byte stack buffer for outRec (the solver writes the hit record there); reads hit record, weapon, firer, aim (+0x18 target, +0x60 name); calls the armour chain on its own structs | the solver's status, or **5** instead of 3 when the hit cannot pierce (picker erases that candidate, tries again: one more engine-RNG roll) | solver called WITHOUT the lock; g_lock exclusive for the pierce check / caches; t_pick (TLS) counts tries / blocks |
| P | picker try-loop head: `mov rdi,[rip+end]` of the function-static candidate vector, then `n = min(size,5)` tries | 0x92B8BA / 0x7A9BFA / 0x73DD5A (vector begin 0x15B3630 / 0x140ED60 / 0x11FAB20, end +8, cap +0x10) | inline replace, 7 bytes = `E9 rel32` -> thunk +48 (absjmp pf_stub_p) + `90 90` | `48 8B 3D <rip end>` (+ `B8 05 00 00 00` next, checked by patch_test); P_PICKP (40 bytes) found only inside [bsite-0x200, bsite), vend == vbeg+8, P_PUSH uses the same vector (L578-589) | pf_stub_p (L1627-1645) -> pf_pick (L1313) | every picker call that reaches the tries (all paths except the static-init guard exit): LIVE ~100-160/s | **writes engine memory: compacts the static candidate array in place and writes its end pointer** (`*g_ve = b + kept`, L1355); reads weapon (+0x90, +0xB0), loaded ammo, target volume list | ebp = new previous-volume index (or -1), rdi = [[g_ve]] (the possibly cut end), `jmp [g_p_back]` (= site+7) | g_lock exclusive after the gates; t_pick opened (TLS) before the gates; DIS: at the head r14 = aim, r13 = weapon, r15 = target, r12d = noAimCheck, ebp = previous index; the next instructions write eax / rdi / rbx / rcx / rdx / r8 before reading them and use no xmm before the next call, so rax, rcx, rdx, r8-r11, xmm0-5, flags are dead (confirms the L1625-1626 comment) |
| X | picker exit: `mov rcx,r14; call Clear(aim)` reached when the tries ran out, no candidates, no weapon, no weapon stuff, or out of range (not on noAimCheck, not on success) | 0x92BA09 / 0x7A9D49 / 0x73DEA9 (Clear 0x853240 / 0x6CE920 / 0x665300) | call-site rel32 redirect, 5 bytes `E8 rel32` -> thunk +64 (absjmp pf_stub_x) | `49 8B CE E8 <rel32 Clear>` preceded by `83 EF 01 0F 89` (L591-599); Clear prologue P_CLR (23 bytes) | pf_stub_x (L1648-1658) -> pf_exhaust(rcx aim, r13 weapon, r15 target) (L1503) | every exit through that clear: LIVE ~8-16/s (62 in the first 7.8 s, +152 in the next 9.5 s) | **calls engine Clear first (always)**; may rewrite a hold; may call the engine weapon stop `e_wstop(weapon,1)` (0x81F480 / 0x699CC0 / 0x631D00), which clears both aim slots, releases the firer, resets burst state (DIS of 0x81F480) | void; rax = garbage. DIS: the 4 callers of the picker (game 0x1AA436, 0x8EDC80, 0x903BA8, 0x92B631) never read rax after the call, so not forwarding Clear's return is safe | Clear and weapon stop called WITHOUT the lock; g_lock exclusive for the re-verdict; t_pick closed |

Other engine-visible effects: two RWX pages per process (near thunk page, 0x1000, within +-0x70000000 of the site; builder trampoline,
4096 bytes anywhere), never freed. No vtable slot, no data table, no import is patched.

Engine functions pen CALLS (all through pointers bound at L1794-1801 when core):
| pointer | game / editor / server | args as called | guards before the call | under g_lock? |
|---|---|---|---|---|
| e_loaded (loaded ammo of an ammo slot) | 0x820B50 / 0x69B440 / 0x6333D0 | (weapon+0xB0 slot) | slot != NULL (L1325, L1409, L1531) | pf_pick / pf_solve: no; pf_exhaust: yes |
| e_P (params of the loaded ammo) | 0x81EC40 / 0x699450 / 0x6314C0 | (weapon) | weapon != NULL, filtered ammo, vehicle target | yes |
| e_wcast (weapon-stuff cast of the item stuff) | 0x83F1E0 / 0x6BA450 / 0x651DF0 | (r14 of the enemy scan) | ctx matches, return address is a scan return, r14 != NULL | yes |
| e_plist ({parameters} lookup) | 0x83DE90 / 0x6B90B0 / 0x650AA0 | (list, ammo+ammotags) | list != NULL (memo m_par) | yes |
| e_rtdc (__RTDynamicCast) | 0xF3A598 / 0xD28858 / 0xC7CAB8 | (obj, 0, src TD, dst TD, 0) | obj != NULL | yes |
| e_pen (projectileDamageTable) | 0x841650 / 0x6BC910 / 0x653F90 | (params, distance float bits) | params != NULL | yes |
| e_dvget (typed value get) | (a8be80) not printed | ({0, pen}, 6) | none needed (stack value) | yes |
| e_armorof (entity armour) | (armorof) not printed | (target) | target is a vehicle actor | yes |
| e_mult (actor multiplier) | (a79530) not printed | (0xB, target, &mult) | as above | yes |
| e_matof (material of armour + record) | 0x7FDDF0 / 0x677650 / 0x611520 | (armour, rec{vol}) | armour != NULL (memo m_mat) | yes |
| e_angle | 0x7FC090 / 0x6759C0 / 0x60F7C0 | (bs, params, cos) | params != NULL | yes |
| e_block (armour block builder, eGameEntityArmor) | 0x7FC310 / 0x675C40 / 0x60FA40 | (blk, target, shooter, rec, bs) | pen >= 1e-5, filtered, enemy (e_rel) | yes |
| e_eval (armour evaluator) | 0x7FC3A0 / 0x675CD0 / 0x60FAD0 | (blk) | b.mat != NULL and not computed-thickness | yes |
| e_thick / e_eff / e_thr | 0x7FC5B0 / 0x7FCA60 / 0x7FCDE0 (ed 0x675EE0 / 0x6762D0 / 0x676650; srv 0x60FCE0 / 0x610190 / 0x610510) | (blk, 0) / (blk, thickness) / (blk) | material min calibre and flag bit 0 pre-checked (L908) | yes |
| e_rel (relation / enemy) | 0xA654B0 / 0x8E5650 / 0x8766C0 | (shooter, target) | both non-NULL | yes |
| e_getvol (volume of an aim record) | 0x8527C0 / 0x6CDE20 / 0x664880 | (aim record or weapon aim slot) | e_getvol != NULL | yes |
| e_solve (aim solver) | 0x833C30 / 0x6AEB80 / 0x6467C0 | the engine's own 6 args | none (pass-through) | **no** |
| e_clear (aim Clear) | 0x853240 / 0x6CE920 / 0x665300 | (aim) | none (replaced call) | **no** |
| e_wstop (weapon cease-fire, IsManual skip, ExhaustStop) | 0x81F480 / 0x699CC0 / 0x631D00 | (weapon, 1) | stop decided, weapon non-NULL, weapon stuff present | **no** (after unlock, L1562) |
IsManualControlled (0x804650 / 0x67E5C0 / 0x617E70) is found but **never called**: is_manual (L739-746) reads its two halves itself.
No engine call is wrapped in SEH; guards are NULL checks only (no VirtualQuery / ReadProcessMemory probes anywhere in the module except
VirtualQuery in alloc_near at install).

## 3. Finders and overlap with the patches of the three dlls
Pen's finder spans on the editor exe (DIS; the game / server spans are at the same offsets inside the same functions):
| finder | how located | span read (editor) | covers a patch of ... |
|---|---|---|---|
| MatchAmmo sites (P_MA) | unique 130-byte pattern from the A2 site to the end of the volume loop | 0x7AAFC3..0x7AB045 | **pen's own A2 (8) and A1 (5)** -> a second pen install fails "not found (unknown build or already patched)" |
| ammo tag offset, P_PLAYER, "same tag test" | scans of [A2-0x200, A2) | 0x7AADC3..0x7AAFC3 | nothing. MatchAmmo starts at 0x7AADA0 = A2-0x223, so stance's 14-byte prologue patch (0x7AADA0..0x7AADAE, stance src L36) is 0x15 bytes before the window. LIVE proves it: pen installed after stance 0.7.41 had patched the prologue |
| ctx builder (P_PAIR + P_BLD) | every `call X; mov rcx,rax; call MatchAmmo` pair in .text; MatchAmmo = .pdata start within 0x400 before A2; P_BLD at X | pairs anywhere; builder 0x7AACC0..0x7AACE5 | **pen's own W (15)** -> second install fails "prologue differs (... already hooked)". Call sites to MatchAmmo / builder are patched by nobody |
| enemy scan returns (`xor edx,edx` before 2 of the pairs, r14 checks, P_SCANW, P_WCAST, P_RANGE) | inside the enemy scan (ed 0x7EF410, the function stance reads) and the cast / range thunks | scan region near 0x7EF741 / 0x7EF8AC; cast 0x6BA450; range() | nothing (stance only reads the scan) |
| armour chain (P_C310, P_C3A0) | unique whole-function patterns | 0x675C40..0x675CC7, 0x675CD0..0x675E2E | nothing |
| AimInfo chain (call sequence before every `call c3a0`) | E8 targets in the 0x700 bytes before the overlay's `call c3a0` (ed 0x6A5420); P_P, P_PEN, P_C090, P_FABS on the targets; dynamic_cast TDs from `lea r8/r9` | 0x6A4D20..0x6A5425 + the callee prologues | nothing |
| B site (P_BSITE) + solver (P_SOLV) | unique pattern, rel32 of the call wildcarded | 0x7A9CF1..0x7A9D15; solver 0x6AEB80.. | **pen's own B**: the opcode `E8` is kept and the rel32 is wildcarded, so P_BSITE still matches a patched site; the second install then fails at P_SOLV (the redirected target is not the solver) -> err_b |
| P site (P_PICKP), P_PUSH, P_GETV / P_GETVB | inside [bsite-0x200, bsite) / [ps-0x100, ps) / [ps-0x80, ps) | 0x7A9BFA..0x7A9C22; push 0x7A9B6C; getv 0x7A9BA6; AimVolume 0x6CDE20 | **pen's own P (7)** -> "not found (... already patched)" |
| X (bytes at the js target T) + P_CLR | T-9..T+8 checks; Clear prologue | 0x7A9D3D..0x7A9D4E; Clear 0x6CE920 | **pen's own X**: `T[3] == E8` still true after the patch, but rel32 then points at the thunk and P_CLR fails -> err_x |
| weapon stop (P_FIRE0, P_WSLOT) | unique `cmp [r+8],0; jne; mov edx,1; mov rcx,rbx; call Stop` | 0x699954..0x699967 inside the function 0x6998E0; stop 0x699CC0..0x699DE0 | nothing. NOTE: the function holding P_FIRE0 (ed 0x6998E0, game 0x81F0C0, server 0x631940) is the one MeleeChase calls **Weapon::SetTarget** and finds with the same instruction sequence; chase patches the 4 CALL SITES of it inside the target evaluation (ed 0x828855, 0x828939, 0x8289D2, 0x828B8D), not its body. Pen's "weapon stop" = chase's "clear" = ed 0x699CC0 |
| sim clock (P_CLK) | unique; getter `mov eax,[rip]; ret` | 0x7A991A..0x7A992C (picker wrapper 0x7A990F); getter | nothing; same global as stance / chase (ed 0x13DF988) |
| IsManualControlled (P_MANUAL, P_PROPON) | unique 40-byte prologue, PropOn callee | 0x67E5C0..0x67E5E8 | nothing (stance calls the same function, chase reads none of it) |
| eBox / ePolyhedron vtables | RTTI type name in .data -> COL in .rdata -> vtable | .data / .rdata scans | nothing: chase's eMoveTask / eVS_Root slot swaps and stance's ExecuteInstant / frame-hook slot swaps write vtable SLOTS; pen compares only COL pointers (vtable[-1]) |

Checked byte ranges of the other two dlls' patches on the editor (DIS overlap check, `work\pen\pen_sites.txt` end of the editor block): stance
MatchAmmo prologue 0x7AADA0+14, chase approach call 0x82FE14+5, chase evaluation tail jmp 0x827927+5, chase SetTarget call sites
0x828855 / 0x828939 / 0x8289D2 / 0x828B8D (+5 each), stance ShieldMelee ray site 0x7F82ED+30 (off in the live ini): **no overlap with any pen
pattern span, scan window or pen patch.** Vtable-slot patches (chase eMoveTask slots 0 / 19, eVS_Root slot 3; stance ExecuteInstant x9,
frame hook) and stance's blow-reach table write (ed 0xF71368, 12 bytes) are data, never read by pen.
Reverse direction (other finders reading pen's patch sites), from the stance / chase sources: stance's range-call finder scans MatchAmmo
from its start and stops at the FIRST SIG_RANGE (ed 0x7AAF..., before A2), stance's enemy-scan finder counts `call MatchAmmo` sites
(pen does not touch them); chase scans the eVS_Root tick / target evaluation / SetTarget, none of which pen patches.
**Install-order consequence:** pen's finders tolerate stance's and chase's patches (LIVE: pen loaded last and installed everything), and
pen's patches are invisible to their finders (no shared bytes). Keeping today's order chase -> stance -> pen is safe; pen first would
also work for the bytes checked here (stance's range finder ends before A2). Outside the three: in today's payload gem_noprone loads
BETWEEN meleestance and penfilter (alphabetical: ... meleechase, meleestance, noprone, penfilter ...); a gem_melee.dll sorted before
noprone moves pen's patches BEFORE noprone's install. A word grep of the other payload sources (`_dlls\*`) for MatchAmmo / picker /
IsManualControlled / Clear / Stop names found only meleestance; byte-level overlap with noprone and the other dlls was NOT checked here.

## 4. State: tables, caches, queues, time
All direct-mapped (one entry per slot, a colliding key overwrites), static arrays in .bss, no heap. "Sim" = read by simulation decisions.
| table | line | entries x bytes (approx) | key | value | lifetime / eviction | flushed | sim? |
|---|---|---|---|---|---|---|---|
| c_blk blocked volumes | L980-1002 | 8192 x 44 | (shooter id, target id, ammo+weapon key, volume index) | sim ms, gdist, unit dir to eye, frame kind | until CacheMs (60 s), shooter > HoldDistanceM nearer, dir turns > 25 deg (frame kinds both non-0), or overwritten by a colliding key | epoch_check | yes (A, P, X via vol_ok) |
| c_hold ammo verdicts | L1005-1023 | 2048 x 32 | (shooter id, target id, ammo+weapon key) | verdict, sim ms, gdist, range state | VerdictHoldMs (8 s); REJECT dropped nearer, ALLOW dropped farther (HoldDistanceM); range state change; overwritten by A (on a miss) or X | epoch_check | yes (A) |
| c_unr unreachable volumes | L1025-1040 | 2048 x 24 | (shooter id, target id, key, volume index) | sim ms | UnreachableMs (3 s) | epoch_check | yes (P) |
| m_ammo | L824-839 | 512 x 64 | ammo eStuff POINTER | filtered, name-tag hash, name | until a colliding pointer | epoch_check | yes (pure: tag content) |
| m_par | L840-851 | 1024 x 24 | (list ptr, ammo ptr) | params ptr | colliding key | epoch_check | yes (pure, inferred) |
| m_bs | L852-875 | 1024 x ~64 | (params ptr, calibre bits, cos bucket) | bullet state after e_angle | colliding key | epoch_check | yes (pure, inferred) |
| m_mat | L876-887 | 2048 x 32 | (armour ptr, volume info ptr) | material ptr | colliding key | epoch_check | yes (pure, inferred) |
| m_thr | L888-919 | 8192 x ~64 | (info, armour, mat, params, factor, mult, calibre, face, cosb, PierceRule) | pen6 threshold | colliding key | epoch_check | yes (pure, inferred) |
| g_vl change-only log | L790-806 | 4096 x 28 | (hook, shooter id, target id, key) | last verdict, repeats | colliding key | epoch_check (+ g_vlines = 0) | **no** (log only) |
| ST stats | L779-787 | 1 | - | counters | process | never (flush count kept) | no |
| t_rec (TLS) | L1060-1065 | per thread | - | last builder call (ctx, weapon, return address, r14) | overwritten by the next builder call on that thread | never | yes (decides the weapon / list A uses) |
| t_pick (TLS) | L1068-1072 | per thread | - | open pick (target, weapon, tries, maxtries, bblocks, cut, keys) | P opens, X closes (any exit through the clear); a pick that leaves on success / noAimCheck stays "open" until the next P overwrites it | never | yes (X decides on it) |
| t_prep (TLS) | L1185 | per thread | (ctx, ammo, shooter, target ptrs, sim ms, epoch, t_rec weapon / r14) | prep_t incl. verdict | next MatchAmmo with another key or another sim ms | epoch compare | yes (A1 / A2 of one MatchAmmo call share a verdict) |
| nm_group buffer (TLS) | L816 | 24 bytes | - | log text | - | - | no |
| near page, trampoline | L1759-1774, L1813 | 2 pages | - | thunks | process | never | - |
New battle (L1044-1057): the first pen hook call that sees `now < g_last_now` clears c_blk, c_hold, c_unr, every memo and g_vl (+ budget),
g_epoch++, logs `new battle [..]`. Caller holds g_lock. Not cleared: t_rec / t_pick (overwritten on use), ST. Pointer-keyed memos are
therefore never reused across battles. Within a battle a freed-and-reallocated engine object at the same address (an ammo stuff, params
entry, armour, volume info) would hit a stale memo (inferred rare: stuff / params / volume info are definition data that live for the
whole mission; armour is per entity: an entity deleted and another created at the same address with another armour object... the key is
the armour object pointer itself, so a stale hit needs the armour object reused with other contents; not seen).
Ids: `ent_id` = dword entity+0xC0; README says ids repeat every battle (hence the flush).

Determinism (MP): keys are entity ids, content hashes and volume indexes; values are sim ms, geometry and engine results; the memo
eviction pattern differs between machines (pointer keys) but a memo hit and a recomputation give the same value (assuming the engine
functions are pure). `t_prep` reuses a result for an identical (ctx pointer, ammo, shooter, target, weapon, r14, sim ms) inside one
sim ms; with every machine running the same code path the reuse pattern is the same (inferred).
Wall-clock use: only `Sleep(10000)` in stats_thread (L1912), log only. No GetTickCount / QueryPerformanceCounter / time() anywhere; no
RNG of its own. All decision time is `now_ms()` = the engine's sim clock dword (L676); 0 forever when the clock finder failed, but then
core = 0 and nothing is installed.
Engine RNG: pen draws none itself, but P (smaller candidate list = different `div` result) and B (status 5 = one more try = one more
draw from the engine RNG: game 0x563680 returns the RNG state, the picker steps it as an LCG `imul ecx,[rax+8],0x7FF8A3ED; add ecx,0x2AA01D31`) change which engine RNG values are consumed ->
every MP machine must run the same pen code (README Multiplayer).

## 5. Helpers a shared core could replace (exact semantics of pen's copies)
| helper | line | pen's semantics |
|---|---|---|
| log | L98-113 `logf_` | lazy `fopen(g_logpath,"a")` on first line; no-op while the path is empty; vfprintf + '\n' + fflush EVERY line; no timestamp, no module prefix, no lock of its own (CRT FILE lock only); called from the sim thread inside hooks (change-only lines, budget LogVerdicts per battle shared by A/B/P/X lines) and from stats_thread. Install lines are unconditional (logf_ ignores cfg_log; only hook lines check cfg_log via vl_change and epoch_check) |
| ini | L1669-1703 | `GetPrivateProfileIntA/StringA("PenFilter", key, default, <dll>.ini)` once in DllMain; clamps; strtok for ExemptTags |
| PE / pattern | L244-269, L344-402 | pat_t (512-byte pattern), pat_parse ("??" wildcard), pat_at, pat_find (first hit + count over [lo,hi)), rel32, sec_range by name, .pdata binary search is_func_start (exact BeginAddress match only, unlike stance's func_of which returns the containing function), find_leas, rtti_vtable (type name -> COL -> vtable, accepts live or ImageBase-relative COL addresses) |
| patch writer | L1743-1779 | write_code = VirtualProtect RWX + memcpy + restore + FlushInstructionCache; **no compare of the original bytes at write time, no thread suspension** (chase's writer suspends the other threads first: chase src\mc\patch.c L68-74); absjmp `FF 25 00000000 abs64`; alloc_near VirtualQuery/VirtualAlloc 0x1000 RWX at granularity steps within +-0x70000000; put_rel E8 / E9 rel32 |
| memory probes | - | none at run time; pointers are trusted after NULL checks; std::string reader bounds (size <= 0x1000, cap <= 0x10000); vector readers bound the element count (<= 256 / 1024 / 4096) |
| tag readers | L116-165, L203-216, L1158-1166 | by NAME STRING (eq_ci, ASCII case-insensitive, length exact) on 0x60-byte Tag vectors, never interned ids: ammo `{tag}` vector = ammo + ammotags(0xEC8) + 0x318; ammo / weapon-stuff name tags = +0x868 + 0x318 (hashed, FNV-1a lower-case); volume tags = [vol+0x30] + 0x398 / +0x3A0; def props = [entity+0x548] + 0x330 (log only). Pen reads NO entity tags (+0x4E8) |
| is vehicle / human | L730 | `t[0x30] & 2 && !(t[0x30] & 4)` (actor, not human); is_manual uses `e[0x30] & 2` (actor) |
| owner / AI-or-player | L731-735 | `[[e+0xDC8]+0x398] & 3 == 1` = owned by a human player (MatchAmmo's own enable_for_user test, verified by P_PLAYER); used only when PlayerUnits=0 |
| direct control | L738-746 | `dc_on && e && (e[0x30]&2) && ([[e+0xDE0]+0x18] != 0 \|\| bit1 of [[e+0xDD8]+0x70])`, each pointer NULL-checked separately (a unit with DE0 set and DD8 NULL counts as manual). Re-implemented, the engine function is never called. **Differs from stance**: stance (src L437-441) returns 0 when either +0xDE0 or +0xDD8 is NULL, then calls the engine IsManualControlled; stance 0.7.12 also treats the unit the local manual-control MANAGER drives (`[mgr]+0x3B0 == unit`, src L444-450) as manual, which is the local player's UI state, not sim state. Pen's verdicts feed the lockstep sim, so pen must not use a manager-based test |
| relation (enemy / friendly) | L1237, L1299, L1417 | engine `e_rel(shooter, target)` (0xA654B0) == 0 -> friendly -> skip; B also skips when the armour block says not an enemy (b.enemy) |
| linked | - | not tested by pen (stance's SkipLinked has no pen equivalent) |
| entity id | L747 | dword +0xC0 |
| position / distance | L748-753, L1238-1240 | position = floats +0x74; A's pen distance = shooter +0x74 to target-info +0x28 (the same point MatchAmmo's range test uses, DIS 0x92CB4A-0x92CB63); P / X distance = origin to origin; B distance = shooter origin to the solver's hit point; eye = shooter origin + 30 units z |
| sim clock | L627-635, L675-676 | dword global found through the picker wrapper's `call Clock` (P_CLK), read directly `*g_clock` (no call) |
| weak refs | - | none: entities are compared by pointer within one call and by id in caches; no lifetime tracking |
| loaded ammo / weapon parts | L1294-1297, L1323-1325 | shooter = [weapon+0x90], weapon stuff = [weapon+0x98], firer = [weapon+0xA8], ammo slot = [weapon+0xB0], aim slots +0xC8 / +0x168 (found), {parameters} list = [weapon+0xA0] (found) |
| cease-fire | L1562 | engine weapon stop (weapon, 1) = chase's clearTarget (same address ed 0x699CC0) |
| new battle | L1044-1057 | sim clock went back, detected lazily by whichever pen hook runs first |

## 6. Outside interface
- **Exports: none** (no `__declspec(dllexport)`, no .def). **Imports from other dlls: none** (kernel32 + CRT only: GetModuleFileNameA,
  GetModuleHandleW, GetPrivateProfile*, VirtualProtect, VirtualAlloc, VirtualQuery, GetSystemInfo, FlushInstructionCache,
  GetCurrentProcess, CreateThread, Sleep, SRW lock, Interlocked*; fopen / vfprintf / snprintf / strtok / _strdup (test)). Nothing in
  stance, chase or teleport references pen (grep of their sources) and pen references none of them.
- ini `[PenFilter]` (file `gem_penfilter.ini` next to the dll). Snapshot ini == live payload ini == out\ ini (diff empty), so no column
  for differences:
  | key | code default (L1672-1701) | snapshot / live ini | notes |
  |---|---|---|---|
  | Enabled | 1 | 1 | 0 = nothing runs, one log line |
  | Log | 1 | 1 | gates verdict lines, new-battle line, stats thread; install lines always written |
  | LogVerdicts | 400 | 400 | change-only lines per battle (A, B, P, X, X-direct-control share it) |
  | PierceTag | pierce | pierce | one tag |
  | ExemptTags | melee_light,melee_heavy,melee | same | split on ',' and ' ', max 16, each < 64 chars |
  | HookRowFilter | 1 | 1 | A + W |
  | HookAimCheck | 1 | 1 | B (also installed for P when 0) |
  | HookAimPriority | 1 | 1 | P cut (P itself also installed for X) |
  | HookExhaust | 1 | 1 | X (needs P) |
  | ExhaustStop | 1 | 1 | X weapon stop |
  | HookApproach | 0 | 0 | ignored, warning when 1 |
  | AimPriority | DEF_PRIO (L87-92) | same string | ';' groups, ',' alternatives, max 8 groups / 64 tags, buffer 2048 |
  | VerdictHoldMs | 8000 | 8000 | < 0 -> 0 |
  | HoldDistanceM | 15 | 15 | x20 units; <= 0 = no distance gate |
  | CacheMs | 60000 | 60000 | < 0 -> 0 |
  | UnreachableMs | 3000 | 3000 | < 0 -> 0 |
  | PierceRule | 1 | 1 | 3 = full penetration, anything else -> 1 |
  | PlayerUnits | 1 | 1 | 0 = skip human-player-owned shooters |
  | SkipDirectControl | 1 | 1 | needs the IsManualControlled finder |
  | Facing | 1 | 1 | needs both TraceVolume vtables |
  | BlockStreak | -1 (absent) | absent | any value -> warning line only |
- Log: `gem_penfilter.log` / `gem_penfilter_editor.log` / `gem_penfilter_server.log` next to the dll (L1947-1948). Lines other people /
  notes rely on: README test step 3 greps `row filter (hook A) installed`, `aim check (hook B) installed`, `aim priority (hook P) installed`,
  `exhausted-pick hook (X) installed`; the `[GEM PenFilter 0.1.1] finders:` block (9 lines, identical in format to selftest.txt lines 3-10,
  used for T3 parity); `A [`, `P [`, `B [`, `X [`, `new battle [`, `stats [` line formats (README "Log" list; dll_hooks.md L173-178 and the
  live-test notes quote them). `[GEM PenFilter %s] active | ...` summary.
- Game-side words read: ammo stuff `{tag}`: PierceTag (`pierce`), ExemptTags (`melee_light`, `melee_heavy`, `melee`); volume TAGS: `no_target`
  and every AimPriority tag (`body`, `cabin`, `bodyback`, `turret`, `mantlet`, `aux_turret1-9`, `arm_left`, `arm_right`, `gun_left`, `gun_right`,
  `hkm_left`, `hkm_right`, `leg_left`, `leg_right`, `frontleftleg` .. `backrightleg`, `mover`, `track`, `trackleft`, `trackright`, `wheel`); ammo and
  weapon-stuff NAME tags (hashed for keys, first two printed); entity def props (first three, log only). Entity tags / props / events
  written: **none**. ammo.set rows are not read as text; the row flag bit 2 (`{volume}`) and the row volume tag (+0x330) are the engine's.

## 7. Link-level facts
- Non-static symbols (DLL build): `g_tagtest` (L693), `pf_record` (L1062), `pf_vol` (L1257), `pf_novol` (L1273), `g_vb`, `g_ve` (L1311),
  `pf_pick` (L1313), `pf_solve` (L1379), `pf_exhaust` (L1503), `g_a2_back`, `g_a2_match`, `g_a2_next`, `g_w_tramp`, `g_p_back` (L1566), asm
  `.globl` labels `pf_stub_a1`, `pf_stub_a2`, `pf_stub_w`, `pf_stub_p`, `pf_stub_x` (L1571-1648), `DllMain` (L1933). All C ones carry
  `__attribute__((used))` because the asm references them by name. Test build adds `t_p_rdi` (L1974), asm labels `t_call_a2`, `ta2_back`,
  `ta2_match`, `ta2_next`, `t_call_a1`, `t_call_w`, `tw_ret`, `t_call_p`, `tp_back`, `t_call_x` (L1979-2012), `main` (L2437).
  Word grep of stance 0.7.42, chase 0.5.4 (all src\mc\*.c) and teleport 0.2.13: **only `DllMain` collides** (stance src L3283, chase
  main.c L122). `g_ve` / `g_vb` / `g_tagtest` / `g_p_back` / `g_w_tramp` / `g_a2_*` / `pf_*` occur in none of them. The `g_*` names are generic:
  a future core TU defining e.g. `g_vb` would collide; prefixing (`pen_`) or making them `static` (top-level asm in the same TU can
  reference file-static symbols; keep `__attribute__((used))`) removes the risk.
- Two top-level `__asm__` blocks (L1567-1660; test L1976-2019), `.intel_syntax noprefix` ... `.att_syntax prefix`, section `.text`, local
  numeric labels `1:` `2:` `3:` `9:`. RIP-relative loads of `g_tagtest`, `g_a2_*`, `g_w_tramp`, `g_ve`, `g_p_back`; direct `call pf_vol` /
  `pf_novol` / `pf_record` / `pf_pick` / `pf_exhaust` (no PLT; same image). The asm assumes those C symbols have exactly these names (no
  leading underscore on x86_64-windows-gnu, which zig cc uses).
- Thread-local storage: `__thread` t_rec, t_pick, t_prep and nm_group's buffer (L816, L1061, L1068, L1185). In a dll this uses the
  PE TLS directory (static TLS); fine for a dll loaded by LoadLibrary on Windows Vista+ (inferred: zig / mingw emits native TLS).
- Static names that would collide only in a unity build (not across TUs): `S`, `ST`, `logf_`, `g_log`, `g_logpath`, `g_clock`, `now_ms`,
  `rel32`, `sec_range`, `find_all`, `load_ini`, `is_vehicle`, `str_get`, `eq_ci`, `vec_has`, `absjmp`, `cfg_*` (stance has static `logf_`,
  `g_log`, `g_clock`, `now_ms`, `find_all`, `load_ini`, `is_vehicle`, `str_get`, `eq_ci`, `vec_has`, `absjmp`, `sites_t`, `MAXT`, `TLEN`; chase has
  `logf_`, `g_log`, `rel32`, `alloc_near`). Typedef / struct names with generic spellings: `sites_t`, `pat_t`, `stats_t`, `bstate_t`, `blk_t`,
  `rec_t`, `vl_t`, `brec_t`, `prep_t`, `vinfo_t`, `hold_t`, `unr_t`, `blk_cache_t`, `*_memo_t`, `fp_*`. A shared header must not define any of
  these names.
- Macros with generic names, never #undef'd: `WIN32_LEAN_AND_MEAN`, `PF_VERSION`, `MAXT`, `TLEN`, `MAXG`, `MAXGT`, `BLK_COS`, `MA_A1`,
  `MA_JNE`, `MA_NEXT_REL`, `BLD_COPY`, `BSITE_CALL`, `PICKP_LEN`, `PICKP_JS`, `V_NONE`, `NCOSB`, `INF_THR`, `PICK_MAX`, `OFF`, `TH_A1..TH_X`,
  `THUNK` (test). #undef'd: `IN_TEXT` (L653), `GI` (L1690), `D` (L1927).
- Headers: windows.h (WIN32_LEAN_AND_MEAN), stdint, stdio, stdlib, string, stdarg, math, limits. Libm: sqrtf, cosf / sinf (test), fabsf.
- Compiler / flags it assumes (build.bat L8-10, L13, L20): zig 0.14 `cc -target x86_64-windows-gnu -mcpu=baseline -O2 -ffp-contract=off
  -fno-fast-math`, dll `-shared -s`; test `-DPF_TEST`. Intel-syntax top-level asm needs clang / gcc. No MSVC build. Float results that feed
  sim decisions (pen6 vs thresholds, cos buckets, direction dots, distances) depend on these flags being identical on every machine
  (one binary).
- Build define `PF_TEST` switches DllMain + stats_thread OFF and the self-test ON in the same file (L1906 `#ifndef PF_TEST` ... L1955
  `#else` ... L2476 `#endif`).

## 8. Self-test
- How built / run: section L1956-L2476 of Part I. One exe per module today (`out\pf_test.exe`), args = exe paths.
- How the exes are mapped: `LoadLibraryExA(path, NULL, DONT_RESOLVE_DLL_REFERENCES)` (L2444): a real image mapping inside the test process
  (sections at their RVAs, relocated, imports not resolved, no code run). The same path loaded twice returns the SAME mapping (Windows
  module refcount). patch_test then patches that mapping in place and allocates near pages next to it; the mapping is never freed.
- What it fakes: every engine function pointer (fk_* L2055-2092), entities / weapons / aim records / volumes as calloc'd blocks with the
  offsets the code reads, the sim clock (`g_clock = &fk_clock`), the TraceVolume vtable (a fake `fvt` set into S.box_vt for one case).
  Finders and patch writes run on the real exes; the logic runs on fakes.
- Cases: ST lines 3-34 per exe (finder lines, "=> hook A OK ..." line, patch test line), then lines 35-62 (stubs, parser, key, P 0-4, P
  fallback, B, blocked cache, verdict hold, change-only log, X 0-7, A stubs + hold, A range state, A weapon key, direct control, new
  battle, compaction, tag reader / hash), then `ALL OK`.
- To make its main() callable from a combined test exe:
  1. Rename `main` (L2437) to e.g. `int pen_selftest(int argc, char **argv)` (or split: `pen_test_exe(const char *path)` for the per-exe
     part L2443-2458 and `pen_test_logic(void)` for L2459-2472) and stop printing `ALL OK` / `SOME FAILED` itself (return the bad flag; the
     combined driver prints the single `ALL OK`).
  2. Compile the test part in the module TU under a new define (`PEN_SELFTEST` instead of `PF_TEST`) that no longer removes the attach
     code (today PF_TEST removes DllMain only; with the shared DllMain gone, nothing else changes).
  3. Prefix or localise the test asm labels and `t_p_rdi` (generic `t_call_*`, `tw_ret`, `tp_back`, `ta2_*`) so the stance / chase test
     harnesses cannot collide.
  4. Logging: the per-exe part writes the finder lines by pointing `g_log` at stdout (L2448-2450) and patch_test silences the log by
     clearing `g_logpath` / `g_log` (L2411). With a shared sink, the core needs a "to stdout / off" switch the test can flip, or the test
     keeps calling its own log_sites with a local FILE.
  5. Patch-test order: because one process maps each exe once, a combined test that runs chase, stance and pen patch tests on the same
     mapping must run them in install order (chase, stance, pen) and accept each other's bytes (pen's finders were shown to tolerate
     stance's prologue patch only live, not in pf_test); or FreeLibrary the mapping between modules so each gets a fresh image.
  6. logic_test (L2109) saves / restores `S` and several globals but leaves caches and ST dirty; run it after patch_test (as today) and
     before anything else that reads pen's state; it sets `hookA_on` etc. back to 0 at the end.

## 9. Port list (smallest edit set; snapshot line numbers)
Goal: pen = its own TU (`src\pen\pen.c` = gem_penfilter.c), attach function called by the shared DllMain, shared log sink, shared ini file,
a few exported-inside-the-dll functions. Everything else (finders, stubs, handlers, caches) stays byte-for-byte.
1. **DllMain out** (L1933-1954): delete. Its work becomes `int pen_attach(const char *ini, uint8_t *exe_base)` = `load_ini(ini);
   install_at(exe_base); return hookA_on || hookB_on || hookP_on;` (log path / exe-name suffix now owned by the core).
2. **stats thread** (L1907-1931): turn the loop body into `void pen_stats(void)` (keep the `last` snapshot as a static) and let the core's
   single stats thread call it every 10 s; or keep the thread as is (it only logs). Remove the CreateThread (L1951).
3. **Log sink** (L98-113): keep the name `logf_` (90+ call sites) but make its body forward to the core (`core_vlog(CORE_PEN, fmt, ap)`,
   prefix `[pen]` + sim ms). Delete `g_logpath`; keep `g_log` only if the self-test still needs a stdout switch (L2283, L2291, L2411,
   L2448-2450). Keep the line texts so log greps and T3 parity still work; the banner `[GEM PenFilter %s]` (L1788, L1790, L1899) can stay.
4. **ini** (L1669-1703): unchanged; pass the shared ini path. Section stays `[PenFilter]`, keys and defaults unchanged (section 6 table).
5. **Sim clock** (L627-635, L675-676, L1792, L1794): bind `g_clock` to the core's clock pointer; keep pen's P_CLK finder only as a parity
   cross-check (core gate: if pen's finder disagrees with the core, log and stay off). `core` gate keeps requiring a clock.
6. **New battle** (L1044-1057 + 5 call sites L1246, L1329, L1393, L1413, L1518): keep the call sites; change the body to compare against
   the core's epoch counter (`if (core_epoch() != g_epoch) { flush; g_epoch = core_epoch(); }`) so the three modules flush at the same
   sim point. The memset list stays. Must stay under g_lock (all callers hold it).
7. **Direct control** (L637-647, L738-746, L1891-1893): either keep pen's own test (recommended for parity) or use a core helper whose
   semantics are EXACTLY pen's (section 5: no manager test, DE0 half alone suffices). If the core adopts stance's 0.7.32 guard (both
   components non-NULL), document the change: a unit with DE0 set and DD8 NULL stops counting as manual for pen.
8. **Patch writes** (L1743-1779, L1805-1890): route the six writes through the core patch registry with their expected bytes:
   W builder 15 bytes `48 89 5C 24 08 57 48 83 EC 20 80 7C 24 58 00`; A1 5 bytes `E8 <TagTest rel32>`; A2 8 bytes `A8 02 0F 84 <rel32>`;
   B 5 bytes `E8 <solver rel32>`; P 7 bytes `48 8B 3D <rel32>`; X 5 bytes `E8 <Clear rel32>` (rel32 values = what the finder read). Keep
   the order A (builder, A1, A2), B, P, X and the per-hook "earlier write stays" behaviour, or make the registry roll a hook back as a unit
   (a behaviour change, harmless). The near page (TH_* slots L1780-1784) and the trampoline can stay module-local.
9. **Symbols**: make the 14 non-static C symbols (5 functions, 9 globals) (L693, L1062, L1257, L1273, L1311, L1313, L1379, L1503, L1566) `static` +
   `__attribute__((used))` or prefix them `pen_`; prefix the asm labels only if another module ever uses `pf_stub_*` (none does).
10. **Self-test** (L1955-2476): section 8 steps 1-6.
11. **Guard** (plan design 7): before install_at, `GetModuleHandleA("gem_penfilter.dll")` -> stay off and log. (Pen's finders already fail
    cleanly on bytes an old copy patched, section 3, but a module that loads first would win silently.)
12. **New inside-dll API** for the other modules (small wrappers, all take g_lock and do the same epoch_check as the hooks):
    - `int pen_ammo_filtered(const void *ammo)` = `ammo_info(ammo)->filtered` (L826-839): "is this pierce ammo pen judges" (one shared
      answer for R1 / R2: melee ammo is exempt by ExemptTags).
    - `int pen_held_verdict(shooter, target, weapon, ammo, uint32_t *since)` = hold_get on wa_key(ammo key, weapon stuff) (L1011-1018):
      -1 none / 0 REJECT / 1 ALLOW, for chase (R3 "chase's hold never clears a weapon pen is holding on a pierceable volume").
    - `int pen_aim_pierceable(weapon, target)` = X's slot logic (L1542-1552) without the stop: 1 when an aim slot of the weapon holds the
      target on a volume vol_ok accepts, 0 when it cannot pierce, -1 unknown / not filtered. Lets chase / the core cease-fire helper ask
      before clearing a weapon.
    - `void pen_flush(void)` (the memset block of L1049-1053) for the core's new-battle point; `void pen_stats(void)`.
    - Later (flanking, not 0.1.0): `pen_body_pierce` below.
    Functions pen could take from the others / the core: the sim clock and epoch, the patch registry, the cease-fire helper (engine
    weapon stop, chase's clearTarget = the same function), the log sink, a trace switch for units tagged `ai_trace` (R5: pen has no
    per-unit trace today, only change-only lines).

### pf_body_pierce for a future flanking module (flanking plan L58-62)
Statement: `int pen_body_pierce(shooter, target, const float eye[3], float dist, int flags, float *dmax)` = "can the LOADED ammo of this
shooter's weapon pierce a body (AimPriority group 0) or turret (group 1) face of this target from eye position E, with the penetration
the weapon has at distance D; and the farthest distance at which it still does". Existing code it wraps:
- weapon / ammo / params: `prep_weapon` (L1286-1301) gates (vehicle target, filtered ammo, PlayerUnits, direct control, params, calibre
  cast, e_rel) and `prep_fill` (L1086-1107) with `pendist = D`. Two changes needed: prep_fill hard-codes the eye as shooter origin + 30
  units (L1101-1102: must take E; the flanking plan wants target z + 1.5 m), and gdist / the hold keys are irrelevant (no caches).
- volume set: the target's volume list walk of `any_volume_ok` (L1168-1182) with `vol_candidate` (L1159-1166) and a `vol_group(v) <= 1`
  filter (L203-216; groups come from AimPriority, so "body / turret" = whatever the ini's first two groups are).
- face test: `vol_ok` (L1130-1156) WITHOUT its first step, the blocked-cache lookup `blk_hit` (L1133) ("no per-position caches"); `box_faces`
  (L1112-1127) already takes the eye; `face_threshold` (L892-919) and its memo m_thr are position-free (keys hold no eye / distance), so
  they are safe to share.
- dmax: vol_ok returns at the FIRST passing face, not the lowest threshold; for dmax keep the minimum threshold over the faces that face E
  (the `best` it already tracks, L1139-1145) and sample `pen6(d) = e_dvget({0, e_pen(params, d)}, 6)` (L1094-1096) over distances
  (projectileDamageTable need not be monotone; the plan samples 8 distances).
- Must take g_lock (shared memos) and must not be called from inside an engine function pen itself calls under the lock (SRW is not
  recursive). Pure function of sim state -> MP-safe if called from the sim thread.

## 10. Risks and questions
Risks (merge-relevant first):
1. **Local calls writing sim state (MP, inferred, unverified).** A writes c_hold on every MatchAmmo miss (hold_put L1193), B writes
   c_blk / c_unr, X writes c_hold and stops weapons. If any of the 8 builder -> MatchAmmo pairs or the 4 picker callers (game 0x1AA436,
   0x8EDC80, 0x903BA8 = eAttack, 0x92B631 = the clock wrapper) runs only on the local machine (UI preview, cursor, sight overlay), pen's
   caches diverge between MP machines and the next sim decision differs (desync). MeleeStance's header says the UI also calls MatchAmmo
   (stance src L19-20). The README's MP section assumes sim-only callers.
2. **Volume world matrix** (vol+0x58+0x30, L930) feeds facing and the blocked-volume direction gate; if the engine refreshes it lazily or
   from rendering, machines can disagree (README risk; MP impact inferred).
3. **Direct-control helper unification**: pen's test (DE0 half alone counts, no manager) vs stance's (both components required, plus the
   local manager actor). Using stance's manager test inside pen's sim decisions would make them depend on the local player's UI.
4. **SRW lock not recursive**: g_lock is held across ~15 engine calls (section 2 table). Any new cross-module call into pen made from
   inside a pen hook (or from engine code pen calls under the lock) deadlocks. e_solve, e_clear, e_wstop are deliberately outside it.
5. **Weapon stop from inside the picker** (X, L1562): never exercised live (LIVE stats "weapons stopped 0" for the whole session);
   eAttack keeps its own aim record and may re-fire (README). The same engine function is chase's hold "clear" (ed 0x699CC0): both may
   stop one weapon in one tick; R3 wants one helper.
6. **Patch writes are plain memcpy with no thread suspension and no re-check of the bytes** (L1743-1750); safe only because they run in
   DllMain at injection. The merged core must keep pen's install at that time (not on a later thread) or use chase's suspending writer.
7. **W records one builder call per thread** (t_rec): MatchAmmo with a ctx built earlier than the last builder call on that thread is
   unfiltered (a_noweap). LIVE shows 0 such calls with stance 0.7.41's prologue hook in between, i.e. stance passes the ctx unchanged; a
   merged stance that copied / rebuilt the ctx would silently turn A off.
8. **HookTiming attribution**: stance measures "engine MatchAmmo" time around its call to the original; pen's A1 / A2 work runs inside it
   and is billed to the engine (R8 wants it split).
9. **Engine static candidate vector** (P): function-static, shared by threads, read / written by the stub and pf_pick without the lock
   (as the engine itself does). Anything else that reads that vector after P sees the cut list.
10. **Pointer-keyed memos** within one battle (section 4); flushed at a new battle only.
11. **New-battle detection** is lazy and per module today; a test stopped in its first seconds is missed (README). A core epoch fixes it.
12. Save / load (single player): a loaded clock later than the last seen one does not flush; entries keyed by ids can apply to other
    entities until they age out (8 s / 3 s / 60 s).
13. B asks the solver for a hit record it would not otherwise build (outRec != NULL): extra work per try (inferred from the call shape).
14. Logging on the sim thread with fflush per line; bounded by LogVerdicts (400) per battle plus install / stats lines.
15. Struct layouts (hit record, armour block, bullet state, aim record, aim slots) are read from engine code, not observed live (README).
16. Taurox `turret_pod.vol` / `turret_rl.vol` swapped SIDE bytes (README; facing wrong on those two).
17. Install with partial failure leaves earlier writes of that hook live (builder detour without A1 / A2): harmless pass-through, but a
    registry that claims bytes must know about it.

Questions (could not settle from the files):
1. Which MatchAmmo pairs and picker callers are lockstep simulation and which are local (risk 1)? The 8 pair sites are not printed by
   pen; the picker callers on the game exe are 0x1AA436, 0x8EDC80, 0x903BA8, 0x92B631 (DIS).
2. Is volume+0x58+0x30 kept current by the simulation on every machine (risk 2)?
3. Which direct-control semantics should the shared helper have for pen (risk 3)?
4. gem_noprone loads between gem_meleestance and gem_penfilter today; with gem_melee sorted before noprone, pen installs before it. Does
   noprone (or any other payload dll) read or patch bytes in pen's spans (section 3 table)? Not checked here (map of the other dlls).
5. Should ExhaustStop default stay 1 in the merged build although the stop path has never run live?
6. Does any engine function pen calls under g_lock (block builder, evaluator, relation, params lookup, dynamic_cast) ever call back into
   MatchAmmo, the ctx builder or the picker on the same thread (would deadlock)? Not seen; not proven.
7. pen's `AimInfo` / armour finders run on the sight-overlay code; is that code identical in the server exe's role (it is found there,
   ST line 27, but the server has no UI)? Found = enough for pen; noted only because the server never runs the overlay.
