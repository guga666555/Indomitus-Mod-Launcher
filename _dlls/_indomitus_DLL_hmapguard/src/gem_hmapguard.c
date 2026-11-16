/* GEM HeightmapGuard 0.1 - skips a stale patch record in the engine's visibility heightmap instead of
 * crashing (Call to Arms: Gates of Hell, GEM 2, 1.067)
 *
 * eVisibilityHeightmap keeps a grid of 10-unit cells; each cell holds a list of the heightmap patches
 * (craters, trenches, any entity with a land heightmap) that touch it. A patch = a vector of 20-byte
 * points (x, y, heights, saved bytes). Two helpers walk every record of every cell in a rectangle:
 *   restore  (server exe+0x6997E0, game exe+0x8848D0, editor exe+0x708940): saved bytes -> height grids
 *   apply    (server exe+0x699980, game exe+0x884A70, editor exe+0x708B00): grids -> saved bytes, new heights
 * The engine sometimes leaves a record in a cell after its patch was freed. The next crater on that cell
 * then reads freed memory: ACCESS_VIOLATION in the restore loop (server crash Nov 15 2026, macro barrage
 * on 3v3_trenches; game crash Oct 29 2026; _notes\known_bugs.md).
 *
 * The dll patches no engine code. It adds a first-chance vectored exception handler that acts on an
 * access violation only when the faulting instruction is inside one of the two per-record loop bodies:
 *   fault in the body [L0, N)        -> continue at N   (the loop's own "next record" step)
 *   fault at N itself (bad list node) -> continue at E   (the loop's own "cell done" step)
 * Both targets use only registers the body never changes (list node, list end) or reload from the stack,
 * so the walk carries on with the next record / next cell. Any other exception is passed on untouched.
 * The engine installs only an unhandled-exception filter, which runs after vectored handlers.
 *
 * Cost: nothing while no exception is raised. What is lost: the stale record's saved heights are not
 * written back to its cell (one 10-unit cell of the visibility heightmap may keep slightly wrong values).
 * A stale record whose memory is still readable never faults and is not seen by this dll.
 *
 * MP: wanted in the dedicated server above all (ini Server=1), and in every client / the editor.
 *
 * Build: build.bat   Offline test (finders + the engine's own loops on a faked grid): build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define HG_VERSION "0.1"
#define HG_SECT    "HeightmapGuard"

/* ---------------------------------------------------------------- config */
static int cfg_log = 1, cfg_maxlines = 200;
#ifndef HG_TEST
static int cfg_enabled = 1, cfg_server = 1;

/* ---------------------------------------------------------------- log (no CRT file locks: used inside the handler) */
static char   g_logpath[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
#endif
static volatile LONG g_lines;
static void logf_(const char *fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 3) n = (int)sizeof buf - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
#ifdef HG_TEST
    fwrite(buf, 1, (size_t)n, stdout); fflush(stdout);
#else
    if (g_log == INVALID_HANDLE_VALUE && g_logpath[0])
        g_log = CreateFileA(g_logpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(g_log, buf, (DWORD)n, &w, NULL);
#endif
}

/* ---------------------------------------------------------------- PE helpers */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text;
static uint8_t *g_base;

static int get_sections(uint8_t *base)
{
    g_base = base; g_text.p = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!strncmp((char *)sh[i].Name, ".text", 8)) { g_text.p = base + sh[i].VirtualAddress; g_text.n = sh[i].Misc.VirtualSize; }
    return g_text.p != NULL;
}
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))

static size_t parse_pat(const char *s, uint8_t *b, uint8_t *m)
{
    size_t n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { b[n] = 0; m[n] = 0; }
        else { b[n] = (uint8_t)strtoul(s, NULL, 16); m[n] = 1; }
        n++; s += 2;
    }
    return n;
}
static uint8_t *scan(uint8_t *p, size_t n, const char *pat, int *count)
{
    uint8_t b[96], m[96], *first = NULL; size_t k = parse_pat(pat, b, m);
    if (count) *count = 0;
    if (k > n) return NULL;
    for (size_t i = 0; i + k <= n; i++) {
        if (p[i] != b[0]) continue;
        size_t j = 1;
        while (j < k && (!m[j] || p[i + j] == b[j])) j++;
        if (j < k) continue;
        if (!first) first = p + i;
        if (!count) break;
        ++*count;
    }
    return first;
}

/* ---------------------------------------------------------------- the two loops */
typedef struct { const char *name; uint8_t *l0, *n, *e; volatile LONG skips; } Site;
static Site g_site[2] = { { "restore", 0, 0, 0, 0 }, { "apply", 0, 0, 0, 0 } };

/* the loop's "next record" step at n: mov rdx,[rdx(+8)] ; cmp rdx,<end> ; jne <loop> -> address after the jne */
static uint8_t *after_next(uint8_t *n, int movlen)
{
    uint8_t *c = n + movlen;                                    /* 4x 3B xx = cmp reg,reg */
    if ((c[0] & 0xF8) != 0x48 || c[1] != 0x3B) return NULL;
    if (c[3] == 0x75) return c + 5;
    if (c[3] == 0x0F && c[4] == 0x85) return c + 9;
    return NULL;
}

static const char *find_all(uint8_t *base)
{
    int na = 0, nb = 0;
    g_site[0].l0 = g_site[1].l0 = NULL;
    if (!get_sections(base)) return "sections not found";

    /* restore: mov rax,[rdx+8] ; mov rcx,[rax+10] ; mov rax,[rcx] ; mov rcx,[rcx+8] ; cmp rax,rcx ; je N */
    uint8_t *a = scan(g_text.p, g_text.n, "48 8B 42 08 48 8B 48 10 48 8B 01 48 8B 49 08 48 3B C1 74 ?? 44 8B", &na);
    if (na != 1) return na ? "restore loop ambiguous" : "restore loop not found";
    uint8_t *an = a + 0x14 + (int8_t)a[0x13];
    if (an <= a + 0x20 || an > a + 0x200 || memcmp(an, "\x48\x8B\x52\x08", 4)) return "restore loop: next-record step not recognised";
    uint8_t *ae = after_next(an, 4);
    if (!ae) return "restore loop: end of list test not recognised";
    if (!scan(a, (size_t)(an - a), "48 83 C0 14 48 3B C1", NULL)) return "restore loop: 20-byte point step not found";

    /* apply: mov rcx,[rdx+10] ; test rcx,rcx ; je N ; mov rax,[rcx] ; mov rcx,[rcx+8] ; cmp rax,rcx ; je N */
    uint8_t *b = scan(g_text.p, g_text.n, "48 8B 4A 10 48 85 C9 0F 84 ?? ?? ?? ?? 48 8B 01 48 8B 49 08 48 3B C1 0F 84", &nb);
    if (nb != 1) return nb ? "apply loop ambiguous" : "apply loop not found";
    uint8_t *bn = b + 13 + *(int32_t *)(b + 9);
    if (bn <= b + 0x20 || bn > b + 0x200 || memcmp(bn, "\x48\x8B\x12", 3)) return "apply loop: next-record step not recognised";
    if (b + 29 + *(int32_t *)(b + 25) != bn) return "apply loop: the two skips differ";
    uint8_t *be = after_next(bn, 3);
    if (!be) return "apply loop: end of list test not recognised";
    if (!scan(b, (size_t)(bn - b), "48 83 C0 14 48 3B C1", NULL)) return "apply loop: 20-byte point step not found";

    g_site[0].l0 = a; g_site[0].n = an; g_site[0].e = ae;
    g_site[1].l0 = b; g_site[1].n = bn; g_site[1].e = be;
    return NULL;
}

/* ---------------------------------------------------------------- the handler */
static LONG CALLBACK hg_veh(EXCEPTION_POINTERS *x)
{
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *ip = (uint8_t *)x->ContextRecord->Rip;
    for (int i = 0; i < 2; i++) {
        Site *s = &g_site[i];
        if (!s->l0 || ip < s->l0 || ip > s->n) continue;
        uint8_t *to = ip == s->n ? s->e : s->n;
        LONG k = InterlockedIncrement(&s->skips);
        if (cfg_log && InterlockedIncrement(&g_lines) <= cfg_maxlines) {
            const ULONG_PTR *inf = x->ExceptionRecord->ExceptionInformation;
            logf_("skip: %s loop, fault at exe+0x%llX %s 0x%llX -> %s (exe+0x%llX) | node %p patch/points %p..%p | %ld in this loop",
                  s->name, OFF(ip), inf[0] == 1 ? "writing" : "reading", (unsigned long long)inf[1],
                  ip == s->n ? "rest of this cell skipped" : "record skipped", OFF(to),
                  (void *)x->ContextRecord->Rdx, (void *)x->ContextRecord->Rax, (void *)x->ContextRecord->Rcx, k);
        }
        x->ContextRecord->Rip = (DWORD64)to;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#ifndef HG_TEST
static void install(int is_server)
{
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM HeightmapGuard %s] NOT installed: %s", HG_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM HeightmapGuard %s] Enabled=0: not installed", HG_VERSION); return; }
    if (is_server && !cfg_server) { logf_("[GEM HeightmapGuard %s] Server=0: not installed in the server", HG_VERSION); return; }
    if (!AddVectoredExceptionHandler(1, hg_veh)) { logf_("[GEM HeightmapGuard %s] NOT installed: handler refused (%lu)", HG_VERSION, GetLastError()); return; }
    logf_("[GEM HeightmapGuard %s] installed | restore loop exe+0x%llX..0x%llX (cell done 0x%llX) | apply loop exe+0x%llX..0x%llX (cell done 0x%llX) | Log=%d",
          HG_VERSION, OFF(g_site[0].l0), OFF(g_site[0].n), OFF(g_site[0].e), OFF(g_site[1].l0), OFF(g_site[1].n), OFF(g_site[1].e), cfg_log);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH], exe[MAX_PATH], *nm;
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        int server = strstr(nm, "_server") != NULL;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path, strstr(nm, "_ed") ? "_editor" : (server ? "_server" : ""));
        cfg_enabled  = GetPrivateProfileIntA(HG_SECT, "Enabled", 1, ini);
        cfg_log      = GetPrivateProfileIntA(HG_SECT, "Log", 1, ini);
        cfg_server   = GetPrivateProfileIntA(HG_SECT, "Server", 1, ini);
        cfg_maxlines = GetPrivateProfileIntA(HG_SECT, "MaxLogLines", 200, ini);
        install(server);
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* hg_test.exe <exe>: finders on the mapped exe, then the engine's own restore / apply helpers run on a
   hand-made one-cell grid whose list holds good records and stale ones (unreadable patch, unreadable
   points, unreadable list node). Without the handler each stale case is an access violation. */
typedef void (*WalkFn)(void *self, void *grid, const int *rect);
typedef struct Node { struct Node *next, *prev; void *patch; } Node;
typedef struct { uint8_t *begin, *end; } Patch;
#pragma pack(push, 1)
typedef struct { int32_t x, y; uint8_t h, h2, saved[10]; } Point;   /* 20 bytes */
#pragma pack(pop)

static __attribute__((aligned(16))) uint8_t t_grid[0x80];
static uint8_t t_g1[100], t_g2[100];
static struct { Node *head; void *pad; } t_cell[1];
static Node  t_head, t_n[3];
static Patch t_p[3];
static Point t_pt[3];
static uint8_t *t_bad;                                              /* reserved, never committed: any access faults */

static uint8_t *func_start(uint8_t *in)
{
    for (uint8_t *a = (uint8_t *)((uintptr_t)in & ~(uintptr_t)15); a > in - 0x200; a -= 16)
        if (a[-1] == 0xCC || a[-1] == 0xC3) return a;
    return NULL;
}
static void build(int stale)                                        /* 0 none, 1 patch pointer, 2 points pointer, 3 list node */
{
    memset(t_grid, 0, sizeof t_grid); memset(t_g1, 7, sizeof t_g1); memset(t_g2, 9, sizeof t_g2);
    *(uint8_t **)(t_grid + 0x00) = t_g2; *(int32_t *)(t_grid + 0x10) = 10;
    *(uint8_t **)(t_grid + 0x18) = t_g1; *(int32_t *)(t_grid + 0x28) = 10;
    *(void **)(t_grid + 0x30) = t_cell;  *(void **)(t_grid + 0x38) = t_cell + 1;
    *(int32_t *)(t_grid + 0x48) = 1;     *(int32_t *)(t_grid + 0x60) = 0;
    t_cell[0].head = &t_head;
    t_head.next = &t_n[0]; t_head.prev = &t_n[2];
    for (int i = 0; i < 3; i++) {
        t_n[i].next = i < 2 ? &t_n[i + 1] : &t_head; t_n[i].prev = i ? &t_n[i - 1] : &t_head; t_n[i].patch = &t_p[i];
        t_pt[i] = (Point){ i + 1, i + 1, (uint8_t)(50 + i), (uint8_t)(60 + i), { (uint8_t)(20 + i), (uint8_t)(30 + i) } };
        t_p[i].begin = (uint8_t *)&t_pt[i]; t_p[i].end = (uint8_t *)(&t_pt[i] + 1);
    }
    if (stale == 1) t_n[1].patch = t_bad;
    if (stale == 2) { t_p[1].begin = t_bad + 4; t_p[1].end = t_bad + 0x800; }
    if (stale == 3) { t_n[1].prev = (Node *)t_bad; t_n[1].next = (Node *)t_bad; }
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: hg_test <exe>\n"); return 1; }
    HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) { printf("%s: load failed %lu\n", argv[1], GetLastError()); return 1; }
    const char *err = find_all((uint8_t *)m);
    if (err) { printf("%s: FAIL %s\n", argv[1], err); return 1; }
    WalkFn f[2] = { (WalkFn)func_start(g_site[0].l0), (WalkFn)func_start(g_site[1].l0) };
    printf("%s:\n", argv[1]);
    for (int i = 0; i < 2; i++)
        printf("  %-7s helper exe+0x%llX | loop exe+0x%llX, next record 0x%llX, cell done 0x%llX\n", g_site[i].name,
               OFF(f[i]), OFF(g_site[i].l0), OFF(g_site[i].n), OFF(g_site[i].e));
    if (!f[0] || !f[1]) { printf("  FAIL helper start not found\n"); return 1; }
    t_bad = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    AddVectoredExceptionHandler(1, hg_veh);

    static const int rect[4] = { 0, 0, 0, 0 };
    static const char *what[4] = { "all records good", "stale: patch unreadable", "stale: points unreadable", "stale: list node unreadable" };
    /* which of the 3 records must have been processed: restore walks back to front, apply front to back */
    /* case 3: record 1 itself is good, the node behind it (in walk order) is not: restore faults on the node's
       patch read, again on the node after "next record", then at "next record" itself; apply on the last two */
    static const int want[2][4][3] = { { {1,1,1}, {1,0,1}, {1,0,1}, {0,1,1} }, { {1,1,1}, {1,0,1}, {1,0,1}, {1,1,0} } };
    static const int faults[2][4] = { { 0, 1, 1, 3 }, { 0, 1, 1, 2 } };
    int bad = 0;
    for (int h = 0; h < 2; h++) for (int c = 0; c < 4; c++) {
        build(c);
        LONG before = g_site[h].skips;
        f[h](NULL, t_grid, rect);
        int ok = g_site[h].skips - before == faults[h][c];
        for (int i = 0; i < 3; i++) {
            int at = (i + 1) * 10 + (i + 1), done;
            if (h == 0) done = t_g1[at] == 20 + i && t_g2[at] == 30 + i;            /* saved bytes written back */
            else        done = t_pt[i].saved[0] == 7 && t_g1[at] == 50 + i;         /* grid saved, new height in */
            if (done != want[h][c][i]) ok = 0;
            if (!want[h][c][i] && t_g1[at] != 7) ok = 0;                            /* a skipped record leaves its cell bytes alone */
        }
        printf("  %-7s %-28s faults caught %ld %s\n", g_site[h].name, what[c], (long)(g_site[h].skips - before), ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    /* an access violation anywhere else must be passed on */
    {
        LONG before = g_site[0].skips + g_site[1].skips;
        EXCEPTION_RECORD er = { 0 }; CONTEXT cx = { 0 }; EXCEPTION_POINTERS xp = { &er, &cx };
        er.ExceptionCode = EXCEPTION_ACCESS_VIOLATION; cx.Rip = (DWORD64)(g_site[0].l0 - 1);
        LONG r1 = hg_veh(&xp);
        cx.Rip = (DWORD64)(g_site[1].n + 1);
        LONG r2 = hg_veh(&xp);
        er.ExceptionCode = 0xE06D7363; cx.Rip = (DWORD64)g_site[0].l0;
        LONG r3 = hg_veh(&xp);
        int ok = r1 == EXCEPTION_CONTINUE_SEARCH && r2 == EXCEPTION_CONTINUE_SEARCH && r3 == EXCEPTION_CONTINUE_SEARCH &&
                 g_site[0].skips + g_site[1].skips == before;
        printf("  faults outside the loops / other exception codes passed on %s\n", ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
