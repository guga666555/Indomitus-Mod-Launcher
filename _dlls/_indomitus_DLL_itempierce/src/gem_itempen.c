/* GEM ItemPierce 0.1 - rounds that pierce an armoured item carry on into the wearer
 * Call to Arms: Gates of Hell (GEM 2, 1.067)
 *
 * Problem: a worn item (SM helmet) gets its volume re-enabled by script so its thickness
 * stops rounds. When a round PIERCES that volume, the engine's hit routine still answers
 * "stop", so the round dies inside the helmet and never reaches the head volume inside it.
 *
 * Engine (game exe offsets, found by pattern at runtime):
 *   BulletStep exe+0x835500 walks the sorted hit records of one bullet step and calls
 *   HitVolume exe+0x836B40 (bullet, record, dir, hitList) per record:
 *     record+0 = volume, volume+0x40 = entity, volume+0x38 = dedupe key
 *     result 0 ignore, 1 carry on (pierced and through), 2 stop, 3 ricochet.
 *   After a pierce HitVolume computes   ebx = (resultFlags & 2) ? 1 : 2   (patch A site);
 *   BulletStep skips a record whose volume+0x38 key equals the previous record's (patch B site).
 *
 * This dll:
 *   A: after a pierce on an entity carrying the script tag (default "pierce_through"),
 *      ebx = 1 -> the round keeps flying (with the damage scale the engine already reduced),
 *      unless the engine says it is out of energy (cl).
 *   B: a record of a tagged entity is never deduped and does not become the dedupe key, so
 *      the wearer's volume behind it is still hit in the same step.
 *   Entity script tags = std::vector of 0x60-byte Tag at entity+0x4E8 (name std::string +0x40).
 *
 * Build: build.bat      Offline finder test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define IP_VERSION "0.1"

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1;
static char cfg_tag[64] = "pierce_through";
static size_t cfg_taglen = 14;
static int  cfg_log = 1;
static uint32_t cfg_tagoff = 0x4E8;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
static void logf_(const char *fmt, ...)
{
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = fopen(g_logpath, "a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- tag check */
static int ieq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* MSVC std::string at p: buf/ptr +0, size +0x10, cap +0x18 */
static int str_is(const uint8_t *p, const char *s, size_t n)
{
    uint64_t size = *(const uint64_t *)(p + 0x10);
    uint64_t cap  = *(const uint64_t *)(p + 0x18);
    if (size != n || cap < size || cap > 0x10000) return 0;
    const char *d = cap > 15 ? *(const char *const *)p : (const char *)p;
    if (!d) return 0;
    return ieq(d, s, n);
}

static int has_tag(const uint8_t *ent)
{
    if (!ent) return 0;
    const uint8_t *b = *(const uint8_t *const *)(ent + cfg_tagoff);
    const uint8_t *e = *(const uint8_t *const *)(ent + cfg_tagoff + 8);
    if (!b || e < b) return 0;
    uintptr_t len = (uintptr_t)(e - b);
    if (len % 0x60 || len > 0x60 * 512) return 0;
    for (const uint8_t *t = b; t < e; t += 0x60)
        if (str_is(t + 0x40, cfg_tag, cfg_taglen)) return 1;
    return 0;
}

static uint32_t st_pass, st_nodedupe;

/* called from the asm stubs (ms_abi, rcx = entity) */
__attribute__((used, ms_abi)) int ip_check_pass(const uint8_t *ent)
{
    if (!cfg_enabled || !has_tag(ent)) return 0;
    st_pass++;
    if (cfg_log && g_lines < 500) {
        g_lines++;
        logf_("pass: round pierced tagged entity %p -> carries on (total %u)", (void *)ent, st_pass);
    }
    return 1;
}

__attribute__((used, ms_abi)) int ip_check_nodedupe(const uint8_t *ent)
{
    if (!cfg_enabled || !has_tag(ent)) return 0;
    st_nodedupe++;
    if (cfg_log >= 2 && g_lines < 500) {
        g_lines++;
        logf_("hit record on tagged entity %p (not deduped)", (void *)ent);
    }
    return 1;
}

/* ---------------------------------------------------------------- stubs */
/* jump targets, set at install */
__attribute__((used)) uint64_t g_a_back;     /* site A + 20 */
__attribute__((used)) uint64_t g_b_go;       /* site B + 21 (= the "mov rdx,r15" after the key code) */
__attribute__((used)) uint64_t g_b_skip;     /* target of the dedupe je */

/* ip_tail_a: runs right after the original 20 bytes (copied into the trampoline).
 * In: ebx = 1/2, cl = out-of-energy flag, r13 = hit entity. */
/* ip_tail_b: replaces  mov rax,[rcx+38h]; test; je go; cmp rax,r14; je skip; mov r14,rax
 * In: rcx = volume. */
__asm__(
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl ip_tail_a\n"
    "ip_tail_a:\n"
    "  cmp ebx, 2\n"
    "  jne 9f\n"
    "  test cl, cl\n"
    "  jnz 9f\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  and rsp, -16\n"
    "  push rax\n  push rcx\n  push rdx\n  push r8\n  push r9\n  push r10\n  push r11\n"
    "  sub rsp, 0x88\n"
    "  movdqu [rsp+0x20], xmm0\n  movdqu [rsp+0x30], xmm1\n  movdqu [rsp+0x40], xmm2\n"
    "  movdqu [rsp+0x50], xmm3\n  movdqu [rsp+0x60], xmm4\n  movdqu [rsp+0x70], xmm5\n"
    "  mov rcx, r13\n"
    "  call ip_check_pass\n"
    "  test eax, eax\n"
    "  jz 1f\n"
    "  mov ebx, 1\n"
    "1:\n"
    "  movdqu xmm0, [rsp+0x20]\n  movdqu xmm1, [rsp+0x30]\n  movdqu xmm2, [rsp+0x40]\n"
    "  movdqu xmm3, [rsp+0x50]\n  movdqu xmm4, [rsp+0x60]\n  movdqu xmm5, [rsp+0x70]\n"
    "  add rsp, 0x88\n"
    "  pop r11\n  pop r10\n  pop r9\n  pop r8\n  pop rdx\n  pop rcx\n  pop rax\n"
    "  mov rsp, rbp\n"
    "  pop rbp\n"
    "9:\n"
    "  jmp qword ptr [rip + g_a_back]\n"
    "\n"
    ".globl ip_tail_b\n"
    "ip_tail_b:\n"
    "  mov rax, [rcx+0x38]\n"
    "  test rax, rax\n"
    "  jz 8f\n"
    "  push rbp\n"
    "  mov rbp, rsp\n"
    "  and rsp, -16\n"
    "  push rcx\n  push rdx\n  push r8\n  push r9\n  push r10\n  push r11\n"
    "  sub rsp, 0x80\n"
    "  movdqu [rsp+0x20], xmm0\n  movdqu [rsp+0x30], xmm1\n  movdqu [rsp+0x40], xmm2\n"
    "  movdqu [rsp+0x50], xmm3\n  movdqu [rsp+0x60], xmm4\n  movdqu [rsp+0x70], xmm5\n"
    "  mov rcx, [rcx+0x40]\n"
    "  call ip_check_nodedupe\n"
    "  movdqu xmm0, [rsp+0x20]\n  movdqu xmm1, [rsp+0x30]\n  movdqu xmm2, [rsp+0x40]\n"
    "  movdqu xmm3, [rsp+0x50]\n  movdqu xmm4, [rsp+0x60]\n  movdqu xmm5, [rsp+0x70]\n"
    "  add rsp, 0x80\n"
    "  pop r11\n  pop r10\n  pop r9\n  pop r8\n  pop rdx\n  pop rcx\n"
    "  mov rsp, rbp\n"
    "  pop rbp\n"
    "  test eax, eax\n"
    "  jnz 8f\n"
    "  mov rax, [rcx+0x38]\n"
    "  cmp rax, r14\n"
    "  je 7f\n"
    "  mov r14, rax\n"
    "8:\n"
    "  jmp qword ptr [rip + g_b_go]\n"
    "7:\n"
    "  jmp qword ptr [rip + g_b_skip]\n"
    ".att_syntax prefix\n"
);
extern void ip_tail_a(void);
extern void ip_tail_b(void);

/* ---------------------------------------------------------------- finders */
#define W 0x100
/* game / server: movzx ebx,[rsp+60h]; comiss xmm6,xmm0; setae cl; shr bl,1; not bl; and ebx,1; inc ebx; test cl,cl */
static const uint16_t SIG_A1[] = {
    0x0F,0xB6,0x5C,0x24,W, 0x0F,0x2F,0xF0, 0x0F,0x93,0xC1, 0xD0,0xEB, 0xF6,0xD3, 0x83,0xE3,0x01, 0xFF,0xC3,
    0x84,0xC9
};
/* editor: comiss xmm6,xmm0; setae cl; movzx ebx,[rsp+60h]; shr; not; and; inc; test cl,cl */
static const uint16_t SIG_A2[] = {
    0x0F,0x2F,0xF0, 0x0F,0x93,0xC1, 0x0F,0xB6,0x5C,0x24,W, 0xD0,0xEB, 0xF6,0xD3, 0x83,0xE3,0x01, 0xFF,0xC3,
    0x84,0xC9
};
#define LEN_A 20
/* mov rax,[rcx+38h]; test rax,rax; je +0Ch; cmp rax,r14; je far; mov r14,rax; mov rdx,r15 */
static const uint16_t SIG_B[] = {
    0x48,0x8B,0x41,0x38, 0x48,0x85,0xC0, 0x74,0x0C, 0x49,0x3B,0xC6, 0x0F,0x84,W,W,W,W, 0x4C,0x8B,0xF0,
    0x49,0x8B,0xD7
};
#define LEN_B 21

static int text_range(uint8_t *base, uint8_t **lo, uint8_t **hi)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!memcmp(s->Name, ".text", 5)) {
            *lo = base + s->VirtualAddress;
            *hi = *lo + s->Misc.VirtualSize;
            return 1;
        }
    return 0;
}

static int match(const uint8_t *p, const uint16_t *sig, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (sig[i] != W && p[i] != sig[i]) return 0;
    return 1;
}

static uint8_t *find_unique(uint8_t *lo, uint8_t *hi, const uint16_t *sig, size_t n, int *count)
{
    uint8_t *hit = NULL; *count = 0;
    for (uint8_t *p = lo; p + n <= hi; p++)
        if (match(p, sig, n)) { if (!hit) hit = p; (*count)++; }
    return hit;
}

static const char *find_all(uint8_t *base, uint8_t **a_out, uint8_t **b_out)
{
    uint8_t *lo, *hi;
    if (!text_range(base, &lo, &hi)) return "no .text section";
    int n1, n2, nb;
    uint8_t *a1 = find_unique(lo, hi, SIG_A1, sizeof SIG_A1 / 2, &n1);
    uint8_t *a2 = find_unique(lo, hi, SIG_A2, sizeof SIG_A2 / 2, &n2);
    if (n1 + n2 == 0) return "pierce-result site not found (unknown exe build, or already patched)";
    if (n1 + n2 > 1) return "pierce-result site not unique";
    uint8_t *a = a1 ? a1 : a2;
    if (a[a1 ? 4 : 10] != 0x60) return "pierce-result site: unexpected stack offset";
    uint8_t *b = find_unique(lo, hi, SIG_B, sizeof SIG_B / 2, &nb);
    if (nb == 0) return "hit-dedupe site not found (unknown exe build, or already patched)";
    if (nb > 1) return "hit-dedupe site not unique";
    /* sanity: both sites belong to the bullet code - B's function calls A's function */
    intptr_t d = a > b ? a - b : b - a;
    if (d > 0x10000) return "sites too far apart";
    *a_out = a; *b_out = b;
    return NULL;
}

/* ---------------------------------------------------------------- ini */
static void load_ini(const char *ini)
{
    cfg_enabled = GetPrivateProfileIntA("ItemPierce", "Enabled", 1, ini);
    GetPrivateProfileStringA("ItemPierce", "Tag", "pierce_through", cfg_tag, sizeof cfg_tag, ini);
    cfg_taglen = strlen(cfg_tag);
    cfg_log = GetPrivateProfileIntA("ItemPierce", "Log", 1, ini);
    cfg_tagoff = (uint32_t)GetPrivateProfileIntA("ItemPierce", "EntityTagsOffset", 0x4E8, ini);
}

#ifndef IP_TEST
/* ---------------------------------------------------------------- install */
static int write_code(uint8_t *at, const uint8_t *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

static void absjmp(uint8_t *p, const void *to)
{
    p[0] = 0xFF; p[1] = 0x25; p[2] = p[3] = p[4] = p[5] = 0;
    uint64_t t = (uint64_t)(uintptr_t)to;
    memcpy(p + 6, &t, 8);
}

static void install(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    uint8_t *a = NULL, *b = NULL;
    if (!cfg_enabled) { logf_("[GEM ItemPierce %s] Enabled=0, not installed", IP_VERSION); return; }
    const char *err = find_all(base, &a, &b);
    if (err) { logf_("[GEM ItemPierce %s] NOT installed: %s", IP_VERSION, err); return; }

    /* trampoline A: original 20 bytes, then jmp ip_tail_a */
    uint8_t *tr = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) { logf_("[GEM ItemPierce] VirtualAlloc failed (%lu)", GetLastError()); return; }
    memcpy(tr, a, LEN_A);
    absjmp(tr + LEN_A, (const void *)ip_tail_a);
    FlushInstructionCache(GetCurrentProcess(), tr, 64);

    g_a_back = (uint64_t)(uintptr_t)(a + LEN_A);
    g_b_go   = (uint64_t)(uintptr_t)(b + LEN_B);
    g_b_skip = (uint64_t)(uintptr_t)(b + 18 + *(int32_t *)(b + 14));

    uint8_t pa[LEN_A], pb[LEN_B];
    memset(pa, 0x90, sizeof pa); memset(pb, 0x90, sizeof pb);
    absjmp(pa, tr);
    absjmp(pb, (const void *)ip_tail_b);
    if (!write_code(b, pb, sizeof pb) || !write_code(a, pa, sizeof pa)) {
        logf_("[GEM ItemPierce] VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    logf_("[GEM ItemPierce %s] installed: pierce site exe+0x%llX, dedupe site exe+0x%llX | Tag=%s Log=%d EntityTagsOffset=0x%X",
          IP_VERSION, (unsigned long long)(a - base), (unsigned long long)(b - base), cfg_tag, cfg_log, cfg_tagoff);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH];
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
        char exe[MAX_PATH], *nm;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path,
                 strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        load_ini(ini);
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* build.bat test -> out\ip_test.exe <exe>...: finders on each mapped exe + tag reader on fake data */
static void fake_str(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    memset(p, 0, 0x20);
    if (n <= 15) { memcpy(p, s, n); *(uint64_t *)(p + 0x18) = 15; }
    else { char *h = _strdup(s); *(char **)p = h; *(uint64_t *)(p + 0x18) = n; }
    *(uint64_t *)(p + 0x10) = n;
}

int main(int argc, char **argv)
{
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        HMODULE m = LoadLibraryExA(argv[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[i], GetLastError()); bad = 1; continue; }
        uint8_t *base = (uint8_t *)m, *a = NULL, *b = NULL;
        const char *err = find_all(base, &a, &b);
        if (err) { printf("%s: FAIL %s\n", argv[i], err); bad = 1; continue; }
        printf("%s: pierce site exe+0x%llX, dedupe site exe+0x%llX, skip target exe+0x%llX\n", argv[i],
               (unsigned long long)(a - base), (unsigned long long)(b - base),
               (unsigned long long)(b + 18 + *(int32_t *)(b + 14) - base));
    }
    /* tag reader */
    static uint8_t ent[0x1000], tags[0x60 * 3];
    memset(tags, 0, sizeof tags);
    fake_str(tags + 0x00 + 0x40, "stuff");
    fake_str(tags + 0x60 + 0x40, "Pierce_Through");
    fake_str(tags + 0xC0 + 0x40, "a_much_longer_tag_name_on_the_heap");
    *(uint8_t **)(ent + 0x4E8) = tags;
    *(uint8_t **)(ent + 0x4F0) = tags + 0x60 * 3;
    cfg_log = 0;
    int t1 = has_tag(ent);
    *(uint8_t **)(ent + 0x4F0) = tags + 0x60;           /* only "stuff" */
    int t2 = has_tag(ent);
    strcpy(cfg_tag, "a_much_longer_tag_name_on_the_heap"); cfg_taglen = strlen(cfg_tag);
    *(uint8_t **)(ent + 0x4F0) = tags + 0x60 * 3;
    int t3 = has_tag(ent);
    *(uint8_t **)(ent + 0x4F0) = tags + 0x50;           /* broken vector */
    int t4 = has_tag(ent);
    printf("tag reader: %d %d %d %d -> %s\n", t1, t2, t3, t4, (t1 && !t2 && t3 && !t4) ? "OK" : "FAIL");
    if (!(t1 && !t2 && t3 && !t4)) bad = 1;
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
