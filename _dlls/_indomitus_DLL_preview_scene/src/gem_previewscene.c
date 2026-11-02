/*
 * GEM PreviewScene 0.2.0 (test build)  -  Call to Arms: Gates of Hell (GEM engine, 1.067)
 *
 * Goal: the conquest unit preview shows a map + mission built by the user in the editor, and the unit selected in
 * conquest is spawned there and moves (soldiers walk, vehicles drive with their crew inside, planes fly), driven
 * by the mission's own triggers and waypoints - the way Men of War II's menu background works (its village mission
 * reacts to the event "metagame_unit_changed1" and spawns the selected unit with mp_spawn).
 *
 * 0.2.0 tests whether that can work in Gates of Hell. It still changes nothing unless the ini asks for it:
 *   1. preview_map=<map>[:<mission>]  - the unit preview's scene "library" loads that map/mission instead of nothing
 *      (the descriptor is rebuilt with the engine's own parser, the same one missions use).
 *   2. unit_changed_event=<name>      - when the unit shown in a preview changes, that mission event is set (the
 *      mission must declare it in its Helpers and reset it in its trigger, like Men of War II does).
 *   3. Logging (always): every scene load; the preview's entities and shown unit template; and the mission commands
 *      that run (event, trigger, spawn, squad_create, reinforcement, waypoint, squad/actor_to_waypoint, board,
 *      delete, entity_state, air_state, scenario) - this shows whether the mission in the preview really runs.
 *
 * Engine facts used (game 1.067, call_to_arms.exe PE timestamp 6AB3DCC7; editor 6AB3DD83 for the scene + mission logs):
 *   Scene::eManager vt[5] Load(scene, desc, flag): desc = 3 x {std::string, 8 bytes} at +0x00/+0x28/+0x50
 *     (map, mission, map folder "/map/<map>/"), scene name std::string at +0x860.
 *   MapDesc 0x140A2BDE0(desc, const std::string*): "map:mission" or a path -> desc (frees what desc held before).
 *     Called by the unit preview loader (scene "library") and the conquest view (scene "campaign") with "".
 *   Mission commands (Helper::eCmd_*): vt[7] Execute(cmd, ctx). eCmd_event: Tag name at +0x10, mode +0x78; its
 *     Execute uses the mission event table (global) with EventFind(table, Tag*) and EventSet(table, Tag*, on, delay).
 *   Tag (0x60 bytes): Tag(std::string) / ~Tag, text std::string at +0x40.
 *   eGUI_MultiplayerLibrary: vt[1] OnEvent (0x101D scene ready, 0xFD2 tick), vt[73] Start (render engine at +0x280),
 *     vt[74] Clear (zeroes the shown template at +0x310).
 *   eLibraryRenderEngine::Quant 0x140363300: entries [engine+0x1E8, +0x1F0), stride 0xB0, entity +0xA8,
 *     controller +0xDB8 (Actor::eFsm), its human part +0xE88 (Human::eFsmHandler).
 *
 * Every address and offset is re-derived at startup from RTTI and byte patterns; a feature whose code is not
 * recognised stays OFF and the log says why. Only vtable slots are swapped; no code is patched. All reads of engine
 * memory for logging are checked with VirtualQuery.
 * Build: build.bat (zig cc / mingw-w64).   Offline finder check: tests/run_offline_check.sh
 */
#ifndef PS_TEST
#include <windows.h>
#include <wchar.h>
#else
#include <sys/mman.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define PS_VERSION "0.2.0"
#define PS_NAME    "GEM PreviewScene"
#define PS_INI     L"\\gem_previewscene.ini"
#define PS_SECT    L"previewscene"

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
#ifdef PS_TEST
static void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
static unsigned long long GetTickCount64(void) { return 0; }
#else
static FILE *g_log;
static int g_console;
static wchar_t g_dir[MAX_PATH];

static void logf_(const char *fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    SYSTEMTIME st; GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [" PS_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    OutputDebugStringA(line);
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, PS_INI);
    wchar_t wkey[64]; MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(PS_SECT, wkey, def, ini);
}

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_dir); wcscat(ini, PS_INI);
    wchar_t wkey[64], wdef[128], wout[300];
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 128);
    GetPrivateProfileStringW(PS_SECT, wkey, wdef, wout, 300, ini);
    WideCharToMultiByte(CP_UTF8, 0, wout, -1, out, (int)n, NULL, NULL);
    /* trim spaces and optional quotes */
    size_t L = strlen(out);
    while (L && (out[L - 1] == ' ' || out[L - 1] == '"')) out[--L] = 0;
    size_t k = 0; while (out[k] == ' ' || out[k] == '"') k++;
    if (k) memmove(out, out + k, L - k + 1);
}
#endif

#define LOG_CAPPED(max, ...) do { static int n_; if (n_ < (max)) { n_++; logf_(__VA_ARGS__); } } while (0)
static void log_state(const char *feature, const char *state) { logf_("%-30s %s", feature, state); }

/* ------------------------------------------------------------------------------------ */
/* PE / pattern helpers (same approach as GEM ConquestUI)                               */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
static uint32_t g_timestamp;
#define EXE_OFF(p) ((unsigned long long)((const uint8_t *)(p) - g_base))

#ifndef PS_TEST
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
#endif

static int in_sec(const Sec *s, const void *a, size_t n) { return (const uint8_t *)a >= s->p && (const uint8_t *)a + n <= s->p + s->n; }
static int in_text(const void *a) { return in_sec(&g_text, a, 1); }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

typedef struct { uint8_t b[160], m[160]; int n; } Sig;

static int sig_parse(const char *s, Sig *o)
{
    o->n = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (o->n >= (int)sizeof o->b) return 0;
        if (s[0] == '?') { o->b[o->n] = 0; o->m[o->n] = 0; s += (s[1] == '?') ? 2 : 1; }
        else {
            char h[3] = { s[0], s[1], 0 }; char *e;
            unsigned long v = strtoul(h, &e, 16);
            if (e != h + 2) return 0;
            o->b[o->n] = (uint8_t)v; o->m[o->n] = 0xFF; s += 2;
        }
        o->n++;
    }
    return o->n > 0;
}

static int sig_match(const uint8_t *p, const Sig *s)
{
    for (int i = 0; i < s->n; i++) if ((p[i] & s->m[i]) != s->b[i]) return 0;
    return 1;
}

static uint8_t *scan(const void *from, size_t n, const char *pat, int *count)
{
    Sig s; if (!sig_parse(pat, &s)) return NULL;
    uint8_t *p = (uint8_t *)from, *first = NULL; int c = 0;
    if ((size_t)s.n > n) return NULL;
    for (size_t i = 0; i + s.n <= n; i++)
        if (sig_match(p + i, &s)) { if (!first) first = p + i; c++; if (!count) break; }
    if (count) *count = c;
    return first;
}

static uint8_t *sig_unique(const char *pat, const char *what)
{
    int c = 0; uint8_t *p = scan(g_text.p, g_text.n, pat, &c);
    if (c != 1) { logf_("  %s: pattern matched %d times (need exactly 1)", what, c); return NULL; }
    return p;
}

static int fn_is(const void *f, const char *pat, const char *what)
{
    Sig s;
    if (!f || !in_text(f) || !sig_parse(pat, &s) || !sig_match((const uint8_t *)f, &s)) {
        logf_("  %s: code at exe+0x%llX not recognised", what, f ? EXE_OFF(f) : 0ULL);
        return 0;
    }
    return 1;
}

static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint8_t *rel32_target(const uint8_t *ins, int disp_at, int ins_len) { return (uint8_t *)ins + ins_len + (int32_t)rd32(ins + disp_at); }

static void **find_vtable_rtti(const char *decorated)
{
    size_t m = strlen(decorated) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = g_data.p; (p = find_bytes(p, g_data.p + g_data.n - p, (const uint8_t *)decorated, m)); p++)
        if (p == g_data.p || p[-1] == 0) { td = p - 16; break; }
    if (!td) return NULL;
    uint32_t tdRva = (uint32_t)(td - g_base);
    for (size_t i = 0; i + 24 <= g_rdata.n; i += 4) {
        uint8_t *col = g_rdata.p + i;
        if (rd32(col) != 1 || rd32(col + 12) != tdRva || rd32(col + 4) != 0) continue;
        if (g_base + rd32(col + 20) != col) continue;
        for (size_t j = 0; j + 8 <= g_rdata.n; j += 8)
            if (*(uint8_t **)(g_rdata.p + j) == col) return (void **)(g_rdata.p + j + 8);
    }
    return NULL;
}

static uint8_t *find_cstr(const char *s)
{
    size_t m = strlen(s) + 1;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)s, m)); p++)
        if (p == g_rdata.p || p[-1] == 0) return p;
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* safe reads (this DLL only looks at engine memory; nothing may crash because of it)  */
/* ------------------------------------------------------------------------------------ */
typedef struct { union { char buf[16]; const char *ptr; } u; size_t size, cap; } MsString;   /* MSVC std::string */

#define VT(o)        (*(void ***)(o))
#define PTR(o, off)  (*(uint8_t **)((uint8_t *)(o) + (off)))
#define U32(o, off)  (*(uint32_t *)((uint8_t *)(o) + (off)))

#ifndef PS_TEST
/* committed, readable memory for [p, p+n) */
static int readable(const void *p, size_t n)
{
    if (!p || (uintptr_t)p < 0x10000) return 0;
    const uint8_t *a = (const uint8_t *)p, *end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
        if (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        if (!(mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return 0;
        a = (const uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    return 1;
}
#else
static int readable(const void *p, size_t n) { (void)n; return p != NULL; }
#endif

/* read-only std::string view of a C string (the engine only reads / copies it) */
static void ms_view(MsString *s, const char *c)
{
    size_t n = strlen(c);
    memset(s, 0, sizeof *s);
    if (n < 16) { memcpy(s->u.buf, c, n); s->cap = 15; }
    else { s->u.ptr = c; s->cap = n; }
    s->size = n;
}

/* copy a std::string into out (quoted, printable), or "<?>" when it does not look like one */
static void ms_read(const void *s, char *out, size_t n)
{
    snprintf(out, n, "<?>");
    if (!readable(s, sizeof(MsString))) return;
    const MsString *m = (const MsString *)s;
    if (m->size > m->cap || m->cap < 15 || m->size > 4096) return;
    const char *c = m->cap > 15 ? m->u.ptr : m->u.buf;
    if (m->cap > 15 && !readable(c, m->size + 1)) return;
    size_t k = 0;
    out[k++] = '"';
    for (size_t i = 0; i < m->size && k + 3 < n; i++) out[k++] = (c[i] >= 32 && c[i] < 127) ? c[i] : '?';
    out[k++] = '"'; out[k] = 0;
}

/* class name of a polymorphic object from its RTTI, or "?" */
static const char *rtti_of(const void *obj)
{
    if (!readable(obj, 8)) return "<bad pointer>";
    void **vt = VT(obj);
    if (!in_sec(&g_rdata, (uint8_t *)vt - 8, 8)) return "<no vtable>";
    uint8_t *col = (uint8_t *)vt[-1];
    if (!in_sec(&g_rdata, col, 24) || rd32(col) != 1) return "<no RTTI>";
    uint8_t *td = g_base + rd32(col + 12);
    if (!in_sec(&g_data, td + 16, 4)) return "<no RTTI>";
    return (const char *)td + 16;
}


/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */
typedef void *(*AnyFn)(void *, void *, void *, void *);
typedef void  (*MapDescFn)(void *desc, const MsString *s);
typedef void  (*TagFromStrFn)(void *tag, const MsString *s);
typedef void  (*TagDtorFn)(void *tag);
typedef void *(*EventFindFn)(void *table, const void *tag);
typedef void  (*EventSetFn)(void *table, const void *tag, unsigned char on, float delay);

enum { SL_SCENE_LOAD = 5, SL_LIB_EVENT = 1, SL_LIB_START = 73, SL_LIB_CLEAR = 74, SL_CMD_EXEC = 7 };
enum { EV_ID = 0x08, EV_ARG = 0x38, EV_TICK = 0xFD2, EV_SCENE_READY = 0x101D };
enum { O_DESC_MAP = 0x00, O_DESC_MISSION = 0x28, O_DESC_FOLDER = 0x50, O_TAG_TEXT = 0x40 };

static void **V_SCENE, **V_LIB;
static uint32_t O_SCENE_NAME = 0x860, O_LIB_ENGINE, O_LIB_CUR;
static uint32_t O_ENG_BEGIN, O_ENG_END, O_ENTRY_ENT, ENTRY_STRIDE, O_ENT_CTRL, O_CTRL_HUMAN = 0xE88;
static uint8_t **g_world;                         /* the current world pointer (0 = none) */
static MapDescFn g_mapDesc;
static TagFromStrFn g_tagFromStr;
static TagDtorFn g_tagDtor;
static EventFindFn g_eventFind;
static EventSetFn g_eventSet;
static void *g_events;                            /* the mission event table (global) */
static int g_okScene, g_okLib, g_okMapDesc, g_okEvents, g_okCmds;

/* Scene::eManager::Load(scene, desc, flag): mov [rsp+8],rbx ; mov [rsp+10],rbp ; mov [rsp+18],rsi ; push rdi ;
   sub rsp,20 ; movzx ebp,r8b ; mov rbx,rdx ; mov rsi,rcx */
static int find_scene(void)
{
    V_SCENE = find_vtable_rtti(".?AVeManager@Scene@@");
    if (!V_SCENE) { logf_("  Scene::eManager: RTTI not found"); return 0; }
    if (!fn_is(V_SCENE[SL_SCENE_LOAD], "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 41 0F B6 E8 48 8B DA 48 8B F1",
               "Scene::eManager::Load")) return 0;
    /* the scene name offset: SceneCreate checks/assigns the name right after comparing it with "library":
       cmp qword [rbx+NAME+0x18],0F ; lea rcx,[rbx+NAME] */
    uint8_t *site = NULL;
    for (size_t i = 0x120; i + 16 <= g_text.n && !site; i++) {
        uint8_t *q = g_text.p + i;
        if (q[0] == 0x48 && q[1] == 0x83 && q[2] == 0xBB && q[7] == 0x0F && q[8] == 0x48 && q[9] == 0x8D && q[10] == 0x8B
            && rd32(q + 3) == rd32(q + 11) + 0x18 && rd32(q + 11) >= 0x400 && rd32(q + 11) < 0x1000) {
            for (uint8_t *b = q - 0x120; b < q; b++)
                if (b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x15) {
                    uint8_t *t = rel32_target(b, 3, 7);
                    if (in_sec(&g_rdata, t, 8) && !memcmp(t, "library", 8)) { site = q; break; }
                }
        }
    }
    if (site) O_SCENE_NAME = rd32(site + 11);
    else {
        /* second form (editor): lea rbx,[rcx+NAME] ; mov rdi,rcx ; cmp qword [rbx+18],0F ... lea rdx,["library"] */
        for (uint8_t *q = g_text.p; (q = scan(q, g_text.p + g_text.n - q, "48 8D 99 ?? ?? ?? ?? 48 8B F9 48 83 7B 18 0F", NULL)); q++) {
            for (uint8_t *b = q; b < q + 0x50; b++)
                if (b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x15) {
                    uint8_t *t = rel32_target(b, 3, 7);
                    if (in_sec(&g_rdata, t, 8) && !memcmp(t, "library", 8)) { site = q; break; }
                }
            if (site) { O_SCENE_NAME = rd32(site + 3); break; }
        }
        if (!site) logf_("  scene name offset not confirmed - using 0x%X", O_SCENE_NAME);
    }
    return 1;
}

/* MapDesc: the call right before "scene->vt[5](scene, &desc, 0)" in the preview / campaign scene loaders:
   lea rdx,[rbp+s] ; lea rcx,[rbp+desc] ; call MapDesc ; mov rax,[rbx] ; lea rdx,[rbp+desc] ; xor r8d,r8d ;
   mov rcx,rbx ; call [rax+28] */
static int find_mapdesc(void)
{
    int c = 0;
    uint8_t *q = scan(g_text.p, g_text.n, "48 8D 55 ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8B 03 48 8D 55 ?? 45 33 C0 48 8B CB FF 50 28", &c);
    if (!q || c < 1 || c > 4) { logf_("  MapDesc call site: matched %d times", c); return 0; }
    uint8_t *f = rel32_target(q + 8, 1, 5);
    if (!fn_is(f, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 33 F6", "MapDesc")) return 0;
    /* it splits on ':' : mov r9b,3A */
    if (!scan(f, 0x60, "41 B1 3A", NULL)) { logf_("  MapDesc: no ':' split"); return 0; }
    g_mapDesc = (MapDescFn)f;
    return 1;
}

static int find_tag_fns(void)
{
    uint8_t *ts = sig_unique("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 48 8D 41 08 48 89 29 0F 57 C0 40 88 69 28 48 8B FA", "Tag(std::string)");
    if (!ts) return 0;
    /* ~Tag: the atexit thunk of any static Tag:  lea rdx,["bandage"] ; lea rcx,[tag] ; call Tag ; lea rcx,[thunk] ; jmp atexit
       thunk: lea rcx,[tag] ; jmp ~Tag ;  ~Tag starts with  add rcx,38 ; jmp */
    uint8_t *s = find_cstr("bandage");
    for (size_t i = 0; s && i + 26 <= g_text.n; i++) {
        uint8_t *q = g_text.p + i;
        if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x15 || rel32_target(q, 3, 7) != s) continue;
        if (q[7] != 0x48 || q[8] != 0x8D || q[9] != 0x0D || q[14] != 0xE8) continue;
        uint8_t *tag = rel32_target(q + 7, 3, 7), *t = q + 19;
        if (t[0] != 0x48 || t[1] != 0x8D || t[2] != 0x0D) continue;
        uint8_t *thunk = rel32_target(t, 3, 7);
        if (!in_text(thunk) || thunk[0] != 0x48 || thunk[1] != 0x8D || thunk[2] != 0x0D || thunk[7] != 0xE9) continue;
        if (rel32_target(thunk, 3, 7) != tag) continue;
        uint8_t *dt = rel32_target(thunk + 7, 1, 5);
        if (!fn_is(dt, "48 83 C1 38 E9", "Tag::~Tag")) return 0;
        g_tagFromStr = (TagFromStrFn)ts; g_tagDtor = (TagDtorFn)dt;
        return 1;
    }
    logf_("  Tag::~Tag not found");
    return 0;
}

/* mission event table: eCmd_event::Execute = mov [rsp+10],rbx ; push rsi ; sub rsp,30 ; mov rbx,rcx ;
   lea rdx,[rcx+10] ; lea rcx,[EVENTS] ; call EventFind ... lea rdx,[rbx+10] ; movzx r8d,dil ; lea rcx,[EVENTS] ; call EventSet */
static void **V_CMD_EVENT;
static int find_events(void)
{
    V_CMD_EVENT = find_vtable_rtti(".?AUeCmd_event@Helper@@");
    if (!V_CMD_EVENT) { logf_("  eCmd_event: RTTI not found"); return 0; }
    uint8_t *f = (uint8_t *)V_CMD_EVENT[SL_CMD_EXEC];
    if (!fn_is(f, "48 89 5C 24 10 56 48 83 EC 30 48 8B D9 48 8D 51 10 48 8D 0D ?? ?? ?? ?? E8", "eCmd_event::Execute")) return 0;
    g_events = rel32_target(f + 17, 3, 7);
    g_eventFind = (EventFindFn)rel32_target(f + 24, 1, 5);
    uint8_t *q = scan(f + 29, 0x120, "48 8D 53 10 44 0F B6 C7 48 8D 0D ?? ?? ?? ?? E8", NULL);
    if (!q || rel32_target(q + 8, 3, 7) != g_events) { logf_("  eCmd_event::Execute: EventSet call not found"); return 0; }
    g_eventSet = (EventSetFn)rel32_target(q + 15, 1, 5);
    if (!in_text((void *)g_eventFind) || !in_text((void *)g_eventSet)) return 0;
    return find_tag_fns();
}

static int find_library(void)
{
    V_LIB = find_vtable_rtti(".?AVeGUI_MultiplayerLibrary@GUI2@@");
    if (!V_LIB) { logf_("  no eGUI_MultiplayerLibrary (expected for the editor and the dedicated server)"); return 0; }
    uint8_t *st = (uint8_t *)V_LIB[SL_LIB_START];
    if (!fn_is(st, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? C6 40 10 01", "MultiplayerLibrary::Start")) return 0;
    O_LIB_ENGINE = rd32(st + 17);
    uint8_t *cl = (uint8_t *)V_LIB[SL_LIB_CLEAR];
    if (!fn_is(cl, "40 53 48 83 EC 40 48 8B D9 E8", "MultiplayerLibrary::Clear")) return 0;
    uint8_t *cur = scan(cl, 0xA0, "48 C7 83 ?? ?? ?? ?? 00 00 00 00 C7 83 ?? ?? ?? ?? FF FF FF FF", NULL);
    if (!cur) { logf_("  MultiplayerLibrary::Clear: shown-template member not found"); return 0; }
    O_LIB_CUR = rd32(cur + 3);
    uint8_t *w = sig_unique("48 83 3D ?? ?? ?? ?? 00 75 ?? 41 B0 01 BA 1D 10 00 00", "world check in library Start");
    if (!w) return 0;
    g_world = (uint8_t **)rel32_target(w, 3, 8);
    uint8_t *lbl = find_cstr("eLibraryRenderEngine::Quant");
    uint8_t *q = sig_unique("48 8B B3 ?? ?? ?? ?? 48 8B BB ?? ?? ?? ?? 48 3B FE 0F 84 ?? ?? ?? ?? 48 89 6C 24 ?? 33 ED 66 90 "
                            "48 8B 9F ?? ?? ?? ?? 48 85 DB 0F 84 ?? ?? ?? ?? F6 43 30 02 74 ?? 48 8B 8B ?? ?? ?? ??",
                            "eLibraryRenderEngine::Quant entity loop");
    if (!q || !lbl) { if (!lbl) logf_("  Quant label not found"); return 0; }
    int labelled = 0;
    for (uint8_t *b = q; b < q + 0x60; b++)
        if (b[0] == 0x4C && b[1] == 0x8D && b[2] == 0x0D && rel32_target(b, 3, 7) == lbl) { labelled = 1; break; }
    if (!labelled) { logf_("  entity loop is not in eLibraryRenderEngine::Quant"); return 0; }
    O_ENG_END = rd32(q + 3); O_ENG_BEGIN = rd32(q + 10);
    O_ENTRY_ENT = rd32(q + 35); O_ENT_CTRL = rd32(q + 57);
    uint8_t *adv = scan(q, 0xC0, "48 81 C7 ?? ?? ?? ?? 48 3B FE", NULL);    /* add rdi,STRIDE ; cmp rdi,rsi */
    if (!adv) { logf_("  entry stride not found"); return 0; }
    ENTRY_STRIDE = rd32(adv + 3);
    return 1;
}

/* mission commands whose Execute (vt[7]) is logged */
#define NCMD 13
static const char *const CMD_NAMES[NCMD] = {
    "event", "trigger", "spawn", "squad_create", "reinforcement", "waypoint", "squad_to_waypoint",
    "actor_to_waypoint", "board", "delete", "entity_state", "air_state", "scenario" };
static void **V_CMD[NCMD];

static int find_cmds(void)
{
    int n = 0;
    for (int i = 0; i < NCMD; i++) {
        char dec[96]; snprintf(dec, sizeof dec, ".?AUeCmd_%s@Helper@@", CMD_NAMES[i]);
        V_CMD[i] = find_vtable_rtti(dec);
        if (V_CMD[i] && !in_text(V_CMD[i][SL_CMD_EXEC])) V_CMD[i] = NULL;
        if (V_CMD[i]) n++; else logf_("  mission command %s: not found", CMD_NAMES[i]);
    }
    return n;
}

/* ------------------------------------------------------------------------------------ */
/* features                                                                             */
/* ------------------------------------------------------------------------------------ */
static AnyFn o_sceneLoad, o_libEvent, o_cmd[NCMD];
static int g_wantScenes = 1, g_wantPreview = 1, g_wantCmds = 1, g_dumpEveryMs = 1000, g_maxDumps = 60, g_cmdLogMax = 40;
static char g_previewMap[260];                    /* "" = vanilla (no map) */
static char g_unitEvent[96];                      /* "" = no event */

static void desc_log(const char *what, void *desc)
{
    char d0[300], d1[300], d2[300];
    ms_read((uint8_t *)desc + O_DESC_MAP, d0, sizeof d0);
    ms_read((uint8_t *)desc + O_DESC_MISSION, d1, sizeof d1);
    ms_read((uint8_t *)desc + O_DESC_FOLDER, d2, sizeof d2);
    logf_("%s map %s | mission %s | folder %s", what, d0, d1, d2);
}

/* every scene load: name + map descriptor; the preview scene can be given a map/mission */
static void *hk_scene_load(void *scene, void *desc, void *flag, void *a4)
{
    char name[160];
    ms_read((uint8_t *)scene + O_SCENE_NAME, name, sizeof name);
    logf_("scene load: scene %p name %s | flag %d | world before %p", scene, name, (int)((uintptr_t)flag & 0xFF),
          g_world && readable(g_world, 8) ? (void *)*g_world : NULL);
    desc_log("  descriptor:", desc);
    if (g_previewMap[0] && g_okMapDesc && !strcmp(name, "\"library\"")) {
        MsString s; ms_view(&s, g_previewMap);
        g_mapDesc(desc, &s);
        desc_log("  PREVIEW MAP -> descriptor:", desc);
    }
    unsigned long long t0 = GetTickCount64();
    void *r = o_sceneLoad(scene, desc, flag, a4);
    logf_("scene load done: name %s after %llu ms | world now %p (%s)", name, GetTickCount64() - t0,
          g_world ? (void *)*g_world : NULL, g_world && *g_world ? rtti_of(*g_world) : "-");
    return r;
}

/* the text of a Tag, or "<?>" */
static void tag_read(const void *tag, char *out, size_t n) { ms_read((const uint8_t *)tag + O_TAG_TEXT, out, n); }

/* set a mission event by name; returns 1 if the running mission declares it */
static int fire_event(const char *name)
{
    if (!g_okEvents) return 0;
    uint8_t tag[0x60] __attribute__((aligned(16)));
    MsString s; ms_view(&s, name);
    memset(tag, 0, sizeof tag);
    g_tagFromStr(tag, &s);
    int declared = g_eventFind(g_events, tag) != NULL;
    if (declared) g_eventSet(g_events, tag, 1, 0.0f);
    g_tagDtor(tag);
    return declared;
}

/* per-library state: last entities and last shown template */
typedef struct { void *lib; void *ents[4]; void *cur; } LibState;
static LibState g_libs[8];
static LibState *lib_state(void *lib)
{
    for (int i = 0; i < 8; i++) if (g_libs[i].lib == lib) return &g_libs[i];
    for (int i = 0; i < 8; i++) if (!g_libs[i].lib) { g_libs[i].lib = lib; return &g_libs[i]; }
    memset(&g_libs[0], 0, sizeof g_libs[0]); g_libs[0].lib = lib; return &g_libs[0];
}

/* a unit template: RTTI and every std::string-looking field in its first 0x100 bytes (to find its name) */
static void log_template(void *tpl)
{
    if (!readable(tpl, 0x100)) { logf_("  template %p (not readable)", tpl); return; }
    logf_("  template %p %s", tpl, rtti_of(tpl));
    int shown = 0;
    for (uint32_t off = 0; off + sizeof(MsString) <= 0x100 && shown < 8; off += 8) {
        char buf[160]; ms_read((uint8_t *)tpl + off, buf, sizeof buf);
        if (buf[0] == '"' && strlen(buf) > 3) { logf_("    +0x%02X %s", off, buf); shown++; }
    }
    for (uint32_t off = 0; off + 0x60 <= 0x100 && shown < 10; off += 8) {   /* Tags: text at +0x40 */
        char buf[160]; tag_read((uint8_t *)tpl + off, buf, sizeof buf);
        if (buf[0] == '"' && strlen(buf) > 3 && readable((uint8_t *)tpl + off, 0x60)) { logf_("    Tag at +0x%02X %s", off, buf); shown++; }
    }
}

static void check_library(void *lib, const char *why)
{
    uint8_t *eng = PTR(lib, O_LIB_ENGINE);
    if (!readable(eng, O_ENG_END + 8)) return;
    LibState *ls = lib_state(lib);
    uint8_t *b = PTR(eng, O_ENG_BEGIN), *e = PTR(eng, O_ENG_END);
    size_t n = (b && e >= b && ENTRY_STRIDE) ? (size_t)(e - b) / ENTRY_STRIDE : 0;
    if (n > 256) n = 0;
    void *cur = readable((uint8_t *)lib + O_LIB_CUR, 8) ? PTR(lib, O_LIB_CUR) : NULL;
    if (cur != ls->cur) {
        ls->cur = cur;
        logf_("preview %p: shown unit changed -> template %p", lib, cur);
        if (cur) {
            log_template(cur);
            if (g_unitEvent[0]) {
                int d = fire_event(g_unitEvent);
                logf_("  mission event \"%s\": %s", g_unitEvent, !g_okEvents ? "not available (engine code not recognised)" :
                      d ? "SET" : "not declared by the running mission (nothing set)");
            }
        }
    }
    void *ents[4] = { 0 };
    for (size_t i = 0; i < n && i < 4; i++) {
        uint8_t *entry = b + i * ENTRY_STRIDE;
        if (readable(entry + O_ENTRY_ENT, 8)) ents[i] = PTR(entry, O_ENTRY_ENT);
    }
    if (!memcmp(ents, ls->ents, sizeof ents) && strcmp(why, "scene ready")) return;
    memcpy(ls->ents, ents, sizeof ents);
    static int dumps;
    if (++dumps > g_maxDumps) { LOG_CAPPED(1, "preview: dump limit reached (max_dumps)"); return; }
    void *w = g_world && readable(g_world, 8) ? (void *)*g_world : NULL;
    logf_("preview %p (%s): engine %p | world %p (%s) | %u entities", lib, why, eng, w, w ? rtti_of(w) : "-", (unsigned)n);
    for (size_t i = 0; i < n && i < 16; i++) {
        uint8_t *entry = b + i * ENTRY_STRIDE;
        uint8_t *ent = readable(entry + O_ENTRY_ENT, 8) ? PTR(entry, O_ENTRY_ENT) : NULL;
        if (!ent || !readable(ent, O_ENT_CTRL + 8)) { logf_("  [%u] entity %p", (unsigned)i, ent); continue; }
        uint8_t *ctrl = PTR(ent, O_ENT_CTRL);
        uint8_t *part = readable(ctrl, O_CTRL_HUMAN + 8) ? PTR(ctrl, O_CTRL_HUMAN) : NULL;
        logf_("  [%u] entity %p %s | flags %02X | controller %s | part %s", (unsigned)i, ent, rtti_of(ent),
              U32(ent, 0x30) & 0xFF, ctrl ? rtti_of(ctrl) : "-", part ? rtti_of(part) : "-");
    }
}

static unsigned long long g_lastCheck;
static void *hk_lib_event(void *lib, void *ev, void *a3, void *a4)
{
    void *r = o_libEvent(lib, ev, a3, a4);
    if (!lib || !ev || !readable(ev, EV_ARG + 4) || !readable(lib, O_LIB_CUR + 8) || !PTR(lib, O_LIB_ENGINE)) return r;
    uint32_t id = U32(ev, EV_ID);
    if (id == EV_SCENE_READY && U32(ev, EV_ARG) == 1) check_library(lib, "scene ready");
    else if (id == EV_TICK) {
        unsigned long long now = GetTickCount64();
        if (now - g_lastCheck >= (unsigned long long)g_dumpEveryMs) { g_lastCheck = now; check_library(lib, "tick"); }
    }
    return r;
}

/* mission commands: call first, then log (the log must not disturb the call) */
static int g_cmdCount[NCMD];
static void cmd_log(int i, void *cmd)
{
    int c = ++g_cmdCount[i];
    if (c > g_cmdLogMax && c % 100) return;
    char extra[200] = "";
    if (!strcmp(CMD_NAMES[i], "event") || !strcmp(CMD_NAMES[i], "trigger")) {
        char nm[160]; tag_read((uint8_t *)cmd + 0x10, nm, sizeof nm);
        snprintf(extra, sizeof extra, " %s", nm);
    }
    logf_("mission: %s%s (#%d) | world %p", CMD_NAMES[i], extra, c, g_world && readable(g_world, 8) ? (void *)*g_world : NULL);
}
#define CMD_HOOK(i) static void *hk_cmd##i(void *c, void *x, void *a3, void *a4) { void *r = o_cmd[i](c, x, a3, a4); cmd_log(i, c); return r; }
CMD_HOOK(0) CMD_HOOK(1) CMD_HOOK(2) CMD_HOOK(3) CMD_HOOK(4) CMD_HOOK(5) CMD_HOOK(6)
CMD_HOOK(7) CMD_HOOK(8) CMD_HOOK(9) CMD_HOOK(10) CMD_HOOK(11) CMD_HOOK(12)
static void *const CMD_HOOKS[NCMD] = { (void *)hk_cmd0, (void *)hk_cmd1, (void *)hk_cmd2, (void *)hk_cmd3, (void *)hk_cmd4,
    (void *)hk_cmd5, (void *)hk_cmd6, (void *)hk_cmd7, (void *)hk_cmd8, (void *)hk_cmd9, (void *)hk_cmd10, (void *)hk_cmd11, (void *)hk_cmd12 };

/* ------------------------------------------------------------------------------------ */
/* install                                                                              */
/* ------------------------------------------------------------------------------------ */
static int find_all(void)
{
    g_okScene = find_scene();
    g_okMapDesc = find_mapdesc();
    g_okEvents = find_events();
    g_okCmds = find_cmds();
    g_okLib = find_library();
    return g_okScene;
}

#ifndef PS_TEST
static int vt_hook(void **vt, int slot, void *fn, AnyFn *orig)
{
    DWORD old;
    *orig = (AnyFn)vt[slot];
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!VirtualProtect(vt + slot, 8, PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer(vt + slot, fn);
    VirtualProtect(vt + slot, 8, old, &old);
    return 1;
}

static void init(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;
    if (!ini_int("enabled", 1)) return;
    g_console = ini_int("console", 0);
    if (ini_int("log", 1)) {
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir);
        wcscat(lp, !_wcsnicmp(en, L"call_to_arms_ed", 15) ? L"\\gem_previewscene_editor.log" :
                   !_wcsnicmp(en, L"call_to_arms_server", 19) ? L"\\gem_previewscene_server.log" : L"\\gem_previewscene.log");
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(PS_NAME); freopen("CONOUT$", "w", stdout); }
    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(PS_NAME " " PS_VERSION " (test build) loaded into %ls (timestamp %08X)", en, g_timestamp);

    g_wantScenes  = ini_int("log_scene_loads", 1);
    g_wantPreview = ini_int("log_preview_units", 1);
    g_wantCmds    = ini_int("log_mission_commands", 1);
    g_dumpEveryMs = ini_int("preview_check_ms", 1000);
    g_maxDumps    = ini_int("max_dumps", 60);
    g_cmdLogMax   = ini_int("mission_log_max", 40);
    ini_str("preview_map", "", g_previewMap, sizeof g_previewMap);
    ini_str("unit_changed_event", "", g_unitEvent, sizeof g_unitEvent);

    find_all();
    if (g_okScene) logf_("scene: eManager vt exe+0x%llX, Load exe+0x%llX, name at +0x%X", EXE_OFF(V_SCENE), EXE_OFF(V_SCENE[SL_SCENE_LOAD]), O_SCENE_NAME);
    if (g_okMapDesc) logf_("map descriptor parser exe+0x%llX", EXE_OFF(g_mapDesc));
    if (g_okEvents) logf_("mission events: table exe+0x%llX, find exe+0x%llX, set exe+0x%llX", EXE_OFF(g_events), EXE_OFF(g_eventFind), EXE_OFF(g_eventSet));
    if (g_okLib) logf_("preview: library vt exe+0x%llX engine +0x%X shown +0x%X | world global exe+0x%llX | entries +0x%X..+0x%X stride 0x%X, entity +0x%X, controller +0x%X",
                       EXE_OFF(V_LIB), O_LIB_ENGINE, O_LIB_CUR, EXE_OFF(g_world), O_ENG_BEGIN, O_ENG_END, ENTRY_STRIDE, O_ENTRY_ENT, O_ENT_CTRL);

    int sceneHook = (g_wantScenes || g_previewMap[0]) && g_okScene && vt_hook(V_SCENE, SL_SCENE_LOAD, (void *)hk_scene_load, &o_sceneLoad);
    log_state("log scene loads", !g_okScene ? "OFF (engine code not recognised)" : sceneHook ? "ON" : !(g_wantScenes || g_previewMap[0]) ? "off (ini)" : "OFF (VirtualProtect failed)");
    char pm[320];
    snprintf(pm, sizeof pm, "\"%s\"", g_previewMap);
    log_state("preview map", !g_previewMap[0] ? "off (preview_map empty: vanilla preview)" : !g_okMapDesc ? "OFF (map parser not recognised)" :
              !sceneHook ? "OFF (scene hook not installed)" : pm);
    int libHook = (g_wantPreview || g_unitEvent[0]) && g_okLib && vt_hook(V_LIB, SL_LIB_EVENT, (void *)hk_lib_event, &o_libEvent);
    log_state("log preview units", !g_okLib ? "OFF (no unit preview in this exe / not recognised)" : libHook ? "ON" : "off (ini)");
    snprintf(pm, sizeof pm, "\"%s\"", g_unitEvent);
    log_state("unit changed -> mission event", !g_unitEvent[0] ? "off (unit_changed_event empty)" : !g_okEvents ? "OFF (mission events not recognised)" :
              !libHook ? "OFF (no unit preview hook)" : pm);
    int cmds = 0;
    if (g_wantCmds)
        for (int i = 0; i < NCMD; i++)
            if (V_CMD[i] && vt_hook(V_CMD[i], SL_CMD_EXEC, CMD_HOOKS[i], &o_cmd[i])) cmds++;
    char cs[64]; snprintf(cs, sizeof cs, "ON (%d of %d commands)", cmds, NCMD);
    log_state("log mission commands", !g_wantCmds ? "off (ini)" : cmds ? cs : "OFF (not recognised)");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); init(h); }
    return TRUE;
}
#endif /* !PS_TEST */

/* ------------------------------------------------------------------------------------ */
/* offline finder test: maps the exe at its image base and runs every finder            */
/* ------------------------------------------------------------------------------------ */
#ifdef PS_TEST
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: ps_test <exe>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); if (fread(file, 1, n, f) != (size_t)n) return 2; fclose(f);
    uint32_t pe = rd32(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint64_t base; memcpy(&base, file + pe + 24 + 24, 8);
    uint32_t imgsz = rd32(file + pe + 24 + 56);
    g_base = mmap((void *)base, imgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_base != (uint8_t *)base) { printf("mmap at image base failed\n"); return 1; }
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), raw = rd32(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s;
    }
    g_timestamp = rd32(file + pe + 8);
    printf("%s timestamp %08X\n", argv[1], g_timestamp);
    find_all();
    if (g_okScene) printf("scene: eManager vt %llX Load %llX name +%X\n", EXE_OFF(V_SCENE), EXE_OFF(V_SCENE[SL_SCENE_LOAD]), O_SCENE_NAME);
    if (g_okMapDesc) printf("MapDesc %llX\n", EXE_OFF(g_mapDesc));
    if (g_okEvents) printf("events: table %llX find %llX set %llX | Tag %llX ~Tag %llX\n", EXE_OFF(g_events), EXE_OFF(g_eventFind), EXE_OFF(g_eventSet),
                           EXE_OFF(g_tagFromStr), EXE_OFF(g_tagDtor));
    printf("mission commands found: %d of %d\n", g_okCmds, NCMD);
    for (int i = 0; i < NCMD; i++) if (V_CMD[i]) printf("  %-18s vt %llX exec %llX\n", CMD_NAMES[i], EXE_OFF(V_CMD[i]), EXE_OFF(V_CMD[i][SL_CMD_EXEC]));
    if (g_okLib) printf("preview: lib vt %llX engine +%X shown +%X world %llX entries +%X..+%X stride %X entity +%X controller +%X\n",
                        EXE_OFF(V_LIB), O_LIB_ENGINE, O_LIB_CUR, EXE_OFF(g_world), O_ENG_BEGIN, O_ENG_END, ENTRY_STRIDE, O_ENTRY_ENT, O_ENT_CTRL);
    printf("scene %s | map parser %s | mission events %s | preview %s\n", g_okScene ? "FOUND" : "NOT FOUND", g_okMapDesc ? "FOUND" : "NOT FOUND",
           g_okEvents ? "FOUND" : "NOT FOUND", g_okLib ? "FOUND" : "NOT FOUND (ok for editor/server)");
    /* the game needs everything; the editor (no unit preview) only the scene and mission-command logs */
    int needed = g_okScene && g_okCmds == NCMD && (V_LIB ? (g_okMapDesc && g_okEvents && g_okLib) : 1);
    printf(needed ? "OK\n" : "FAILED\n");
    return !needed;
}
#endif
