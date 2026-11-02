/* GEM ScriptExt - {multiplier X} in {health_damage_pierce|blast ...}
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 */

/* ------------------------------------------------------------------------------------ */
/* optional {multiplier X} in {health_damage_pierce ...} / {health_damage_blast ...}    */
/* ------------------------------------------------------------------------------------ */
/* Interaction::eHealthDamagePierce / eHealthDamageBlast (RTTI) are 0x60-byte objects: +0x28 effects vector<Tag>,
   +0x40 throw-off table vector<float2>, +0x58 float explosive_treshold, +0x5C padding. Both use the base class's
   common block parser as vtable slot 5 ({effects}, {table}, {explosive_treshold}; any other block word is logged as an
   error (0xBAD0001) and skipped) and the shared execute (slot 9):
       x = slot11(this, ctx)          explosive amount: compared with explosive_treshold and the throw-off table
       d = slot12(...) * slot10(this, ctx, flags) * k   damage -> SetHealth(vit, GetHealth - d), effects by d / max
   So scaling the slot 10 result (GetAmount) changes the damage the unit takes and the hit/die effect choice, and leaves
   explosive_treshold / throw-off untouched.
   {multiplier X}: the parser's "unknown block" branch calls the error logger with a 5-byte call; that call is redirected
   to a stub that looks at the block word (the parser's local std::string), and for "multiplier" inside a pierce/blast
   object reads the number and stores it in the padding word (magic | X * 1000). Any other word -> the engine's own log
   call, unchanged. Slot 5 of both classes writes the default (1.0) before parsing, slot 10 multiplies. Without the DLL
   the engine logs "unknown" for the block and its reader then reports the unread number as a script error, so scripts
   using it must only load through the launcher. Registers / frame slot used by the stub are read from the parser's own
   code (they differ between the game/server and the editor build); tests/mult_stub_test.c exercises the stub. */
typedef void  (*MultParseFn)(uint8_t *a, void *reader);
typedef float (*MultAmountFn)(uint8_t *a, void *ctx, void *out);
static void **g_vtPierce, **g_vtBlast;
static MultParseFn g_multCommon;                                   /* common block parser (slot 5 of both classes) */
static MultAmountFn g_multAmountOrigP, g_multAmountOrigB;
static RdPeekFn g_multPeek; static RdFloatFn g_multReadFloat;
static uint8_t *g_multSite;                                        /* the E8 call of the unknown-block error logger */
static uint32_t g_multPadOff, g_multThrOff;
static int g_multThisReg = -1, g_multReaderReg = -1, g_multFrameReg = -1; static int32_t g_multFrameDisp;
static int g_okMult;
#define MULT_MAGIC 0x4D550000u                                     /* 'MU' | multiplier x 1000 (0..65535) in the low word */
#define MULT_ONE   1000u

/* x64 register number of a 'mov rcx,REG' at p (48/49 8B C8+r or 48/4C 89 C1+r<<3), -1 otherwise */
static int mov_rcx_src(const uint8_t *p)
{
    if ((p[0] == 0x48 || p[0] == 0x49) && p[1] == 0x8B && (p[2] & 0xF8) == 0xC8) return (p[2] & 7) + (p[0] & 1) * 8;
    if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0xC1) return ((p[2] >> 3) & 7) + ((p[0] >> 2) & 1) * 8;
    return -1;
}
static int callee_saved(int r) { return r == 3 || r == 5 || r == 6 || r == 7 || (r >= 12 && r <= 15); }

static int find_mult(void)
{
    g_vtPierce = find_vtable_rtti(".?AUeHealthDamagePierce@Interaction@@");
    g_vtBlast  = find_vtable_rtti(".?AUeHealthDamageBlast@Interaction@@");
    if (!g_vtPierce || !g_vtBlast) { logf_("multiplier: damage classes not found"); return 0; }
    for (int i = 5; i <= 12; i++)
        if (!in_text(g_vtPierce[i]) || !in_text(g_vtBlast[i])) { logf_("multiplier: vtables not recognised"); return 0; }
    if (g_vtPierce[5] != g_vtBlast[5] || g_vtPierce[9] != g_vtBlast[9] || g_vtPierce[10] == g_vtBlast[10])
        { logf_("multiplier: pierce/blast no longer share the block parser / execute"); return 0; }
    uint8_t *cp = (uint8_t *)g_vtPierce[5];
    g_multCommon = (MultParseFn)cp;

    /* execute (slot 9) must call slot 10 (call [rax+0x50]) and compare explosive_treshold with the slot 11 value */
    { uint8_t *ex = (uint8_t *)g_vtPierce[9]; static const uint8_t c10[] = { 0xFF, 0x50, 0x50 }, c11[] = { 0xFF, 0x50, 0x58 };
      if (!find_bytes(ex, 0x600, c10, 3) || !find_bytes(ex, 0x200, c11, 3)) { logf_("multiplier: execute does not call GetAmount as expected"); return 0; } }

    /* Peek: mov edx,4 ; call  (first thing the block loop does) */
    for (uint8_t *q = cp; q < cp + 0x40 && !g_multPeek; q++)
        if (q[0] == 0xBA && *(uint32_t *)(q + 1) == 4) g_multPeek = (RdPeekFn)next_call(q + 5, 8, NULL);
    if (!g_multPeek) { logf_("multiplier: reader Peek not found"); return 0; }

    /* the {explosive_treshold} branch: lea rdx,"explosive_treshold" */
    uint8_t *s = find_cstr("explosive_treshold"), *L = NULL;
    for (uint8_t *q = cp; s && q < cp + 0x600 && !L; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15 && q + 7 + *(int32_t *)(q + 3) == s) L = q;
    if (!L) { logf_("multiplier: explosive_treshold branch not found"); return 0; }

    /* before it: lea rcx,[rsp|rbp+disp] (the block word, MSVC std::string) ... cmp len,0x12 */
    uint8_t *cmp = NULL;
    for (uint8_t *q = L - 4; q > L - 0x10 && !cmp; q--) if ((q[0] == 0x48 || q[0] == 0x49) && q[1] == 0x83 && (q[2] & 0xF8) == 0xF8 && q[3] == 0x12) cmp = q;
    for (uint8_t *q = cmp ? cmp - 4 : NULL; q && q > cmp - 0x18 && g_multFrameReg < 0; q--) {
        if (q[0] != 0x48 || q[1] != 0x8D) continue;
        if (q[2] == 0x4C && q[3] == 0x24) { g_multFrameReg = 4; g_multFrameDisp = (int8_t)q[4]; }               /* lea rcx,[rsp+d8]  */
        else if (q[2] == 0x8C && q[3] == 0x24) { g_multFrameReg = 4; g_multFrameDisp = *(int32_t *)(q + 4); }   /* lea rcx,[rsp+d32] */
        else if (q[2] == 0x4D) { g_multFrameReg = 5; g_multFrameDisp = (int8_t)q[3]; }                           /* lea rcx,[rbp+d8]  */
        else if (q[2] == 0x8D) { g_multFrameReg = 5; g_multFrameDisp = *(int32_t *)(q + 3); }                    /* lea rcx,[rbp+d32] */
    }
    if (!cmp || g_multFrameReg < 0) { logf_("multiplier: block word slot not recognised"); return 0; }

    /* after it: call memcmp ; test eax,eax ; jne ERR ; mov rcx,READER ; call ReadFloat ; movss [THIS+thr],xmm0 */
    uint8_t *q = L + 7, *err = NULL;
    if (q[0] != 0xE8 || q[5] != 0x85 || q[6] != 0xC0) { logf_("multiplier: keyword compare not recognised"); return 0; }
    q += 7;
    if (q[0] == 0x75) { err = q + 2 + (int8_t)q[1]; q += 2; }
    else if (q[0] == 0x0F && q[1] == 0x85) { err = q + 6 + *(int32_t *)(q + 2); q += 6; }
    else { logf_("multiplier: keyword branch not recognised"); return 0; }
    g_multReaderReg = mov_rcx_src(q);
    if (g_multReaderReg < 0 || q[3] != 0xE8) { logf_("multiplier: reader register not recognised"); return 0; }
    g_multReadFloat = (RdFloatFn)(q + 8 + *(int32_t *)(q + 4));
    q += 8;
    { int rex = (q[0] == 0xF3 && (q[1] & 0xF0) == 0x40) ? q[1] : 0;
      const uint8_t *m = rex ? q + 2 : q + 1;                     /* movss [THIS+disp8],xmm0 = F3 [REX] 0F 11 01 000 rrr d8 */
      if (q[0] != 0xF3 || m[0] != 0x0F || m[1] != 0x11 || (m[2] & 0xC0) != 0x40 || (m[2] & 0x38) != 0 || (m[2] & 7) == 4)
          { logf_("multiplier: explosive_treshold store not recognised"); return 0; }
      g_multThisReg = (m[2] & 7) + ((rex & 1) ? 8 : 0); g_multThrOff = m[3]; }
    if (!in_text((void *)g_multReadFloat) || !callee_saved(g_multThisReg) || !callee_saved(g_multReaderReg) || g_multThisReg == g_multReaderReg)
        { logf_("multiplier: parser registers not usable (this r%d, reader r%d)", g_multThisReg, g_multReaderReg); return 0; }

    /* ERR: mov r9d,0xBAD0001 (+ the other logger args) ; call LOG */
    static const uint8_t bad[] = { 0x41, 0xB9, 0x01, 0x00, 0xAD, 0x0B };   /* mov r9d,0x0BAD0001 */
    uint8_t *b = find_bytes(err, 0x20, bad, sizeof bad);
    if (!b) { logf_("multiplier: unknown-block error call not found"); return 0; }
    {   /* after mov r9d: only the other argument loads (mov edx,imm32 / lea rcx,[rip] / mov r8,[rip]) before the call */
        uint8_t *k = b + sizeof bad;
        for (int n = 0; n < 4 && k[0] != 0xE8; n++) {
            if (k[0] == 0xBA) k += 5;
            else if ((k[0] == 0x48 || k[0] == 0x4C) && (k[1] == 0x8D || k[1] == 0x8B) && (k[2] & 0xC7) == 0x05) k += 7;
            else break;
        }
        if (k[0] == 0xE8) g_multSite = k;
    }
    if (!g_multSite || g_multSite - err > 0x24 || !in_text(g_multSite + 5 + *(int32_t *)(g_multSite + 1)))
        { logf_("multiplier: unknown-block error call not found"); return 0; }

    /* object layout: the multiplier word lives in the padding after explosive_treshold; both ctors must leave it alone */
    g_multPadOff = g_multThrOff + 4;
    const char *names[2] = { "health_damage_pierce", "health_damage_blast" };
    for (int i = 0; i < 2; i++) {
        uint8_t *ctor = find_table_ctor(names[i]); uint32_t size = 0;
        for (uint8_t *k = ctor; k && k < ctor + 0x10 && !size; k++) if (k[0] == 0xB9) size = *(uint32_t *)(k + 1);
        if (!ctor || size < g_multPadOff + 4 || size > 0x400) { logf_("multiplier: %s object size %u leaves no room", names[i], size); return 0; }
        for (uint8_t *k = ctor; k < ctor + 0x60; k++)
            if ((k[0] == 0x89 || k[0] == 0xC7) && (k[1] & 0xC0) == 0x40 && k[2] == g_multPadOff) { logf_("multiplier: %s padding is used by the engine", names[i]); return 0; }
        uint8_t *lv = NULL;                                       /* the ctor stores this class's vtable */
        for (uint8_t *k = ctor; k < ctor + 0x60 && !lv; k++)
            if ((k[0] & 0xFB) == 0x48 && k[1] == 0x8D && (k[2] & 0xC7) == 0x05 && k + 7 + *(int32_t *)(k + 3) == (uint8_t *)(i ? g_vtBlast : g_vtPierce)) lv = k;
        if (!lv) { logf_("multiplier: %s ctor does not build the expected class", names[i]); return 0; }
    }
    logf_("multiplier: pierce vt exe+0x%llX blast vt exe+0x%llX, block parser exe+0x%llX (this r%d, reader r%d, word @ %s%+d), "
          "explosive_treshold +0x%X, multiplier word +0x%X, error call @ exe+0x%llX; Peek exe+0x%llX Float exe+0x%llX",
          EXE_OFF(g_vtPierce), EXE_OFF(g_vtBlast), EXE_OFF(cp), g_multThisReg, g_multReaderReg, g_multFrameReg == 4 ? "rsp" : "rbp",
          (int)g_multFrameDisp, g_multThrOff, g_multPadOff, EXE_OFF(g_multSite), EXE_OFF(g_multPeek), EXE_OFF(g_multReadFloat));
    return 1;
}

#ifndef SX_TEST
/* ---- runtime: {multiplier X} for health_damage_pierce / health_damage_blast ---- */
/* registers saved by mult_stub, lowest address first (= last pushed) */
typedef struct { uint64_t rcx, rdx, r8, r9, rbx, rbp, rsi, rdi, r12, r13, r14, r15; } SxRegs;
__attribute__((used)) void *sx_mult_log_orig;                  /* the engine's error logger (the redirected call's target) */
static volatile LONG g_multParsed, g_multApplied;

static uint64_t sx_reg(const SxRegs *r, int n)
{
    switch (n) {
    case 3: return r->rbx;  case 5: return r->rbp;  case 6: return r->rsi;  case 7: return r->rdi;
    case 12: return r->r12; case 13: return r->r13; case 14: return r->r14; case 15: return r->r15;
    }
    return 0;
}

/* called instead of the parser's "unknown block" error log. 1 = it was our {multiplier X} (consumed, no log),
   0 = anything else (the stub then runs the engine's log call with the original arguments) */
__attribute__((used)) int sx_mult_unknown_block(SxRegs *r)
{
    const uint8_t *frame = g_multFrameReg == 4 ? (const uint8_t *)r + sizeof *r + 8      /* the parser's rsp (before the call) */
                                               : (const uint8_t *)(uintptr_t)r->rbp;
    const MsString *w = (const MsString *)(frame + g_multFrameDisp);                  /* the block word just read */
    if (w->size != 10 || w->cap < 10) return 0;
    if (_strnicmp(w->cap > 15 ? w->u.ptr : w->u.buf, "multiplier", 10)) return 0;
    uint8_t *a = (uint8_t *)(uintptr_t)sx_reg(r, g_multThisReg);
    void *rd = (void *)(uintptr_t)sx_reg(r, g_multReaderReg);
    void **vt = a ? *(void ***)a : NULL;
    if (vt != g_vtPierce && vt != g_vtBlast) {
        static int n; if (n++ < 5) logf_("{multiplier ...}: only health_damage_pierce and health_damage_blast take it - this one is ignored");
        return 0;
    }
    if (!g_multPeek(rd, 9, 0)) {
        static int n; if (n++ < 5) logf_("{multiplier ...}: needs a number, e.g. {multiplier 2.0} - ignored");
        return 0;
    }
    float v = g_multReadFloat(rd), c = v;
    if (!(c >= 0.0f)) c = c != c ? 1.0f : 0.0f;                                         /* NaN -> 1, negative -> 0 */
    if (c > 65.535f) c = 65.535f;
    if (c != v) { static int n; if (n++ < 5) logf_("{multiplier %g}: out of range, using %.3f (allowed 0 .. 65.535)", v, c); }
    uint32_t q = (uint32_t)(c * 1000.0f + 0.5f);
    *(uint32_t *)(a + g_multPadOff) = MULT_MAGIC | q;
    if (g_verbose && InterlockedIncrement(&g_multParsed) <= 20)
        logf_("{multiplier %.3f} read for health_damage_%s", q / 1000.0, vt == g_vtPierce ? "pierce" : "blast");
    return 1;
}

/* stub the redirected call lands in: save the argument and callee-saved registers, ask sx_mult_unknown_block, then
   either return to the parser (handled) or tail-jump to the engine's logger with everything restored */
__attribute__((naked)) static void mult_stub(void)
{
    __asm__ volatile(
        "push %r15\n\t" "push %r14\n\t" "push %r13\n\t" "push %r12\n\t"
        "push %rdi\n\t" "push %rsi\n\t" "push %rbp\n\t" "push %rbx\n\t"
        "push %r9\n\t"  "push %r8\n\t"  "push %rdx\n\t" "push %rcx\n\t"
        "mov %rsp, %rcx\n\t"                                  /* SxRegs* */
        "sub $40, %rsp\n\t"                                   /* shadow space; rsp is 16-aligned here */
        "call sx_mult_unknown_block\n\t"
        "add $40, %rsp\n\t"
        "pop %rcx\n\t" "pop %rdx\n\t" "pop %r8\n\t"  "pop %r9\n\t"
        "pop %rbx\n\t" "pop %rbp\n\t" "pop %rsi\n\t" "pop %rdi\n\t"
        "pop %r12\n\t" "pop %r13\n\t" "pop %r14\n\t" "pop %r15\n\t"
        "test %eax, %eax\n\t"
        "jnz 1f\n\t"
        "jmp *sx_mult_log_orig(%rip)\n"
        "1:\n\t"
        "ret\n\t");
}

static void mult_parse_hook(uint8_t *a, void *reader)
{
    *(uint32_t *)(a + g_multPadOff) = MULT_MAGIC | MULT_ONE;  /* default 1.0x; a {multiplier X} block overwrites it */
    g_multCommon(a, reader);
}
static float mult_scale(const uint8_t *a, float v, const char *what)
{
    uint32_t w = *(const uint32_t *)(a + g_multPadOff);
    if ((w & 0xFFFF0000u) != MULT_MAGIC || (w & 0xFFFFu) == MULT_ONE) return v;
    float m = (float)(w & 0xFFFFu) / 1000.0f, r = v * m;
    if (g_verbose && InterlockedIncrement(&g_multApplied) <= 30)
        logf_("health_damage_%s: damage %.2f x %.3f = %.2f", what, v, m, r);
    return r;
}
static float mult_amount_pierce(uint8_t *a, void *ctx, void *out) { return mult_scale(a, g_multAmountOrigP(a, ctx, out), "pierce"); }
static float mult_amount_blast(uint8_t *a, void *ctx, void *out)  { return mult_scale(a, g_multAmountOrigB(a, ctx, out), "blast"); }

static int install_mult(void)
{
    /* make both vtables writable first, so a failure leaves everything as it was */
    DWORD oldP, oldB;
    if (!VirtualProtect(g_vtPierce + 5, 6 * 8, PAGE_READWRITE, &oldP)) { logf_("multiplier: VirtualProtect on a vtable failed"); return 0; }
    if (!VirtualProtect(g_vtBlast + 5, 6 * 8, PAGE_READWRITE, &oldB)) {
        VirtualProtect(g_vtPierce + 5, 6 * 8, oldP, &oldP); logf_("multiplier: VirtualProtect on a vtable failed"); return 0;
    }
    /* 1. slot 5 writes the default (1.0) and runs the engine's block parser, slot 10 multiplies the damage */
    g_multAmountOrigP = (MultAmountFn)g_vtPierce[10]; g_multAmountOrigB = (MultAmountFn)g_vtBlast[10];
    g_vtPierce[5] = (void *)mult_parse_hook; g_vtPierce[10] = (void *)mult_amount_pierce;
    g_vtBlast[5]  = (void *)mult_parse_hook; g_vtBlast[10]  = (void *)mult_amount_blast;
    /* 2. the parser's unknown-block log call -> mult_stub (reads {multiplier X}, anything else goes on to the logger) */
    int ok = redirect_call(g_multSite, (void *)mult_stub, &sx_mult_log_orig, "multiplier");
    if (!ok) {                                                   /* undo 1 */
        g_vtPierce[5] = (void *)g_multCommon; g_vtPierce[10] = (void *)g_multAmountOrigP;
        g_vtBlast[5]  = (void *)g_multCommon; g_vtBlast[10]  = (void *)g_multAmountOrigB;
    }
    VirtualProtect(g_vtBlast + 5, 6 * 8, oldB, &oldB);
    VirtualProtect(g_vtPierce + 5, 6 * 8, oldP, &oldP);
    return ok;
}
#endif /* SX_TEST */
