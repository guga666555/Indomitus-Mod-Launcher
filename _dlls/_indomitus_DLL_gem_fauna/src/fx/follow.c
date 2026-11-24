/* GEM Fauna - squad formation for fauna followers
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * Simulation change: every MP peer needs the DLL.
 *
 * Why: the squad update (every 0.3 s) writes each member's formation slot (world x,y) to brain+SLOT and raises the
 * follow request (brain flags bit 9, bit 10 = fresh). Infantry walk to their slot. Vehicle-class units - so fauna - run
 * the vehicle follow handler instead, which ignores the slot: it asks GetLeader(brain) for the leader, does nothing
 * while the unit is within R = scale x 15.0 (= 15 m) of the LEADER'S position, else drives toward the leader with stop
 * radius 0.9 R. So fauna followers stand still until the leader is 15 m away, then trail 13.5 m behind.
 *
 * How: the two GetLeader calls of that code (one in the handler itself, one in the vehicle root state that re-targets
 * while following) are redirected. For a fauna member of a squad with no real vehicle in it the answer is a thread-
 * local record whose position (+0x74 x, +0x78 y, +0x7C z) is the unit's own formation slot (brain+SLOT, own z); at the
 * handler site the stub also shrinks R (held in xmm6, which the handler saved at entry) to scale x fauna_formation_radius.
 * Everything else - move task, stop radius, "moved since last issue" check, following flag - is the engine's own code,
 * so fauna keep formation like infantry. Any other case gets the real leader and the untouched 15 m.
 */

typedef uint8_t *(*GetLeaderFn)(void *brain);
static GetLeaderFn g_getLeader;
static uint8_t *g_fwHandlerSite, *g_fwRootSite;        /* E8 of the two GetLeader calls */
static float *g_worldScale;                            /* engine global: world units per metre (20) */
static const float *g_fwRadius15;                      /* the handler's 15.0 */
static uint32_t g_brainSlotOff;                        /* brain+SLOT = formation slot {float x, y} */
static int g_okFormation, g_wantFormation = 1, g_formationRadiusCm = 150;

static int find_follow(void)
{
    if (!g_squadOff || !g_squadLeadOff || !g_brainOff) { logf_("formation: squad layout unknown (box-select finder first)"); return 0; }
    /* GetLeader(brain): mov rax,[rcx+SQUAD] ; test rax,rax ; je ; mov rax,[rax+LEADER] ; ret ; ret */
    uint8_t sig[21] = { 0x48,0x8B,0x81, 0,0,0,0, 0x48,0x85,0xC0,0x74,0x08, 0x48,0x8B,0x80, 0,0,0,0, 0xC3,0xC3 };
    memcpy(sig + 3, &g_squadOff, 4); memcpy(sig + 15, &g_squadLeadOff, 4);
    uint8_t *gl = find_bytes(g_text.p, g_text.n, sig, sizeof sig);
    if (!gl || find_bytes(gl + 1, g_text.p + g_text.n - gl - 1, sig, sizeof sig)) { logf_("formation: GetLeader not recognised"); return 0; }
    /* handler: movss xmm6,[rip+SCALE] ; mulss xmm6,[rip+15.0] ; (mov rcx,[rdi+30]) ; call GetLeader ; ... movsd xmm3,[rax+74] */
    int nh = 0, nr = 0; uint8_t *hs = NULL, *rs = NULL; float *scale = NULL; const float *k15 = NULL;
    for (uint8_t *q = g_text.p; q + 32 <= g_text.p + g_text.n; q++) {
        if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x10 && q[3] == 0x35 && q[8] == 0xF3 && q[9] == 0x0F && q[10] == 0x59 && q[11] == 0x35) {
            for (int k = 16; k < 24; k++)
                if (q[k] == 0xE8 && q + k + 5 + *(int32_t *)(q + k + 1) == gl) {
                    float *sc = (float *)(q + 8 + *(int32_t *)(q + 4)); const float *c = (const float *)(q + 16 + *(int32_t *)(q + 12));
                    if (in_data(sc) && in_rdata(c) && *c == 15.0f) { nh++; hs = q + k; scale = sc; k15 = c; }
                    break;
                }
        }
        /* root state: cmp byte [rsi+3B9],0 ; je rel32 ; mov rcx,[rsi+30] ; call GetLeader ; test rax,rax ; je */
        if (q[0] == 0x80 && q[1] == 0xBE && q[2] == 0xB9 && q[3] == 0x03 && q[4] == 0 && q[5] == 0 && q[6] == 0 && q[7] == 0x0F && q[8] == 0x84 &&
            q[13] == 0x48 && q[14] == 0x8B && q[15] == 0x4E && q[16] == 0x30 && q[17] == 0xE8 && q + 22 + *(int32_t *)(q + 18) == gl &&
            q[22] == 0x48 && q[23] == 0x85 && q[24] == 0xC0) { nr++; rs = q + 17; }
    }
    if (nh != 1 || nr != 1) { logf_("formation: GetLeader call sites not recognised (handler %d, root %d)", nh, nr); return 0; }
    /* formation slot: the squad formation writer does  mov rax,[rXX+BRAIN] ; ... ; movsd [rax+SLOT],xmm0 */
    int ns = 0; uint32_t slot = 0;
    for (uint8_t *q = g_text.p + 16; q + 8 <= g_text.p + g_text.n; q++) {
        if (q[0] != 0xF2 || q[1] != 0x0F || q[2] != 0x11 || q[3] != 0x80) continue;
        for (uint8_t *j = q - 16; j < q - 6; j++)
            if ((j[0] & 0xF8) == 0x48 && j[1] == 0x8B && (j[2] & 0xF8) == 0x80 && *(uint32_t *)(j + 3) == g_brainOff) { ns++; slot = *(uint32_t *)(q + 4); break; }
    }
    if (ns != 1 || slot < 0x100 || slot > 0x400) { logf_("formation: formation slot offset not recognised (%d matches)", ns); return 0; }
    g_getLeader = (GetLeaderFn)gl; g_fwHandlerSite = hs; g_fwRootSite = rs; g_worldScale = scale; g_fwRadius15 = k15; g_brainSlotOff = slot;
    logf_("formation: GetLeader exe+0x%llX, follow handler call @ exe+0x%llX (scale @ exe+0x%llX x 15.0 @ exe+0x%llX), root re-target call @ exe+0x%llX, slot brain+0x%X",
          EXE_OFF(gl), EXE_OFF(hs), EXE_OFF(scale), EXE_OFF(k15), EXE_OFF(rs), slot);
    return 1;
}

#ifndef FX_TEST
/* every vehicle-class member is fauna (humans welcome); 0 for a squad with a real vehicle or an empty/odd vector */
static int squad_no_real_vehicle(uint8_t *sq)
{
    uint8_t **b = *(uint8_t ***)(sq + g_squadMemOff), **en = *(uint8_t ***)(sq + g_squadMemOff + 8);
    if (!b || en < b || en - b > 64) return 0;
    for (uint8_t **q = b; q < en; q++) if (*q && ((*q)[ENT_FLAGS] & 8) && !is_fauna(*q)) return 0;
    return 1;
}

static _Thread_local __attribute__((aligned(16))) uint8_t t_slotRec[0x80];

/* GetLeader(brain) as seen by the vehicle follow code. rOut (handler site only): 0 = keep the engine's R, else the R to use */
static uint8_t *fx_follow_target(uint8_t *brain, float *rOut)
{
    if (rOut) *rOut = 0.0f;
    uint8_t *leader = g_getLeader(brain);
    if (!leader || !g_wantFormation || !brain) return leader;
    uint8_t *e = *(uint8_t **)(brain + g_brainEntOff);
    if (!e || e == leader || !is_fauna(e)) return leader;
    uint8_t *sq = *(uint8_t **)(brain + g_squadOff);
    if (!sq || !squad_no_real_vehicle(sq)) return leader;
    float sx = *(float *)(brain + g_brainSlotOff), sy = *(float *)(brain + g_brainSlotOff + 4);
    if (sx == 0.0f && sy == 0.0f) return leader;                  /* no slot assigned yet */
    *(float *)(t_slotRec + 0x74) = sx; *(float *)(t_slotRec + 0x78) = sy; *(float *)(t_slotRec + 0x7C) = *(float *)(e + 0x7C);
    if (rOut) *rOut = *g_worldScale * ((float)g_formationRadiusCm / 100.0f);
    static int n; if (g_verbose && n < 8) { n++; logf_("formation: fauna %p -> slot (%.1f, %.1f) instead of leader %p (%s)", e, sx / *g_worldScale, sy / *g_worldScale, leader, rOut ? "handler" : "re-target"); }
    return t_slotRec;
}
static uint8_t *fx_follow_target_plain(void *brain) { return fx_follow_target((uint8_t *)brain, NULL); }

/* handler site: the call must also set xmm6 (= R) when a slot is returned. Near stub, called straight from the E8:
     sub rsp,38 ; lea rdx,[rsp+30] ; call [rip+fn] ; movss xmm0,[rsp+30] ; xorps xmm1,xmm1 ; ucomiss xmm0,xmm1 ; je +3 ;
     movaps xmm6,xmm0 ; add rsp,38 ; ret ; ... abs64 fn @ +0x40
   rcx (brain) is untouched before the call; rax (the record) survives the tail. */
static int install_follow(void)
{
    uint8_t *pad = alloc_near(g_fwHandlerSite, 0x80);
    if (!pad) { logf_("formation: no memory within 2GB for the handler stub"); return 0; }
    static const uint8_t stub[] = { 0x48,0x83,0xEC,0x38, 0x48,0x8D,0x54,0x24,0x30, 0xFF,0x15,0x31,0x00,0x00,0x00,
                                    0xF3,0x0F,0x10,0x44,0x24,0x30, 0x0F,0x57,0xC9, 0x0F,0x2E,0xC1, 0x74,0x03, 0x0F,0x28,0xF0,
                                    0x48,0x83,0xC4,0x38, 0xC3 };
    memset(pad, 0xCC, 0x80);
    memcpy(pad, stub, sizeof stub);
    void *fn = (void *)fx_follow_target; memcpy(pad + 0x40, &fn, 8);          /* call [rip+0x31] from +0x0F -> +0x40 */
    FlushInstructionCache(GetCurrentProcess(), pad, 0x80);
    if (!patch_call_rel32(g_fwHandlerSite, pad, "formation (follow handler)")) { VirtualFree(pad, 0, MEM_RELEASE); return 0; }
    return redirect_call(g_fwRootSite, (void *)fx_follow_target_plain, NULL, "formation (root re-target)");
}
#endif /* FX_TEST */
