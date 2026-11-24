/* gem_melee core self-test (spec 8.3): cases C-1 .. C-10. test_core_main(argc, argv), argv = the three game exes (no args: the
 * default install, GOH_BIN overrides the folder). Temporary files go to "<this exe's folder>\tmp\". C-10 is last: its VEH and exit
 * hooks stay for the process. */
#include "../core/core_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_tmp[MAX_PATH];
static int  g_ok;
#define CHECK(c) do { if (!(c)) { printf("    check failed (line %d): %s\n", __LINE__, #c); g_ok = 0; } } while (0)

static void tmp_path(char *out, size_t cap, const char *name) { snprintf(out, cap, "%s\\%s", g_tmp, name); }

static char *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { if (n) *n = 0; return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)sz + 1);
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[got] = 0;
    if (n) *n = got;
    return b;
}

static int count_lines(const char *path)
{
    size_t n; char *b = read_all(path, &n);
    int k = 0;
    for (size_t i = 0; i < n; i++) if (b[i] == '\n') k++;
    free(b);
    return k;
}

static int count_sub(const char *path, const char *sub)
{
    char *b = read_all(path, NULL);
    int k = 0;
    if (b) for (const char *p = b; (p = strstr(p, sub)); p++) k++;
    free(b);
    return k;
}

static int dig(char c) { return c >= '0' && c <= '9'; }

/* ^\d\d:\d\d:\d\d\.\d{3} +(\d+|-) \[(core|chase|stance|pen|link)\] ; *text = what follows */
static int line_ok(const char *l, const char **text)
{
    static const char *const tags[] = { "core", "chase", "stance", "pen", "link" };
    if (!(dig(l[0]) && dig(l[1]) && l[2] == ':' && dig(l[3]) && dig(l[4]) && l[5] == ':' && dig(l[6]) && dig(l[7]) && l[8] == '.'
          && dig(l[9]) && dig(l[10]) && dig(l[11]) && l[12] == ' ')) return 0;
    const char *p = l + 12;
    while (*p == ' ') p++;
    if (*p == '-') p++;
    else { if (!dig(*p)) return 0; while (dig(*p)) p++; }
    if (p[0] != ' ' || p[1] != '[') return 0;
    p += 2;
    for (int i = 0; i < 5; i++) {
        size_t k = strlen(tags[i]);
        if (!strncmp(p, tags[i], k) && p[k] == ']' && p[k + 1] == ' ') { if (text) *text = p + k + 2; return 1; }
    }
    return 0;
}

static void fake_str(uint8_t *p, const char *s)      /* MSVC std::string (stance's test helper, ms:3306-3313) */
{
    size_t n = strlen(s);
    memset(p, 0, 0x20);
    if (n <= 15) { memcpy(p, s, n); *(uint64_t *)(p + 0x18) = 15; }
    else { char *h = _strdup(s); *(char **)p = h; *(uint64_t *)(p + 0x18) = n; }
    *(uint64_t *)(p + 0x10) = n;
}

static volatile uint32_t g_fake, g_other;
static volatile LONG g_fires;
static uint32_t g_fire_e[16], g_fire_now[16];
static void on_battle(uint32_t e, uint32_t now) { LONG k = InterlockedIncrement(&g_fires) - 1; if (k < 16) { g_fire_e[k] = e; g_fire_now[k] = now; } }
static volatile LONG g_dummy;
static void on_dummy(uint32_t e, uint32_t now) { (void)e; (void)now; InterlockedIncrement(&g_dummy); }
/* listener call order (WP0_verify n2): every listener appends its id, in the order the core calls them */
static volatile LONG g_seq_n; static int g_seq[64];
static void seq_add(int id) { LONG k = InterlockedIncrement(&g_seq_n) - 1; if (k < 64) g_seq[k] = id; }
static void on_seq_a(uint32_t e, uint32_t now) { (void)e; (void)now; seq_add(1); }
static void on_seq_b(uint32_t e, uint32_t now) { (void)e; (void)now; seq_add(2); }

static int report(const char *name)
{
    printf("%s: %s\n", name, g_ok ? "OK" : "FAIL");
    fflush(stdout);
    return g_ok;
}

/* ------------------------------------------------------------------------------------------------ C-1 sink */
static int c1_sink(void)
{
    g_ok = 1;
    char path[MAX_PATH]; tmp_path(path, sizeof path, "sink.log");
    DeleteFileA(path);
    core_test_set_clock(NULL);
    core_test_sink_file(path);
    core_log(CORE_MOD_CORE, "c1 line %d", 1);
    core_log(CORE_MOD_CHASE, "c1 chase");
    core_log(CORE_MOD_STANCE, "c1 stance");
    core_log(CORE_MOD_PEN, "c1 pen");
    core_log(CORE_MOD_LINK, "c1 link");
    g_fake = 123456; core_test_set_clock(&g_fake);
    core_log(CORE_MOD_STANCE, "c1 with clock");
    core_test_set_clock(NULL);
    char *big = (char *)malloc(5001); memset(big, 'x', 5000); big[5000] = 0;
    core_log(CORE_MOD_PEN, "%s", big);
    free(big);
    core_test_sink(CORE_SINK_DISCARD);
    size_t n1; char *b1 = read_all(path, &n1);
    core_log(CORE_MOD_CORE, "c1 discarded");
    core_test_sink_file(path); core_test_sink(CORE_SINK_DISCARD);
    size_t n2; char *b2 = read_all(path, &n2);
    CHECK(b1 && b2 && n1 == n2);
    int lines = 0, okfmt = 1, bigok = 0, clockok = 0, dashok = 0;
    if (b1) {
        for (char *l = b1, *e; *l; l = e + 1) {
            e = strchr(l, '\n'); if (!e) { okfmt = 0; break; }
            *e = 0; if (e > l && e[-1] == '\r') e[-1] = 0;
            const char *t = NULL;
            lines++;
            if (!line_ok(l, &t)) { okfmt = 0; printf("    bad line: %.80s\n", l); continue; }
            if (strlen(t) == 5000 && t[0] == 'x' && t[4999] == 'x') bigok = 1;
            if (!strcmp(t, "c1 with clock") && strstr(l, "    123456 [stance] ")) clockok = 1;
            if (!strcmp(t, "c1 line 1") && strstr(l, "        - [core] ")) dashok = 1;
        }
    }
    CHECK(lines == 7); CHECK(okfmt); CHECK(bigok); CHECK(clockok); CHECK(dashok);
    free(b1); free(b2);
    return report("C-1 sink");
}

/* ------------------------------------------------------------------------------------------------ C-2 ini */
static int c2_ini(void)
{
    g_ok = 1;
    char path[MAX_PATH]; tmp_path(path, sizeof path, "c2.ini");
    FILE *f = fopen(path, "wb");
    fputs("[Core]\r\nLog=0\r\n[T]\r\nNum=42\r\nStr=hello world\r\n", f); fclose(f);
    core_test_set_ini(path);
    CHECK(!strcmp(core_ini_path(), path));
    wchar_t w[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH);
    CHECK(!wcscmp(core_ini_pathw(), w));
    CHECK(core_ini_int("T", "Num", 7) == 42);
    CHECK(core_ini_int("t", "num", 7) == 42);                               /* case-insensitive, as GetPrivateProfile */
    CHECK(core_ini_int("T", "Missing", 7) == 7);
    CHECK(core_ini_int("Nope", "Num", -3) == -3);
    char s[64];
    core_ini_str("T", "Str", "d", s, sizeof s); CHECK(!strcmp(s, "hello world"));
    core_ini_str("T", "Nope", "dflt", s, sizeof s); CHECK(!strcmp(s, "dflt"));
    core_ini_str("T", "Str", "d", s, 4); CHECK(strlen(s) < 4);
    char missing[MAX_PATH]; tmp_path(missing, sizeof missing, "no_such_file.ini");
    DeleteFileA(missing);
    core_test_set_ini(missing);
    CHECK(core_ini_int("T", "Num", 7) == 7);
    core_ini_str("T", "Str", "dd", s, sizeof s); CHECK(!strcmp(s, "dd"));
    core_test_set_ini("");
    CHECK(core_ini_int("T", "Num", 9) == 9);
    core_ini_str("T", "Str", "e", s, sizeof s); CHECK(!strcmp(s, "e"));
    return report("C-2 ini");
}

/* ------------------------------------------------------------------------------------------------ C-3 epoch */
/* C-3d threads: each counts itself in, then spins on the start flag, so all of them poll at the same moment (an event wake-up
 * staggers them and never hits the race; fix round 1, B1) */
enum { C3D_MAX_THREADS = 4, C3D_ROUNDS = 1000 };
static volatile LONG g_c3d_ready, g_c3d_go;
static DWORD WINAPI c3d_thread(LPVOID p)
{
    (void)p;
    InterlockedIncrement(&g_c3d_ready);
    while (!g_c3d_go) YieldProcessor();
    core_epoch_poll(CORE_MOD_CORE);
    return 0;
}

static int c3_epoch(void)
{
    g_ok = 1;
    char path[MAX_PATH]; tmp_path(path, sizeof path, "epoch.log");
    DeleteFileA(path);
    core_test_sink_file(path);
    g_fake = 0; core_test_set_clock(&g_fake);
    CHECK(core_epoch_listen(on_battle) == 1);
    /* ... 1050 -> 50 is a back-step of exactly NewBattleBackMs (1000): fires; 1049 -> 50 (999): does not (WP0_verify n2) */
    static const uint32_t vals[] = { 100, 5000, 9000, 3000, 3000, 0, 0, 50, 49, 1050, 50, 1049, 50 };
    static const int fires[] = { 0, 0, 0, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3 };
    for (int i = 0; i < 13; i++) {
        g_fake = vals[i];
        uint32_t r = core_epoch_poll(CORE_MOD_CORE);
        if (g_fires != fires[i] || r != (uint32_t)fires[i]) { printf("    step %d clock %u: fires %ld (want %d), poll %u\n", i, vals[i], (long)g_fires, fires[i], r); g_ok = 0; }
    }
    CHECK(g_fire_e[0] == 1 && g_fire_now[0] == 3000 && g_fire_e[1] == 2 && g_fire_now[1] == 0 && g_fire_e[2] == 3 && g_fire_now[2] == 50);
    CHECK(core_epoch() == 3);
    CHECK(core_epoch_poll(CORE_MOD_STANCE) == 3 && core_epoch_poll(CORE_MOD_PEN) == 3);
    CHECK(count_sub(path, "--- new battle 1: sim clock 9000 -> 3000 ms") == 1 && count_sub(path, "--- new battle 2: sim clock 3000 -> 0 ms") == 1);
    CHECK(count_sub(path, "--- new battle 3: sim clock 1050 -> 50 ms") == 1 && count_sub(path, "--- new battle 4") == 0);
    /* freeze: poll returns 0 and never fires */
    core_test_freeze_epoch(1);
    g_fake = 100000; CHECK(core_epoch_poll(CORE_MOD_CORE) == 0);
    g_fake = 0; CHECK(core_epoch_poll(CORE_MOD_STANCE) == 0);
    CHECK(g_fires == 3);
    core_test_freeze_epoch(0);
    CHECK(core_epoch_poll(CORE_MOD_CORE) == 3 && g_fires == 3);
    /* at most 4 listeners: on_battle (above), then on_seq_a, on_dummy, on_seq_b; called in registration order (n2) */
    CHECK(core_epoch_listen(on_seq_a) == 1 && core_epoch_listen(on_dummy) == 1 && core_epoch_listen(on_seq_b) == 1);
    CHECK(core_epoch_listen(on_dummy) == 0);
    CHECK(core_epoch_listen(NULL) == 0);
    g_seq_n = 0;
    g_fake = 9000; core_epoch_poll(CORE_MOD_CORE); g_fake = 10; core_epoch_poll(CORE_MOD_CORE);
    CHECK(g_fires == 4 && g_seq_n == 2 && g_seq[0] == 1 && g_seq[1] == 2 && g_dummy == 1);
    int ok3 = g_ok;

    /* C-3b: no core clock: detector idle, poll 0 */
    g_ok = 1;
    core_test_set_clock(NULL);
    LONG f0 = g_fires;
    g_fake = 5000; CHECK(core_epoch_poll(CORE_MOD_CORE) == 0);
    g_fake = 0;    CHECK(core_epoch_poll(CORE_MOD_STANCE) == 0);
    g_fake = 5000; CHECK(core_epoch_poll(CORE_MOD_PEN) == 0);
    CHECK(g_fires == f0 && core_now() == 0 && core_clock_ptr() == NULL);
    core_epoch_bind(CORE_MOD_STANCE, &g_other);                          /* no core clock: nothing, no line */
    int ok3b = g_ok;

    /* C-3c: a module bound to another clock keeps its bind-time value */
    g_ok = 1;
    core_test_set_clock(&g_fake);
    CHECK(core_now() == 5000 && core_clock_ptr() == &g_fake);
    core_epoch_poll(CORE_MOD_CORE);
    CHECK(count_sub(path, "WARNING") == 0);
    uint32_t e0 = core_epoch();
    core_epoch_bind(CORE_MOD_PEN, &g_other);
    CHECK(count_sub(path, "WARNING: pen clock") == 1 && count_sub(path, "pen keeps only its own new-battle rule") == 1);
    g_fake = 100000; core_epoch_poll(CORE_MOD_CORE);
    g_fake = 0; core_epoch_poll(CORE_MOD_CORE);
    CHECK(core_epoch() == e0 + 1 && g_fires == f0 + 1);
    CHECK(core_epoch_poll(CORE_MOD_STANCE) == e0 + 1);
    CHECK(core_epoch_poll(CORE_MOD_PEN) == e0);
    core_epoch_bind(CORE_MOD_PEN, &g_fake);                              /* the core pointer re-attaches */
    CHECK(core_epoch_poll(CORE_MOD_PEN) == e0 + 1);
    core_epoch_bind(CORE_MOD_STANCE, NULL);                              /* a NULL module clock counts as different */
    CHECK(count_sub(path, "WARNING: stance clock NULL") == 1);
    g_fake = 100000; core_epoch_poll(CORE_MOD_CORE); g_fake = 0; core_epoch_poll(CORE_MOD_CORE);
    CHECK(core_epoch_poll(CORE_MOD_STANCE) == e0 + 1 && core_epoch_poll(CORE_MOD_PEN) == e0 + 2);
    core_epoch_bind(CORE_MOD_STANCE, &g_fake);
    CHECK(core_epoch_poll(CORE_MOD_STANCE) == e0 + 2);
    CHECK(count_sub(path, "WARNING") == 2);
    int ok3c = g_ok;

    /* C-3d: threads released together after one back-step, 1000 rounds: exactly one fire per round. 4 threads (2 on a 2-cpu
     * machine), each spinning on one start flag; a poll without the fire-branch CAS double-fires in about half of the rounds */
    g_ok = 1;
    core_test_sink(CORE_SINK_DISCARD);
    SYSTEM_INFO si; GetSystemInfo(&si);
    int nt = si.dwNumberOfProcessors < C3D_MAX_THREADS ? (int)si.dwNumberOfProcessors : C3D_MAX_THREADS;
    if (nt < 2) nt = 2;
    LONG base = g_fires; int bad = 0;
    for (int r = 0; r < C3D_ROUNDS && !bad; r++) {
        g_fake = 200000; core_epoch_poll(CORE_MOD_CORE);
        g_fake = 0;
        g_c3d_ready = 0; g_c3d_go = 0;
        HANDLE th[C3D_MAX_THREADS];
        int made = 0;
        for (int k = 0; k < nt; k++) if ((th[made] = CreateThread(NULL, 0, c3d_thread, NULL, 0, NULL)) != NULL) made++;
        while (g_c3d_ready < made) YieldProcessor();
        InterlockedExchange(&g_c3d_go, 1);
        if (made) WaitForMultipleObjects((DWORD)made, th, TRUE, INFINITE);
        for (int k = 0; k < made; k++) CloseHandle(th[k]);
        if (made != nt) { printf("    round %d: CreateThread failed (%d of %d threads)\n", r, made, nt); bad = 1; }
        else if (g_fires != base + r + 1) { printf("    round %d: fires %ld, want %ld\n", r, (long)(g_fires - base), (long)(r + 1)); bad = 1; }
    }
    CHECK(!bad);
    CHECK(g_dummy == g_fires - 3 && g_seq_n == 2 * (g_fires - 3));        /* every listener is called once per fire */
    int ok3d = g_ok;
    printf("C-3 epoch: %s\nC-3b no clock: %s\nC-3c bind: %s\nC-3d %d threads released together, %d rounds: %s\n",
           ok3 ? "OK" : "FAIL", ok3b ? "OK" : "FAIL", ok3c ? "OK" : "FAIL", nt, C3D_ROUNDS, ok3d ? "OK" : "FAIL");
    g_ok = ok3 && ok3b && ok3c && ok3d;
    core_test_set_clock(NULL);
    return g_ok;
}

/* ------------------------------------------------------------------------------------------------ C-4 registry */
static volatile LONG g_spin, g_spin_stop;
static DWORD WINAPI c4_spinner(LPVOID p) { (void)p; while (!g_spin_stop) InterlockedIncrement(&g_spin); return 0; }
static HANDLE g_park_ev;
static DWORD WINAPI c4_parked(LPVOID p) { (void)p; WaitForSingleObject(g_park_ev, 60000); return 0; }
static volatile LONG g_writer_ok = -1;
static DWORD WINAPI c4_writer(LPVOID p)                                 /* a code write from a thread that did not open the batch */
{
    static const uint8_t b[4] = { 0x11, 0x22, 0x33, 0x44 };
    g_writer_ok = core_patch_code(CORE_MOD_CORE, "t batch other thread", p, 4, NULL, b);
    return 0;
}
static core_susp_stats_t c4_stats(void) { core_susp_stats_t s; core_patch_suspend_stats(&s); return s; }
static long c4_walks(void) { return c4_stats().walks; }
static DWORD c4_handles(void) { DWORD n = 0; GetProcessHandleCount(GetCurrentProcess(), &n); return n; }
static void c4_stop_spinner(HANDLE sp)                                  /* a registry that never resumes must fail here, not hang */
{
    g_spin_stop = 1;
    if (!sp) return;
    DWORD c; do c = ResumeThread(sp); while (c != (DWORD)-1 && c > 1);
    CHECK(WaitForSingleObject(sp, 5000) == WAIT_OBJECT_0); CloseHandle(sp);
}
static HANDLE c4_jmp_self(uint8_t *at)                                  /* a thread spinning on `jmp $` at `at` */
{
    at[0] = 0xEB; at[1] = 0xFE;
    FlushInstructionCache(GetCurrentProcess(), at, 2);
    HANDLE h = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)at, NULL, 0, NULL);
    Sleep(30);                                                           /* it is now spinning at `at` */
    return h;
}

static int c4_registry(const char *self_path)
{
    g_ok = 1;
    char path[MAX_PATH]; tmp_path(path, sizeof path, "patch.log");
    DeleteFileA(path);
    core_test_sink_file(path);
    core_patch_t e;
    uint8_t *pg = (uint8_t *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memset(pg, 0xCC, 4096);
    int base = core_patch_count();
    CHECK(base == 0);
    /* code write */
    static const uint8_t b5[5] = { 0xE9, 1, 2, 3, 4 }, b4[4] = { 0x11, 0x22, 0x33, 0x44 }, b1[1] = { 0x90 };
    CHECK(core_patch_code(CORE_MOD_CHASE, "t code", pg, 5, NULL, b5) == 1);
    CHECK(!memcmp(pg, b5, 5));
    CHECK(core_patch_get(base, &e) && e.state == CORE_PATCH_WRITTEN && e.kind == CORE_PATCH_CODE && e.mod == CORE_MOD_CHASE && e.len == 5
          && e.addr == pg && !strcmp(e.name, "t code") && e.orig[0] == 0xCC && e.orig[4] == 0xCC && !memcmp(e.now, b5, 5));
    /* overlap (another module): refused, memory unchanged */
    CHECK(core_patch_code(CORE_MOD_PEN, "t overlap", pg + 3, 4, NULL, b4) == 0);
    CHECK(pg[3] == 3 && pg[4] == 4 && pg[5] == 0xCC && pg[6] == 0xCC);
    CHECK(core_patch_get(base + 1, &e) && e.state == CORE_PATCH_REFUSED_OVERLAP);
    CHECK(core_patch_data(CORE_MOD_STANCE, "t overlap data", pg + 4, 1, NULL, b1) == 0 && pg[4] == 4);
    /* expected bytes */
    static const uint8_t ex_bad[4] = { 0, 0, 0, 0 }, ex_ok[4] = { 0xCC, 0xCC, 0xCC, 0xCC };
    CHECK(core_patch_code(CORE_MOD_STANCE, "t bytes", pg + 16, 4, ex_bad, b4) == 0);
    CHECK(pg[16] == 0xCC && pg[19] == 0xCC);
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_REFUSED_BYTES);
    CHECK(core_patch_code(CORE_MOD_STANCE, "t bytes ok", pg + 16, 4, ex_ok, b4) == 1 && !memcmp(pg + 16, b4, 4));
    /* pointer writes: expected value, old value */
    void **slot = (void **)(pg + 64), *old = NULL;
    *slot = (void *)(uintptr_t)0x1111;
    CHECK(core_patch_ptr(CORE_MOD_STANCE, "t ptr bad", slot, (void *)(uintptr_t)0x2222, (void *)(uintptr_t)0x3333, &old) == 0);
    CHECK(*slot == (void *)(uintptr_t)0x1111 && old == NULL);
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_REFUSED_BYTES && e.kind == CORE_PATCH_PTR);
    CHECK(core_patch_ptr(CORE_MOD_STANCE, "t ptr", slot, (void *)(uintptr_t)0x1111, (void *)(uintptr_t)0x3333, &old) == 1);
    CHECK(old == (void *)(uintptr_t)0x1111 && *slot == (void *)(uintptr_t)0x3333);
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_WRITTEN && e.kind == CORE_PATCH_PTR && e.len == 8
          && *(uint64_t *)e.orig == 0x1111 && *(uint64_t *)e.now == 0x3333);
    void **slot2 = (void **)(pg + 72); *slot2 = (void *)(uintptr_t)0x4444;
    CHECK(core_patch_ptr(CORE_MOD_CHASE, "t ptr2", slot2, NULL, (void *)(uintptr_t)0x5555, NULL) == 1 && *slot2 == (void *)(uintptr_t)0x5555);
    /* data write */
    static const float van[3] = { 17.0f, 29.0f, 35.0f }, nw[3] = { 45.0f, 45.0f, 45.0f };
    memcpy(pg + 128, van, 12);
    CHECK(core_patch_data(CORE_MOD_STANCE, "t data", pg + 128, 12, van, nw) == 1 && !memcmp(pg + 128, nw, 12));
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.kind == CORE_PATCH_DATA && e.state == CORE_PATCH_WRITTEN);
    /* bad lengths: REFUSED_FULL */
    uint8_t b33[33]; memset(b33, 0x90, sizeof b33);
    CHECK(core_patch_code(CORE_MOD_CORE, "t len0", pg + 200, 0, NULL, b4) == 0);
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_REFUSED_FULL);
    CHECK(core_patch_code(CORE_MOD_CORE, "t len33", pg + 256, 33, NULL, b33) == 0 && pg[256] == 0xCC);
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_REFUSED_FULL);
    /* FAILED: a FILE_MAP_READ view of a read-only file (this exe), which VirtualProtect cannot make writable */
    HANDLE fh = CreateFileA(self_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE mh = fh != INVALID_HANDLE_VALUE ? CreateFileMappingA(fh, NULL, PAGE_READONLY, 0, 0, NULL) : NULL;
    uint8_t *view = mh ? (uint8_t *)MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0) : NULL;
    CHECK(view != NULL);
    if (view) {
        uint8_t keep[8]; memcpy(keep, view + 0x40, 8);
        CHECK(core_patch_code(CORE_MOD_CORE, "t ro code", view + 0x40, 4, NULL, b4) == 0);
        CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_FAILED);
        CHECK(core_patch_data(CORE_MOD_CORE, "t ro data", view + 0x44, 4, NULL, b4) == 0);
        CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_FAILED);
        CHECK(core_patch_ptr(CORE_MOD_CORE, "t ro ptr", (void **)(view + 0x48), NULL, (void *)(uintptr_t)1, NULL) == 0);
        CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_FAILED);
        CHECK(!memcmp(keep, view + 0x40, 8));
        UnmapViewOfFile(view);
    }
    if (mh) CloseHandle(mh);
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    /* thread suspension (WP0_verify n1, [Core] PatchSuspendThreads=1 default): a thread running elsewhere is suspended for the write
     * and resumed after it (its counter keeps moving); a thread that keeps executing inside the target range makes the write FAILED,
     * bytes unchanged */
    CHECK(core_patch_suspend_on() == 1);
    g_spin_stop = 0; g_spin = 0;
    HANDLE sp = CreateThread(NULL, 0, c4_spinner, NULL, 0, NULL);
    CHECK(sp != NULL);
    while (sp && g_spin < 1000) YieldProcessor();
    long w0 = c4_walks();
    CHECK(core_patch_code(CORE_MOD_CORE, "t susp spin", pg + 600, 4, NULL, b4) == 1 && !memcmp(pg + 600, b4, 4));
    CHECK(c4_walks() == w0 + 1);                                         /* outside a batch: one thread walk for this write */
    LONG s0 = g_spin; Sleep(50);
    CHECK(g_spin > s0);                                                  /* resumed */
    HANDLE lp = c4_jmp_self(pg + 512);
    CHECK(lp != NULL);
    static const uint8_t b2[2] = { 0x90, 0x90 };
    w0 = c4_walks();
    CHECK(core_patch_code(CORE_MOD_CORE, "t susp inside", pg + 512, 2, NULL, b2) == 0 && pg[512] == 0xEB && pg[513] == 0xFE);
    CHECK(c4_walks() == w0 + 20);                                        /* the bounded retry: 20 attempts, a walk each */
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_FAILED);
    CHECK(count_sub(path, "FAILED (could not stop the other threads safely): core t susp inside") == 1);
    LONG s1 = g_spin; Sleep(50);
    CHECK(g_spin > s1);                                                  /* the spinner was resumed after the failed attempts too */
    if (lp) { TerminateThread(lp, 0); WaitForSingleObject(lp, 5000); CloseHandle(lp); }   /* the test's own `jmp $` thread */
    c4_stop_spinner(sp);

    /* the same cases inside the attach batch (STARTUP_batch, review INST-1). As in DllMain, every thread exists before the batch's
     * first code write (under the loader lock no thread created during the attach runs user code before it returns) */
    g_spin_stop = 0; g_spin = 0;
    sp = CreateThread(NULL, 0, c4_spinner, NULL, 0, NULL);
    CHECK(sp != NULL);
    while (sp && g_spin < 1000) YieldProcessor();
    HANDLE lb = c4_jmp_self(pg + 520);
    CHECK(lb != NULL);
    DWORD h0 = c4_handles();
    core_susp_stats_t st0 = c4_stats();
    core_patch_batch_begin();
    CHECK(c4_walks() == st0.walks && c4_stats().open == 0);              /* the walk is lazy: none before the first code write */
    CHECK(core_patch_code(CORE_MOD_CORE, "t batch spin", pg + 608, 4, NULL, b4) == 1 && !memcmp(pg + 608, b4, 4));
    core_susp_stats_t st1 = c4_stats();
    CHECK(st1.walks == st0.walks + 1 && st1.open >= 2 && c4_handles() >= h0 + 2);   /* one walk; it keeps the other threads' handles */
    s0 = g_spin; Sleep(50);
    CHECK(g_spin > s0);                                                  /* resumed after a good write */
    DWORD t0 = GetTickCount();
    CHECK(core_patch_code(CORE_MOD_CORE, "t batch inside", pg + 520, 2, NULL, b2) == 0 && pg[520] == 0xEB && pg[521] == 0xFE);
    CHECK(GetTickCount() - t0 < 5000);                                   /* bounded: 20 attempts on the kept handles */
    CHECK(core_patch_get(core_patch_count() - 1, &e) && e.state == CORE_PATCH_FAILED);
    CHECK(count_sub(path, "FAILED (could not stop the other threads safely): core t batch inside") == 1);
    s1 = g_spin; Sleep(50);
    CHECK(g_spin > s1);                                                  /* resumed after a FAILED write */
    if (lb) { TerminateThread(lb, 0); CHECK(WaitForSingleObject(lb, 5000) == WAIT_OBJECT_0); }   /* exits between two writes (handle kept) */
    CHECK(core_patch_code(CORE_MOD_CORE, "t batch after exit", pg + 616, 4, NULL, b4) == 1 && !memcmp(pg + 616, b4, 4));
    CHECK(core_patch_code(CORE_MOD_CORE, "t batch more", pg + 624, 4, NULL, b4) == 1 && !memcmp(pg + 624, b4, 4));
    LONG s2 = g_spin; Sleep(50);
    CHECK(g_spin > s2);
    CHECK(c4_walks() == st0.walks + 1);                                  /* ONE walk for the batch's 4 code writes */
    HANDLE wt = CreateThread(NULL, 0, c4_writer, pg + 680, 0, NULL);     /* another thread's write takes its own walk (the kept handles */
    CHECK(wt != NULL);                                                   /* are the opener's: they hold neither that thread nor the opener) */
    if (wt) { CHECK(WaitForSingleObject(wt, 10000) == WAIT_OBJECT_0); CloseHandle(wt); }
    CHECK(g_writer_ok == 1 && !memcmp(pg + 680, b4, 4) && c4_walks() == st0.walks + 2);
    core_patch_batch_end();
    core_susp_stats_t st2 = c4_stats();
    CHECK(st2.open == 0 && c4_handles() == h0);                          /* batch_end closed every handle the walk opened */
    CHECK(st2.batched == st0.batched + 4 && st2.susp == st0.susp + 4 && st2.failed == st0.failed + 1 && st2.batches == st0.batches + 1);
    CHECK(st2.exited >= st0.exited + 1);                                 /* the `jmp $` thread: skipped once it had exited, never resumed */
    if (sp) { CHECK(SuspendThread(sp) == 0); ResumeThread(sp); }         /* nothing left the spinner suspended */
    if (lb) CloseHandle(lb);
    w0 = c4_walks();                                                     /* outside a batch again: one walk per write */
    CHECK(core_patch_code(CORE_MOD_CORE, "t walk 1", pg + 632, 4, NULL, b4) == 1 && core_patch_code(CORE_MOD_CORE, "t walk 2", pg + 640, 4, NULL, b4) == 1);
    CHECK(c4_walks() == w0 + 2 && c4_stats().batched == st2.batched);
    c4_stop_spinner(sp);

    /* more other threads than the table holds (1024): FAILED, nothing written, as before the batch; inside a batch a failed walk is
     * tried again at the next write (as each write walked for itself); nothing stays open */
    enum { C4_PARK = 1030 };
    static HANDLE pk[C4_PARK];
    int np = 0;
    g_park_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(g_park_ev != NULL);
    for (int i = 0; g_park_ev && i < C4_PARK; i++)
        if ((pk[np] = CreateThread(NULL, 64 * 1024, c4_parked, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL)) != NULL) np++;
    CHECK(np == C4_PARK);
    DWORD hp = c4_handles();
    w0 = c4_walks();
    CHECK(core_patch_code(CORE_MOD_CORE, "t over", pg + 648, 4, NULL, b4) == 0 && pg[648] == 0xCC);
    CHECK(c4_walks() == w0 + 1);
    core_patch_batch_begin();
    CHECK(core_patch_code(CORE_MOD_CORE, "t batch over a", pg + 656, 4, NULL, b4) == 0 && pg[656] == 0xCC);
    CHECK(core_patch_code(CORE_MOD_CORE, "t batch over b", pg + 664, 4, NULL, b4) == 0 && pg[664] == 0xCC);
    CHECK(c4_walks() == w0 + 3 && c4_stats().open == 0);
    core_patch_batch_end();
    CHECK(c4_handles() == hp);
    CHECK(count_sub(path, "FAILED (could not stop the other threads safely): core t over") == 1);
    CHECK(count_sub(path, "FAILED (could not stop the other threads safely): core t batch over a") == 1);
    CHECK(count_sub(path, "FAILED (could not stop the other threads safely): core t batch over b") == 1);
    if (g_park_ev) SetEvent(g_park_ev);
    for (int i = 0; i < np; i++) { CHECK(WaitForSingleObject(pk[i], 10000) == WAIT_OBJECT_0); CloseHandle(pk[i]); }
    if (g_park_ev) CloseHandle(g_park_ev);

    /* report: one summary line + one line per entry */
    int before = count_lines(path), cnt = core_patch_count();
    core_patch_report();
    CHECK(count_lines(path) - before == 1 + cnt);
    CHECK(cnt == 26 && count_sub(path, "patches: 12 written, 14 refused/failed") == 1);
    CHECK(count_sub(path, "REFUSED_OVERLAP") >= 2 && count_sub(path, "REFUSED (bytes differ: expected") == 2);
    /* writes after the report are logged as they happen; the table holds CORE_PATCH_MAX entries */
    int fill = 0;
    while (core_patch_count() < CORE_PATCH_MAX) fill += core_patch_code(CORE_MOD_CORE, "t fill", pg + 1024 + 2 * core_patch_count(), 1, NULL, b1);
    CHECK(fill == CORE_PATCH_MAX - cnt && count_sub(path, "runtime patch #") == fill);
    CHECK(core_patch_code(CORE_MOD_CORE, "t full", pg + 3000, 1, NULL, b1) == 0 && pg[3000] == 0xCC);
    CHECK(core_patch_count() == CORE_PATCH_MAX && count_sub(path, "patch REFUSED (table full") == 1);
    CHECK(!core_patch_get(CORE_PATCH_MAX, &e) && !core_patch_get(-1, &e));
    core_test_sink(CORE_SINK_DISCARD);
    return report("C-4 registry");
}

/* ------------------------------------------------------------------------------------------------ C-5 ceasefire */
static void *g_cf_w; static uint64_t g_cf_a; static int g_cf_n;
static uint64_t fake_stop(void *w, uint64_t a) { g_cf_w = w; g_cf_a = a; g_cf_n++; return 0xABCDu; }
static void *g_veto_w; static int g_veto_mod = -1;
static int fake_veto(int mod, void *w) { g_veto_mod = mod; return w == g_veto_w; }

static int c5_ceasefire(void)
{
    g_ok = 1;
    static int w1, w2, w3, wv, wc;
    g_fake = 500; core_test_set_clock(&g_fake);
    uint64_t r = 0;
    CHECK(core_ceasefire(CORE_MOD_PEN, NULL, (void *)fake_stop, &r) == 0);
    CHECK(core_ceasefire(CORE_MOD_PEN, &w1, NULL, &r) == 0 && g_cf_n == 0);
    CHECK(core_ceasefire(CORE_MOD_PEN, &w1, (void *)fake_stop, &r) == 1 && g_cf_n == 1 && g_cf_w == &w1 && g_cf_a == 1 && r == 0xABCD);
    CHECK(core_ceasefire(CORE_MOD_CHASE, &w2, (void *)fake_stop, NULL) == 1 && g_cf_n == 2 && g_cf_w == &w2);
    core_link_hooks_t h; memset(&h, 0, sizeof h); h.ceasefire_veto = fake_veto; g_veto_w = &wv;
    core_set_link_hooks(&h);
    r = 7;
    CHECK(core_ceasefire(CORE_MOD_PEN, &wv, (void *)fake_stop, &r) == 0 && g_cf_n == 2 && r == 7 && g_veto_mod == CORE_MOD_PEN);
    CHECK(core_ceasefire(CORE_MOD_PEN, &w3, (void *)fake_stop, &r) == 1 && g_cf_n == 3 && g_cf_w == &w3);
    /* collision: one weapon stopped by two modules under one clock value */
    long calls[CORE_MOD_N], vetoed[CORE_MOD_N], coll0, coll;
    core_ceasefire_stats(calls, vetoed, &coll0);
    g_fake = 600;
    core_ceasefire(CORE_MOD_CHASE, &wc, (void *)fake_stop, NULL);
    core_ceasefire(CORE_MOD_PEN, &wc, (void *)fake_stop, NULL);          /* +1 */
    core_ceasefire(CORE_MOD_PEN, &wc, (void *)fake_stop, NULL);          /* same module as the last: no */
    g_fake = 601;
    core_ceasefire(CORE_MOD_CHASE, &wc, (void *)fake_stop, NULL);        /* another clock value: no */
    core_ceasefire_stats(calls, vetoed, &coll);
    CHECK(coll == coll0 + 1);
    CHECK(calls[CORE_MOD_PEN] == 4 && calls[CORE_MOD_CHASE] == 3 && vetoed[CORE_MOD_PEN] == 1 && vetoed[CORE_MOD_CHASE] == 0);
    core_set_link_hooks(NULL);
    CHECK(core_ceasefire(CORE_MOD_PEN, &wv, (void *)fake_stop, NULL) == 1);   /* no hooks: never vetoed */
    core_ceasefire_stats(NULL, NULL, NULL);
    core_test_set_clock(NULL);
    return report("C-5 ceasefire");
}

/* ------------------------------------------------------------------------------------------------ C-6 unit info */
static const void *g_traced;
static int fake_trace_unit(const void *u) { return u == g_traced; }

static int c6_unit(void)
{
    g_ok = 1;
    core_unit_t u;
    uint8_t *e = (uint8_t *)calloc(1, 0x2000), *br = (uint8_t *)calloc(1, 0x400), *pr = (uint8_t *)calloc(1, 0x100), *dd = (uint8_t *)calloc(1, 0x100);
    e[0x30] = 6; e[0xDA8] = 3; *(uint8_t **)(e + 0xDC8) = br; br[0x398] = 1;
    *(uint8_t **)(e + 0xDE0) = pr; *(int32_t *)(pr + 0x18) = 1;
    *(uint32_t *)(e + 0x1C0) = 1u << 28; *(uint32_t *)(e + 0xC0) = 0xABCD;
    g_fake = 1000; core_test_set_clock(&g_fake);
    CHECK(core_unit(NULL, &u) == 0);
    CHECK(core_unit(e, &u) == 1);
    CHECK(u.e == e && u.clock == 1000 && u.human == 1 && u.actor == 1 && u.vehicle == 0 && u.owner == 3 && u.player == 1 && u.dc_sim == 1
          && u.linked == 1 && u.id == 0xABCD && u.traced == 0);
    e[0xDA8] = 9; br[0x398] = 2;
    CHECK(core_unit(e, &u) == 1 && u.owner == 3 && u.player == 1);       /* cache hit under the same clock */
    g_fake = 1001;
    CHECK(core_unit(e, &u) == 1 && u.owner == 9 && u.player == 0 && u.clock == 1001);   /* re-read for another clock */
    /* vehicle with the DD8 bit, DE0 NULL */
    uint8_t *v = (uint8_t *)calloc(1, 0x2000);
    v[0x30] = 2; *(uint8_t **)(v + 0xDD8) = dd; *(uint32_t *)(dd + 0x70) = 2; *(uint32_t *)(v + 0xC0) = 0x77;
    CHECK(core_unit(v, &u) == 1 && u.vehicle == 1 && u.human == 0 && u.actor == 1 && u.dc_sim == 1 && u.player == 0 && u.linked == 0 && u.id == 0x77);
    /* dc_sim needs the actor bit; property 0 and DD8 bit 0 only = not under direct control */
    uint8_t *h = (uint8_t *)calloc(1, 0x2000);
    h[0x30] = 4; *(uint8_t **)(h + 0xDE0) = pr;
    CHECK(core_unit(h, &u) == 1 && u.human == 1 && u.actor == 0 && u.vehicle == 0 && u.dc_sim == 0);
    uint8_t *v2 = (uint8_t *)calloc(1, 0x2000), *pr0 = (uint8_t *)calloc(1, 0x100), *dd1 = (uint8_t *)calloc(1, 0x100);
    v2[0x30] = 2; *(uint8_t **)(v2 + 0xDE0) = pr0; *(uint8_t **)(v2 + 0xDD8) = dd1; *(uint32_t *)(dd1 + 0x70) = 1;
    CHECK(core_unit(v2, &u) == 1 && u.dc_sim == 0 && u.vehicle == 1);
    /* traced: link's trace_unit decides */
    core_link_hooks_t hk; memset(&hk, 0, sizeof hk); hk.trace_unit = fake_trace_unit; g_traced = e;
    core_set_link_hooks(&hk);
    g_fake = 1002;
    CHECK(core_unit(e, &u) == 1 && u.traced == 1);
    CHECK(core_unit(v, &u) == 1 && u.traced == 0);
    core_set_link_hooks(NULL);
    core_test_set_clock(NULL);
    return report("C-6 unit info");
}

/* ------------------------------------------------------------------------------------------------ C-7 / C-7b tag vectors */
static uint8_t g_tags[0x60 * 8];
static void vec_set(uint8_t *v, const uint8_t *b, const uint8_t *e) { *(const uint8_t **)v = b; *(const uint8_t **)(v + 8) = e; }

static int c7_vec(void)
{
    g_ok = 1;
    uint8_t v[16];
    memset(g_tags, 0, sizeof g_tags);
    fake_str(g_tags + 0x40, "Pierce");                                   /* SSO */
    fake_str(g_tags + 0x60 + 0x40, "a_much_longer_tag_name_heap");       /* heap */
    fake_str(g_tags + 0xC0 + 0x40, "bolt");
    vec_set(v, g_tags, g_tags + 0x60 * 3);
    CHECK(core_vec_has_ci(NULL, "x") == 0);
    CHECK(core_vec_has_ci(v, NULL) == 0);
    CHECK(core_vec_has_ci(v, "pierce") == 1 && core_vec_has_ci(v, "PIERCE") == 1);
    CHECK(core_vec_has_ci(v, "A_Much_Longer_Tag_Name_Heap") == 1);
    CHECK(core_vec_has_ci(v, "bolt") == 1 && core_vec_has_ci(v, "he") == 0 && core_vec_has_ci(v, "pierc") == 0 && core_vec_has_ci(v, "pierce2") == 0);
    vec_set(v, NULL, g_tags); CHECK(core_vec_has_ci(v, "bolt") == 0);
    vec_set(v, g_tags, NULL); CHECK(core_vec_has_ci(v, "bolt") == 0);
    vec_set(v, g_tags, g_tags + 0x50); CHECK(core_vec_has_ci(v, "pierce") == -1);           /* len % 0x60 */
    vec_set(v, g_tags + 0x60, g_tags); CHECK(core_vec_has_ci(v, "pierce") == -1);           /* end < begin */
    vec_set(v, g_tags, g_tags); CHECK(core_vec_has_ci(v, "pierce") == 0);                   /* empty */
    int ok7 = g_ok;

    g_ok = 1;
    char out[64];
    memset(out, 'Z', sizeof out); CHECK(core_vec_names(NULL, out, sizeof out) == 0 && out[0] == 0);
    vec_set(v, g_tags, g_tags); memset(out, 'Z', sizeof out); CHECK(core_vec_names(v, out, sizeof out) == 0 && out[0] == 0);
    vec_set(v, NULL, NULL); memset(out, 'Z', sizeof out); CHECK(core_vec_names(v, out, sizeof out) == 0 && out[0] == 0);
    vec_set(v, g_tags, g_tags + 0x50); memset(out, 'Z', sizeof out); CHECK(core_vec_names(v, out, sizeof out) == 0 && out[0] == 0);
    vec_set(v, g_tags, g_tags + 0x60 * 65); memset(out, 'Z', sizeof out); CHECK(core_vec_names(v, out, sizeof out) == 0 && out[0] == 0);  /* > 64 names */
    /* entry 3: broken header (cap < size): skipped */
    memset(g_tags + 0x120, 0, 0x60);
    *(uint64_t *)(g_tags + 0x120 + 0x40 + 0x10) = 5; *(uint64_t *)(g_tags + 0x120 + 0x40 + 0x18) = 3;
    fake_str(g_tags + 0x180 + 0x40, "Melee");
    vec_set(v, g_tags, g_tags + 0x60 * 5);
    CHECK(core_vec_names(v, out, sizeof out) == 4 && !strcmp(out, "Pierce a_much_longer_tag_name_heap bolt Melee"));
    char small[32]; memset(small, 'Z', sizeof small);
    CHECK(core_vec_names(v, small, 9) == 1 && !strcmp(small, "Pierce"));
    int canary = 1; for (int i = 9; i < 32; i++) if (small[i] != 'Z') canary = 0;
    CHECK(canary);
    memset(small, 'Z', sizeof small);
    CHECK(core_vec_names(v, small, 8) == 0 && small[0] == 0 && small[1] == 'Z');
    CHECK(core_vec_names(v, small, 1) == 0 && small[0] == 0);
    CHECK(core_vec_names(v, small, 0) == 0);
    int ok7b = g_ok;
    printf("C-7 vec_has_ci: %s\nC-7b vec_names: %s\n", ok7 ? "OK" : "FAIL", ok7b ? "OK" : "FAIL");
    g_ok = ok7 && ok7b;
    return g_ok;
}

/* ------------------------------------------------------------------------------------------------ C-8 timing / bg */
static int g_order[16], g_norder;
#define BGF(n) static void bg##n(void) { if (g_norder < 16) g_order[g_norder++] = n; }
BGF(0) BGF(1) BGF(2) BGF(3) BGF(4) BGF(5) BGF(6) BGF(7) BGF(8)

static int c8_timing(void)
{
    g_ok = 1;
    int64_t q; long n;
    core_time_read(CORE_T_PEN_A, &q, &n); CHECK(q == 0 && n == 0);
    core_time_add(CORE_T_PEN_A, 100); core_time_add(CORE_T_PEN_A, 250);
    core_time_read(CORE_T_PEN_A, &q, &n); CHECK(q == 350 && n == 2);
    core_time_add(CORE_T_N, 5); core_time_add(-1, 5); core_time_add(99, 5);
    core_time_read(CORE_T_PEN_A, &q, &n); CHECK(q == 350 && n == 2);
    core_time_read(99, &q, &n); CHECK(q == 0 && n == 0);
    CHECK(core_timing_on == 0);
    { CORE_TIME_BEGIN(t0); CHECK(t0 == 0); CORE_TIME_END(CORE_T_PEN_A, t0); }
    core_time_read(CORE_T_PEN_A, &q, &n); CHECK(n == 2);
    core_timing_on = 1;
    { CORE_TIME_BEGIN(t1); CHECK(t1 != 0); CORE_TIME_END(CORE_T_PEN_A, t1); }
    core_timing_on = 0;
    core_time_read(CORE_T_PEN_A, &q, &n); CHECK(n == 3 && q >= 350);
    static core_bg_fn fns[9] = { bg0, bg1, bg2, bg3, bg4, bg5, bg6, bg7, bg8 };
    CHECK(core_bg_count() == 0);
    for (int i = 0; i < 8; i++) CHECK(core_bg_register(CORE_MOD_LINK, fns[i]) == 1);
    CHECK(core_bg_register(CORE_MOD_LINK, fns[8]) == 0);
    CHECK(core_bg_register(CORE_MOD_LINK, NULL) == 0);
    CHECK(core_bg_count() == 8);
    core_bg_run_once();
    CHECK(g_norder == 8);
    for (int i = 0; i < 8 && i < g_norder; i++) CHECK(g_order[i] == i);
    return report("C-8 timing / bg");
}

/* ------------------------------------------------------------------------------------------------ C-9 clock finder */
static int c9_clock(int nexe, char **exes)
{
    g_ok = 1;
    static const struct { const char *name; uint32_t rva; } want[3] = {
        { "call_to_arms.exe", 0x1584568 }, { "call_to_arms_ed.exe", 0x13DF988 }, { "call_to_arms_server.exe", 0x11CBA58 } };
    for (int i = 0; i < nexe; i++) {
        const char *nm = exes[i] + strlen(exes[i]);
        while (nm > exes[i] && nm[-1] != '\\' && nm[-1] != '/') nm--;
        int w = -1;
        for (int k = 0; k < 3; k++) if (!_stricmp(nm, want[k].name)) w = k;
        HMODULE m = LoadLibraryExA(exes[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("    %s: load failed %lu\n", exes[i], GetLastError()); g_ok = 0; continue; }
        int hits = 0;
        volatile uint32_t *c = core_clock_find((const uint8_t *)m, &hits);
        unsigned long long rva = c ? (unsigned long long)((const uint8_t *)(uintptr_t)c - (const uint8_t *)m) : 0;
        printf("  %s: clock exe+0x%llX (%d getter calls)%s\n", nm, rva, hits, w < 0 ? " (unknown exe: not checked)" : "");
        if (w >= 0) CHECK(c && rva == want[w].rva);
        FreeLibrary(m);
    }
    CHECK(nexe == 3);
    return report("C-9 clock finder");
}

/* ------------------------------------------------------------------------------------------------ C-10 crash log (last) */
static LONG CALLBACK c10_continue(EXCEPTION_POINTERS *ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    return c == 0xC0000094 || c == 0xE06D7363 ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static int c10_crash(const char *self_path)
{
    g_ok = 1;
    const char *nm = strrchr(self_path, '\\'); nm = nm ? nm + 1 : self_path;
    core_test_set_exe((uint8_t *)GetModuleHandleW(NULL), nm);
    char path[MAX_PATH]; tmp_path(path, sizeof path, "crash.log");
    DeleteFileA(path);
    core_test_sink_file(path);
    core_log_fixed(CORE_MOD_CORE, "c10 before any normal line");          /* the crash-path writer does not open the file (RP-5): dropped */
    core_log(CORE_MOD_CORE, "c10 start");                                /* as core_init's banner: the normal writer opens it */
    PVOID h = AddVectoredExceptionHandler(0, c10_continue);
    CHECK(h != NULL);
    core_crash_install();
    RaiseException(0xC0000094, 0, 0, NULL);
    RaiseException(0xE06D7363, 0, 0, NULL);
    core_test_sink(CORE_SINK_DISCARD);
    CHECK(count_sub(path, "crash log                      ON") == 1);
    CHECK(count_sub(path, "EXCEPTION 0xC0000094 at") == 1);
    CHECK(count_sub(path, "C++ exception thrown") == 1);
    CHECK(count_sub(path, "exit trace") == 1);
    CHECK(count_sub(path, "c10 before any normal line") == 0 && count_sub(path, "c10 start") == 1);
    return report("C-10 crash log");
}

int test_core_main(int argc, char **argv)
{
    char self[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH) { printf("cannot read my own path\nSOME FAILED\n"); return 1; }
    snprintf(g_tmp, sizeof g_tmp, "%s", self);
    char *bs = strrchr(g_tmp, '\\'); if (bs) *bs = 0;
    strncat(g_tmp, "\\tmp", sizeof g_tmp - strlen(g_tmp) - 1);
    CreateDirectoryA(g_tmp, NULL);
    char defs[3][MAX_PATH], *exes[3];
    int nexe = argc - 1;
    if (nexe > 3) nexe = 3;
    if (nexe < 1) {
        const char *bin = getenv("GOH_BIN");
        if (!bin || !*bin) bin = "C:/Program Files (x86)/Steam/steamapps/common/Call to Arms - Gates of Hell/binaries/x64";
        static const char *const nm[3] = { "call_to_arms.exe", "call_to_arms_ed.exe", "call_to_arms_server.exe" };
        for (int i = 0; i < 3; i++) { snprintf(defs[i], MAX_PATH, "%s/%s", bin, nm[i]); exes[i] = defs[i]; }
        nexe = 3;
    } else for (int i = 0; i < nexe; i++) exes[i] = argv[i + 1];
    printf("gem_melee core self-test (" GEM_MELEE_NAME " " GEM_MELEE_VERSION ")\n");
    int all = 1;
    all &= c1_sink();
    all &= c2_ini();
    all &= c3_epoch();
    all &= c4_registry(self);
    all &= c5_ceasefire();
    all &= c6_unit();
    all &= c7_vec();
    all &= c8_timing();
    all &= c9_clock(nexe, exes);
    all &= c10_crash(self);
    printf(all ? "ALL OK\n" : "SOME FAILED\n");
    fflush(stdout);
    return all ? 0 : 1;
}

#ifdef TEST_CORE_STANDALONE
int main(int argc, char **argv) { return test_core_main(argc, argv); }
#endif
