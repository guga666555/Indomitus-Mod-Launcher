/* GEM ScriptExt - core
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * Logging and ini, PE sections, byte/string/RIP-reference search, RTTI vtable lookup.
 * Everything else builds on these.
 */

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
static int g_verbose = 1;
#ifdef SX_TEST
static void logf_(const char *fmt, ...){ va_list ap; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); printf("\n"); }
#else
static FILE *g_log;
static int g_console, g_logfile = 1;
static wchar_t g_dir[MAX_PATH];

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [" SX_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    OutputDebugStringA(line);
}

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, SX_INI);
    wchar_t wkey[64], wdef[128], wout[256]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_ACP, 0, def, -1, wdef, 128);
    GetPrivateProfileStringW(SX_SECT, wkey, wdef, wout, 256, ini);
    if (!WideCharToMultiByte(CP_ACP, 0, wout, -1, out, (int)n, NULL, NULL))   /* too long for out: keep what fits */
        out[n - 1] = 0;
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, SX_INI);
    wchar_t wkey[64]; MultiByteToWideChar(CP_ACP, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(SX_SECT, wkey, def, ini);
}

/* read memory another thread may be changing or freeing: 0 instead of a fault */
static int rd_mem(const void *src, void *dst, size_t n) { SIZE_T got = 0; return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n; }
#endif /* SX_TEST */

/* ------------------------------------------------------------------------------------ */
/* PE helpers                                                                           */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
static uint32_t g_timestamp;

#ifndef SX_TEST
static int get_sections(void)
{
    g_base = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
    g_timestamp = nt->FileHeader.TimeDateStamp;
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { g_base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh[i].Name, ".data", 8)) g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p;
}
#endif /* SX_TEST */

static int in_text(const void *a)  { return (uint8_t *)a >= g_text.p  && (uint8_t *)a < g_text.p  + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }
static int in_data(const void *a)  { return (uint8_t *)a >= g_data.p  && (uint8_t *)a < g_data.p  + g_data.n; }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

/* NUL-terminated string in .rdata that starts at a string boundary */
static uint8_t *find_cstr(const char *s)
{
    size_t m = strlen(s) + 1;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)s, m)); p++)
        if (p == g_rdata.p || p[-1] == 0) return p;
    return NULL;
}

/* bounded search for the next E8 call; returns target */
static uint8_t *next_call(uint8_t *p, size_t window, uint8_t **site)
{
    for (size_t i = 0; i < window; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = p + i + 5 + *(int32_t *)(p + i + 1);
            if (in_text(t)) { if (site) *site = p + i; return t; }
        }
    return NULL;
}

/* k-th RIP-relative 'lea reg,[rip+disp]' in .text whose target == want */
static uint8_t *find_lea_to(const void *want, int which)
{
    int k = 0; uint8_t *from = g_text.p; size_t n = g_text.n;
    for (size_t i = 0; i + 7 <= n; i++) {
        uint8_t r = from[i];
        if ((r & 0xF8) != 0x48 || from[i + 1] != 0x8D || (from[i + 2] & 0xC7) != 0x05) continue;
        uint8_t *t = from + i + 7 + *(int32_t *)(from + i + 3);
        if (t == want) { if (k++ == which) return from + i; }
    }
    return NULL;
}

/* any RIP-relative reference (lea/mov/cmp..., 2-byte opcode with optional REX) to want, within [from, from+n) */
static uint8_t *find_rip_ref(uint8_t *from, size_t n, const void *want)
{
    for (size_t i = 0; i + 6 <= n; i++) {
        size_t j = i;
        if ((from[j] & 0xF0) == 0x40) j++;
        uint8_t op = from[j];
        if (op != 0x8D && op != 0x8B && op != 0x89 && op != 0x39 && op != 0x3B && op != 0x83) continue;
        if ((from[j + 1] & 0xC7) != 0x05) continue;
        uint8_t *t = from + j + 6 + *(int32_t *)(from + j + 2);
        if (op == 0x83) t += 1;                         /* cmp [rip+disp],imm8 has one more byte */
        if (t == want) return from + i;
    }
    return NULL;
}

/* walk back from p to the start of its function = the first byte after the int3 padding before it (NULL if none) */
static uint8_t *func_start(uint8_t *p, size_t maxback)
{
    for (size_t k = 0; k < maxback && p - k > g_text.p; k++) {
        uint8_t *q = p - k;
        if (q[-1] == 0xCC && q[0] != 0xCC) return q;
    }
    return NULL;
}

/* length of the position-independent prologue instructions covering at least 5 bytes, or 0 */
static int prologue_len(const uint8_t *f)
{
    int n = 0;
    while (n < 5) {
        const uint8_t *p = f + n; int len = 0;
        uint8_t rex = (p[0] & 0xF0) == 0x40 ? p[0] : 0; const uint8_t *q = rex ? p + 1 : p;
        if ((q[0] & 0xF8) == 0x50) len = (int)(q - p) + 1;                                          /* push r64 */
        else if ((q[0] == 0x89 || q[0] == 0x8B) && (q[1] & 0xC0) == 0xC0) len = (int)(q - p) + 2;    /* mov r64,r64 */
        else if (q[0] == 0x89 && (q[1] & 0xC7) == 0x44 && q[2] == 0x24) len = (int)(q - p) + 4;      /* mov [rsp+disp8],r64 */
        else if (q[0] == 0x83 && q[1] == 0xEC) len = (int)(q - p) + 3;                              /* sub rsp,imm8 */
        else if (q[0] == 0x81 && q[1] == 0xEC) len = (int)(q - p) + 6;                              /* sub rsp,imm32 */
        else return 0;                                                                              /* anything else (incl. rip-relative) */
        n += len;
    }
    return n <= 15 ? n : 0;
}

/* number of E8 call sites in .text targeting f */
static int count_callers(const uint8_t *f)
{
    int n = 0;
    for (size_t i = 0; i + 5 <= g_text.n; i++)
        if (g_text.p[i] == 0xE8 && g_text.p + i + 5 + *(int32_t *)(g_text.p + i + 1) == f) n++;
    return n;
}

#define EXE_OFF(p) ((unsigned long long)((const uint8_t *)(p) - g_base))

/* ---- RTTI: vtable of a class by its decorated name (.?AVname@ns@@) ---- */
static void **find_vtable_rtti(const char *decorated)
{
    size_t m = strlen(decorated) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = g_data.p; (p = find_bytes(p, g_data.p + g_data.n - p, (const uint8_t *)decorated, m)); p++)
        if (p == g_data.p || p[-1] == 0) { td = p - 16; break; }
    if (!td) return NULL;
    uint32_t tdRva = (uint32_t)(td - g_base);
    /* complete object locator: {sig=1, offset, cdOffset, rva typedesc, rva hierarchy, rva self} */
    for (size_t i = 0; i + 24 <= g_rdata.n; i += 4) {
        uint8_t *col = g_rdata.p + i;
        if (*(uint32_t *)col != 1 || *(uint32_t *)(col + 12) != tdRva || *(uint32_t *)(col + 4) != 0) continue;
        if (g_base + *(uint32_t *)(col + 20) != col) continue;
        for (size_t j = 0; j + 8 <= g_rdata.n; j += 8)
            if (*(uint8_t **)(g_rdata.p + j) == col) return (void **)(g_rdata.p + j + 8);
    }
    return NULL;
}
