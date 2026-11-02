/*
 * GEM ConquestUI 0.4.2  -  Call to Arms: Gates of Hell (GEM engine, 1.067), Dynamic Campaign ("Conquest") UI
 *
 *   1. Research tree unit preview
 *      Clicking a unit node in the campaign research tree shows that unit in an inspector panel under the
 *      tree (the engine's own multiplayer-library unit card, the same one the campaign page uses for
 *      reinforcements). Researched nodes become clickable for the preview but can not be (de)selected for
 *      research. The inspector already exists in the vanilla layout, commented out by the developers:
 *      mod_side/DynamicCampaignResearchTree.xml re-enables it. The developers commented it out because a multiplayer
 *      library nobody starts crashes the game on its first scene event (OnEvent uses the render engine created by
 *      Start without a null check); the library guard makes that impossible while this DLL runs. The mod ships the
 *      layout under the vanilla name, so a Steam start without the launcher still crashes when research is opened.
 *
 *   2. Research tree zoom
 *      The mouse wheel over the research tree zooms it (40%-150% by default), anchored at the mouse; drag still pans.
 *      Icons are moved and resized from the zoom-1 layout, the lines are rebuilt by the engine's own LineSetup.
 *      zoom_modifier (default ctrl; shift|alt|empty) keeps the vanilla wheel scrolling and zooms only while held.
 *      Notches within zoom_delay ms (150) of the last level are dropped: each level re-lays out the whole tree.
 *
 *   3. Unlocked portraits in Reinforcements / Airstrikes / Emplacements
 *      portrait_mode=1  only unlocked (researched) units are shown, in the game's own order
 *      portrait_mode=0  every unit is shown, unlocked ones first
 *      A "btn_portraits" button (mod_side/DynamicCampaignOverview.xml) switches between the two at runtime.
 *
 * Re-implementation of the ideas of two third-party DLLs studied on the user's request
 * (goh_research_tree_preview.dll 1.0.3, goh_dynamic_campaign_unlocked_portraits.dll); no code was copied.
 * Differences: no code patches at all (only vtable slot swaps), no MinHook / MSVC runtime, the portrait filter
 * hides portraits with the engine's own visibility flag instead of destroying and re-creating them, and every
 * engine address/offset is re-derived at startup from RTTI, vtable slots and byte patterns; if one is not
 * recognised the feature stays OFF and the log says why.
 *
 * Engine facts used (game 1.067 call_to_arms.exe, PE timestamp 6AB3DCC7; only the game exe has this UI):
 *   eDialog (GUI2): flags +0x48 (bit0 layout valid, bit1 visible), name std::string +0x58, children
 *     vector<eDialog*> +0xD8/+0xE0, parent +0xF0, notify-mapping list +0xC8; vt[4] invalidate layout,
 *     vt[5] visibility changed, vt[14] position changed, vt[16] SetSize, vt[22]/vt[23] Get{Min,Max}Size, vt[57]
 *     SetPreferredSize, vt[58] repaint, vt[60] FindChild(name). Hiding = clear bit1 + vt[5]
 *     (inlined like this all over the engine); eWrapLayout skips invisible children.
 *   eCampaign{Reinforcements,Airstrikes,Emplacements}Panel: vt[73] Refresh (creates a portrait per eligible
 *     unit, never removes one), wrap panel +0x1C8, vector<{template*, portrait*}> +0x1F8/+0x200.
 *   eGUI_ReinforcementsPortrait: unlocked flag +0x2E0 (set by the panel's UpdatePortrait = research state 1),
 *     reveal-animation state +0x2B0 (0 = not revealed: not drawn at all, 1 = sliding in, 2 = done).
 *   ePortraitWrapPanel: FIFO of portraits to reveal; vt[43] enqueue, vt[44] dequeue. The head advances
 *     only when the head portrait finished its animation, so a hidden portrait must never stay queued.
 *   eGUI_DynamicCampaignView: vt[20] Update (refreshes the 3 panels at +0x8D8/+0x8E0/+0x8E8), vt[34] Notify,
 *     vt[69] Bind (buttons: flags+0x180 |= 1, AddNotifyMapping(btn, view, 7 = click, code)),
 *     preparation inspector (eGUI_MultiplayerLibrary) +0x1D0.
 *   eGUI_CampaignResearchTree: vt[34] Notify (7/8 = icon clicks), vt[62] Close, vt[69] Bind, owner view +0x1E8,
 *     unitsContainer +0x230. eGUI_CampaignResearchIcon: research node +0x2E0 (byte 0 = unit), state +0x2E8
 *     (1 = researched), vt[73] "is disabled" (= researched), vt[86] state-frame map.
 *   eGUI_MultiplayerLibrary: vt[1] OnEvent (0xFB6, 0x101D = 3D scene ready), vt[73] Start, vt[74] Clear,
 *     vt[75] SetContext(Tag*), ShowTemplate(lib, template), +0x298 scene ready, +0x310 shown template.
 *   Unit template by name: ConfigLookup(Tag(name)).
 *   Research tree zoom: eDialog position +0x98, size +0xA0, preferred/min/max size +0x120/+0x128/+0x130, vt[14]
 *     position changed, vt[16] SetSize; tree linesContainer +0x220, scrollPane +0x210 (GetScroll/SetScroll are the
 *     ones its drag notification uses); eGUI_CampaginResearchLine icons +0x1A0/+0x1A8, LineSetup(line, a, b);
 *     the wheel goes to the first hovered ancestor with flag 0x10 in +0x4C: the scroll pane, vt[51] OnMouseWheel(notches,
 *     negative = wheel up); eScrollPaneLayout vt[4] Arrange sets the scroll ranges. GUI mouse position and
 *     eDialog::ToLocal are taken from eScrollBar's mouse-button handler (vt[39]).
 *   Localized texts: LocEntry(key) -> LocText(entry) = const std::string* (the key itself when there is no text),
 *     found where the research tree labels "clearResearch".
 *
 * UI only: nothing here touches the simulation (safe for co-op campaigns, no desync).
 * Build: build.bat (zig cc / mingw-w64).   Offline finder check: tests/run_offline_check.sh
 */
#ifndef CU_TEST
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

#define CU_VERSION "0.4.2"
#define CU_NAME    "GEM ConquestUI"
#define CU_INI     L"\\gem_conquestui.ini"
#define CU_LOG     L"\\gem_conquestui.log"
#define CU_SECT    L"conquestui"

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
static int g_verbose = 1;
#ifdef CU_TEST
static void logf_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
static short GetAsyncKeyState(int key) { (void)key; return 0; }   /* the zoom modifier: never held offline */
static unsigned long long g_testNow;                                /* the zoom test drives the clock */
static unsigned long long GetTickCount64(void) { return g_testNow; }
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
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [" CU_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    OutputDebugStringA(line);
}

static void ini_path(wchar_t *out) { wcscpy(out, g_dir); wcscat(out, CU_INI); }

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64], wdef[128], wout[256];
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 128);
    GetPrivateProfileStringW(CU_SECT, wkey, wdef, wout, 256, ini);
    if (!WideCharToMultiByte(CP_UTF8, 0, wout, -1, out, (int)n, NULL, NULL)) out[0] = 0;   /* too long: empty */
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64]; MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(CU_SECT, wkey, def, ini);
}
#endif

/* log a message at most `max` times (counter per call site); VLOG only with verbose=1 */
#define LOG_CAPPED(max, ...) do { static int n_; if (n_ < (max)) { n_++; logf_(__VA_ARGS__); } } while (0)
#define VLOG(max, ...)       do { if (g_verbose) LOG_CAPPED(max, __VA_ARGS__); } while (0)

/* one aligned "<feature>   <state>" line of the startup summary */
static void log_state(const char *feature, const char *state) { logf_("%-30s %s", feature, state); }

/* ------------------------------------------------------------------------------------ */
/* PE / pattern helpers                                                                 */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;
static uint32_t g_timestamp;
#define EXE_OFF(p) ((unsigned long long)((const uint8_t *)(p) - g_base))

#ifndef CU_TEST
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

static int in_text(const void *a) { return (const uint8_t *)a >= g_text.p && (const uint8_t *)a < g_text.p + g_text.n; }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

/* masked byte pattern: "48 8B ?? 05" */
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

/* first match of pat inside [from, from+n); *count (optional) = number of matches */
static uint8_t *scan(const void *from, size_t n, const char *pat, int *count)
{
    Sig s; if (!sig_parse(pat, &s)) return NULL;
    uint8_t *p = (uint8_t *)from, *first = NULL; int c = 0;
    if (in_text(p) && p + n > g_text.p + g_text.n) n = (size_t)(g_text.p + g_text.n - p);   /* never past .text */
    if ((size_t)s.n > n) return NULL;
    for (size_t i = 0; i + s.n <= n; i++)
        if (sig_match(p + i, &s)) { if (!first) first = p + i; c++; if (!count) break; }
    if (count) *count = c;
    return first;
}

/* exactly one match in .text, or NULL (logged) */
static uint8_t *sig_unique(const char *pat, const char *what)
{
    int c = 0; uint8_t *p = scan(g_text.p, g_text.n, pat, &c);
    if (c != 1) { logf_("  %s: pattern matched %d times (need exactly 1)", what, c); return NULL; }
    return p;
}

/* function prefix check */
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

/* number of E8 call sites in .text targeting f; *site = the first */
static int callers(const uint8_t *f, uint8_t **site)
{
    int n = 0;
    for (size_t i = 0; i + 5 <= g_text.n; i++)
        if (g_text.p[i] == 0xE8 && rel32_target(g_text.p + i, 1, 5) == f) { if (!n && site) *site = g_text.p + i; n++; }
    return n;
}

/* start of the function containing p: the byte after the int3 padding before it */
static uint8_t *func_start(uint8_t *p, size_t maxback)
{
    for (size_t k = 0; k < maxback && p - k > g_text.p; k++)
        { uint8_t *q = p - k; if (q[-1] == 0xCC && q[0] != 0xCC) return q; }
    return NULL;
}

/* first function called (E8) from [from, from+n) whose code starts with pat */
static uint8_t *called_fn(uint8_t *from, size_t n, const char *pat)
{
    Sig s; if (!sig_parse(pat, &s)) return NULL;
    for (uint8_t *q = from; q < from + n; q++) {
        if (*q != 0xE8) continue;
        uint8_t *t = rel32_target(q, 1, 5);
        if (in_text(t) && t + s.n <= g_text.p + g_text.n && sig_match(t, &s)) return t;
    }
    return NULL;
}

/* RTTI: primary vtable of a class by its decorated name (.?AVname@ns@@) */
static void **find_vtable_rtti(const char *decorated)
{
    size_t m = strlen(decorated) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = g_data.p; (p = find_bytes(p, g_data.p + g_data.n - p, (const uint8_t *)decorated, m)); p++)
        if (p == g_data.p || p[-1] == 0) { td = p - 16; break; }
    if (!td) return NULL;
    uint32_t tdRva = (uint32_t)(td - g_base);
    /* complete object locator {sig=1, offset=0, cdOffset, rva typedesc, rva hierarchy, rva self} */
    for (size_t i = 0; i + 24 <= g_rdata.n; i += 4) {
        uint8_t *col = g_rdata.p + i;
        if (rd32(col) != 1 || rd32(col + 12) != tdRva || rd32(col + 4) != 0) continue;
        if (g_base + rd32(col + 20) != col) continue;
        for (size_t j = 0; j + 8 <= g_rdata.n; j += 8)
            if (*(uint8_t **)(g_rdata.p + j) == col) return (void **)(g_rdata.p + j + 8);
    }
    return NULL;
}

/* NUL-terminated string in .rdata at a string boundary */
static uint8_t *find_cstr(const char *s)
{
    size_t m = strlen(s) + 1;
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, (const uint8_t *)s, m)); p++)
        if (p == g_rdata.p || p[-1] == 0) return p;
    return NULL;
}

/* "name" -> static Tag initialiser: lea rdx,[str] ; lea rcx,[tag] ; call Tag::Tag ; lea rcx,[thunk] ; jmp atexit
   thunk: lea rcx,[tag] ; jmp ~Tag   -> returns ~Tag */
static uint8_t *find_tag_dtor_via(const char *name)
{
    uint8_t *s = find_cstr(name);
    if (!s) return NULL;
    for (size_t i = 0; i + 26 <= g_text.n; i++) {
        uint8_t *q = g_text.p + i;
        if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x15 || rel32_target(q, 3, 7) != s) continue;
        if (q[7] != 0x48 || q[8] != 0x8D || q[9] != 0x0D || q[14] != 0xE8) continue;
        uint8_t *tag = rel32_target(q + 7, 3, 7), *t = q + 19;
        if (t[0] != 0x48 || t[1] != 0x8D || t[2] != 0x0D) continue;
        uint8_t *thunk = rel32_target(t, 3, 7);
        if (!in_text(thunk) || thunk[0] != 0x48 || thunk[1] != 0x8D || thunk[2] != 0x0D || thunk[7] != 0xE9) continue;
        if (rel32_target(thunk, 3, 7) != tag) continue;
        return rel32_target(thunk + 7, 1, 5);
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* engine types                                                                         */
/* ------------------------------------------------------------------------------------ */
typedef struct { union { char buf[16]; const char *ptr; } u; size_t size, cap; } MsString;   /* MSVC std::string */
typedef struct { int32_t x, y; } Pt;                                                      /* tPoint2<int> */

/* read-only std::string view of a C string (the engine only reads / copies it) */
static void ms_view(MsString *s, const char *c)
{
    size_t n = strlen(c);
    memset(s, 0, sizeof *s);
    if (n < 16) { memcpy(s->u.buf, c, n); s->cap = 15; }
    else { s->u.ptr = c; s->cap = n; }
    s->size = n;
}

#define VT(o)        (*(void ***)(o))
#define PTR(o, off)  (*(uint8_t **)((uint8_t *)(o) + (off)))
#define U32(o, off)  (*(uint32_t *)((uint8_t *)(o) + (off)))
#define U8(o, off)   (*(uint8_t *)((uint8_t *)(o) + (off)))
#define PT(o, off)   (*(Pt *)((uint8_t *)(o) + (off)))

typedef void *(*AnyFn)(void *, void *, void *, void *);   /* pass-through: all four argument registers + rax */
typedef void *(*FindChildFn)(void *dlg, const MsString *name);
typedef void  (*NotifyMapFn)(void *src, void *dst, int cmd, int code);
typedef void  (*SetCheckedFn)(void *icon, int checked, int suspendLayout);
typedef void  (*ShowTemplateFn)(void *lib, void *tpl);
typedef void *(*ConfigLookupFn)(const void *tag);
typedef void  (*TagFromStrFn)(void *tag, const MsString *s);
typedef void  (*TagDtorFn)(void *tag);
typedef void  (*ThisFn)(void *self);
typedef void  (*ThisPtrFn)(void *self, const void *p);
typedef void  (*SetTextFn)(void *dlg, const MsString *s);
typedef void *(*LocEntryFn)(const char *key);                  /* localization entry of a key (created if missing) */
typedef const MsString *(*LocTextFn)(void *entry);             /* its text in the game's language (the key if missing) */
typedef void  (*SelectPortraitFn)(void *view, void *portrait, int flag);
typedef void  (*SetSizeFn)(void *dlg, unsigned long long size);   /* size passed by value: x | y << 32 */
typedef void  (*LineSetupFn)(void *line, void *iconA, void *iconB);
typedef Pt   *(*GetScrollFn)(void *pane, Pt *out);
typedef void  (*SetScrollFn)(void *pane, const Pt *pos, int flag);
typedef Pt   *(*CursorFn)(Pt *out);
typedef Pt   *(*ToLocalFn)(void *dlg, Pt *out, const Pt *guiPos);

/* vtables (RTTI) */
static void **V_TREE, **V_ICON, **V_LIB, **V_VIEW, **V_WRAP, **V_PORTRAIT, **V_FBTN, **V_PANEL[3];
static void **V_LINE, **V_PANE, **V_PANELAYOUT, **V_SCROLLBAR;   /* research zoom (optional classes) */
static const char *const PANEL_NAMES[3] = { "reinforcements", "airstrikes", "emplacements" };

/* vtable slots (indices) */
enum {
    /* every eDialog */
    SL_DTOR = 0, SL_ONEVENT = 1, SL_INVALIDATE = 4, SL_VISCHANGED = 5, SL_POSCHANGED = 14, SL_SETSIZE = 16, SL_LAYOUTINIT = 19,
    SL_VIEW_UPDATE = 20, SL_GETMIN = 22, SL_GETMAX = 23, SL_SETTEXT = 25, SL_NOTIFY = 34, SL_MOUSEBTN = 39, SL_WHEEL = 51,
    SL_SETPREF = 57, SL_REPAINT = 58, SL_FINDCHILD = 60, SL_CLOSE = 62, SL_BIND = 69,
    /* layout managers (eScrollPaneLayout) */
    SL_LM_ARRANGE = 4,
    /* ePortraitWrapPanel */
    SL_REVEAL_ADD = 43, SL_REVEAL_DEL = 44,
    /* eGUI_MultiplayerLibrary */
    SL_LIB_START = 73, SL_LIB_CLEAR = 74, SL_LIB_CTX = 75,
    /* eGUI_CampaignResearchIcon */
    SL_ICON_DISABLED = 73, SL_ICON_STATEMAP = 86,
    /* eCampaign*Panel */
    SL_PANEL_REFRESH = 73, SL_PANEL_UPDATEPORTRAIT = 75,
};

/* engine event / notification fields */
enum { EV_ID = 0x08, EV_SENDER = 0x10, EV_ARG = 0x38 };
enum {
    NOTIFY_ON_CLICK = 7,                               /* AddNotifyMapping command: the source was clicked */
    TREE_ICON_TOGGLE = 7, TREE_ICON_DESELECT = 8,      /* research tree Notify codes of its icons */
    EV_SCENE_UNLOAD = 0xFB6, EV_SCENE_READY = 0x101D,  /* library scene events (checked by find_library_guard) */
};

/* offsets, re-derived by the finders; O_DLG_FLAGS is fixed because every finder pattern hard-codes it */
enum { O_DLG_FLAGS = 0x48 };
static uint32_t O_DLG_NAME, O_DLG_CHILD, O_DLG_PARENT, O_DLG_NOTIFY;
static uint32_t O_TREE_OWNER, O_TREE_UNITS, O_ICON_NODE, O_ICON_STATE;
static uint32_t O_LIB_ENGINE, O_LIB_CTX, O_LIB_READY, O_LIB_CUR, O_VIEW_PREP;
static uint32_t O_PANEL_WRAP, O_PANEL_PAIRS, O_PORTRAIT_UNLOCKED, O_PORTRAIT_REVEAL, O_FBTN_FRAME;
static uint32_t O_VIEW_PANEL[3], O_VIEW_SEL, O_WRAP_QUEUE;
static uint32_t O_DLG_POS, O_DLG_SIZE, O_DLG_PREF, O_DLG_MIN, O_DLG_MAX, O_TREE_LINES, O_TREE_PANE, O_LINE_A, O_LINE_B;

/* engine functions */
static FindChildFn g_findChild;
static NotifyMapFn g_notifyMap;
static SetCheckedFn g_setChecked;
static ShowTemplateFn g_showTemplate;
static ConfigLookupFn g_configLookup;
static TagFromStrFn g_tagFromStr;
static TagDtorFn g_tagDtor;
static ThisFn g_libStart, g_libClear;
static ThisPtrFn g_libCtx;
static SetTextFn g_btnSetText;
static LocEntryFn g_locEntry;
static LocTextFn g_locText;
static SelectPortraitFn g_selectPortrait;                     /* view: select/deselect a portrait (NULL = deselect) */
static LineSetupFn g_lineSetup;                               /* research line geometry from its two icons */
static GetScrollFn g_getScroll;
static SetScrollFn g_setScroll;
static CursorFn g_cursor;                                     /* mouse position in GUI coordinates */
static ToLocalFn g_toLocal;                                   /* GUI position -> dialog-local position */

/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */
/* library Start (vt[73]) creates the render engine that OnEvent uses: ... mov rax,[rbx+ENGINE] ; mov byte [rax+10],1 */
static uint8_t *find_lib_start(void)
{
    uint8_t *st = (uint8_t *)V_LIB[SL_LIB_START];
    if (!fn_is(st, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 83 ?? ?? ?? ?? C6 40 10 01", "MultiplayerLibrary::Start")) return NULL;
    O_LIB_ENGINE = rd32(st + 17);
    return st;
}

/* shared eDialog facts: child vector, parent, name, notify list, flag/visibility convention, FindChild */
static int find_dialog_common(void)
{
    /* vt[4] of any dialog: test byte [rcx+FLAGS],1 ; jne ; mov rcx,[rcx+PARENT] ; test rcx,rcx */
    uint8_t *inv = (uint8_t *)V_WRAP[SL_INVALIDATE];
    if (!fn_is(inv, "F6 41 48 01 75 ?? 48 8B 89 ?? ?? ?? ?? 48 85 C9", "eDialog::InvalidateLayout")) return 0;
    O_DLG_PARENT = rd32(inv + 9);
    if (!fn_is(V_WRAP[SL_VISCHANGED], "48 89 5C 24 18 57 48 83 EC 20 48 8B F9", "eDialog::OnVisibilityChanged")) return 0;
    if (V_PORTRAIT[SL_INVALIDATE] != V_WRAP[SL_INVALIDATE] || V_PORTRAIT[SL_VISCHANGED] != V_WRAP[SL_VISCHANGED]) {
        logf_("  portrait/wrap panel do not share the eDialog layout virtuals"); return 0;
    }
    /* the engine hides/shows dialogs inline: mov eax,[rbx+48] ; test al,2 ; jne ; or eax,2 ; mov rcx,rbx ; mov [rbx+48],eax ; mov rax,[rbx] ; call [rax+28] */
    if (!scan(g_text.p, g_text.n, "8B 43 48 A8 02 75 ?? 83 C8 02 48 8B CB 89 43 48 48 8B 03 FF 50 28", NULL)) { logf_("  visibility convention (flags bit1 + vt[5]) not seen in the engine"); return 0; }
    /* eDialog::AddChild: mov rbx,[rsi+END] ; lea rdi,[rsi+BEGIN] */
    uint8_t *add = sig_unique("48 89 5C 24 08 48 89 6C 24 18 48 89 54 24 10 56 57 41 56 48 83 EC 20 41 0F B6 E8 4C 8B F2 48 8B F1 48 3B D1", "eDialog::AddChild");
    if (!add) return 0;
    uint8_t *v = scan(add, 0x80, "48 8B 9E ?? ?? ?? ?? 48 8D BE ?? ?? ?? ??", NULL);
    if (!v || rd32(v + 3) != rd32(v + 10) + 8) { logf_("  eDialog child vector not recognised"); return 0; }
    O_DLG_CHILD = rd32(v + 10);
    uint8_t *par = scan(add, 0x120, "49 89 B6 ?? ?? ?? ??", NULL);              /* mov [r14+PARENT],rsi */
    if (!par || rd32(par + 3) != O_DLG_PARENT) { logf_("  eDialog parent field mismatch"); return 0; }
    /* FindChild(dlg, name) = vt[60]: mov r8d,-1 ; jmp FindChild(dlg, name, depth) */
    if (!fn_is(V_VIEW[SL_FINDCHILD], "41 B8 FF FF FF FF E9", "eDialog::FindChild")) return 0;
    if (V_TREE[SL_FINDCHILD] != V_VIEW[SL_FINDCHILD]) { logf_("  FindChild differs between classes"); return 0; }
    g_findChild = (FindChildFn)V_VIEW[SL_FINDCHILD];
    return 1;
}

/* view Bind: or byte [rax+FRAME],1 ; ... call AddNotifyMapping(btn, view, 7, code) - the engine binds its buttons so */
static int find_button_support(void)   /* AddNotifyMapping, eFrameButton::SetText and the localized texts */
{
    uint8_t *bind = (uint8_t *)V_VIEW[SL_BIND];
    if (!fn_is(bind, "40 55 53 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??", "DynamicCampaignView::Bind")) return 0;
    uint8_t *o = scan(bind, 0x2000, "80 88 ?? ?? ?? ?? 01", NULL);
    if (!o) { logf_("  view Bind: button flag store not found"); return 0; }
    O_FBTN_FRAME = rd32(o + 2);
    uint8_t *call = scan(o, 0x20, "E8", NULL);
    if (!call) { logf_("  view Bind: AddNotifyMapping call not found"); return 0; }
    uint8_t *f = rel32_target(call, 1, 5);
    /* mov [rsp+10],rbx ; mov [rsp+20],r9d ; mov [rsp+18],r8d ; push rbp,rsi,rdi,r14,r15 ; lea rbp ; sub rsp ; mov r10,[rcx+LIST] ; mov rdi,rdx ;
       mov r15,rcx ; mov rax,[r10] ; cmp rax,r10 ; je ; ... mov rdx,[rax+20] ; lea rbx,[rax+20] ; test rdx,rdx ; je ; cmp rdx,rdi ; jne ;
       movsxd rcx,r8d ; cmp [rax+10],rcx ; jne ; cmp r9d,[rax+18] */
    if (!fn_is(f, "48 89 5C 24 10 44 89 4C 24 20 44 89 44 24 18 55 56 57 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4C 8B 91 ?? ?? ?? ?? 48 8B FA 4C 8B F9 49 8B 02 49 3B C2", "AddNotifyMapping")) return 0;
    if (!scan(f, 0x70, "48 8B 50 20 48 8D 58 20 48 85 D2 74 ?? 48 3B D7 75 ?? 49 63 C8 48 39 48 10 75 ?? 44 3B 48 18", NULL)) {
        logf_("  AddNotifyMapping: mapping list layout not recognised"); return 0;
    }
    O_DLG_NOTIFY = rd32(f + 37);
    g_notifyMap = (NotifyMapFn)f;
    /* eFrameButton::SetText(const std::string&): compares with the text at +0x78, assigns, relayouts */
    if (!fn_is(V_FBTN[SL_SETTEXT], "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 48 83 7A 18 0F", "eFrameButton::SetText")) g_btnSetText = NULL;
    else g_btnSetText = (SetTextFn)V_FBTN[SL_SETTEXT];
    /* localized texts, the way the research tree labels its buttons: lea rcx,["main_menu2/dynamic_campaign/clearResearch"] ;
       call LocEntry ; mov rcx,rax ; call LocText ; mov rdx,rax ; lea rcx,[rsp+X] ; call std::string(copy).
       Optional: without it the button labels are used as written. */
    uint8_t *li = (uint8_t *)V_TREE[SL_LAYOUTINIT], *cr = find_cstr("main_menu2/dynamic_campaign/clearResearch"), *p = li;
    while (cr && in_text(li) && (p = scan(p, li + 0xC00 - p, "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B D0", NULL))) {
        if (rel32_target(p, 3, 7) == cr) {
            uint8_t *le = rel32_target(p + 7, 1, 5), *lt = rel32_target(p + 15, 1, 5);
            if (fn_is(le, "40 53 48 81 EC A0 00 00 00", "Localization::Entry") && fn_is(lt, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 43 50 48 83 C4 20 5B C3", "Localization::Text"))
                { g_locEntry = (LocEntryFn)le; g_locText = (LocTextFn)lt; }
            break;
        }
        p++;
    }
    if (!g_locEntry) logf_("  localized texts not found - the button labels are used as written");
    return 1;
}

/* research tree basics used by the preview and the zoom: Close, Notify, dtor, Bind (unitsContainer), LayoutInit */
static int find_tree(void)
{
    /* tree Close: mov [rsp+10],rbx ; push rdi ; sub rsp,N ; cmp byte [rcx+X],0 ; mov rdi,rcx ; je ; mov rcx,[rcx+OWNER] ; call view refresh */
    uint8_t *close = (uint8_t *)V_TREE[SL_CLOSE];
    if (!fn_is(close, "48 89 5C 24 10 57 48 81 EC ?? ?? ?? ?? 80 B9 ?? ?? ?? ?? 00 48 8B F9 74 ?? 48 8B 89 ?? ?? ?? ?? E8", "ResearchTree::Close")) return 0;
    O_TREE_OWNER = rd32(close + 28);
    uint8_t *nm = scan(close, 0xA0, "48 83 7F ?? 0F 48 8D 4F ??", NULL);           /* cmp [rdi+NAME+18],0F ; lea rcx,[rdi+NAME] */
    if (!nm || nm[3] != (uint8_t)(nm[8] + 0x18)) { logf_("  dialog name field not recognised"); return 0; }
    O_DLG_NAME = nm[8];
    if (!fn_is(V_TREE[SL_NOTIFY], "40 55 53 57 41 56 48 8B EC 48 83 EC 68 44 8B 42 08", "ResearchTree::Notify")) return 0;
    if (!fn_is(V_TREE[SL_DTOR], "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 8B FA", "ResearchTree::~ResearchTree")) return 0;
    /* tree Bind: movsd xmm0,["unitsContainer"] ; lea rdx,[r14+UNITS] ; ... FindChild into the member */
    uint8_t *bind = (uint8_t *)V_TREE[SL_BIND];
    if (!fn_is(bind, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 64 24 20 55 41 56 41 57", "ResearchTree::Bind")) return 0;
    uint8_t *uc = find_cstr("unitsContainer"), *p = bind; O_TREE_UNITS = 0;
    while (uc && (p = scan(p, bind + 0x1400 - p, "F2 0F 10 05 ?? ?? ?? ?? 49 8D 96 ?? ?? ?? ??", NULL))) {
        if (rel32_target(p, 4, 8) == uc) { O_TREE_UNITS = rd32(p + 11); break; }
        p++;
    }
    if (!O_TREE_UNITS) { logf_("  ResearchTree::Bind: unitsContainer member not found"); return 0; }
    /* the icons are created by tree vt[19] (layout init: base init -> vt[69] Bind, then one icon per research node):
       mov [r14+STATE],eax ... mov rax,[r14] ; mov rcx,r14 ; mov [r14+NODE],rsi ; call [rax+98] */
    uint8_t *li = (uint8_t *)V_TREE[SL_LAYOUTINIT];
    if (!fn_is(li, "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24", "ResearchTree::LayoutInit")) return 0;
    uint8_t *nd = scan(li, 0xC00, "49 8B 06 49 8B CE 49 89 B6 ?? ?? ?? ?? FF 90 98 00 00 00", NULL);
    if (!nd) { logf_("  ResearchTree::LayoutInit: icon node member not found"); return 0; }
    O_ICON_NODE = rd32(nd + 9);
    return 1;
}

static int find_research_preview(void)
{
    uint8_t *li = (uint8_t *)V_TREE[SL_LAYOUTINIT];
    /* icon vt[73]: cmp dword [rcx+STATE],1 ; sete al ; ret */
    uint8_t *dis = (uint8_t *)V_ICON[SL_ICON_DISABLED];
    if (!fn_is(dis, "83 B9 ?? ?? ?? ?? 01 0F 94 C0 C3", "ResearchIcon::IsDisabled")) return 0;
    O_ICON_STATE = rd32(dis + 2);
    uint8_t *sst = scan(li, 0xC00, "41 89 86 ?? ?? ?? ?? EB ?? 45 33 F6", NULL);   /* icon ctor tail: mov [r14+STATE],eax ; jmp ; xor r14d,r14d */
    if (!sst || rd32(sst + 3) != O_ICON_STATE) { logf_("  ResearchIcon state field mismatch"); return 0; }
    if (!fn_is(V_ICON[SL_ICON_STATEMAP], "C7 02 04 00 00 00 B0 01 C7 42 04 01 00 00 00 48 C7 42 08 02 00 00 00 C3", "ResearchIcon::StateMap")) return 0;
    /* library */
    if (!fn_is(V_LIB[SL_ONEVENT], "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 8B 4A 08", "MultiplayerLibrary::OnEvent")) return 0;
    uint8_t *st = find_lib_start();
    if (!st) return 0;
    uint8_t *cl = (uint8_t *)V_LIB[SL_LIB_CLEAR];
    if (!fn_is(cl, "40 53 48 83 EC 40 48 8B D9 E8", "MultiplayerLibrary::Clear")) return 0;
    uint8_t *cur = scan(cl, 0xA0, "48 C7 83 ?? ?? ?? ?? 00 00 00 00 C7 83 ?? ?? ?? ?? FF FF FF FF", NULL);
    if (!cur) { logf_("  MultiplayerLibrary::Clear: shown-template member not found"); return 0; }
    O_LIB_CUR = rd32(cur + 3);
    uint8_t *cx = (uint8_t *)V_LIB[SL_LIB_CTX];
    if (!fn_is(cx, "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8D 8B ?? ?? ?? ?? 48 8B D7", "MultiplayerLibrary::SetContext")) return 0;
    O_LIB_CTX = rd32(cx + 24);
    g_libStart = (ThisFn)st; g_libClear = (ThisFn)cl; g_libCtx = (ThisPtrFn)cx;
    /* ShowTemplate(lib, template): returns early if template == shown or the scene is not ready */
    uint8_t *sh = sig_unique("48 89 4C 24 08 55 56 41 54 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 45 33 E4 4C 8B F9", "MultiplayerLibrary::ShowTemplate");
    if (!sh) return 0;
    uint8_t *rc = scan(sh, 0x180, "49 3B B7 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 45 38 A7 ?? ?? ?? ??", NULL);
    if (!rc || rd32(rc + 3) != O_LIB_CUR) { logf_("  ShowTemplate: shown/ready members not recognised"); return 0; }
    O_LIB_READY = rd32(rc + 16);
    g_showTemplate = (ShowTemplateFn)sh;
    /* the view previews its reinforcement portraits the same way: mov rcx,[rbx+PREP] ; call ShowTemplate (single caller) */
    uint8_t *site = NULL;
    if (callers(sh, &site) != 1 || site[-7] != 0x48 || site[-6] != 0x8B || site[-5] != 0x8B) { logf_("  view preparation inspector not found"); return 0; }
    O_VIEW_PREP = rd32(site - 4);
    /* that caller is the view's SelectPortrait(view, portrait, flag): mov rbx,rcx ; mov rdi,rdx ; mov rcx,[rcx+SELECTED] ...
       with portrait = NULL it only deselects (unchecks, clears the inspector). Optional: without it a hidden portrait stays selected. */
    uint8_t *sp = func_start(site, 0x200);
    if (sp && fn_is(sp, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 30 48 8B D9 48 8B FA 48 8B 89 ?? ?? ?? ?? 48 85 C9", "DynamicCampaignView::SelectPortrait")) {
        O_VIEW_SEL = rd32(sp + 30); g_selectPortrait = (SelectPortraitFn)sp;
    }
    /* unit template by name: Tag(std::string) -> ConfigLookup(Tag) -> ~Tag */
    uint8_t *ts = sig_unique("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 48 8D 41 08 48 89 29 0F 57 C0 40 88 69 28 48 8B FA", "Tag(std::string)");
    uint8_t *lk = sig_unique("48 83 EC 68 48 83 79 58 0F 48 8D 51 40 76 03 48 8B 12 4C 8B 41 50 0F 57 C0", "ConfigLookup(Tag)");
    uint8_t *td = find_tag_dtor_via("bandage");
    if (!ts || !lk || !td || !fn_is(td, "48 83 C1 38 E9", "Tag::~Tag")) return 0;
    g_tagFromStr = (TagFromStrFn)ts; g_configLookup = (ConfigLookupFn)lk; g_tagDtor = (TagDtorFn)td;
    /* SetChecked(icon, checked, suspendLayout) */
    uint8_t *sc = sig_unique("48 89 5C 24 08 57 48 83 EC 20 48 8B 99 ?? ?? ?? ?? 0F B6 FA 45 84 C0 74 ?? 8B 43 48 A8 10", "ResearchIcon::SetChecked");
    if (!sc) return 0;
    g_setChecked = (SetCheckedFn)sc;
    return 1;
}

static int find_portraits(void)
{
    /* panel Refresh (shared by the 3 classes): sub rsp,48 ; ... mov r14,rcx ; mov rcx,[rcx+WRAP] ; mov eax,[rcx+48] ; test al,10 */
    for (int i = 0; i < 3; i++)
        if (!fn_is(V_PANEL[i][SL_PANEL_REFRESH], "48 83 EC 48 48 89 5C 24 58 48 89 7C 24 40 4C 89 74 24 38 4C 8B F1 48 8B 89 ?? ?? ?? ?? 8B 41 48 A8 10", PANEL_NAMES[i])) return 0;
    uint8_t *rf = (uint8_t *)V_PANEL[0][SL_PANEL_REFRESH];
    O_PANEL_WRAP = rd32(rf + 25);
    /* mov rax,[r14+PAIRS+8] ; lea r15,[r14+PAIRS] */
    uint8_t *pr = scan(rf, 0x100, "49 8B 86 ?? ?? ?? ?? 4D 8D BE ?? ?? ?? ??", NULL);
    if (!pr || rd32(pr + 3) != rd32(pr + 10) + 8) { logf_("  panel portrait vector not recognised"); return 0; }
    O_PANEL_PAIRS = rd32(pr + 10);
    /* UpdatePortrait: ... call SetUnlocked(portrait, researched) = mov rax,[rcx] ; mov [rcx+UNLOCKED],dl ; rex.w jmp [rax+REPAINT] */
    uint8_t *up = (uint8_t *)V_PANEL[0][SL_PANEL_UPDATEPORTRAIT];
    if (!fn_is(up, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC ?? 48 8B 9A", "Panel::UpdatePortrait")) return 0;
    uint8_t *su = called_fn(up, 0x140, "48 8B 01 88 91 ?? ?? ?? ?? 48 FF A0");
    if (!su) { logf_("  portrait unlocked flag not found in UpdatePortrait"); return 0; }
    O_PORTRAIT_UNLOCKED = rd32(su + 5);
    /* wrap panel reveal queue: vt[43] enqueue, vt[44] dequeue (ends by starting the new head: call StartReveal) */
    if (!fn_is(V_WRAP[SL_REVEAL_ADD], "40 55 56 41 55 48 83 EC 50 48 8B C2 4C 8D 0D", "PortraitWrapPanel::EnqueueReveal")) return 0;
    uint8_t *dq = (uint8_t *)V_WRAP[SL_REVEAL_DEL];
    /* mov rax,[rcx+QUEUE] (list sentinel) ; mov rdi,rcx ; mov rbx,[rax] ; cmp rbx,rax ; je ; ... cmp [rbx+10],rdx (node -> portrait) */
    if (!fn_is(dq, "48 89 5C 24 08 57 48 83 EC 20 48 8B 81 ?? ?? ?? ?? 48 8B F9 48 8B 18 48 3B D8 74 ?? 0F 1F 40 00 48 39 53 10", "PortraitWrapPanel::DequeueReveal")) return 0;
    O_WRAP_QUEUE = rd32(dq + 13);
    uint8_t *sr = called_fn(dq, 0x90, "40 53 48 83 EC 20 83 B9 ?? ?? ?? ?? 00 48 8B D9 0F 85");   /* StartReveal */
    if (!sr) { logf_("  portrait reveal animation not recognised"); return 0; }
    O_PORTRAIT_REVEAL = rd32(sr + 8);
    /* view Update refreshes its 3 panels: mov rcx,[rdi+PANEL] ; test rcx,rcx ; je ; mov rax,[rcx] ; call [rax+REFRESH*8] (x3) */
    uint8_t *vu = (uint8_t *)V_VIEW[SL_VIEW_UPDATE];
    if (!fn_is(vu, "40 55 53 57 41 55 41 57 48 8D AC 24", "DynamicCampaignView::Update")) return 0;
    uint8_t *p3 = scan(vu, 0xC00, "48 8B 8F ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 48 02 00 00 48 8B 8F ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 48 02 00 00 48 8B 8F ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 FF 90 48 02 00 00", NULL);
    if (!p3) { logf_("  view panel members not found"); return 0; }
    for (int i = 0; i < 3; i++) O_VIEW_PANEL[i] = rd32(p3 + 3 + 21 * i);
    if (!fn_is(V_VIEW[SL_NOTIFY], "48 89 5C 24 18 48 89 7C 24 20 55 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 8B 42 08", "DynamicCampaignView::Notify")) return 0;
    return 1;
}

/* research tree zoom: dialog geometry, the research lines, the tree's scroll pane and the GUI mouse position */
static int find_zoom(void)
{
    /* vt[14] OnPositionChanged: push rbx ; sub rsp,20 ; mov r8,[rcx+PARENT] ; mov rbx,rcx ; mov ecx,[rcx+POS] */
    uint8_t *pc = (uint8_t *)V_ICON[SL_POSCHANGED];
    if (!fn_is(pc, "40 53 48 83 EC 20 4C 8B 81 ?? ?? ?? ?? 48 8B D9 8B 89 ?? ?? ?? ??", "eDialog::OnPositionChanged")) return 0;
    if (rd32(pc + 9) != O_DLG_PARENT) { logf_("  OnPositionChanged: parent field mismatch"); return 0; }
    O_DLG_POS = rd32(pc + 18);
    /* vt[16] SetSize(size by value): mov [rsp+10],rdx ; push rbx ; sub rsp,20 ; mov rbx,rcx ; mov ecx,[rsp+38] ; cmp [rbx+SIZE],ecx */
    uint8_t *ss = (uint8_t *)V_ICON[SL_SETSIZE];
    if (!fn_is(ss, "48 89 54 24 10 53 48 83 EC 20 48 8B D9 8B 4C 24 38 39 8B ?? ?? ?? ?? 75", "eDialog::SetSize")) return 0;
    O_DLG_SIZE = rd32(ss + 19);
    /* vt[57] SetPreferredSize: mov rax,[rdx] ; mov [rcx+PREF],rax ; ret.  vt[22]/vt[23] Get{Min,Max}Size: mov rax,[rcx+X] ; mov [rdx],rax */
    if (!fn_is(V_ICON[SL_SETPREF], "48 8B 02 48 89 81 ?? ?? ?? ?? C3", "eDialog::SetPreferredSize")) return 0;
    O_DLG_PREF = rd32((uint8_t *)V_ICON[SL_SETPREF] + 6);
    const char *mm = "48 8B 81 ?? ?? ?? ?? 48 89 02 48 8B C2 C3";
    if (!fn_is(V_ICON[SL_GETMIN], mm, "eDialog::GetMinSize") || !fn_is(V_ICON[SL_GETMAX], mm, "eDialog::GetMaxSize")) return 0;
    O_DLG_MIN = rd32((uint8_t *)V_ICON[SL_GETMIN] + 3);
    O_DLG_MAX = rd32((uint8_t *)V_ICON[SL_GETMAX] + 3);
    /* LayoutInit places each icon at (size + spacing) * grid: movsxd rcx,[r14+SIZE] ; xorps xmm0,xmm0 ; add rcx,rdi */
    uint8_t *li = (uint8_t *)V_TREE[SL_LAYOUTINIT];
    uint8_t *ip = scan(li, 0xC00, "49 63 8E ?? ?? ?? ?? 0F 57 C0 48 03 CF", NULL);
    if (!ip || rd32(ip + 3) != O_DLG_SIZE) { logf_("  ResearchTree::LayoutInit: icon placement not recognised"); return 0; }
    /* lines: mov rdx,[rbx+LINES] ; mov rcx,rsi ; call SetParent ; ... mov r8,rdi ; mov rcx,rsi ; mov rdx,[rax] ; call LineSetup */
    uint8_t *ln = scan(li, 0xC00, "48 8B 93 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 49 8B D6 48 8D 8B ?? ?? ?? ?? E8", NULL);
    uint8_t *lc = ln ? scan(ln, 0x60, "4C 8B C7 48 8B CE 48 8B 10 E8", NULL) : NULL;
    if (!lc) { logf_("  ResearchTree::LayoutInit: research lines not recognised"); return 0; }
    O_TREE_LINES = rd32(ln + 3);
    /* LineSetup(line, iconA, iconB) computes the line from the icons' centres: ... mov r15,rcx ; mov [rcx+A],rdx ; mov [rcx+B],r8 */
    uint8_t *ls = rel32_target(lc + 9, 1, 5);
    if (!fn_is(ls, "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 4C 8B F9 48 89 91 ?? ?? ?? ?? 4C 89 81", "ResearchLine::Setup")) return 0;
    O_LINE_A = rd32(ls + 35); O_LINE_B = rd32(ls + 42);
    g_lineSetup = (LineSetupFn)ls;
    if (!(V_LINE = find_vtable_rtti(".?AVeGUI_CampaginResearchLine@GUI2@@"))) { logf_("  class eGUI_CampaginResearchLine not found"); return 0; }
    /* tree Notify, drag (code 11): GetScroll(pane) - delta -> SetScroll(pane, pos, 0) */
    uint8_t *dn = scan(V_TREE[SL_NOTIFY], 0x100, "48 8B 5A 20 48 8D 55 ?? 48 8B 89 ?? ?? ?? ?? E8 ?? ?? ?? ?? 45 33 C0 48 8D 55 ?? 8B 48 04 8B 00 2B 4B 04 2B 03 89 4D ?? 49 8B 8E ?? ?? ?? ?? 89 45 ?? E8", NULL);
    if (!dn || rd32(dn + 11) != rd32(dn + 43)) { logf_("  ResearchTree::Notify: drag scrolling not recognised"); return 0; }
    O_TREE_PANE = rd32(dn + 11);
    uint8_t *gs = rel32_target(dn + 15, 1, 5), *st = rel32_target(dn + 50, 1, 5);
    if (!fn_is(gs, "48 8B 05 ?? ?? ?? ?? 48 89 02 48 8B 81 ?? ?? ?? ?? 48 85 C0 74", "ScrollPane::GetScroll")) return 0;
    if (!fn_is(st, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 0F B6 F8 48 8B F2 48 8B D9", "ScrollPane::SetScroll")) return 0;
    g_getScroll = (GetScrollFn)gs; g_setScroll = (SetScrollFn)st;
    /* the scroll pane's wheel handler (vt[51]) and its layout manager's Arrange (vt[4], sets the scroll ranges) */
    V_PANE = find_vtable_rtti(".?AVeScrollPane@GUI2@@");
    V_PANELAYOUT = find_vtable_rtti(".?AVeScrollPaneLayout@GUI2@@");
    if (!V_PANE || !V_PANELAYOUT) { logf_("  class eScrollPane / eScrollPaneLayout not found"); return 0; }
    if (!fn_is(V_PANE[SL_WHEEL], "80 B9 ?? ?? ?? ?? 00 74 ?? 83 B9 ?? ?? ?? ?? 00 74 ?? 0F AF 91", "ScrollPane::OnMouseWheel")) return 0;
    if (!fn_is(V_PANELAYOUT[SL_LM_ARRANGE], "4C 8B DC 56 48 81 EC 80 00 00 00 48 8B 82", "ScrollPaneLayout::Arrange")) return 0;
    /* GUI mouse position: the scroll bar's mouse-button handler (vt[39]) reads it and makes it local:
       lea rcx,[rsp+X] ; call MousePosition ; mov r8,rax ; lea rdx,[rsp+Y] ; mov rcx,rbx ; call ToLocal */
    V_SCROLLBAR = find_vtable_rtti(".?AVeScrollBar@@");
    uint8_t *cu = V_SCROLLBAR ? scan(V_SCROLLBAR[SL_MOUSEBTN], 0x120, "48 8D 4C 24 ?? E8 ?? ?? ?? ?? 4C 8B C0 48 8D 54 24 ?? 48 8B CB E8", NULL) : NULL;
    if (!cu) { logf_("  GUI mouse position not found"); return 0; }
    uint8_t *cf = rel32_target(cu + 5, 1, 5), *tl = rel32_target(cu + 21, 1, 5);
    if (!fn_is(cf, "48 8B 05 ?? ?? ?? ?? 48 89 01 48 8B C1 C3", "GUI::MousePosition")) return 0;
    if (!fn_is(tl, "40 53 48 83 EC 20 49 8B 00 48 8B DA 48 89 02 48 85 C9 74", "eDialog::ToLocal")) return 0;
    g_cursor = (CursorFn)cf; g_toLocal = (ToLocalFn)tl;
    return 1;
}

/* Library guard. eGUI_MultiplayerLibrary::OnEvent uses its render engine (+0x280, created by Start) without a null
   check on 4 events: 0x469 and 0x101D (scene ready) -> engine+0x12 write, 0xFB6 (scene unload) -> engine Clear,
   0xFD2 (tick, when shown) -> engine Quant. A library nobody started (the research tree inspector of the vanilla
   layout, commented out by the developers for this reason) crashes the game on the first of these events.
   The guard passes those 4 events of an engine-less library to eDialog::OnEvent only (what the library forwards
   every event to after its own handling). */
static uint32_t g_libEngineEvents[4];
static AnyFn g_dialogOnEvent;

static int find_library_guard(void)
{
    /* mov [rsp+8],rbx ; push rdi ; sub rsp,20 ; mov rbx,rcx ; mov rdi,rdx ; mov ecx,[rdx+8] ; sub ecx,A ; je ; sub ecx,B ; je ;
       sub ecx,C ; je ; cmp ecx,D ; jne ; cmp dword [rdx+38],1 ... ; tail: mov rdx,rdi ; mov rcx,rbx ; ... ; pop rdi ; jmp eDialog::OnEvent */
    uint8_t *f = (uint8_t *)V_LIB[SL_ONEVENT];
    if (!fn_is(f, "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 48 8B FA 8B 4A 08 81 E9 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 81 E9 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 83 E9 ?? 0F 84 ?? ?? ?? ?? 83 F9 ?? 0F 85", "MultiplayerLibrary::OnEvent (guard)")) return 0;
    uint32_t a = rd32(f + 21), b = rd32(f + 33); uint8_t c = f[45], d = f[54];
    g_libEngineEvents[0] = a; g_libEngineEvents[1] = a + b; g_libEngineEvents[2] = a + b + c; g_libEngineEvents[3] = a + b + c + d;
    if (g_libEngineEvents[1] != EV_SCENE_UNLOAD || g_libEngineEvents[3] != EV_SCENE_READY) { logf_("  library events not the expected ones"); return 0; }
    uint8_t *t = scan(f, 0x200, "48 8B D7 48 8B CB 48 8B 5C 24 30 48 83 C4 20 5F E9", NULL);
    void **vd = find_vtable_rtti(".?AVeDialog@@");
    if (!t || !vd || rel32_target(t + 16, 1, 5) != (uint8_t *)vd[SL_ONEVENT]) { logf_("  library OnEvent does not forward to eDialog::OnEvent"); return 0; }
    g_dialogOnEvent = (AnyFn)vd[SL_ONEVENT];
    return find_lib_start() != NULL;
}

static int g_okCommon, g_okButtons, g_okTree, g_okPreview, g_okZoom, g_okPortraits, g_okGuard;

static int find_all(void)
{
    static const struct { void ***vt; const char *cls; } classes[] = {
        { &V_TREE, "eGUI_CampaignResearchTree" }, { &V_ICON, "eGUI_CampaignResearchIcon" },
        { &V_LIB, "eGUI_MultiplayerLibrary" }, { &V_VIEW, "eGUI_DynamicCampaignView" },
        { &V_WRAP, "ePortraitWrapPanel" }, { &V_PORTRAIT, "eGUI_ReinforcementsPortrait" }, { &V_FBTN, "eFrameButton" },
        { &V_PANEL[0], "eCampaignReinforcementsPanel" }, { &V_PANEL[1], "eCampaignAirstrikesPanel" },
        { &V_PANEL[2], "eCampaignEmplacementsPanel" },
    };
    int all = 1;
    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        char dec[96]; snprintf(dec, sizeof dec, ".?AV%s@GUI2@@", classes[i].cls);
        if (!(*classes[i].vt = find_vtable_rtti(dec))) { logf_("  class %s not found", classes[i].cls); all = 0; }
    }
    if (!all) return 0;
    g_okCommon = find_dialog_common();
    g_okButtons = g_okCommon && find_button_support();
    g_okTree = g_okCommon && find_tree();
    g_okPreview = g_okTree && find_research_preview();
    g_okZoom = g_okTree && find_zoom();
    g_okPortraits = g_okCommon && find_portraits();
    g_okGuard = find_library_guard();
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* dialog helpers                                                                       */
/* ------------------------------------------------------------------------------------ */
static void vcall(void *o, int slot) { ((ThisFn)VT(o)[slot])(o); }

static int dlg_visible(const void *d) { return (U32(d, O_DLG_FLAGS) >> 1) & 1u; }

/* the engine's own inline show/hide: flags bit1 + vt[5] (releases hover/focus, invalidates the layout) */
static int dlg_set_visible(void *d, int v)
{
    if (dlg_visible(d) == (v != 0)) return 0;
    U32(d, O_DLG_FLAGS) = (U32(d, O_DLG_FLAGS) & ~2u) | (v ? 2u : 0u);
    vcall(d, SL_VISCHANGED);
    return 1;
}

static void *find_child(void *dlg, const char *name)
{
    MsString s; ms_view(&s, name);
    return g_findChild(dlg, &s);
}

/* plausible std::vector [b, e) of at most max elements */
static int vec_ok(void *b, void *e, size_t stride, size_t max)
{
    if (!b || !e) return b == e;
    if ((uint8_t *)e < (uint8_t *)b) return 0;
    size_t n = (size_t)((uint8_t *)e - (uint8_t *)b);
    return n % stride == 0 && n / stride <= max;
}

static int is_panel(void *o) { void **vt = VT(o); return vt == V_PANEL[0] || vt == V_PANEL[1] || vt == V_PANEL[2]; }

/* ------------------------------------------------------------------------------------ */
/* 1. research tree preview                                                             */
/* ------------------------------------------------------------------------------------ */
static int g_wantPreview = 1;
static char g_inspectorName[64] = "inspector";
static AnyFn o_treeBind, o_treeNotify, o_treeClose, o_treeDtor, o_libEvent, o_iconDisabled, o_iconStateMap;

static struct {
    uint8_t *tree, *lib, *units;   /* bound research tree, its inspector, its unitsContainer */
    void *tpl;                     /* template to show (NULL = clear) */
    int bound, clicked, pending, busy;
} g_rp;

static void rp_apply(void)
{
    if (!g_rp.bound || !g_rp.pending || g_rp.busy) return;
    uint8_t *lib = g_rp.lib;
    if (!U8(lib, O_LIB_READY)) return;                 /* 3D scene not ready: retried on its "ready" event */
    g_rp.pending = 0; g_rp.busy = 1;
    if (g_rp.tpl) {
        if (PTR(lib, O_LIB_CUR) != g_rp.tpl) g_libClear(lib);
        g_showTemplate(lib, g_rp.tpl);
        VLOG(40, "research preview: showing template %p", g_rp.tpl);
    } else {
        g_libClear(lib);
        VLOG(40, "research preview: node has no unit template - cleared");
    }
    g_rp.busy = 0;
}

static void rp_unbind(const char *why)
{
    if (!g_rp.tree) return;
    memset(&g_rp, 0, sizeof g_rp);
    VLOG(40, "research preview: inspector detached (%s)", why);
}

static void rp_preview(uint8_t *icon)
{
    void *tpl = NULL;
    uint8_t *node = PTR(icon, O_ICON_NODE);
    if (node && node[0] == 0) {                        /* unit node */
        _Alignas(16) uint8_t tag[0x80];
        memset(tag, 0, sizeof tag);
        g_tagFromStr(tag, (const MsString *)(icon + O_DLG_NAME));
        tpl = g_configLookup(tag);
        g_tagDtor(tag);
    }
    g_rp.tpl = tpl; g_rp.clicked = 1; g_rp.pending = 1;
    rp_apply();
}

/* tree vt[19] (layout init) runs the base layout init, which loads the XML and calls this Bind; the icons are created
   afterwards, so the binding is complete before the first icon exists (icons re-read vt[73]/vt[86] on every layout pass) */
static void *hk_tree_bind(void *tree, void *a2, void *a3, void *a4)
{
    void *r = o_treeBind(tree, a2, a3, a4);
    if (!tree || VT(tree) != V_TREE) return r;
    uint8_t *insp = find_child(tree, g_inspectorName);
    if (!insp || VT(insp) != V_LIB) {
        LOG_CAPPED(3, "research preview: the tree has no \"%s\" multiplayer library - install mod_side/DynamicCampaignResearchTree.xml", g_inspectorName);
        return r;
    }
    if (g_rp.bound && g_rp.tree == tree && g_rp.lib == insp) return r;
    uint8_t *owner = PTR(tree, O_TREE_OWNER);
    if (!owner || VT(owner) != V_VIEW) { LOG_CAPPED(3, "research preview: research tree owner is not the campaign view"); return r; }
    uint8_t *prep = PTR(owner, O_VIEW_PREP);
    if (!prep || VT(prep) != V_LIB || prep == insp) { LOG_CAPPED(3, "research preview: campaign view inspector not found"); return r; }
    if (g_rp.tree) rp_unbind("replaced by a new research tree");
    g_rp.tree = tree; g_rp.lib = insp; g_rp.units = PTR(tree, O_TREE_UNITS);
    g_libStart(insp);                                  /* allocates the inspector scene that SetContext/Clear use */
    g_libCtx(insp, prep + O_LIB_CTX);                  /* same faction/army context as the campaign page inspector */
    g_rp.bound = 1;
    VLOG(40, "research preview: inspector bound (tree %p, library %p, scene ready %u)", (void *)tree, (void *)insp, U8(insp, O_LIB_READY));
    rp_apply();
    return r;
}

static int rp_is_tree_icon(uint8_t *icon)
{
    return icon && g_rp.tree && VT(icon) == V_ICON && g_rp.units && PTR(icon, O_DLG_PARENT) == g_rp.units;
}

/* bool returned in al (the rest of rax is garbage) */
static int al_true(void *r) { uint8_t b; memcpy(&b, &r, 1); return b != 0; }

static void *hk_tree_notify(void *tree, void *ev, void *a3, void *a4)
{
    if (g_rp.bound && tree == g_rp.tree && ev) {
        uint32_t code = U32(ev, EV_ID);
        uint8_t *icon = PTR(ev, EV_SENDER);
        if ((code == TREE_ICON_TOGGLE || code == TREE_ICON_DESELECT) && rp_is_tree_icon(icon)) {
            void *r = NULL;
            if (U32(icon, O_ICON_STATE) == 1) g_setChecked(icon, 0, 1);   /* researched: preview only, no research (de)selection */
            else r = o_treeNotify(tree, ev, a3, a4);
            if (g_rp.bound && tree == g_rp.tree) rp_preview(icon);
            return r;
        }
    }
    return o_treeNotify(tree, ev, a3, a4);
}

static int g_guardOn;

static int is_engine_event(uint32_t id)
{
    for (int i = 0; i < 4; i++) if (id == g_libEngineEvents[i]) return 1;
    return 0;
}

/* library OnEvent: the guard (always, when installed) and the research preview's scene events */
static void *hk_lib_event(void *lib, void *ev, void *a3, void *a4)
{
    if (g_guardOn && lib && ev && !PTR(lib, O_LIB_ENGINE) && is_engine_event(U32(ev, EV_ID))) {
        LOG_CAPPED(5, "library guard: library %p was never started - event 0x%X not passed to it (would have crashed)", lib, U32(ev, EV_ID));
        return g_dialogOnEvent(lib, ev, a3, a4);
    }
    void *r = o_libEvent(lib, ev, a3, a4);
    if (g_rp.bound && lib == g_rp.lib && ev) {
        uint32_t id = U32(ev, EV_ID);
        if (id == EV_SCENE_UNLOAD) g_rp.pending = g_rp.clicked;
        else if (id == EV_SCENE_READY && U32(ev, EV_ARG) == 1) {
            if (g_rp.clicked) { g_libClear(lib); g_rp.pending = 1; }
            VLOG(10, "research preview: inspector scene ready");
            rp_apply();
        }
    }
    return r;
}

/* researched icons are "disabled" (not clickable) in vanilla; keep them clickable for the preview */
static void *hk_icon_disabled(void *icon, void *a2, void *a3, void *a4)
{
    void *r = o_iconDisabled(icon, a2, a3, a4);
    if (al_true(r) && rp_is_tree_icon(icon)) return (void *)0;
    return r;
}

/* ...but keep their "researched" look. vt[86] fills the frame used for the button states {normal, hover, pressed,
   disabled} = {4, 1, 2, 0}; a researched icon is no longer drawn "disabled", so its normal state gets the disabled frame */
static void *hk_icon_statemap(void *icon, void *out, void *a3, void *a4)
{
    void *r = o_iconStateMap(icon, out, a3, a4);
    if (al_true(r) && out && rp_is_tree_icon(icon) && U32(icon, O_ICON_STATE) == 1)
        ((uint32_t *)out)[0] = ((uint32_t *)out)[3];
    return r;
}

/* ------------------------------------------------------------------------------------ */
/* 2. research tree zoom                                                                */
/* ------------------------------------------------------------------------------------ */
/* LayoutInit places each icon at (size + spacing) * grid and builds each line once from its icons' centres. A zoom
   step scales the icons from that zoom-1 layout (their pictures use zoomMode="fit"), rebuilds the lines with the
   engine's LineSetup and keeps the point under the mouse in place. */
#define KEY_HELD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)     /* the offline stub always says no */
static int g_wantZoom = 1, g_zoomKey = 0, g_zoomRemember = 1;      /* g_zoomKey: virtual key that must be held, 0 = none */
static double g_zoom = 1.0, g_zoomMin = 0.4, g_zoomMax = 1.5, g_zoomStep = 1.2;
static unsigned g_zoomDelayMs = 150;                               /* least time between two zoom levels */
static AnyFn o_treeLayoutInit, o_paneWheel, o_paneArrange;

#define MAX_ZOOM_ICONS 1024
typedef struct { uint8_t *icon; Pt pos, size, pref, min, max; int live; } ZoomIcon;
static struct {
    uint8_t *tree, *pane, *units, *lines;   /* bound research tree and its containers */
    int n; ZoomIcon it[MAX_ZOOM_ICONS];     /* zoom-1 geometry of its icons */
    int pending; Pt target;                 /* scroll position to set after the next layout passes */
    unsigned long long lastStep;            /* GetTickCount64 of the last zoom level */
} g_zm;

static int32_t iround(double r) { return (int32_t)(r < 0 ? r - 0.5 : r + 0.5); }
static int32_t zscale(int32_t v, double z) { return iround(v * z); }
static Pt pt_scale(Pt p, double z) { Pt r = { zscale(p.x, z), zscale(p.y, z) }; return r; }
static int pt_eq(Pt a, Pt b) { return a.x == b.x && a.y == b.y; }
static unsigned long long pt_pack(Pt p) { return (uint32_t)p.x | (unsigned long long)(uint32_t)p.y << 32; }

/* the containers keep their order, so the search starts after the previous hit (one compare in the usual case) */
static ZoomIcon *zm_find(const uint8_t *icon)
{
    static int hint;
    for (int k = 0; k < g_zm.n; k++) {
        int i = (hint + k) % g_zm.n;
        if (g_zm.it[i].icon == icon) { hint = i + 1; return &g_zm.it[i]; }
    }
    return NULL;
}

/* children of a container, if its vector looks sane */
static int child_vec(uint8_t *dlg, uint8_t ***b, uint8_t ***e)
{
    *b = (uint8_t **)PTR(dlg, O_DLG_CHILD); *e = (uint8_t **)PTR(dlg, O_DLG_CHILD + 8);
    return vec_ok(*b, *e, sizeof(void *), 4 * MAX_ZOOM_ICONS);
}

static void zm_unbind(const char *why)
{
    if (!g_zm.tree) return;
    g_zm.tree = g_zm.pane = g_zm.units = g_zm.lines = NULL; g_zm.n = 0; g_zm.pending = 0;
    VLOG(20, "research zoom: tree released (%s)", why);
}

/* records the zoom-1 geometry right after LayoutInit placed the icons */
static void zm_capture(uint8_t *tree)
{
    zm_unbind("replaced by a new research tree");
    uint8_t *units = PTR(tree, O_TREE_UNITS), *lines = PTR(tree, O_TREE_LINES), *pane = PTR(tree, O_TREE_PANE), **b, **e;
    if (!units || !lines || !pane || VT(pane) != V_PANE || !child_vec(units, &b, &e)) {
        LOG_CAPPED(3, "research zoom: research tree containers not found - no zoom for this tree"); return;
    }
    for (; b < e && g_zm.n < MAX_ZOOM_ICONS; b++) {
        uint8_t *c = *b;
        if (!c || VT(c) != V_ICON) continue;
        ZoomIcon *t = &g_zm.it[g_zm.n++];
        t->icon = c; t->pos = PT(c, O_DLG_POS); t->size = PT(c, O_DLG_SIZE);
        t->pref = PT(c, O_DLG_PREF); t->min = PT(c, O_DLG_MIN); t->max = PT(c, O_DLG_MAX);
    }
    g_zm.tree = tree; g_zm.pane = pane; g_zm.units = units; g_zm.lines = lines;
    VLOG(20, "research zoom: tree bound (%d icons)", g_zm.n);
}

/* sets every live icon of the bound tree to zoom z (from its zoom-1 geometry) and rebuilds the lines */
static void zm_apply(double z)
{
    uint8_t **b, **e;
    if (!g_zm.tree || !child_vec(g_zm.units, &b, &e)) return;
    int one = z == 1.0, moved = 0, lines = 0;
    for (int i = 0; i < g_zm.n; i++) g_zm.it[i].live = 0;
    for (; b < e; b++) {                    /* only the container's current children are touched */
        uint8_t *ic = *b;
        ZoomIcon *t = zm_find(ic);
        if (!t || VT(ic) != V_ICON) continue;
        t->live = 1;
        Pt sz = pt_scale(t->size, z);
        if (sz.x < 4) sz.x = 4;
        if (sz.y < 4) sz.y = 4;
        /* the canvas measures its content with the preferred size: an unset one (0) follows the zoomed size */
        PT(ic, O_DLG_PREF) = one ? t->pref : (t->pref.x || t->pref.y) ? pt_scale(t->pref, z) : sz;
        PT(ic, O_DLG_MIN) = one ? t->min : pt_scale(t->min, z);
        PT(ic, O_DLG_MAX) = one ? t->max : pt_scale(t->max, z);
        if (!pt_eq(PT(ic, O_DLG_SIZE), sz)) ((SetSizeFn)VT(ic)[SL_SETSIZE])(ic, pt_pack(sz));
        Pt pos = pt_scale(t->pos, z);
        if (!pt_eq(PT(ic, O_DLG_POS), pos)) { PT(ic, O_DLG_POS) = pos; vcall(ic, SL_POSCHANGED); moved++; }
    }
    if (child_vec(g_zm.lines, &b, &e))
        for (; b < e; b++) {
            uint8_t *l = *b;
            if (!l || VT(l) != V_LINE) continue;
            ZoomIcon *ia = zm_find(PTR(l, O_LINE_A)), *ib = zm_find(PTR(l, O_LINE_B));
            if (ia && ib && ia->live && ib->live) { g_lineSetup(l, ia->icon, ib->icon); lines++; }
        }
    vcall(g_zm.units, SL_INVALIDATE);
    vcall(g_zm.lines, SL_INVALIDATE);
    VLOG(8, "research zoom: %d%% (%d icons moved, %d lines rebuilt)", (int)(z * 100 + 0.5), moved, lines);
}

/* one wheel event: notches < 0 = wheel up = zoom in, anchored at the mouse. Each level moves every icon and rebuilds
   every line, so notches arriving within zoom_delay ms of the last level are dropped. */
static void zm_wheel(int notches)
{
    unsigned long long now = GetTickCount64();
    if (g_zm.lastStep && now - g_zm.lastStep < g_zoomDelayMs) return;
    double z = g_zoom;
    int steps = notches < 0 ? -notches : notches;
    if (steps > 10) steps = 10;
    for (int i = 0; i < steps; i++) {
        double n = notches < 0 ? z * g_zoomStep : z / g_zoomStep;
        if ((z - 1.0) * (n - 1.0) < 0 || (n > 0.98 && n < 1.02)) n = 1.0;   /* 100% is always a stop: never skipped */
        z = n;
    }
    if (z < g_zoomMin) z = g_zoomMin;
    if (z > g_zoomMax) z = g_zoomMax;
    if (z == g_zoom) return;
    /* the content point P under the mouse moves to P * r: scrolling by P * (r - 1) keeps it under the mouse */
    Pt cur, loc, sc;
    g_cursor(&cur);
    g_toLocal(g_zm.units, &loc, &cur);      /* unitsContainer coordinates (scroll included) */
    g_getScroll(g_zm.pane, &sc);
    double r = z / g_zoom;
    g_zm.target.x = sc.x + zscale(loc.x, r - 1.0);
    g_zm.target.y = sc.y + zscale(loc.y, r - 1.0);
    g_zoom = z;
    zm_apply(z);
    g_setScroll(g_zm.pane, &g_zm.target, 0);   /* clamped to the old ranges; set again after the next layout passes */
    g_zm.pending = 2;
    g_zm.lastStep = now ? now : 1;
}

/* tree vt[19]: the icons and lines exist once the original returns */
static void *hk_tree_layoutinit(void *tree, void *a2, void *a3, void *a4)
{
    void *r = o_treeLayoutInit(tree, a2, a3, a4);
    if (tree && VT(tree) == V_TREE) {
        zm_capture(tree);
        if (!g_zoomRemember) g_zoom = 1.0;
        if (g_zoom != 1.0) { zm_apply(g_zoom); VLOG(20, "research zoom: reopened at %d%%", (int)(g_zoom * 100 + 0.5)); }
    }
    return r;
}

/* scroll pane vt[51] OnMouseWheel(notches), shared by every scroll pane: only the bound tree's pane zooms */
static void *hk_pane_wheel(void *pane, void *notches, void *a3, void *a4)
{
    if (pane && pane == g_zm.pane && (!g_zoomKey || KEY_HELD(g_zoomKey))) {
        zm_wheel((int)(long long)notches);   /* an int in edx */
        return NULL;
    }
    return o_paneWheel(pane, notches, a3, a4);
}

/* eScrollPaneLayout vt[4] Arrange(pane): sets the scroll ranges, so a pending scroll position fits afterwards */
static void *hk_pane_arrange(void *lm, void *pane, void *a3, void *a4)
{
    void *r = o_paneArrange(lm, pane, a3, a4);
    if (g_zm.pending && pane && pane == g_zm.pane) { g_zm.pending--; g_setScroll(pane, &g_zm.target, 0); }
    return r;
}

/* tree lifetime (shared by the preview and the zoom) */
static void *hk_tree_close(void *tree, void *a2, void *a3, void *a4)
{
    if (tree && tree == g_rp.tree) {
        uint8_t *lib = g_rp.lib;
        rp_unbind("tree closed");
        if (lib && PTR(lib, O_LIB_ENGINE)) g_libClear(lib);
    }
    if (tree && tree == g_zm.tree) zm_unbind("tree closed");
    return o_treeClose(tree, a2, a3, a4);
}

static void *hk_tree_dtor(void *tree, void *flags, void *a3, void *a4)
{
    if (tree && tree == g_rp.tree) rp_unbind("tree destroyed");
    if (tree && tree == g_zm.tree) zm_unbind("tree destroyed");
    return o_treeDtor(tree, flags, a3, a4);
}

/* ------------------------------------------------------------------------------------ */
/* 3. unlocked portraits                                                                */
/* ------------------------------------------------------------------------------------ */
enum { MODE_ALL_UNLOCKED_FIRST = 0, MODE_UNLOCKED_ONLY = 1 };
static int g_wantPortraits = 1, g_mode = MODE_UNLOCKED_ONLY, g_wantToggle = 1;
#define LABEL_SHOW_LOCKED "main_menu2/dynamic_campaign/gem_show_locked"   /* localization keys of the button */
#define LABEL_HIDE_LOCKED "main_menu2/dynamic_campaign/gem_hide_locked"
#define LABEL_SHOW_LOCKED_EN "SHOW LOCKED"                          /* shown when the key has no text */
#define LABEL_HIDE_LOCKED_EN "HIDE LOCKED"
static char g_toggleName[64] = "btn_portraits", g_labelOnly[96] = LABEL_SHOW_LOCKED, g_labelAll[96] = LABEL_HIDE_LOCKED;
#define TOGGLE_CODE 0x47430001                         /* our notification code: 'GC' 1 (engine codes are small) */
static AnyFn o_panelRefresh[3], o_viewBind, o_viewNotify;

static const char *mode_name(int mode) { return mode == MODE_UNLOCKED_ONLY ? "unlocked only" : "all, unlocked first"; }

typedef struct { void *tpl; uint8_t *portrait; } Pair;   /* panel vector element (only its layout matters here) */
#define MAX_PORTRAITS 1024                             /* per panel; the zoom has its own MAX_ZOOM_ICONS */
#define MAX_LIST_WALK (4 * MAX_PORTRAITS)              /* longest std::list walk before it counts as corrupt */

/* MSVC std::list node: next, prev, value */
#define LIST_NEXT(node) (*(uint8_t **)(node))
#define LIST_VAL(node)  PTR(node, 0x10)
/* notify-mapping list node value: command (int64), code, destination dialog */
enum { NM_CMD = 0x10, NM_CODE = 0x18, NM_DST = 0x20 };

static int is_panel_portrait(uint8_t *c, uint8_t *wrap)
{
    return c && VT(c) == V_PORTRAIT && PTR(c, O_DLG_PARENT) == wrap;
}

/* visible but never revealed: must be in the reveal queue */
static int needs_reveal(const uint8_t *p) { return dlg_visible(p) && U32(p, O_PORTRAIT_REVEAL) == 0; }

/* read-only walk of the wrap panel's reveal queue (std::list of weak refs to portraits).
   Needs a rebuild when it holds a hidden portrait of this panel, or when its not-yet-revealed entries are not
   exactly the portraits that need a reveal, in display order. */
static int queue_needs_rebuild(uint8_t *wrap, uint8_t **ord, size_t k)
{
    uint8_t *head = PTR(wrap, O_WRAP_QUEUE);
    if (!head) return 0;                    /* no list to fix (never seen: MSVC std::list always has its sentinel) */
    size_t want = 0, n = 0;
    for (uint8_t *node = LIST_NEXT(head); node != head; node = LIST_NEXT(node)) {
        if (!node || ++n > MAX_LIST_WALK) return 1;
        uint8_t *p = LIST_VAL(node);
        if (!is_panel_portrait(p, wrap)) continue;
        if (!dlg_visible(p)) return 1;
        if (U32(p, O_PORTRAIT_REVEAL) != 0) continue;  /* the head (animating) or an already revealed entry */
        while (want < k && !needs_reveal(ord[want])) want++;
        if (want >= k || ord[want] != p) return 1;
        want++;
    }
    for (; want < k; want++) if (needs_reveal(ord[want])) return 1;   /* not queued */
    return 0;
}

/* apply the mode to one panel: order (unlocked first), visibility, reveal queue */
static void pf_apply(void *panel, const char *why)
{
    if (!panel || !is_panel(panel)) return;
    uint8_t *wrap = PTR(panel, O_PANEL_WRAP);
    if (!wrap || VT(wrap) != V_WRAP) return;
    /* sanity check of both containers before touching anything (the pairs vector is only checked) */
    Pair *pb = (Pair *)PTR(panel, O_PANEL_PAIRS), *pe = (Pair *)PTR(panel, O_PANEL_PAIRS + 8);
    uint8_t **cb = (uint8_t **)PTR(wrap, O_DLG_CHILD), **ce = (uint8_t **)PTR(wrap, O_DLG_CHILD + 8);
    if (!vec_ok(pb, pe, sizeof(Pair), MAX_PORTRAITS) || !vec_ok(cb, ce, sizeof(void *), MAX_PORTRAITS)) { LOG_CAPPED(5, "portraits: panel containers look wrong - skipped"); return; }
    size_t nc = (size_t)(ce - cb);

    /* 1. unlocked first (stable), only among the portrait slots of the child vector: a pure permutation */
    static uint8_t *ord[MAX_PORTRAITS]; static size_t slot[MAX_PORTRAITS];
    size_t k = 0, m = 0, reordered = 0;
    for (size_t i = 0; i < nc; i++) if (is_panel_portrait(cb[i], wrap)) slot[k++] = i;
    for (size_t j = 0; j < k; j++) if (U8(cb[slot[j]], O_PORTRAIT_UNLOCKED)) ord[m++] = cb[slot[j]];
    size_t unlocked = m;
    for (size_t j = 0; j < k; j++) if (!U8(cb[slot[j]], O_PORTRAIT_UNLOCKED)) ord[m++] = cb[slot[j]];
    for (size_t j = 0; j < k; j++) if (cb[slot[j]] != ord[j]) { cb[slot[j]] = ord[j]; reordered++; }
    if (reordered) {
        U32(wrap, O_DLG_FLAGS) &= ~1u;                  /* what eDialog::AddChild does after changing the vector */
        vcall(wrap, SL_INVALIDATE);
        vcall(wrap, SL_REPAINT);
    }

    /* 2. visibility */
    size_t hidden = 0, shown = 0;
    for (size_t j = 0; j < k; j++) {
        int vis = g_mode == MODE_ALL_UNLOCKED_FIRST || U8(ord[j], O_PORTRAIT_UNLOCKED);
        if (dlg_set_visible(ord[j], vis)) { if (vis) shown++; else hidden++; }
    }

    /* 3. reveal queue: no hidden portrait in it (a hidden head would stall every reveal behind it), and the
          portraits that need a reveal queued in display order. Rebuilt only when the queue says otherwise. */
    int rebuilt = queue_needs_rebuild(wrap, ord, k);
    if (rebuilt) {
        ThisPtrFn enq = (ThisPtrFn)VT(wrap)[SL_REVEAL_ADD], deq = (ThisPtrFn)VT(wrap)[SL_REVEAL_DEL];
        /* 3a. hidden portraits out, last queued first: dequeuing the head starts the next one, so going from the back
               never promotes (and starts) another hidden portrait */
        static uint8_t *q[MAX_LIST_WALK]; size_t nq = 0;
        uint8_t *head = PTR(wrap, O_WRAP_QUEUE);
        for (uint8_t *node = LIST_NEXT(head); node && node != head && nq < MAX_LIST_WALK; node = LIST_NEXT(node)) q[nq++] = LIST_VAL(node);
        for (size_t i = nq; i-- > 0;) if (is_panel_portrait(q[i], wrap) && !dlg_visible(q[i])) deq(wrap, q[i]);
        /* 3b. the portraits that need a reveal: re-queued in display order */
        for (size_t j = 0; j < k; j++) if (needs_reveal(ord[j])) { deq(wrap, ord[j]); enq(wrap, ord[j]); }
    }
    if (reordered || hidden || shown || rebuilt)
        VLOG(16, "portraits (%s): %zu portraits, %zu unlocked, mode %d -> %zu moved, %zu hidden, %zu shown%s",
             why, k, unlocked, g_mode, reordered, hidden, shown, rebuilt ? ", reveal queue re-sequenced" : "");
}

/* the toggle button shows what a click will do. A label with a '/' and no space is a localization key (the .pot files
   in localizations/<language>/interface/text); a key the game has no text for comes back as the key itself, and then
   the English fallback is shown */
static void pf_label(void *view)
{
    if (!g_btnSetText) return;
    int only = g_mode == MODE_UNLOCKED_ONLY;
    const char *txt = only ? g_labelOnly : g_labelAll, *fallback = only ? LABEL_SHOW_LOCKED_EN : LABEL_HIDE_LOCKED_EN;
    if (!txt[0]) return;
    uint8_t *btn = find_child(view, g_toggleName);
    if (!btn || VT(btn) != V_FBTN) return;
    MsString s;
    if (strchr(txt, '/') && !strchr(txt, ' ')) {
        void *e = g_locEntry ? g_locEntry(txt) : NULL;
        const MsString *t = e ? g_locText(e) : NULL;
        const char *tc = t ? (t->cap > 15 ? t->u.ptr : t->u.buf) : NULL;
        if (t && t->size && strcmp(tc, txt)) { g_btnSetText(btn, t); return; }
        txt = fallback;
    }
    ms_view(&s, txt);
    g_btnSetText(btn, &s);
}

static void *pf_refresh(int i, void *panel, void *a2, void *a3, void *a4)
{
    void *r = o_panelRefresh[i](panel, a2, a3, a4);
    pf_apply(panel, PANEL_NAMES[i]);
    return r;
}
static void *hk_panel_refresh0(void *p, void *a2, void *a3, void *a4) { return pf_refresh(0, p, a2, a3, a4); }
static void *hk_panel_refresh1(void *p, void *a2, void *a3, void *a4) { return pf_refresh(1, p, a2, a3, a4); }
static void *hk_panel_refresh2(void *p, void *a2, void *a3, void *a4) { return pf_refresh(2, p, a2, a3, a4); }

/* already mapped (view Bind can run more than once for the same button)? walks the engine's mapping list */
static int notify_mapped(uint8_t *btn, void *view)
{
    uint8_t *head = PTR(btn, O_DLG_NOTIFY);
    if (!head) return 0;
    int n = 0;
    for (uint8_t *node = LIST_NEXT(head); node && node != head && n < 256; node = LIST_NEXT(node), n++) {
        void *dst = PTR(node, NM_DST);
        if ((!dst || dst == view) && *(int64_t *)(node + NM_CMD) == NOTIFY_ON_CLICK && U32(node, NM_CODE) == TOGGLE_CODE) return 1;
    }
    return 0;
}

static void *hk_view_bind(void *view, void *a2, void *a3, void *a4)
{
    void *r = o_viewBind(view, a2, a3, a4);
    if (!g_wantToggle || !view || VT(view) != V_VIEW) return r;
    uint8_t *btn = find_child(view, g_toggleName);
    if (!btn || VT(btn) != V_FBTN) {
        LOG_CAPPED(3, "portraits: no \"%s\" button on the campaign page - install mod_side/DynamicCampaignOverview.xml (mode stays %d)", g_toggleName, g_mode);
        return r;
    }
    if (!notify_mapped(btn, view)) {
        U8(btn, O_FBTN_FRAME) |= 1;                     /* the engine does the same for btn_open_research & co */
        g_notifyMap(btn, view, NOTIFY_ON_CLICK, TOGGLE_CODE);
        VLOG(10, "portraits: \"%s\" bound to the mode switch", g_toggleName);
    }
    pf_label(view);
    return r;
}

static void *hk_view_notify(void *view, void *ev, void *a3, void *a4)
{
    if (!ev || U32(ev, EV_ID) != TOGGLE_CODE || !view || VT(view) != V_VIEW) return o_viewNotify(view, ev, a3, a4);
    g_mode = g_mode == MODE_UNLOCKED_ONLY ? MODE_ALL_UNLOCKED_FIRST : MODE_UNLOCKED_ONLY;
    logf_("portraits: mode switched to %d (%s)", g_mode, mode_name(g_mode));
    for (int i = 0; i < 3; i++) {
        uint8_t *p = PTR(view, O_VIEW_PANEL[i]);
        if (p && is_panel(p)) pf_apply(p, "toggle");
    }
    uint8_t *sel = g_selectPortrait ? PTR(view, O_VIEW_SEL) : NULL;
    if (sel && VT(sel) == V_PORTRAIT && !dlg_visible(sel)) {
        g_selectPortrait(view, NULL, 0);                /* the selected portrait is now hidden: deselect it, clear the inspector */
        VLOG(10, "portraits: the selected portrait was hidden - deselected");
    }
    pf_label(view);
    return (void *)1;
}

/* ------------------------------------------------------------------------------------ */
/* install                                                                              */
/* ------------------------------------------------------------------------------------ */
#ifndef CU_TEST
typedef struct { void **vt; int slot; void *fn; AnyFn *orig; } Hook;

/* swap one vtable slot, published with one atomic store */
static int vt_set(void **vt, int slot, void *fn)
{
    DWORD old;
    if (!VirtualProtect(vt + slot, 8, PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer(vt + slot, fn);
    VirtualProtect(vt + slot, 8, old, &old);
    return 1;
}

/* all hooks of a feature or none: each original is stored before its slot is swapped, and on a failure the slots
   already swapped are restored */
static int install_set(Hook *h, int n)
{
    for (int i = 0; i < n; i++) {
        *h[i].orig = (AnyFn)h[i].vt[h[i].slot];
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!vt_set(h[i].vt, h[i].slot, h[i].fn)) {
            while (--i >= 0) vt_set(h[i].vt, h[i].slot, (void *)*h[i].orig);
            return 0;
        }
    }
    return 1;
}

static int g_libHooked;                         /* V_LIB[SL_ONEVENT] already swapped (by the guard or the preview) */

static int install_guard(void)
{
    Hook h[] = { { V_LIB, SL_ONEVENT, (void *)hk_lib_event, &o_libEvent } };
    if (!install_set(h, 1)) return 0;
    g_libHooked = 1; g_guardOn = 1;
    return 1;
}

/* tree dtor and Close, shared by the preview and the zoom: installed before either */
static int install_tree_lifetime(void)
{
    static int done;
    Hook h[] = {
        { V_TREE, SL_DTOR, (void *)hk_tree_dtor, &o_treeDtor },
        { V_TREE, SL_CLOSE, (void *)hk_tree_close, &o_treeClose },
    };
    return done || (done = install_set(h, 2));
}

static int install_preview(void)
{
    if (!install_tree_lifetime()) return 0;
    Hook h[] = {
        { V_TREE, SL_BIND, (void *)hk_tree_bind, &o_treeBind },
        { V_TREE, SL_NOTIFY, (void *)hk_tree_notify, &o_treeNotify },
        { V_ICON, SL_ICON_DISABLED, (void *)hk_icon_disabled, &o_iconDisabled },
        { V_ICON, SL_ICON_STATEMAP, (void *)hk_icon_statemap, &o_iconStateMap },
        { V_LIB, SL_ONEVENT, (void *)hk_lib_event, &o_libEvent },   /* last: only when the guard did not hook it */
    };
    int n = (int)(sizeof h / sizeof h[0]) - (g_libHooked ? 1 : 0);
    if (!install_set(h, n)) return 0;
    g_libHooked = 1;
    return 1;
}

static int install_zoom(void)
{
    if (!install_tree_lifetime()) return 0;
    Hook h[] = {
        { V_TREE, SL_LAYOUTINIT, (void *)hk_tree_layoutinit, &o_treeLayoutInit },
        { V_PANELAYOUT, SL_LM_ARRANGE, (void *)hk_pane_arrange, &o_paneArrange },
        { V_PANE, SL_WHEEL, (void *)hk_pane_wheel, &o_paneWheel },   /* last: the trigger */
    };
    return install_set(h, 3);
}

static int install_portraits(void)
{
    Hook h[] = {
        { V_PANEL[0], SL_PANEL_REFRESH, (void *)hk_panel_refresh0, &o_panelRefresh[0] },
        { V_PANEL[1], SL_PANEL_REFRESH, (void *)hk_panel_refresh1, &o_panelRefresh[1] },
        { V_PANEL[2], SL_PANEL_REFRESH, (void *)hk_panel_refresh2, &o_panelRefresh[2] },
        { V_VIEW, SL_BIND, (void *)hk_view_bind, &o_viewBind },
        { V_VIEW, SL_NOTIFY, (void *)hk_view_notify, &o_viewNotify },
    };
    int n = (int)(sizeof h / sizeof h[0]) - (g_wantToggle && g_okButtons ? 0 : 2);   /* view hooks only for the button */
    return install_set(h, n);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* zoom settings; the percentages and the delay are clamped to sane ranges */
static const char *g_zoomKeyLabel = "wheel";   /* set by zm_read_ini (default: ctrl) */
static void zm_read_ini(void)
{
    static const struct { const char *name; int vk; const char *label; } keys[] = {
        { "ctrl", 0x11, "ctrl+wheel" }, { "shift", 0x10, "shift+wheel" }, { "alt", 0x12, "alt+wheel" },   /* VK_* */
    };
    g_wantZoom     = ini_int("research_zoom", 1);
    g_zoomRemember = ini_int("zoom_remember", 1);
    g_zoomMin      = clampi(ini_int("zoom_min", 40), 20, 100) / 100.0;
    g_zoomMax      = clampi(ini_int("zoom_max", 150), 100, 300) / 100.0;
    g_zoomStep     = 1.0 + clampi(ini_int("zoom_step", 20), 2, 100) / 100.0;
    g_zoomDelayMs  = (unsigned)clampi(ini_int("zoom_delay", 150), 0, 1000);
    char key[16]; ini_str("zoom_modifier", "ctrl", key, sizeof key);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!_stricmp(key, keys[i].name)) { g_zoomKey = keys[i].vk; g_zoomKeyLabel = keys[i].label; }
}

#define NOT_RECOGNISED "OFF (engine code not recognised)"
static const char *installed(int ok) { return ok ? "ON" : "OFF (VirtualProtect failed)"; }

static void init(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;
    if (!ini_int("enabled", 1)) return;
    g_logfile = ini_int("log", 1); g_console = ini_int("console", 0); g_verbose = ini_int("verbose", 1);
    if (g_logfile) {                                    /* one log per exe: an editor start must not overwrite the game's log */
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir);
        wcscat(lp, !_wcsnicmp(en, L"call_to_arms_ed", 15) ? L"\\gem_conquestui_editor.log" :
                   !_wcsnicmp(en, L"call_to_arms_server", 19) ? L"\\gem_conquestui_server.log" : CU_LOG);
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(CU_NAME); freopen("CONOUT$", "w", stdout); }
    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(CU_NAME " " CU_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);

    g_wantPreview   = ini_int("research_preview", 1);
    g_wantPortraits = ini_int("portrait_filter", 1);
    g_mode          = ini_int("portrait_mode", 1) ? MODE_UNLOCKED_ONLY : MODE_ALL_UNLOCKED_FIRST;
    g_wantToggle    = ini_int("portrait_toggle", 1);
    ini_str("inspector", "inspector", g_inspectorName, sizeof g_inspectorName);
    ini_str("toggle_button", "btn_portraits", g_toggleName, sizeof g_toggleName);
    ini_str("toggle_label_unlocked_only", LABEL_SHOW_LOCKED, g_labelOnly, sizeof g_labelOnly);
    ini_str("toggle_label_all", LABEL_HIDE_LOCKED, g_labelAll, sizeof g_labelAll);
    zm_read_ini();

    if (!find_all()) { logf_("no Dynamic Campaign UI in this exe (expected for the editor and the dedicated server) - inert"); return; }

    log_state("library guard", g_okGuard ? installed(install_guard()) : NOT_RECOGNISED);   /* before the preview: it shares the hook */
    log_state("research tree preview", !g_wantPreview ? "off (ini)" : g_okPreview ? installed(install_preview()) : NOT_RECOGNISED);
    if (!g_wantZoom) log_state("research tree zoom", "off (ini)");
    else if (!g_okZoom) log_state("research tree zoom", NOT_RECOGNISED);
    else {
        char zs[96];
        snprintf(zs, sizeof zs, "%s (%s, %d%%-%d%%, %u ms between levels)", installed(install_zoom()), g_zoomKeyLabel,
                 (int)(g_zoomMin * 100 + 0.5), (int)(g_zoomMax * 100 + 0.5), g_zoomDelayMs);
        log_state("research tree zoom", zs);
    }
    if (!g_wantPortraits) { log_state("unlocked portraits", "off (ini)"); return; }
    if (!g_okPortraits) { log_state("unlocked portraits", NOT_RECOGNISED); return; }
    int ok = install_portraits();
    char st[96], btn[96];
    snprintf(st, sizeof st, "%s (start mode %d = %s)", installed(ok), g_mode, mode_name(g_mode));
    log_state("unlocked portraits", st);
    snprintf(btn, sizeof btn, "mode switch button \"%s\"", g_toggleName);
    if (ok) log_state(btn, !g_wantToggle ? "off (ini)" : g_okButtons ? "ON" : NOT_RECOGNISED);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); init(h); }
    return TRUE;
}
#endif /* !CU_TEST */

/* ------------------------------------------------------------------------------------ */
/* offline finder test: maps the exe at its image base and runs every finder            */
/* ------------------------------------------------------------------------------------ */
#ifdef CU_TEST
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: cu_test <exe>\n"); return 2; }
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
    if (!find_all()) { printf("no Dynamic Campaign UI classes (expected for the editor / server)\nINERT\n"); return 0; }
    printf("common %d  notify %d  tree %d  preview %d  zoom %d  portraits %d\n", g_okCommon, g_okButtons, g_okTree, g_okPreview, g_okZoom, g_okPortraits);
    printf("vtables: tree %llX icon %llX lib %llX view %llX wrap %llX portrait %llX fbtn %llX panels %llX %llX %llX\n",
           EXE_OFF(V_TREE), EXE_OFF(V_ICON), EXE_OFF(V_LIB), EXE_OFF(V_VIEW), EXE_OFF(V_WRAP), EXE_OFF(V_PORTRAIT), EXE_OFF(V_FBTN),
           EXE_OFF(V_PANEL[0]), EXE_OFF(V_PANEL[1]), EXE_OFF(V_PANEL[2]));
    printf("dialog: flags %X name %X children %X parent %X notify %X | fbtn frame %X\n", O_DLG_FLAGS, O_DLG_NAME, O_DLG_CHILD, O_DLG_PARENT, O_DLG_NOTIFY, O_FBTN_FRAME);
    printf("tree: owner %X units %X | icon node %X state %X | lib engine %X ctx %X ready %X cur %X | view prep %X\n",
           O_TREE_OWNER, O_TREE_UNITS, O_ICON_NODE, O_ICON_STATE, O_LIB_ENGINE, O_LIB_CTX, O_LIB_READY, O_LIB_CUR, O_VIEW_PREP);
    printf("panel: wrap %X pairs %X | portrait unlocked %X reveal %X | wrap queue %X | view panels %X %X %X selected %X (SelectPortrait %llX)\n",
           O_PANEL_WRAP, O_PANEL_PAIRS, O_PORTRAIT_UNLOCKED, O_PORTRAIT_REVEAL, O_WRAP_QUEUE, O_VIEW_PANEL[0], O_VIEW_PANEL[1], O_VIEW_PANEL[2],
           O_VIEW_SEL, g_selectPortrait ? EXE_OFF(g_selectPortrait) : 0ULL);
    printf("fns: Loc %llX/%llX FindChild %llX NotifyMap %llX SetChecked %llX ShowTemplate %llX ConfigLookup %llX Tag(str) %llX ~Tag %llX LibStart %llX LibClear %llX LibCtx %llX SetText %llX\n",
           g_locEntry ? EXE_OFF(g_locEntry) : 0ULL, g_locText ? EXE_OFF(g_locText) : 0ULL, EXE_OFF(g_findChild), EXE_OFF(g_notifyMap), EXE_OFF(g_setChecked), EXE_OFF(g_showTemplate), EXE_OFF(g_configLookup), EXE_OFF(g_tagFromStr),
           EXE_OFF(g_tagDtor), EXE_OFF(g_libStart), EXE_OFF(g_libClear), EXE_OFF(g_libCtx), g_btnSetText ? EXE_OFF(g_btnSetText) : 0ULL);
    printf("guard %d: engine-using events %X %X %X %X, eDialog::OnEvent %llX\n", g_okGuard,
           g_libEngineEvents[0], g_libEngineEvents[1], g_libEngineEvents[2], g_libEngineEvents[3], g_dialogOnEvent ? EXE_OFF(g_dialogOnEvent) : 0ULL);
    printf("zoom %d: pos %X size %X pref %X min %X max %X | tree lines %X pane %X | line a %X b %X | LineSetup %llX GetScroll %llX SetScroll %llX "
           "Cursor %llX ToLocal %llX | vt line %llX pane %llX panelayout %llX scrollbar %llX\n", g_okZoom,
           O_DLG_POS, O_DLG_SIZE, O_DLG_PREF, O_DLG_MIN, O_DLG_MAX, O_TREE_LINES, O_TREE_PANE, O_LINE_A, O_LINE_B,
           g_lineSetup ? EXE_OFF(g_lineSetup) : 0ULL, g_getScroll ? EXE_OFF(g_getScroll) : 0ULL, g_setScroll ? EXE_OFF(g_setScroll) : 0ULL,
           g_cursor ? EXE_OFF(g_cursor) : 0ULL, g_toLocal ? EXE_OFF(g_toLocal) : 0ULL, V_LINE ? EXE_OFF(V_LINE) : 0ULL,
           V_PANE ? EXE_OFF(V_PANE) : 0ULL, V_PANELAYOUT ? EXE_OFF(V_PANELAYOUT) : 0ULL, V_SCROLLBAR ? EXE_OFF(V_SCROLLBAR) : 0ULL);
    int ok = g_okCommon && g_okButtons && g_okTree && g_okPreview && g_okZoom && g_okPortraits && g_btnSetText && g_locEntry && g_selectPortrait && g_okGuard;
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
