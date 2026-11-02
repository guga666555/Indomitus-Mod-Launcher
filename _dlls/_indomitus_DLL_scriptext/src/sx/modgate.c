/* GEM ScriptExt - mod gate
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * require_mod: named events fire only while an enabled mod's name contains that text (same check as BreedTags).
 * The other features need no gate: their script words only exist in the mod's own files.
 */

/* ------------------------------------------------------------------------------------ */
/* the engine's mod list (found from its "mod.off" handling), read with ReadProcessMemory  */
/* ------------------------------------------------------------------------------------ */
static uint8_t **g_modVec; static uint32_t g_modNameOff, g_modOnOff;

/* lea rcx,"mod.off" ; ... mov rbx,[rip+begin] ; mov rdi,[rip+end] ; loop: lea r,[r14+NAME] ... mov byte [r14+ON],0 */
static int find_mod_list(void)
{
    uint8_t *str = find_cstr("mod.off");
    if (!str) { logf_("mod list: 'mod.off' not found"); return 0; }
    uint8_t *lea = find_lea_to(str, 0);
    if (!lea) { logf_("mod list: 'mod.off' xref not found"); return 0; }
    uint8_t *beg = NULL, *end = NULL;
    for (uint8_t *q = lea; q < lea + 0x40; q++) {
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x1D && !beg) beg = q + 7 + *(int32_t *)(q + 3);
        else if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x3D && beg) { end = q + 7 + *(int32_t *)(q + 3); break; }
    }
    if (!beg || end != beg + 8) { logf_("mod list: vector not found"); return 0; }
    for (uint8_t *q = lea; q < lea + 0x200 && !(g_modNameOff && g_modOnOff); q++) {
        if (q[0] == 0x49 && q[1] == 0x8D && (q[2] & 0xC7) == 0x46 && !g_modNameOff) g_modNameOff = q[3];
        if (q[0] == 0x41 && q[1] == 0xC6 && q[2] == 0x86 && q[7] == 0 && !g_modOnOff) g_modOnOff = *(uint32_t *)(q + 3);
    }
    if (!g_modNameOff || !g_modOnOff) { logf_("mod list: entry layout not recognised"); return 0; }
    g_modVec = (uint8_t **)beg;
    logf_("mod list: vector @ exe+0x%llX, entry name +0x%X, enabled flag +0x%X", EXE_OFF(g_modVec), g_modNameOff, g_modOnOff);
    return 1;
}

#ifndef SX_TEST
static char g_requireMod[64];                                 /* ini require_mod; empty = no gate */
static volatile int g_modActive = 1;                          /* 0 while require_mod is set and no matching mod is enabled */

/* 1 = a matching mod is enabled, 0 = none, -1 = the list cannot be read */
static int mod_enabled(void)
{
    uint8_t *b, *e;
    if (!rd_mem(&g_modVec[0], &b, 8) || !rd_mem(&g_modVec[1], &e, 8) || !b || e < b || e - b > 8 * 512) return -1;
    for (uint8_t *p = b; p < e; p += 8) {
        uint8_t *mod; MsString nm; uint8_t on; char name[128] = "";
        if (!rd_mem(p, &mod, 8) || !mod) continue;
        if (!rd_mem(mod + g_modOnOff, &on, 1) || !rd_mem(mod + g_modNameOff, &nm, sizeof nm)) continue;
        if (nm.cap <= 15 && nm.size > 15) continue;               /* torn read while the menu edits the list */
        if (nm.size >= sizeof name) nm.size = sizeof name - 1;
        if (nm.cap > 15) { if (!rd_mem(nm.u.ptr, name, nm.size)) continue; } else memcpy(name, nm.u.buf, nm.size);
        name[nm.size] = 0;
        if (!on) continue;
        size_t L = strlen(g_requireMod), N = strlen(name);
        for (size_t i = 0; i + L <= N; i++) if (!_strnicmp(name + i, g_requireMod, L)) return 1;
    }
    return 0;
}
#endif /* SX_TEST */
