/*
 * GEM ResearchPath 0.4.0  -  Call to Arms: Gates of Hell (GEM engine, 1.067), Dynamic Campaign research tree
 *
 *   1. Path highlight
 *      Hovering a unit highlights it and every prerequisite you still have to research to get it (transitively;
 *      a researched unit ends the walk):
 *        - the icons are drawn with another frame of their own button (ini highlight_frame, 1 = hover), and their
 *          glow widget is shown (ini glow_widget, default "rp_glow": a hidden PictureDialog the mod adds to
 *          UnitPortraitView.xml, texture /interface/scene/portrait_squad/frame_glow, customizable);
 *        - the research lines along the path (and, with line_to_researched=1, the links back to the researched units
 *          it starts from) get the texture ini line_texture (default progress_line_selected, a file the mod ships
 *          next to the vanilla progress_line) and the color ini line_color (white = the texture as drawn).
 *   2. "Requires" tooltip
 *      The tooltip of a locked unit ("Research previous unit to unlock") gets the missing units appended, deepest
 *      prerequisite first (the order to research them in), each display name once, at most 4 by default.
 *
 * UI only (safe in co-op campaigns). Vtable slot swaps only, no code patches. Every engine address and offset is
 * found at startup (RTTI, vtable slots, byte patterns, cross-checks); anything not recognised turns its feature OFF
 * and the log says why. Works next to GEM ConquestUI, which hooks some of the same slots: a slot that is already
 * swapped is analysed through its original pointer read from the exe file on disk, and the hooks chain.
 *
 * Engine (game 1.067 call_to_arms.exe, PE timestamp 6AB3DCC7; the editor and the server have no research tree):
 *   eDialog: parent +0xF0, children vector +0xD8/+0xE0, name std::string +0x58, flags +0x48 (bit 17 = updated);
 *     vt[58] marks the dialog and its parents for update, vt[30] draw runs vt[59] update when bit 17 is clear.
 *   eGUI_CampaignResearchTree: vt[0] dtor, vt[19] LayoutInit (creates icons and lines), vt[30] draw, vt[62] Close,
 *     vt[69] Bind (unitsContainer +0x230); linesContainer +0x220.
 *   eGUI_CampaignResearchIcon: research node +0x2E0 (the research map value: byte 0 = 0 for a unit, prerequisite
 *     keys vector<std::string> at +0x20/+0x28), state +0x2E8 (1 researched, 2 researchable, 3 locked, 4 DLC),
 *     dialog name = research key. vt[59] refresh calls vt[74](icon, std::string* hint) -> bool (state 3: the locked
 *     hint, then copied into the button tooltip) and vt[86](icon, int frames[4]) -> bool ({normal, hover, pressed,
 *     disabled} frames of the button). Its overlays (is_locked, ...) are toggled with SetActive(dialog, bool) (flags
 *     bit 1 visible + vt[5], bit 2 enabled + vt[6]: what <Active value=".."/> in a layout sets); vt[60] FindChild(name).
 *   eGUI_CampaginResearchLine: dependent icon +0x1A0, prerequisite icon +0x1A8 (both set by LineSetup, which
 *     LayoutInit calls per prerequisite and ConquestUI's zoom calls again). FX holder +0x148, entry id 0 =
 *     {sprite, id, Sprite::eFXAnimator}. The animator's color +0x60 (0xAARRGGBB) is baked into the sprite's cached
 *     quad, rebuilt only while the sprite's "built" byte +0x14 is 0. The texture comes from the literal
 *     /interface/pages/main/dynamic_campaign/progress_line: ResourcePath ctor + assign + Normalize,
 *     Sprite::Load(sprite, &path, 0x11, 0) (material +0x48; the animator is kept), Material::SetState(mat,
 *     StateBits(0x40000)), Sprite::Finalize(sprite) (clears "built").
 *   The GUI's hovered dialog: a weak reference (cleared when that dialog dies).
 *   Names (as on the icon labels): unit -> GetUnitName(ConfigLookup(Tag(key)), &s, 1); other node ->
 *     Localize("main_menu2/dynamic_campaign/" + key).
 *
 * Build: build.bat (zig cc).   Offline checks: tests/run_offline_check.sh
 */
#ifndef RP_TEST
#define WIN32_LEAN_AND_MEAN                             /* no winsock: it declares bind, connect, ... */
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
#include <stddef.h>

#define RP_VERSION "0.4.0"
#define RP_NAME    "GEM ResearchPath"
#define RP_SECT    L"researchpath"

/* ------------------------------------------------------------------------------------ */
/* logging / ini                                                                        */
/* ------------------------------------------------------------------------------------ */
static int g_verbose = 1;
#ifdef RP_TEST
static int g_quiet;
static void logf_(const char *fmt, ...) { if (g_quiet) return; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); }
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
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [" RP_NAME "] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (g_console) { fputs(line, stdout); fflush(stdout); }
    OutputDebugStringA(line);
}

static void ini_path(wchar_t *out) { wcscpy(out, g_dir); wcscat(out, L"\\gem_researchpath.ini"); }

static void ini_str(const char *key, const char *def, char *out, size_t n)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64], wdef[256], wout[512];
    MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64); MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 256);
    GetPrivateProfileStringW(RP_SECT, wkey, wdef, wout, 512, ini);
    WideCharToMultiByte(CP_UTF8, 0, wout, -1, out, (int)n, NULL, NULL);
    out[n - 1] = 0;
}

static int ini_int(const char *key, int def)
{
    wchar_t ini[MAX_PATH]; ini_path(ini);
    wchar_t wkey[64]; MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 64);
    return (int)GetPrivateProfileIntW(RP_SECT, wkey, def, ini);
}
#endif

#define LOG_CAPPED(max, ...) do { static int n_; if (n_ < (max)) { n_++; logf_(__VA_ARGS__); } } while (0)
#define VLOG(max, ...)       do { if (g_verbose) LOG_CAPPED(max, __VA_ARGS__); } while (0)

/* one line of text for the log: new lines shown as \n */
static const char *log_text(const char *s)
{
    static char b[512];
    size_t n = 0;
    for (; *s && n + 3 < sizeof b; s++) {
        if (*s == '\n') { b[n++] = '\\'; b[n++] = 'n'; }
        else b[n++] = *s;
    }
    b[n] = 0;
    return b;
}

/* ------------------------------------------------------------------------------------ */
/* PE / pattern helpers                                                                 */
/* ------------------------------------------------------------------------------------ */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base, *g_hdr;                         /* image base, PE headers (in memory = on disk) */
static uint32_t g_timestamp;
static unsigned long long g_prefBase;                   /* ImageBase in the PE header: vtables on disk point relative to it */
#define EXE_OFF(p) ((unsigned long long)((const uint8_t *)(p) - g_base))

static int in_text(const void *a) { return (const uint8_t *)a >= g_text.p && (const uint8_t *)a < g_text.p + g_text.n; }
static uint32_t rd32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint8_t *rel32_target(const uint8_t *ins, int disp_at, int ins_len) { return (uint8_t *)ins + ins_len + (int32_t)rd32(ins + disp_at); }
/* target of the E8 call at offset `at` of a pattern match */
static uint8_t *call_at(uint8_t *match, int at) { return match && match[at] == 0xE8 ? rel32_target(match + at, 1, 5) : NULL; }

static uint8_t *find_bytes(uint8_t *p, size_t n, const uint8_t *pat, size_t m)
{
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == pat[0] && !memcmp(p + i, pat, m)) return p + i;
    return NULL;
}

/* NUL-terminated string at a string boundary of a section */
static uint8_t *find_cstr(Sec s, const char *str)
{
    size_t m = strlen(str) + 1;
    for (uint8_t *p = s.p; (p = find_bytes(p, s.p + s.n - p, (const uint8_t *)str, m)); p++)
        if (p == s.p || p[-1] == 0) return p;
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

/* first match in [from, from+n); *count (optional) = number of matches */
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

/* exactly one match in .text, or NULL (logged) */
static uint8_t *sig_unique(const char *pat, const char *what)
{
    int c = 0; uint8_t *p = scan(g_text.p, g_text.n, pat, &c);
    if (c != 1) { logf_("  %s: pattern matched %d times (need exactly 1)", what, c); return NULL; }
    return p;
}

/* function prefix check (logged) */
static int fn_is(const void *f, const char *pat, const char *what)
{
    Sig s;
    if (!f || !in_text(f) || !sig_parse(pat, &s) || !sig_match((const uint8_t *)f, &s)) {
        logf_("  %s: code at exe+0x%llX not recognised", what, (f && in_text(f)) ? EXE_OFF(f) : 0ULL);
        return 0;
    }
    return 1;
}

/* RTTI: primary vtable of a class by its decorated name (.?AVname@ns@@) */
static void **find_vtable_rtti(const char *decorated)
{
    uint8_t *name = find_cstr(g_data, decorated);
    if (!name) return NULL;
    uint32_t tdRva = (uint32_t)(name - 16 - g_base);
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

/* "name" -> its static Tag's initialiser: lea rdx,[str] ; lea rcx,[tag] ; call Tag::Tag ; lea rcx,[thunk] ; jmp atexit
   (thunk: lea rcx,[tag] ; jmp ~Tag) -> ~Tag */
static uint8_t *find_tag_dtor_via(const char *name)
{
    uint8_t *s = find_cstr(g_rdata, name);
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
/* original vtable slots                                                                */
/* A slot another DLL already swapped points outside the exe's .text; its original is read from the exe file on  */
/* disk (the untouched vtable) and rebased. Each slot is resolved (and reported) once per find_all.             */
/* ------------------------------------------------------------------------------------ */
static FILE *g_exeFile;
static int g_slotsFromDisk;
static struct { void **vt; int slot; void *orig; } g_slotCache[16];
static int g_nSlotCache;

static void *slot_from_disk(void **vt, int slot)
{
    if (!g_exeFile || !g_hdr) return NULL;
    uint32_t rva = (uint32_t)((uint8_t *)(vt + slot) - g_base);
    uint32_t pe = rd32(g_hdr + 0x3C);
    uint16_t nsec = *(uint16_t *)(g_hdr + pe + 6), opt = *(uint16_t *)(g_hdr + pe + 20);
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = g_hdr + pe + 24 + opt + 40 * i;
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), raw = rd32(sh + 20);
        if (rva < va || rva + 8 > va + (vs < rs ? vs : rs)) continue;
        unsigned long long v = 0;
        if (fseek(g_exeFile, (long)(raw + (rva - va)), SEEK_SET) || fread(&v, 8, 1, g_exeFile) != 1) return NULL;
        uint8_t *p = g_base + (v - g_prefBase);         /* rebased (ASLR) */
        return in_text(p) ? p : NULL;
    }
    return NULL;
}

static void *slot_orig(void **vt, int slot)
{
    void *cur = vt[slot];
    if (in_text(cur)) return cur;
    for (int i = 0; i < g_nSlotCache; i++)
        if (g_slotCache[i].vt == vt && g_slotCache[i].slot == slot) return g_slotCache[i].orig;
    void *o = slot_from_disk(vt, slot);
    if (o) g_slotsFromDisk++;
    if (g_nSlotCache < (int)(sizeof g_slotCache / sizeof g_slotCache[0]))
        { g_slotCache[g_nSlotCache].vt = vt; g_slotCache[g_nSlotCache].slot = slot; g_slotCache[g_nSlotCache].orig = o; g_nSlotCache++; }
    return o;
}

/* ------------------------------------------------------------------------------------ */
/* engine types, offsets, functions                                                     */
/* ------------------------------------------------------------------------------------ */
typedef struct { union { char buf[16]; const char *ptr; } u; size_t size, cap; } MsString;   /* MSVC std::string */

static const char *ms_data(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

/* read-only std::string view of a buffer (the engine only reads / copies it) */
static void ms_view(MsString *s, const char *c, size_t n)
{
    memset(s, 0, sizeof *s);
    if (n < 16) { memcpy(s->u.buf, c, n); s->cap = 15; }
    else { s->u.ptr = c; s->cap = n; }
    s->size = n;
}

/* plausible std::string (size <= cap, small buffer when cap == 15, bounded) */
static int ms_ok(const MsString *s, size_t max) { return s->cap >= 15 && s->size <= s->cap && s->size <= max && (s->cap == 15 || s->u.ptr); }

#define VT(o)        (*(void ***)(o))
#define PTR(o, off)  (*(uint8_t **)((uint8_t *)(o) + (off)))
#define U32(o, off)  (*(uint32_t *)((uint8_t *)(o) + (off)))
#define U8(o, off)   (*(uint8_t *)((uint8_t *)(o) + (off)))

typedef void *(*AnyFn)(void *, void *, void *, void *);   /* pass-through: four argument registers + rax */
typedef void  (*ThisFn)(void *self);
typedef void  (*TagFromStrFn)(void *tag, const MsString *s);
typedef void *(*ConfigLookupFn)(const void *tag);
typedef MsString *(*UnitNameFn)(void *tpl, MsString *out, int flag);
typedef void *(*LocStrFn)(const MsString *key);
typedef const MsString *(*LocTextFn)(void *entry);
typedef void  (*AssignFn)(MsString *s, const char *p, size_t n);
typedef void  (*DeleteFn)(void *p, size_t n);
typedef void  (*SpriteLoadFn)(void *sprite, void *path, int flags, int extra);
typedef int   (*StateBitsFn)(int state);
typedef void  (*SetStateFn)(void *mat, int bits);
typedef uint8_t *(*FindChildFn)(void *dlg, const MsString *name);
typedef void  (*SetActiveFn)(void *dlg, int active);

static void **V_TREE, **V_ICON, **V_LINE, **V_ANIM;

enum {
    SL_DTOR = 0, SL_INVALIDATE = 4, SL_LAYOUTINIT = 19, SL_DRAW = 30, SL_MARKDIRTY = 58, SL_REFRESH = 59,
    SL_FINDCHILD = 60, SL_CLOSE = 62, SL_BIND = 69, SL_ICON_HINT = 74, SL_ICON_STATEMAP = 86,
};
enum { ST_RESEARCHED = 1, ST_RESEARCHABLE = 2, ST_LOCKED = 3, ST_DLC = 4 };
#define STR_STRIDE 0x20                                 /* sizeof(std::string) */
/* line FX holder (layout checked byte-exact by find_lines): entries vector +0x20/+0x28 of 0x28-byte
   {sprite +0x00, id +0x08, animator +0x10, ...} */
enum { FX_VEC = 0x20, FX_STRIDE = 0x28, FX_SPRITE = 0x00, FX_ID = 0x08, FX_ANIM = 0x10 };

/* offsets (all found by the finders) */
static uint32_t O_DLG_PARENT, O_DLG_CHILD, O_DLG_NAME, O_TREE_UNITS, O_ICON_NODE, O_ICON_STATE, O_VAL_PREREQ;
static uint32_t O_TREE_LINES, O_LINE_A, O_LINE_B, O_LINE_FX, O_ANIM_COLOR, O_SPR_BUILT, O_SPR_ANIM, O_SPR_MAT;

/* tooltip and names */
static TagFromStrFn g_tagFromStr;
static ThisFn g_tagDtor;
static ConfigLookupFn g_configLookup;
static UnitNameFn g_unitName;
static LocStrFn g_locStr;
static LocTextFn g_locText;
static AssignFn g_assign;
static DeleteFn g_delete;
static char g_locPrefix[64];                            /* "main_menu2/dynamic_campaign/" read from the exe */
static size_t g_locPrefixLen;
/* hover, glow */
static uint8_t **g_hovered;
static FindChildFn g_findChild;                         /* (dialog, name) -> descendant with that id, or NULL */
static SetActiveFn g_setActive;                         /* what a layout's <Active value=".."/> does */                             /* GUI hovered dialog (weak reference: first qword = pointer) */
/* line texture: LineSetup's resource path and sprite texture load */
static ThisFn g_pathCtor, g_pathNorm, g_pathDtor, g_spriteFinalize;
static SpriteLoadFn g_spriteLoad;
static StateBitsFn g_stateBits;
static SetStateFn g_setState;
static int g_loadFlags, g_matState;                    /* 0x11, 0x40000 */
static char g_lineTexVanilla[128], g_lineTexSel[192];  /* full resource paths */
static size_t g_lineTexVanillaLen, g_lineTexSelLen;

/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */
static int find_dialog(void)
{
    /* vt[4] InvalidateLayout: test byte [rcx+48],1 ; jne ; mov rcx,[rcx+PARENT] ; test rcx,rcx */
    uint8_t *inv = slot_orig(V_ICON, SL_INVALIDATE);
    if (!fn_is(inv, "F6 41 48 01 75 ?? 48 8B 89 ?? ?? ?? ?? 48 85 C9", "eDialog::InvalidateLayout")) return 0;
    O_DLG_PARENT = rd32(inv + 9);
    /* eDialog::AddChild: mov rbx,[rsi+END] ; lea rdi,[rsi+BEGIN] */
    uint8_t *add = sig_unique("48 89 5C 24 08 48 89 6C 24 18 48 89 54 24 10 56 57 41 56 48 83 EC 20 41 0F B6 E8 4C 8B F2 48 8B F1 48 3B D1", "eDialog::AddChild");
    if (!add) return 0;
    uint8_t *v = scan(add, 0x80, "48 8B 9E ?? ?? ?? ?? 48 8D BE ?? ?? ?? ??", NULL);
    if (!v || rd32(v + 3) != rd32(v + 10) + 8) { logf_("  eDialog child vector not recognised"); return 0; }
    O_DLG_CHILD = rd32(v + 10);
    /* vt[58] MarkForUpdate: cmp byte [rcx+X],0 ; jne ; and dword [rcx+48],~0x20000 ; mov rcx,[rcx+PARENT] ; ... jmp [rax+1D0] */
    uint8_t *md = slot_orig(V_ICON, SL_MARKDIRTY);
    if (!fn_is(md, "80 B9 ?? ?? ?? ?? 00 75 ?? 81 61 48 FF FF FD FF 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 ?? 48 8B 01 48 FF A0 D0 01 00 00", "eDialog::MarkForUpdate")) return 0;
    if (rd32(md + 19) != O_DLG_PARENT) { logf_("  MarkForUpdate: parent field mismatch"); return 0; }
    /* the tree's vt[30] draw = eDialog's: visible? then "updated" bit 17 clear -> vt[59] update */
    if (!fn_is(slot_orig(V_TREE, SL_DRAW), "48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B F9 41 0F B6 E8 8B 49 48 48 8B F2 8B C1 D1 E8 A8 01 0F 84 ?? ?? ?? ?? 48 89 5C 24 48 0F BA E1 11 72 ?? 48 8B 07 48 8B CF FF 90 D8 01 00 00", "eDialog::Draw")) return 0;
    return 1;
}

/* research tree: name field (Close), unitsContainer (Bind), icon node and research node layout (LayoutInit) */
static int find_tree(void)
{
    uint8_t *close = slot_orig(V_TREE, SL_CLOSE);
    if (!fn_is(close, "48 89 5C 24 10 57 48 81 EC ?? ?? ?? ?? 80 B9 ?? ?? ?? ?? 00 48 8B F9 74 ?? 48 8B 89 ?? ?? ?? ?? E8", "ResearchTree::Close")) return 0;
    uint8_t *nm = scan(close, 0xA0, "48 83 7F ?? 0F 48 8D 4F ??", NULL);   /* cmp [rdi+NAME+18],0F ; lea rcx,[rdi+NAME] */
    if (!nm || nm[3] != (uint8_t)(nm[8] + 0x18)) { logf_("  dialog name field not recognised"); return 0; }
    O_DLG_NAME = nm[8];
    if (!fn_is(slot_orig(V_TREE, SL_DTOR), "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 8B FA", "ResearchTree::~ResearchTree")) return 0;
    /* Bind: movsd xmm0,["unitsContainer"] ; lea rdx,[r14+UNITS] ; ... FindChild into the member */
    uint8_t *bind = slot_orig(V_TREE, SL_BIND);
    if (!fn_is(bind, "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 64 24 20 55 41 56 41 57", "ResearchTree::Bind")) return 0;
    uint8_t *uc = find_cstr(g_rdata, "unitsContainer");
    O_TREE_UNITS = 0;
    for (uint8_t *p = bind; uc && (p = scan(p, bind + 0x1400 - p, "F2 0F 10 05 ?? ?? ?? ?? 49 8D 96 ?? ?? ?? ??", NULL)); p++)
        if (rel32_target(p, 4, 8) == uc) { O_TREE_UNITS = rd32(p + 11); break; }
    if (!O_TREE_UNITS) { logf_("  ResearchTree::Bind: unitsContainer member not found"); return 0; }

    uint8_t *li = slot_orig(V_TREE, SL_LAYOUTINIT);
    if (!fn_is(li, "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24", "ResearchTree::LayoutInit")) return 0;
    /* mov rax,[r14] ; mov rcx,r14 ; mov [r14+NODE],rsi ; call [rax+98]   (rsi = &mapnode.value) */
    uint8_t *nd = scan(li, 0xC00, "49 8B 06 49 8B CE 49 89 B6 ?? ?? ?? ?? FF 90 98 00 00 00", NULL);
    if (!nd) { logf_("  ResearchTree::LayoutInit: icon node member not found"); return 0; }
    O_ICON_NODE = rd32(nd + 9);
    /* lea rsi,[rbx+VALUE] ; mov rcx,r13 ; mov r8,rsi   -> the icon node is the map node's value */
    uint8_t *vl = scan(li, 0x200, "48 8D 73 ?? 49 8B CD 4C 8B C6", NULL);
    /* line loop: mov r12,[rbx+END] ; lea r13,[rbx+KEY] ; mov r14,[r13+BEGIN-KEY] ; cmp r14,r12 */
    uint8_t *ll = scan(li, 0xC00, "4C 8B 63 ?? 4C 8D 6B ?? 4D 8B 75 ?? 4D 3B F4", NULL);
    if (!vl || !ll) { logf_("  research node layout not recognised (LayoutInit)"); return 0; }
    uint32_t val = vl[3], key = ll[7], beg = (uint32_t)ll[7] + ll[11], end = ll[3];
    if (end != beg + 8 || beg <= val) { logf_("  research node prerequisite vector not recognised"); return 0; }
    /* cross-check with GetState(research, key): ... cmp [rdi+41],al ; jne ; mov rbx,[rdi+BEGIN] ; mov rbp,[rdi+END];
       right after the call: mov ecx,[r14+STATE] (the icon's state) */
    uint8_t *gc = scan(li, 0xC00, "48 8D 53 ?? 49 8B CD E8 ?? ?? ?? ?? 41 8B 8E", NULL);
    uint8_t *gs = gc ? rel32_target(gc + 7, 1, 5) : NULL;
    uint8_t *pq = gs && in_text(gs) ? scan(gs, 0x100, "38 47 ?? 0F 85 ?? ?? ?? ?? 48 8B 5F ?? 48 8B 6F ?? 48 3B DD", NULL) : NULL;
    if (!gc || gc[3] != key || !pq || pq[12] != beg || pq[16] != end) { logf_("  GetState does not confirm the prerequisite vector"); return 0; }
    O_VAL_PREREQ = beg - val;
    O_ICON_STATE = rd32(gc + 15);
    return 1;
}

/* icon: the locked hint (slot 74), the refresh using it (slot 59), the frame map (slot 86), names, string helpers */
static int find_icon(void)
{
    /* slot 74: push rbx ; sub rsp,40 ; cmp dword [rcx+STATE],3 ; mov rbx,rdx ; jne ; lea rcx,[key] ; call Loc(cstr) ;
       mov rcx,rax ; call LocText */
    uint8_t *h = slot_orig(V_ICON, SL_ICON_HINT);
    if (!fn_is(h, "40 53 48 83 EC 40 83 B9 ?? ?? ?? ?? 03 48 8B DA 0F 85 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 E8", "ResearchIcon::LockedHint")) return 0;
    if (rd32(h + 8) != O_ICON_STATE) { logf_("  LockedHint: state field mismatch"); return 0; }
    uint8_t *locText = rel32_target(h + 37, 1, 5);
    /* the std::string free MSVC inlines: inc rdx ; cmp rdx,1000 ; jb ; mov r8,[rcx-8] ; add rdx,27 ; ... ; mov rcx,r8 ;
       call operator delete(p, n) */
    uint8_t *df = scan(h, 0x100, "48 FF C2 48 81 FA 00 10 00 00 72 ?? 4C 8B 41 F8 48 83 C2 27 49 2B C8 48 8D 41 F8 48 83 F8 1F 77 ?? 49 8B C8 E8", NULL);
    uint8_t *del = call_at(df, 36);
    if (!del || !in_text(del) || del[0] != 0xE9) { logf_("  operator delete not recognised"); return 0; }
    /* slot 59 refresh: mov rax,[rsi] ; lea rdx,[rsp+X] ; mov rcx,rsi ; call [rax+250] ... assign(tooltip, p, n) ...
       call [rax+2B0] */
    uint8_t *rf = slot_orig(V_ICON, SL_REFRESH);
    if (!fn_is(rf, "48 89 4C 24 08 55 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 01 48 8B F1 FF 90", "ResearchIcon::Refresh")) return 0;
    if (!scan(rf, 0x200, "48 8B 06 48 8D 54 24 ?? 48 8B CE FF 90 50 02 00 00", NULL) || !scan(rf, 0xD00, "FF 90 B0 02 00 00", NULL)) {
        logf_("  Refresh does not use the hint / frame map slots"); return 0;
    }
    uint8_t *as = call_at(scan(rf, 0x300, "4D 8B C6 48 8B CB 49 0F 47 D7 E8", NULL), 10);
    if (!fn_is(as, "48 89 5C 24 10 48 89 6C 24 18 56 57 41 57 48 83 EC 20 48 8B 69 18 49 8B F0 4C 8B FA 48 8B D9 4C 3B C5", "std::string::assign")) return 0;
    /* slot 86 frame map {normal, hover, pressed, disabled} = {4, 1, 2, 0} */
    if (!fn_is(slot_orig(V_ICON, SL_ICON_STATEMAP), "C7 02 04 00 00 00 B0 01 C7 42 04 01 00 00 00 48 C7 42 08 02 00 00 00 C3", "ResearchIcon::FrameMap")) return 0;

    /* names, as LayoutInit puts them on the label (both branches end in the same SetDescription call) */
    uint8_t *li = slot_orig(V_TREE, SL_LAYOUTINIT);
    uint8_t *un = scan(li, 0xC00, "41 B0 01 48 8D 54 24 ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B D0 49 8B CE E8", NULL);
    uint8_t *lc = scan(li, 0xC00, "E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B D0 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 48 8D 54 24 ?? 49 8B CE E8", NULL);
    if (!un || !lc || call_at(un, 22) != call_at(lc, 34) || call_at(lc, 8) != locText) { logf_("  label name functions not recognised"); return 0; }
    uint8_t *gun = call_at(un, 11), *ls = call_at(lc, 0);
    if (!fn_is(gun, "48 89 5C 24 18 48 89 74 24 20 48 89 4C 24 08 55 57 41 54 41 55 41 56", "GetUnitName")) return 0;
    if (!fn_is(ls, "48 89 5C 24 08 57 48 81 EC 80 00 00 00 48 8B F9 48 8B D1 48 8D 4C 24 20 E8", "Localize(std::string)")) return 0;
    if (!fn_is(locText, "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 43 ?? 48 83 C4 20 5B C3", "Localization text")) return 0;
    /* the key prefix: movups xmm0,[prefix] ... mov qword [rsp+X],1C (its length) */
    uint8_t *pf = scan(li, 0x400, "0F 10 05 ?? ?? ?? ?? 48 89 44 24 ?? 48 8D 53 20 48 C7 44 24 ?? ?? 00 00 00", NULL);
    if (!pf) { logf_("  label key prefix not found"); return 0; }
    size_t pl = pf[21];
    uint8_t *pt = rel32_target(pf, 3, 7);
    if (pl < 4 || pl >= sizeof g_locPrefix || pt < g_rdata.p || pt + pl > g_rdata.p + g_rdata.n) { logf_("  label key prefix not recognised"); return 0; }
    for (size_t i = 0; i < pl; i++) if (pt[i] < 0x20 || pt[i] > 0x7E) { logf_("  label key prefix not text"); return 0; }
    memcpy(g_locPrefix, pt, pl); g_locPrefix[pl] = 0; g_locPrefixLen = pl;

    /* unit template by name: Tag(std::string) -> ConfigLookup(Tag) -> ~Tag */
    uint8_t *ts = sig_unique("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 33 ED 48 8D 41 08 48 89 29 0F 57 C0 40 88 69 28 48 8B FA", "Tag(std::string)");
    uint8_t *lk = sig_unique("48 83 EC 68 48 83 79 58 0F 48 8D 51 40 76 03 48 8B 12 4C 8B 41 50 0F 57 C0", "ConfigLookup(Tag)");
    uint8_t *td = find_tag_dtor_via("bandage");
    if (!ts || !lk || !td || !fn_is(td, "48 83 C1 38 E9", "Tag::~Tag")) return 0;

    g_tagFromStr = (TagFromStrFn)ts; g_configLookup = (ConfigLookupFn)lk; g_tagDtor = (ThisFn)td;
    g_unitName = (UnitNameFn)gun; g_locStr = (LocStrFn)ls; g_locText = (LocTextFn)locText;
    g_assign = (AssignFn)as; g_delete = (DeleteFn)del;
    return 1;
}

/* the GUI's hovered dialog: mov rbx,[rip+H] ; lea rcx,[rip+H] ; mov rdx,rdi ; call WeakRef::Set ; test rbx,rbx ; je ; mov edx,2 */
static int find_hover(void)
{
    uint8_t *p = sig_unique("48 8B 1D ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B D7 E8 ?? ?? ?? ?? 48 85 DB 74 ?? BA 02 00 00 00", "GUI hover update");
    if (!p) return 0;
    uint8_t *a = rel32_target(p, 3, 7), *b = rel32_target(p + 7, 3, 7);
    if (a != b || a < g_data.p || a + 8 > g_data.p + g_data.n) { logf_("  hovered dialog global not recognised"); return 0; }
    g_hovered = (uint8_t **)a;
    return 1;
}

/* the glow widget: FindChild (vt[60]) and SetActive, which the icon refresh uses for its own overlays:
   mov rcx,[rsi+OVERLAY] ; test al,al ; je ; mov dl,1 ; call SetActive */
static int find_glow(void)
{
    uint8_t *fc = slot_orig(V_ICON, SL_FINDCHILD);
    if (!fn_is(fc, "41 B8 FF FF FF FF E9", "eDialog::FindChild")) return 0;
    uint8_t *sa = call_at(scan(slot_orig(V_ICON, SL_REFRESH), 0x200, "48 8B 8E ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? B2 01 E8", NULL), 17);
    /* SetActive(dlg, on): ... mov ecx,[rcx+48] ; bit1 -> vt[5] ; bit2 -> vt[6] */
    if (!fn_is(sa, "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 0F B6 FA 8B 49 48 8B C1 D1 E8 83 E0 01 3B C7 74 ?? 83 E1 FD 8D 04 3F 0B C1 48 8B CB 89 43 48 48 8B 03 FF 50 28", "eDialog::SetActive")) return 0;
    g_findChild = (FindChildFn)fc; g_setActive = (SetActiveFn)sa;
    return 1;
}

/* LineSetup, from LayoutInit's per-prerequisite loop (r13 = &node key, r14 = &prerequisite key, see find_tree):
   mov rdx,[rbx+LINES] ; mov rcx,rsi ; call SetParent ; mov rdx,r14 ; lea rcx,[rbx+MAP] ; call map[] ;
   mov rdx,r13 ; lea rcx,[rbx+MAP] ; mov rdi,[rax] ; call map[] ; mov r8,rdi ; mov rcx,rsi ; mov rdx,[rax] ;
   call LineSetup(line, icon of the node, icon of its prerequisite) */
static uint8_t *find_line_setup(void)
{
    uint8_t *li = slot_orig(V_TREE, SL_LAYOUTINIT);
    uint8_t *ln = scan(li, 0xC00, "48 8B 93 ?? ?? ?? ?? 48 8B CE E8 ?? ?? ?? ?? 49 8B D6 48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 49 8B D5 48 8D 8B ?? ?? ?? ?? 48 8B 38 E8 ?? ?? ?? ?? 4C 8B C7 48 8B CE 48 8B 10 E8", NULL);
    if (!ln) { logf_("  ResearchTree::LayoutInit: research lines not recognised"); return NULL; }
    O_TREE_LINES = rd32(ln + 3);
    return call_at(ln, 57);
}

/* research lines: icon fields, FX entry, the sprite's animator color and its quad cache */
static int find_lines(void)
{
    V_LINE = find_vtable_rtti(".?AVeGUI_CampaginResearchLine@GUI2@@");
    V_ANIM = find_vtable_rtti(".?AVeFXAnimator@Sprite@@");
    if (!V_LINE || !V_ANIM) { logf_("  class eGUI_CampaginResearchLine / Sprite::eFXAnimator not found"); return 0; }
    uint8_t *ls = find_line_setup();
    /* LineSetup: ... mov r15,rcx ; mov [rcx+A],rdx ; mov [rcx+B],r8 */
    if (!fn_is(ls, "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 4C 8B F9 48 89 91 ?? ?? ?? ?? 4C 89 81", "ResearchLine::Setup")) return 0;
    O_LINE_A = rd32(ls + 35); O_LINE_B = rd32(ls + 42);
    /* xor r8d,r8d ; lea rdx,[rbp+X] ; lea rcx,[r15+FX] ; call FxHolder::Entry(holder, &out, id 0) */
    uint8_t *fe = scan(ls, 0x400, "45 33 C0 48 8D 55 ?? 49 8D 8F ?? ?? ?? ?? E8", NULL);
    if (!fe || !fn_is(call_at(fe, 14), "48 8B 41 20 48 8B 49 28 48 3B C1 74 ?? 0F 1F 00 44 39 40 08 74 ?? 48 83 C0 28 48 3B C1 75 ?? 48 89 0A 48 8B C2 C3", "FxHolder::Entry")) return 0;
    O_LINE_FX = rd32(fe + 10);
    /* the new animator goes to entry+0x10: mov rax,[rbp+X] ; ... ; mov [rax+10],r9 ; call Commit */
    if (!scan(ls, 0x400, "48 8B 45 ?? 48 8D 55 ?? 45 0F B6 C6 49 8B C9 4C 89 48 10 E8", NULL)) { logf_("  ResearchLine::Setup: animator store not recognised"); return 0; }
    /* eFXAnimator vt[3] (the FX parser's "color" setter tail): or byte [rcx+40],2 ; mov byte [rcx+COLOR+3],FF ; ret */
    uint8_t *v3 = (uint8_t *)V_ANIM[3];
    if (!fn_is(v3, "80 49 40 02 C6 41 ?? FF C3", "eFXAnimator::SetColorAlpha")) return 0;
    O_ANIM_COLOR = (uint32_t)v3[6] - 3;
    /* LineSetup creates it white: mov dword [r9+COLOR],FFFFFFFF */
    char pat[64]; snprintf(pat, sizeof pat, "41 C7 41 %02X FF FF FF FF", O_ANIM_COLOR);
    if (O_ANIM_COLOR > 0x7F || !scan(ls, 0x400, pat, NULL)) { logf_("  line sprite color not recognised"); return 0; }
    /* the sprite caches its quad (color baked in); Sprite::Draw rebuilds it only while byte +BUILT is 0:
       mov rax,[rcx+ANIM] ; ... test byte [rax+40],1 ; jne ; mov byte [rcx+BUILT],0 ; cmp byte [rcx+BUILT],0 */
    uint8_t *sd = sig_unique("48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 30 48 8B 81 ?? ?? ?? ?? 49 8B E8 48 8B DA 48 8B F9 48 85 C0 74 ?? F6 40 40 01 75 ?? C6 41 ?? 00 80 79 ?? 00", "Sprite::Draw");
    if (!sd || sd[44] != sd[48]) { logf_("  sprite quad cache not recognised"); return 0; }
    O_SPR_ANIM = rd32(sd + 18); O_SPR_BUILT = sd[44];
    /* LineSetup: mov rcx,[rbp+X] ; call FxEntry::Commit (binds the animator) ; mov eax,[rdi+8] ; mov byte [rdi+BUILT],r13b */
    snprintf(pat, sizeof pat, "E8 ?? ?? ?? ?? 8B 47 08 44 88 6F %02X", O_SPR_BUILT);
    if (!scan(ls, 0x500, pat, NULL)) { logf_("  LineSetup does not reset the sprite cache as expected"); return 0; }
    return 1;
}

/* the line texture load, as LineSetup does it (optional: without it path lines get the color only) */
static int find_line_texture(void)
{
    uint8_t *ls = find_line_setup();
    if (!ls) return 0;
    /* call PathCtor ; mov r8d,LEN ; lea rdx,[literal] ; lea rcx,[rbp+P] ; call assign ; lea rcx,[rbp+P] ; call Normalize */
    uint8_t *pb = scan(ls, 0x80, "E8 ?? ?? ?? ?? 41 B8 ?? 00 00 00 48 8D 15 ?? ?? ?? ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 8D 4D ?? E8", NULL);
    /* xor r9d,r9d ; lea rdx,[rbp+P] ; mov r8d,FLAGS ; mov rcx,rdi ; call Load ; mov rbx,[rdi+MAT] ; mov ecx,STATE ;
       call StateBits ; mov edx,eax ; mov rcx,rbx ; call SetState ; mov rcx,rdi ; call Finalize */
    uint8_t *lq = scan(ls, 0x300, "45 33 C9 48 8D 55 ?? 41 B8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B 5F ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B D0 48 8B CB E8 ?? ?? ?? ?? 48 8B CF E8", NULL);
    /* lea rcx,[rbp+P] ; call PathDtor ; lea r11,[rsp+..]   (end of LineSetup) */
    uint8_t *pd = scan(ls, 0x500, "48 8D 4D ?? E8 ?? ?? ?? ?? 4C 8D 9C 24", NULL);
    /* one and the same path local everywhere, and the same assign the tooltip uses */
    if (!pb || !lq || !pd || pb[21] != pb[30] || lq[6] != pb[21] || pd[3] != pb[21] || call_at(pb, 22) != (uint8_t *)g_assign) {
        logf_("  line texture load not recognised"); return 0;
    }
    uint8_t *lit = rel32_target(pb + 11, 3, 7);
    size_t litLen = pb[7];
    if (litLen < 8 || litLen >= sizeof g_lineTexVanilla || lit < g_rdata.p || lit + litLen > g_rdata.p + g_rdata.n || !memchr(lit, '/', litLen)) {
        logf_("  line texture name not recognised"); return 0;
    }
    g_pathCtor = (ThisFn)call_at(pb, 0); g_pathNorm = (ThisFn)call_at(pb, 31); g_pathDtor = (ThisFn)call_at(pd, 4);
    g_spriteLoad = (SpriteLoadFn)call_at(lq, 16); g_stateBits = (StateBitsFn)call_at(lq, 30);
    g_setState = (SetStateFn)call_at(lq, 40); g_spriteFinalize = (ThisFn)call_at(lq, 48);
    if (!fn_is((void *)g_pathCtor, "33 C0 0F 57 C0 0F 11 01 48 89 41 10 48 C7 41 18 0F 00 00 00 88 01 48 89 41 20", "ResourcePath::ctor") ||
        !fn_is((void *)g_pathDtor, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 79 20", "ResourcePath::dtor") ||
        !fn_is((void *)g_spriteLoad, "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 70 48 8B EA 41 8B C1", "Sprite::Load") ||
        !fn_is((void *)g_spriteFinalize, "8B 41 2C 2B 41 24 99 2B C2 D1 F8", "Sprite::Finalize")) return 0;
    g_loadFlags = (int)rd32(lq + 9); O_SPR_MAT = lq[24]; g_matState = (int)rd32(lq + 26);
    char lp[32]; snprintf(lp, sizeof lp, "48 8B 4B %02X", O_SPR_MAT);   /* Load releases the old material: mov rcx,[rbx+MAT] */
    if (!scan((void *)g_spriteLoad, 0x80, lp, NULL)) { logf_("  sprite material field mismatch"); return 0; }
    memcpy(g_lineTexVanilla, lit, litLen); g_lineTexVanilla[litLen] = 0; g_lineTexVanillaLen = litLen;
    return 1;
}

static int g_okDialog, g_okTree, g_okIcon, g_okHover, g_okLines, g_okLineTex, g_okGlow;

static int find_all(void)
{
    g_nSlotCache = 0; g_slotsFromDisk = 0;
    V_TREE = find_vtable_rtti(".?AVeGUI_CampaignResearchTree@GUI2@@");
    V_ICON = find_vtable_rtti(".?AVeGUI_CampaignResearchIcon@GUI2@@");
    if (!V_TREE || !V_ICON) return 0;
    g_okDialog = find_dialog();
    g_okTree = g_okDialog && find_tree();
    g_okIcon = g_okTree && find_icon();
    g_okHover = find_hover();
    g_okLines = g_okTree && find_lines();
    g_okLineTex = g_okLines && g_okIcon && find_line_texture();
    g_okGlow = g_okIcon && find_glow();
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* research graph of the bound tree                                                     */
/* ------------------------------------------------------------------------------------ */
#define MAX_ICONS 2048
#define MAX_PREREQ 64
#define MAX_LINES 4096

typedef struct {
    uint8_t *icon;
    uint8_t *glow;               /* its glow widget (a child of the icon), NULL = none */
    char *key; size_t klen;      /* research key (= icon name), own copy */
    char *name;                  /* display name, computed on first use */
    int hl;                      /* on the hovered path now */
    unsigned seen;               /* walk generation */
} Ent;

typedef struct {
    uint8_t *line;
    Ent *a, *b;                  /* dependent unit, its prerequisite (NULL = no icon) */
    int on;                      /* drawn as a path line now */
    uint8_t *sprite, *anim;      /* the sprite / animator we changed (compared only, never dereferenced once replaced) */
    int tex;                     /* our texture is on that sprite */
    uint32_t orig;               /* the animator's color before */
} Line;

static struct {
    uint8_t *tree, *units, *lines;
    int n; Ent e[MAX_ICONS];     /* sorted by key */
    int hash[MAX_ICONS * 2];     /* icon pointer -> index + 1 */
    int nl; Line l[MAX_LINES];
    unsigned gen;
    uint8_t *lastHover; Ent *target;
} g_b;

/* settings */
static int g_wantHighlight = 1, g_hlFrame = 1, g_wantLines = 1, g_lineToResearched = 1, g_useLineTex;
static uint32_t g_lineColor = 0xFFFFFFFF;               /* 0xAARRGGBB; white = the texture as drawn */
static int g_wantGlow = 1;
static char g_glowName[32] = "rp_glow";
static int g_wantTooltip = 1, g_maxNames = 4;
static char g_reqPrefix[128] = "\nRequires: ", g_reqSep[16] = ", ", g_reqMore[64] = ", +%d more";

static size_t hslot(const void *p) { unsigned long long v = (unsigned long long)(size_t)p; v ^= v >> 17; v *= 0x9E3779B97F4A7C15ull; return (size_t)(v >> 40) % (MAX_ICONS * 2); }

static Ent *ent_of_icon(const void *icon)
{
    if (!icon || !g_b.tree) return NULL;
    for (size_t h = hslot(icon), k = 0; k < MAX_ICONS * 2; k++, h = (h + 1) % (MAX_ICONS * 2)) {
        int i = g_b.hash[h];
        if (!i) return NULL;
        if (g_b.e[i - 1].icon == icon) return &g_b.e[i - 1];
    }
    return NULL;
}

static int key_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    int c = memcmp(a, b, an < bn ? an : bn);
    return c ? c : (an < bn ? -1 : an > bn);
}
static int ent_cmp(const void *x, const void *y)
{
    const Ent *a = x, *b = y;
    return key_cmp(a->key, a->klen, b->key, b->klen);
}

static Ent *ent_of_key(const char *k, size_t n)
{
    int lo = 0, hi = g_b.n;
    while (lo < hi) {
        int m = (lo + hi) / 2, c = key_cmp(g_b.e[m].key, g_b.e[m].klen, k, n);
        if (!c) return &g_b.e[m];
        if (c < 0) lo = m + 1; else hi = m;
    }
    return NULL;
}

static int icon_state(const Ent *e) { return (int)U32(e->icon, O_ICON_STATE); }

/* prerequisite keys of an icon's research node: vector<std::string> */
static int prereqs(const Ent *e, MsString **b)
{
    uint8_t *node = PTR(e->icon, O_ICON_NODE);
    if (!node) return 0;
    MsString *pb = (MsString *)PTR(node, O_VAL_PREREQ), *pe = (MsString *)PTR(node, O_VAL_PREREQ + 8);
    if (!pb || pe < pb) return 0;
    size_t bytes = (size_t)((uint8_t *)pe - (uint8_t *)pb);
    if (bytes % STR_STRIDE || bytes / STR_STRIDE > MAX_PREREQ) return 0;
    *b = pb;
    return (int)(bytes / STR_STRIDE);
}

static void tree_unbind(const char *why)
{
    if (!g_b.tree) return;
    for (int i = 0; i < g_b.n; i++) { free(g_b.e[i].key); free(g_b.e[i].name); }
    memset(&g_b, 0, sizeof g_b);
    VLOG(20, "tree released (%s)", why);
}

/* children of a container, if its vector looks sane */
static int children(uint8_t *dlg, uint8_t ***b, uint8_t ***e, ptrdiff_t max)
{
    *b = (uint8_t **)PTR(dlg, O_DLG_CHILD); *e = (uint8_t **)PTR(dlg, O_DLG_CHILD + 8);
    return *b && *e >= *b && *e - *b <= max;
}

/* records the icons and lines of a tree right after LayoutInit created them */
static void tree_bind(uint8_t *tree)
{
    tree_unbind("replaced by a new research tree");
    uint8_t *units = PTR(tree, O_TREE_UNITS), **cb, **ce;
    if (!units || !children(units, &cb, &ce, 4 * MAX_ICONS)) { LOG_CAPPED(3, "research tree icons not found - inactive for this tree"); return; }
    int skipped = 0;
    for (; cb < ce; cb++) {
        uint8_t *c = *cb;
        if (!c || VT(c) != V_ICON) continue;
        const MsString *nm = (const MsString *)(c + O_DLG_NAME);
        if (g_b.n >= MAX_ICONS || !ms_ok(nm, 256) || !nm->size) { skipped++; continue; }
        Ent *e = &g_b.e[g_b.n];
        if (!(e->key = malloc(nm->size + 1))) { skipped++; continue; }
        memcpy(e->key, ms_data(nm), nm->size); e->key[nm->size] = 0; e->klen = nm->size;
        e->icon = c; e->glow = NULL; e->name = NULL; e->hl = 0; e->seen = 0;
        g_b.n++;
    }
    qsort(g_b.e, (size_t)g_b.n, sizeof(Ent), ent_cmp);
    for (int i = 0; i < g_b.n; i++) {
        size_t h = hslot(g_b.e[i].icon);
        while (g_b.hash[h]) h = (h + 1) % (MAX_ICONS * 2);
        g_b.hash[h] = i + 1;
    }
    g_b.tree = tree; g_b.units = units; g_b.lastHover = (uint8_t *)1;

    int glows = 0;
    if (g_okGlow && g_wantGlow && g_wantHighlight) {
        MsString gn; ms_view(&gn, g_glowName, strlen(g_glowName));
        for (int i = 0; i < g_b.n; i++) {
            uint8_t *g = g_findChild(g_b.e[i].icon, &gn);
            if (g && g != g_b.e[i].icon) { g_b.e[i].glow = g; glows++; }
        }
        if (!glows) LOG_CAPPED(3, "no \"%s\" widget in the research icons - add it to UnitPortraitView.xml (mod_side) for the glow", g_glowName);
    }

    int links = 0, noIcon = 0;
    for (int i = 0; i < g_b.n; i++) {
        MsString *pb; int np = prereqs(&g_b.e[i], &pb);
        for (int j = 0; j < np; j++)
            if (ms_ok(&pb[j], 256)) { if (ent_of_key(ms_data(&pb[j]), pb[j].size)) links++; else noIcon++; }
    }
    int linesOk = 0;
    uint8_t **lb, **le;
    if (g_okLines && g_wantLines && (g_b.lines = PTR(tree, O_TREE_LINES)) && children(g_b.lines, &lb, &le, 4 * MAX_LINES)) {
        for (; lb < le && g_b.nl < MAX_LINES; lb++) {
            uint8_t *l = *lb;
            if (!l || VT(l) != V_LINE) continue;
            Line *L = &g_b.l[g_b.nl++];
            memset(L, 0, sizeof *L);
            L->line = l; L->a = ent_of_icon(PTR(l, O_LINE_A)); L->b = ent_of_icon(PTR(l, O_LINE_B));
            if (L->a && L->b) linesOk++;
        }
    }
    VLOG(20, "tree bound: %d icons (%d with a glow widget), %d prerequisite links (%d to units without an icon), %d lines (%d between two icons)%s",
         g_b.n, glows, links, noIcon, g_b.nl, linesOk, skipped ? " - some icons skipped" : "");
}

/* the units still to research for e: its not-researched prerequisites, transitively (a researched one ends the walk),
   deepest first; e itself last when included. Returns the count written to out (at most max). */
static int missing_chain(Ent *e, int include_self, Ent **out, int max)
{
    static struct { Ent *e; int next; } st[MAX_ICONS];   /* UI thread only, never nested */
    int sp = 0, n = 0;
    unsigned gen = ++g_b.gen;
    if (!gen) { for (int i = 0; i < g_b.n; i++) g_b.e[i].seen = 0; gen = g_b.gen = 1; }
    e->seen = gen;
    st[sp].e = e; st[sp].next = 0; sp++;
    while (sp) {
        Ent *c = st[sp - 1].e;
        MsString *pb; int np = prereqs(c, &pb);
        Ent *child = NULL;
        while (st[sp - 1].next < np) {
            MsString *k = &pb[st[sp - 1].next++];
            if (!ms_ok(k, 256)) continue;
            Ent *p = ent_of_key(ms_data(k), k->size);
            if (!p || p->seen == gen) continue;
            p->seen = gen;
            if (icon_state(p) == ST_RESEARCHED) continue;
            child = p; break;
        }
        if (child && sp < MAX_ICONS) { st[sp].e = child; st[sp].next = 0; sp++; continue; }
        sp--;                                           /* post-order: every prerequisite before its dependent */
        if ((c != e || include_self) && n < max) out[n++] = c;
    }
    return n;
}

/* ------------------------------------------------------------------------------------ */
/* display names                                                                        */
/* ------------------------------------------------------------------------------------ */
/* frees an engine-allocated std::string the way MSVC inlines it */
static void ms_free(MsString *s)
{
    if (s->cap <= 15) return;
    size_t sz = s->cap + 1; void *p = (void *)s->u.ptr;
    if (sz >= 0x1000) { p = ((void **)p)[-1]; sz += 0x27; }
    g_delete(p, sz);
}

static char *dup_n(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (r) { memcpy(r, s, n); r[n] = 0; }
    return r;
}

static const char *ent_name(Ent *e)
{
    if (e->name) return e->name;
    uint8_t *node = PTR(e->icon, O_ICON_NODE);
    if (node && node[0] == 0) {                          /* unit: the template's display name */
        _Alignas(16) uint8_t tag[0x80];
        MsString k; ms_view(&k, e->key, e->klen);
        memset(tag, 0, sizeof tag);
        g_tagFromStr(tag, &k);
        void *tpl = g_configLookup(tag);
        g_tagDtor(tag);
        if (tpl) {
            MsString s; memset(&s, 0, sizeof s);
            g_unitName(tpl, &s, 1);
            if (ms_ok(&s, 4096) && s.size) e->name = dup_n(ms_data(&s), s.size);
            ms_free(&s);
        }
    } else if (node) {                                   /* other research node: localized "<prefix><key>" */
        char buf[512];
        if (g_locPrefixLen + e->klen < sizeof buf) {
            memcpy(buf, g_locPrefix, g_locPrefixLen); memcpy(buf + g_locPrefixLen, e->key, e->klen);
            MsString k; ms_view(&k, buf, g_locPrefixLen + e->klen);
            void *ent = g_locStr(&k);
            const MsString *t = ent ? g_locText(ent) : NULL;
            if (t && ms_ok(t, 4096) && t->size) e->name = dup_n(ms_data(t), t->size);
        }
    }
    if (!e->name) e->name = dup_n(e->key, e->klen);       /* fallback: the research key */
    return e->name ? e->name : "?";
}

/* "<prefix>A, B, C[, +N more]" for e (distinct names) into buf; returns its length (0 = nothing missing) */
static size_t requires_text(Ent *e, char *buf, size_t cap)
{
    static Ent *chain[MAX_ICONS];
    static const char *names[MAX_ICONS];
    int n = missing_chain(e, 0, chain, MAX_ICONS), un = 0;
    if (!n) return 0;
    for (int i = 0; i < n; i++) {                        /* some research nodes share a display name */
        const char *nm = ent_name(chain[i]);
        int dup = 0;
        for (int j = 0; j < un && !dup; j++) dup = !strcmp(names[j], nm);
        if (!dup) names[un++] = nm;
    }
    size_t len = 0;
#define PUT(s) do { size_t l_ = strlen(s); if (len + l_ >= cap) goto full; memcpy(buf + len, (s), l_); len += l_; } while (0)
    PUT(g_reqPrefix);
    int shown = 0;
    for (; shown < un && shown < g_maxNames; shown++) {
        if (shown) PUT(g_reqSep);
        PUT(names[shown]);
    }
    if (shown < un) { char more[80]; snprintf(more, sizeof more, g_reqMore, un - shown); PUT(more); }
full:
#undef PUT
    buf[len < cap ? len : cap - 1] = 0;
    return len;
}

/* ------------------------------------------------------------------------------------ */
/* path lines                                                                           */
/* ------------------------------------------------------------------------------------ */
static void mark_dirty(uint8_t *dlg) { ((ThisFn)VT(dlg)[SL_MARKDIRTY])(dlg); }

/* the line's sprite and its animator: FX entry 0 (the sprite must have that animator bound) */
static uint8_t *line_anim(const Line *L, uint8_t **sprite)
{
    uint8_t *h = L->line + O_LINE_FX, *b = PTR(h, FX_VEC), *e = PTR(h, FX_VEC + 8);
    if (!b || e < b || (size_t)(e - b) % FX_STRIDE || (size_t)(e - b) / FX_STRIDE > 16) return NULL;
    for (; b < e; b += FX_STRIDE)
        if (U32(b, FX_ID) == 0) {
            uint8_t *an = PTR(b, FX_ANIM), *sp = PTR(b, FX_SPRITE);
            if (!an || VT(an) != V_ANIM || !sp || PTR(sp, O_SPR_ANIM) != an) return NULL;
            *sprite = sp;
            return an;
        }
    return NULL;
}

/* new color into the animator; the sprite rebuilds its cached quad on the next draw */
static void set_line_color(uint8_t *an, uint8_t *sp, uint32_t c)
{
    U32(an, O_ANIM_COLOR) = c;
    U8(sp, O_SPR_BUILT) = 0;
}

/* LineSetup's texture load on an existing line sprite (Finalize also clears its "built" byte) */
static void set_line_texture(uint8_t *sp, const char *name, size_t n)
{
    _Alignas(16) uint8_t path[0x40];
    memset(path, 0, sizeof path);
    g_pathCtor(path);
    g_assign((MsString *)path, name, n);
    g_pathNorm(path);
    g_spriteLoad(sp, path, g_loadFlags, 0);
    uint8_t *mat = PTR(sp, O_SPR_MAT);
    if (mat) g_setState(mat, g_stateBits(g_matState));
    g_spriteFinalize(sp);
    g_pathDtor(path);
}

static void line_on(Line *L)
{
    uint8_t *sp, *an = line_anim(L, &sp);
    if (!an) return;
    if (!L->on || L->anim != an || L->sprite != sp) {   /* first time, or LineSetup built a new sprite */
        L->orig = U32(an, O_ANIM_COLOR); L->anim = an; L->sprite = sp; L->tex = 0;
    }
    if (g_useLineTex && !L->tex) { set_line_texture(sp, g_lineTexSel, g_lineTexSelLen); L->tex = 1; }
    set_line_color(an, sp, g_lineColor);
    L->on = 1;
    mark_dirty(L->line);
}

static void line_off(Line *L)
{
    uint8_t *sp, *an = line_anim(L, &sp);
    if (an && sp == L->sprite && L->tex) set_line_texture(sp, g_lineTexVanilla, g_lineTexVanillaLen);
    if (an && an == L->anim && U32(an, O_ANIM_COLOR) == g_lineColor) set_line_color(an, sp, L->orig);
    L->on = 0; L->anim = NULL; L->sprite = NULL; L->tex = 0;
    mark_dirty(L->line);
}

static int line_on_path(const Line *L)
{
    return L->a && L->b && L->a->hl && (L->b->hl || (g_lineToResearched && icon_state(L->b) == ST_RESEARCHED));
}

/* a path line can be rebuilt as a vanilla one (ConquestUI's zoom calls LineSetup): switch it again */
static void lines_recheck(void)
{
    for (int i = 0; i < g_b.nl; i++) {
        Line *L = &g_b.l[i];
        if (!L->on) continue;
        uint8_t *sp, *an = line_anim(L, &sp);
        if (an && (an != L->anim || sp != L->sprite || U32(an, O_ANIM_COLOR) != g_lineColor)) line_on(L);
    }
}

/* ------------------------------------------------------------------------------------ */
/* hover -> path                                                                        */
/* ------------------------------------------------------------------------------------ */
/* hovered dialog -> research icon of the bound tree (the hovered one can be the icon or one of its children) */
static Ent *hovered_ent(uint8_t *h)
{
    for (int k = 0; h && k < 8; k++, h = PTR(h, O_DLG_PARENT)) {
        if (VT(h) == V_ICON) return PTR(h, O_DLG_PARENT) == g_b.units ? ent_of_icon(h) : NULL;
        if (h == g_b.units || h == g_b.tree) return NULL;
    }
    return NULL;
}

static void set_target(Ent *t)
{
    static Ent *chain[MAX_ICONS];
    int n = t && icon_state(t) != ST_RESEARCHED ? missing_chain(t, 1, chain, MAX_ICONS) : 0;   /* researched: nothing */
    unsigned gen = ++g_b.gen;
    for (int i = 0; i < n; i++) chain[i]->seen = gen;
    int iconsOn = 0, iconsOff = 0, linesOn = 0, linesOff = 0;
    for (int i = 0; i < g_b.n; i++) {
        Ent *e = &g_b.e[i];
        int want = e->seen == gen;
        if (want == e->hl) continue;
        e->hl = want;
        if (e->glow) g_setActive(e->glow, want);
        mark_dirty(e->icon);
        if (want) iconsOn++; else iconsOff++;
    }
    for (int i = 0; i < g_b.nl; i++) {
        Line *L = &g_b.l[i];
        int want = line_on_path(L);
        if (want && !L->on) { line_on(L); linesOn += L->on; }
        else if (!want && L->on) { line_off(L); linesOff++; }
    }
    g_b.target = t;
    if (t) VLOG(10, "hover %s (state %d): %d unit(s) on the path - icons %d on / %d off, lines %d on / %d off",
                t->key, icon_state(t), n, iconsOn, iconsOff, linesOn, linesOff);
}

/* ------------------------------------------------------------------------------------ */
/* hooks                                                                                */
/* ------------------------------------------------------------------------------------ */
static AnyFn o_treeLayoutInit, o_treeClose, o_treeDtor, o_treeDraw, o_iconHint, o_iconFrames;

/* bool returned in al (the rest of rax is garbage) */
static int al_true(void *r) { uint8_t b; memcpy(&b, &r, 1); return b != 0; }

/* tree draw (every frame while shown): follow the hover before the icons are drawn */
static void *hk_tree_draw(void *tree, void *a2, void *a3, void *a4)
{
    if (tree && tree == g_b.tree && g_wantHighlight && g_hovered) {
        uint8_t *h = *g_hovered;
        if (h != g_b.lastHover) {
            g_b.lastHover = h;
            Ent *t = hovered_ent(h);
            if (t != g_b.target) set_target(t);
        }
        if (g_b.target && g_b.nl) lines_recheck();
    }
    return o_treeDraw(tree, a2, a3, a4);
}

/* tree vt[19]: the icons and lines exist once the original returns */
static void *hk_tree_layoutinit(void *tree, void *a2, void *a3, void *a4)
{
    void *r = o_treeLayoutInit(tree, a2, a3, a4);
    if (tree && VT(tree) == V_TREE) tree_bind(tree);
    return r;
}

static void *hk_tree_close(void *tree, void *a2, void *a3, void *a4)
{
    if (tree && tree == g_b.tree) {
        if (g_b.target) set_target(NULL);                /* icons and lines are still alive: back to vanilla */
        tree_unbind("tree closed");
    }
    return o_treeClose(tree, a2, a3, a4);
}

static void *hk_tree_dtor(void *tree, void *flags, void *a3, void *a4)
{
    if (tree && tree == g_b.tree) tree_unbind("tree destroyed");
    return o_treeDtor(tree, flags, a3, a4);
}

/* icon frame map: an icon on the path gets another frame of its button as its "normal" frame */
static void *hk_icon_frames(void *icon, void *out, void *a3, void *a4)
{
    void *r = o_iconFrames(icon, out, a3, a4);
    if (al_true(r) && out) {
        Ent *e = ent_of_icon(icon);
        if (e && e->hl) ((uint32_t *)out)[0] = ((uint32_t *)out)[g_hlFrame];
    }
    return r;
}

/* icon slot 74: the locked tooltip ("Research previous unit to unlock") + the missing units */
static void *hk_icon_hint(void *icon, void *out, void *a3, void *a4)
{
    void *r = o_iconHint(icon, out, a3, a4);
    if (!g_wantTooltip || !al_true(r) || !out) return r;
    Ent *e = ent_of_icon(icon);
    MsString *s = out;
    if (!e || icon_state(e) != ST_LOCKED || !ms_ok(s, 4096)) return r;
    static char buf[4096];
    size_t base = s->size;
    if (base >= sizeof buf / 2) return r;
    memcpy(buf, ms_data(s), base);
    size_t add = requires_text(e, buf + base, sizeof buf - base);
    if (!add) return r;
    g_assign(s, buf, base + add);
    VLOG(10, "tooltip %s: %s", e->key, log_text(buf + base));
    return r;
}

/* ------------------------------------------------------------------------------------ */
/* install                                                                              */
/* ------------------------------------------------------------------------------------ */
#ifndef RP_TEST
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

/* all or nothing; each original (possibly another DLL's hook: we chain it) is stored before its slot is swapped */
static int install(void)
{
    Hook h[] = {
        { V_TREE, SL_DTOR, (void *)hk_tree_dtor, &o_treeDtor },          /* lifetime first */
        { V_TREE, SL_CLOSE, (void *)hk_tree_close, &o_treeClose },
        { V_ICON, SL_ICON_HINT, (void *)hk_icon_hint, &o_iconHint },
        { V_ICON, SL_ICON_STATEMAP, (void *)hk_icon_frames, &o_iconFrames },
        { V_TREE, SL_DRAW, (void *)hk_tree_draw, &o_treeDraw },
        { V_TREE, SL_LAYOUTINIT, (void *)hk_tree_layoutinit, &o_treeLayoutInit },   /* last: the trigger */
    };
    int n = (int)(sizeof h / sizeof h[0]);
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

/* <game>\mods\<mod>\resource\[40k_]<res>.* exists?  (40k_ = the first folder gem_vfs_redirect serves)
   -> 1 and that file, game-relative, in `found` */
static int mod_has_resource(const char *res, wchar_t *found, size_t nfound)
{
    wchar_t root[MAX_PATH]; GetModuleFileNameW(NULL, root, MAX_PATH);
    for (int up = 0; up < 3; up++) { wchar_t *b = wcsrchr(root, L'\\'); if (!b) return 0; *b = 0; }   /* <game>\binaries\x64\<exe> */
    wchar_t wres[192]; MultiByteToWideChar(CP_UTF8, 0, res[0] == '/' ? res + 1 : res, -1, wres, 192);
    for (wchar_t *c = wres; *c; c++) if (*c == L'/') *c = L'\\';
    wchar_t wdir[192]; wcscpy(wdir, wres);                /* folder part of the resource, for the report */
    wchar_t *ls = wcsrchr(wdir, L'\\'); if (ls) ls[1] = 0; else wdir[0] = 0;
    wchar_t pat[MAX_PATH]; _snwprintf(pat, MAX_PATH, L"%ls\\mods\\*", root);
    WIN32_FIND_DATAW md; HANDLE hm = FindFirstFileW(pat, &md);
    if (hm == INVALID_HANDLE_VALUE) return 0;
    int ok = 0;
    do {
        if (!(md.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || md.cFileName[0] == L'.') continue;
        for (int pre = 0; pre < 2 && !ok; pre++) {
            const wchar_t *px = pre ? L"40k_" : L"";
            wchar_t fp[MAX_PATH];
            _snwprintf(fp, MAX_PATH, L"%ls\\mods\\%ls\\resource\\%ls%ls.*", root, md.cFileName, px, wres);
            WIN32_FIND_DATAW fd; HANDLE hf = FindFirstFileW(fp, &fd);
            if (hf == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                _snwprintf(found, nfound, L"mods\\%ls\\resource\\%ls%ls%ls", md.cFileName, px, wdir, fd.cFileName);
                found[nfound - 1] = 0;
                ok = 1;
            } while (!ok && FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    } while (!ok && FindNextFileW(hm, &md));
    FindClose(hm);
    return ok;
}

/* ini text: \n = new line, \s = a space (keeps spaces at the start/end of a value) */
static void unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; }
        else if (r[0] == '\\' && r[1] == 's') { *w++ = ' '; r++; }
        else *w++ = *r;
    }
    *w = 0;
}

static char g_lineTexName[64];
static int g_lineTexCheck = 1;

static void read_ini(void)
{
    g_wantHighlight    = ini_int("highlight", 1);
    g_hlFrame          = ini_int("highlight_frame", 1);
    if (g_hlFrame < 0 || g_hlFrame > 3) g_hlFrame = 1;
    g_wantGlow         = ini_int("glow", 1);
    ini_str("glow_widget", "rp_glow", g_glowName, sizeof g_glowName);
    g_wantLines        = ini_int("lines", 1);
    g_lineToResearched = ini_int("line_to_researched", 1);
    ini_str("line_texture", "progress_line_selected", g_lineTexName, sizeof g_lineTexName);
    g_lineTexCheck     = ini_int("line_texture_check", 1);
    char lc[32]; ini_str("line_color", "FFFFFFFF", lc, sizeof lc);
    const char *q = lc[0] == '#' ? lc + 1 : (lc[0] == '0' && (lc[1] == 'x' || lc[1] == 'X')) ? lc + 2 : lc;
    char *e; unsigned long v = strtoul(q, &e, 16);
    if (e != q && !*e) g_lineColor = (uint32_t)(strlen(q) <= 6 ? (v | 0xFF000000ul) : v);   /* RRGGBB = opaque */
    g_wantTooltip      = ini_int("tooltip", 1);
    g_maxNames         = ini_int("tooltip_max_names", 4);
    if (g_maxNames < 1) g_maxNames = 1;
    if (g_maxNames > 64) g_maxNames = 64;
    ini_str("tooltip_prefix", "\\nRequires:\\s", g_reqPrefix, sizeof g_reqPrefix); unescape(g_reqPrefix);
    ini_str("tooltip_separator", ",\\s", g_reqSep, sizeof g_reqSep); unescape(g_reqSep);
    char more[64]; ini_str("tooltip_more", ",\\s+%d more", more, sizeof more); unescape(more);
    const char *pc = strchr(more, '%');                  /* exactly one %d and no other conversion, else the default */
    if (pc && pc[1] == 'd' && !strchr(pc + 2, '%')) strcpy(g_reqMore, more);
}

/* the selected-line texture: <folder of the vanilla line>/<line_texture>, used only if a mod ships it */
static const char *setup_line_texture(void)
{
    if (!g_lineTexName[0]) return "vanilla (line_texture empty)";
    if (!g_okLineTex) return "vanilla (engine texture load not recognised)";
    for (const char *c = g_lineTexName; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-'))
            return "vanilla (line_texture: only letters, digits, _ and -; no folder, no extension)";
    const char *slash = strrchr(g_lineTexVanilla, '/');
    snprintf(g_lineTexSel, sizeof g_lineTexSel, "%.*s%s", (int)(slash + 1 - g_lineTexVanilla), g_lineTexVanilla, g_lineTexName);
    g_lineTexSelLen = strlen(g_lineTexSel);
    if (!g_lineTexCheck) { g_useLineTex = 1; return "ON (not checked: line_texture_check=0)"; }
    wchar_t found[MAX_PATH];
    if (!mod_has_resource(g_lineTexSel, found, MAX_PATH)) {
        logf_("selected-line texture %s.* not found under mods\\*\\resource\\ - path lines keep the vanilla texture", g_lineTexSel);
        return "vanilla (file not found in any mod)";
    }
    g_useLineTex = 1;
    logf_("selected-line texture %s = %ls", g_lineTexSel, found);
    return "ON";
}

static void log_hooked_slots(void)
{
    if (!g_nSlotCache) return;
    char s[160]; size_t n = 0;
    for (int i = 0; i < g_nSlotCache && n + 24 < sizeof s; i++)
        n += (size_t)snprintf(s + n, sizeof s - n, "%s %s[%d]", i ? "," : "", g_slotCache[i].vt == V_TREE ? "tree" : "icon", g_slotCache[i].slot);
    logf_("already hooked by another DLL (originals read from the exe file, hooks chained):%s", s);
}

static void init(HMODULE self)
{
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;
    if (!ini_int("enabled", 1)) return;
    g_console = ini_int("console", 0); g_verbose = ini_int("verbose", 1);
    if (ini_int("log", 1)) {                             /* one log per exe */
        wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir);
        wcscat(lp, !_wcsnicmp(en, L"call_to_arms_ed", 15) ? L"\\gem_researchpath_editor.log" :
                   !_wcsnicmp(en, L"call_to_arms_server", 19) ? L"\\gem_researchpath_server.log" : L"\\gem_researchpath.log");
        g_log = _wfopen(lp, L"w");
    }
    if (g_console) { AllocConsole(); SetConsoleTitleA(RP_NAME); freopen("CONOUT$", "w", stdout); }

    g_base = g_hdr = (uint8_t *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    g_timestamp = nt->FileHeader.TimeDateStamp;
    memcpy(&g_prefBase, (uint8_t *)&nt->OptionalHeader + 24, 8);     /* OptionalHeader.ImageBase */
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { g_base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh[i].Name, ".data", 8)) g_data = s;
    }
    if (!g_text.p || !g_rdata.p || !g_data.p) { logf_("not a PE I understand"); return; }
    logf_(RP_NAME " " RP_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);
    read_ini();

    if (!find_all()) { logf_("no Dynamic Campaign research tree in this exe (expected for the editor and the dedicated server) - inert"); return; }
    log_hooked_slots();
    if (!g_okTree || !g_okIcon) { logf_("%-22s OFF (engine code not recognised)", "research path"); return; }
    if (!g_okHover) { g_wantHighlight = 0; logf_("%-22s OFF (hovered dialog not found)", "path highlight"); }
    if (!g_wantHighlight && !g_wantTooltip) { logf_("everything off in the ini"); return; }
    int linesActive = g_wantHighlight && g_wantLines && g_okLines;
    const char *tex = linesActive ? setup_line_texture() : "";

    int ok = install();
    static const char *const frames[4] = { "normal frame = no visible change", "hover frame", "pressed frame", "disabled frame" };
    char st[128];
    snprintf(st, sizeof st, "ON (%s)", frames[g_hlFrame]);
    logf_("%-22s %s", "path highlight", !ok ? "OFF (VirtualProtect failed)" : !g_wantHighlight ? "off" : st);
    snprintf(st, sizeof st, "ON (widget \"%s\")", g_glowName);
    logf_("%-22s %s", "path glow", !ok ? "OFF (VirtualProtect failed)" : !g_wantHighlight ? "off (needs the highlight)" :
          !g_wantGlow || !g_glowName[0] ? "off (ini)" : !g_okGlow ? "OFF (engine code not recognised)" : st);
    snprintf(st, sizeof st, "ON (color %08X%s)", g_lineColor, g_lineToResearched ? ", + links to researched units" : "");
    logf_("%-22s %s", "path lines", !ok ? "OFF (VirtualProtect failed)" : !g_wantHighlight ? "off (needs the highlight)" :
          !g_wantLines ? "off (ini)" : !g_okLines ? "OFF (engine code not recognised)" : st);
    if (ok && linesActive) logf_("%-22s %s", "path line texture", tex);
    logf_("%-22s %s", "requires tooltip", !ok ? "OFF (VirtualProtect failed)" : g_wantTooltip ? "ON" : "off (ini)");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        g_exeFile = _wfopen(exe, L"rb");                /* only to read original vtable slots another DLL swapped */
        init(h);
        if (g_exeFile) { fclose(g_exeFile); g_exeFile = NULL; }
    }
    return TRUE;
}
#endif /* !RP_TEST */

#ifdef RP_TEST
#include "../tests/rp_test_main.c"
#endif
