/* GEM Battalions 0.1.0 - phase-0 PROBE for Call to Arms: Gates of Hell (1.067, x64).
 *
 * Logs only. Changes no game behaviour. Goal: map the Conquest (dynamic campaign)
 * logic the Battalions DLL will need later: campaign state object and roster,
 * regions and ownership, turn end, enemy attack, battle launch and result
 * write-back, save/load.
 *
 * Everything is discovered at runtime from the engine's own MSVC RTTI, so no
 * engine address is hard-coded. What it does:
 *   1. Scans the exe for RTTI type descriptors, complete object locators and
 *      vtables; logs every class whose name matches [trace] classes.
 *   2. Wraps every virtual slot of those classes with a transparent trampoline
 *      (arity-agnostic: saves rcx/rdx/r8/r9/xmm0-3, logs, tail-jumps to the
 *      original). Logs "Class::vt[i] #n this from caller a2 a3 a4", the first
 *      `first` calls per slot and then every `every`-th.
 *   3. eGUI_DynamicCampaignView: on Bind dumps its fields (strings, vectors,
 *      pointers to objects with their class names), recursing into campaign
 *      objects; on the first Update lists its child dialogs and scans .data for
 *      campaign singletons; on every Notify logs the arguments; on Notify,
 *      Update and scene loads diffs all tracked objects and logs changed qwords.
 *   4. Scene::eManager vt[5] Load: logs the scene name and descriptor strings
 *      with the caller (battle launch path).
 *   5. CreateFileW (exe IAT): logs save/profile files with the caller.
 *
 * Hooks are installed from a worker thread after `install_delay_ms`, so this
 * DLL wraps the slots AFTER every other payload DLL installed its own hooks in
 * DllMain (ConquestUI hooks the same view slots). Build: build.bat (zig cc).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "gb_names.h"

#define GB_VERSION "0.1.0"
#define GB_NAME "gem_battalions"

/* ------------------------------------------------------------------ config */
static struct {
    int log, verbose, trace, file_hooks, game_only;
    int trace_first, trace_every, max_slots, install_delay_ms;
    int dump_bytes, dump_depth, diff_min_ms, max_log_mb;
    char trace_classes[1024];
    char trace_exclude[1024];
    char dump_classes[1024];
} g_cfg;

/* ------------------------------------------------------------------ log */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_cs;     /* log */
static CRITICAL_SECTION g_obj_cs; /* tracked objects, dumps, diffs */
static LONG g_log_bytes, g_log_max;
static HMODULE g_self;
static wchar_t g_dir[MAX_PATH];

static void gb_log(const char *fmt, ...)
{
    char buf[1400];
    SYSTEMTIME st;
    va_list ap;
    int n;
    DWORD w;

    if (g_log == INVALID_HANDLE_VALUE)
        return;
    GetLocalTime(&st);
    n = snprintf(buf, sizeof buf, "%02u:%02u:%02u.%03u [%5lu] ", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, (unsigned long)GetCurrentThreadId());
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - (size_t)n - 3, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 3)
        n = (int)sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_cs);
    if (g_log_bytes < g_log_max) {
        WriteFile(g_log, buf, (DWORD)n, &w, NULL);
        g_log_bytes += n;
        if (g_log_bytes >= g_log_max) {
            const char *m = "--- log size cap reached (max_log_mb); logging stopped ---\r\n";
            WriteFile(g_log, m, (DWORD)strlen(m), &w, NULL);
        }
    }
    LeaveCriticalSection(&g_cs);
}

#define VLOG(...) do { if (g_cfg.verbose) gb_log(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ exe image */
typedef struct {
    uint8_t *beg, *end;
    DWORD chr;
    char name[9];
} Section;

static struct {
    uint8_t *base;
    uint32_t size;
    uint32_t timestamp;
    Section sec[48];
    int nsec;
    char exe_name[64];
    int is_editor, is_server;
} g_exe;

static int in_image(const void *p)
{
    return (const uint8_t *)p >= g_exe.base && (const uint8_t *)p < g_exe.base + g_exe.size;
}

static const Section *sec_of(const void *p)
{
    int i;
    for (i = 0; i < g_exe.nsec; i++)
        if ((const uint8_t *)p >= g_exe.sec[i].beg && (const uint8_t *)p < g_exe.sec[i].end)
            return &g_exe.sec[i];
    return NULL;
}

static int in_exe_code(const void *p)
{
    const Section *s = in_image(p) ? sec_of(p) : NULL;
    return s && (s->chr & IMAGE_SCN_MEM_EXECUTE);
}

static int parse_exe(void)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sh;
    int i;
    wchar_t path[MAX_PATH];
    const wchar_t *b;
    char lower[64];

    g_exe.base = (uint8_t *)GetModuleHandleW(NULL);
    if (!g_exe.base)
        return 0;
    dos = (IMAGE_DOS_HEADER *)g_exe.base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS64 *)(g_exe.base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    g_exe.size = nt->OptionalHeader.SizeOfImage;
    g_exe.timestamp = nt->FileHeader.TimeDateStamp;
    sh = IMAGE_FIRST_SECTION(nt);
    g_exe.nsec = 0;
    for (i = 0; i < nt->FileHeader.NumberOfSections && g_exe.nsec < 48; i++) {
        Section *s = &g_exe.sec[g_exe.nsec++];
        s->beg = g_exe.base + sh[i].VirtualAddress;
        s->end = s->beg + sh[i].Misc.VirtualSize;
        s->chr = sh[i].Characteristics;
        memcpy(s->name, sh[i].Name, 8);
        s->name[8] = 0;
    }
    GetModuleFileNameW(NULL, path, MAX_PATH);
    b = wcsrchr(path, L'\\');
    b = b ? b + 1 : path;
    for (i = 0; b[i] && i < 63; i++)
        lower[i] = (char)((b[i] >= L'A' && b[i] <= L'Z') ? b[i] + 32 : (b[i] < 128 ? b[i] : '?'));
    lower[i] = 0;
    strcpy(g_exe.exe_name, lower);
    g_exe.is_editor = strstr(lower, "_ed") != NULL;
    g_exe.is_server = strstr(lower, "server") != NULL;
    return 1;
}

/* ------------------------------------------------------------------ safe memory */
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = (uintptr_t)p;
    if (a < 0x10000 || a + n < a)
        return 0;
    if (in_image(p))
        return a + n <= (uintptr_t)g_exe.base + g_exe.size;
    if (!VirtualQuery(p, &mbi, sizeof mbi))
        return 0;
    if (mbi.State != MEM_COMMIT)
        return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return 0;
    if (mbi.Protect == 0)
        return 0;
    return a + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static int executable(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (in_image(p))
        return in_exe_code(p);
    if ((uintptr_t)p < 0x10000 || !VirtualQuery(p, &mbi, sizeof mbi))
        return 0;
    return mbi.State == MEM_COMMIT &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static const char *fmt_addr(char *buf, size_t cap, const void *p)
{
    HMODULE m = NULL;
    if (!p) {
        snprintf(buf, cap, "null");
    } else if (in_image(p)) {
        snprintf(buf, cap, "exe+0x%llX", (unsigned long long)((const uint8_t *)p - g_exe.base));
    } else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCWSTR)p, &m) && m) {
        wchar_t w[MAX_PATH];
        const wchar_t *b;
        char nm[64];
        int i;
        GetModuleFileNameW(m, w, MAX_PATH);
        b = wcsrchr(w, L'\\');
        b = b ? b + 1 : w;
        for (i = 0; b[i] && i < 63; i++)
            nm[i] = (char)(b[i] < 128 ? b[i] : '?');
        nm[i] = 0;
        snprintf(buf, cap, "%s+0x%llX", nm, (unsigned long long)((const uint8_t *)p - (const uint8_t *)m));
    } else {
        snprintf(buf, cap, "%p", p);
    }
    return buf;
}

/* MSVC std::string (x64): 16-byte buffer/pointer, size, capacity. */
static int read_std_string(const void *s, char *out, size_t cap)
{
    const uint8_t *b = (const uint8_t *)s;
    uint64_t sz, cp;
    const char *src;
    size_t i;
    if (!readable(s, 32))
        return 0;
    sz = *(const uint64_t *)(b + 16);
    cp = *(const uint64_t *)(b + 24);
    if (cp < 15 || cp > 0x100000 || sz > cp)
        return 0;
    if (cp == 15) {
        src = (const char *)b;
        if (sz > 15)
            return 0;
    } else {
        src = *(const char *const *)b;
        if (!readable(src, (size_t)sz + 1))
            return 0;
    }
    if (src[sz] != 0)
        return 0;
    for (i = 0; i < sz; i++)
        if ((unsigned char)src[i] < 0x20 || (unsigned char)src[i] > 0x7E)
            return 0;
    if (sz >= cap)
        sz = cap - 1;
    memcpy(out, src, (size_t)sz);
    out[sz] = 0;
    return 1;
}

/* ------------------------------------------------------------------ RTTI */
typedef struct {
    uint32_t sig, offset, cdoff, td_rva, cd_rva, self_rva;
} RttiCOL;

typedef struct {
    const RttiCOL *col;
    const char *mangled;
    char name[128];
    void **vt;
    uint32_t nslots; /* counted against exe code sections */
    uint32_t offset; /* 0 = primary vtable */
    int kind;        /* K_* */
    int traced;
} ClassInfo;

enum { K_NONE = 0, K_VIEW = 1, K_SCENE = 2 };

static const RttiCOL **g_cols;
static int g_ncols, g_cols_cap;
static ClassInfo *g_cls;
static int g_ncls, g_cls_cap;

static int cmp_ptr(const void *a, const void *b)
{
    const void *x = *(const void *const *)a, *y = *(const void *const *)b;
    return x < y ? -1 : x > y;
}

static int cmp_cls_vt(const void *a, const void *b)
{
    const ClassInfo *x = a, *y = b;
    return x->vt < y->vt ? -1 : x->vt > y->vt;
}

static const char *td_name(uint32_t td_rva)
{
    const uint8_t *td;
    if (td_rva < 0x1000 || td_rva + 24 >= g_exe.size)
        return NULL;
    td = g_exe.base + td_rva;
    if (!in_image(*(void **)td)) /* type_info vftable */
        return NULL;
    if (memcmp(td + 16, ".?A", 3) != 0)
        return NULL;
    return (const char *)td + 16;
}

static void push_col(const RttiCOL *c)
{
    if (g_ncols == g_cols_cap) {
        int ncap = g_cols_cap ? g_cols_cap * 2 : 4096;
        const RttiCOL **n = (const RttiCOL **)HeapAlloc(GetProcessHeap(), 0, (size_t)ncap * sizeof *n);
        if (!n)
            return;
        if (g_cols) {
            memcpy(n, g_cols, (size_t)g_ncols * sizeof *n);
            HeapFree(GetProcessHeap(), 0, g_cols);
        }
        g_cols = n;
        g_cols_cap = ncap;
    }
    g_cols[g_ncols++] = c;
}

static ClassInfo *push_cls(void)
{
    if (g_ncls == g_cls_cap) {
        int ncap = g_cls_cap ? g_cls_cap * 2 : 4096;
        ClassInfo *n = (ClassInfo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)ncap * sizeof *n);
        if (!n)
            return NULL;
        if (g_cls) {
            memcpy(n, g_cls, (size_t)g_ncls * sizeof *n);
            HeapFree(GetProcessHeap(), 0, g_cls);
        }
        g_cls = n;
        g_cls_cap = ncap;
    }
    return &g_cls[g_ncls++];
}

static int sec_scannable(const Section *s)
{
    return (s->chr & IMAGE_SCN_MEM_READ) && !(s->chr & IMAGE_SCN_MEM_EXECUTE) &&
           !(s->chr & IMAGE_SCN_MEM_DISCARDABLE) && s->end > s->beg;
}

static void rtti_scan(void)
{
    int i;
    DWORD t0 = GetTickCount();

    /* 1. complete object locators: {1, off, cdoff, td, cd, self} with self == own RVA */
    for (i = 0; i < g_exe.nsec; i++) {
        const Section *s = &g_exe.sec[i];
        const uint8_t *p;
        if (!sec_scannable(s))
            continue;
        for (p = s->beg; p + sizeof(RttiCOL) <= s->end; p += 4) {
            const RttiCOL *c = (const RttiCOL *)p;
            if (c->sig != 1)
                continue;
            if (c->self_rva != (uint32_t)(p - g_exe.base))
                continue;
            if (!td_name(c->td_rva))
                continue;
            push_col(c);
        }
    }
    qsort(g_cols, (size_t)g_ncols, sizeof *g_cols, cmp_ptr);

    /* 2. vtables: a qword holding a COL address; the vtable starts right after it */
    for (i = 0; i < g_exe.nsec; i++) {
        const Section *s = &g_exe.sec[i];
        const uint8_t *p;
        if (!sec_scannable(s))
            continue;
        for (p = s->beg; p + 16 <= s->end; p += 8) {
            const void *v = *(const void *const *)p;
            const RttiCOL **hit;
            ClassInfo *ci;
            void **vt;
            uint32_t n;
            if (!in_image(v))
                continue;
            hit = (const RttiCOL **)bsearch(&v, g_cols, (size_t)g_ncols, sizeof *g_cols, cmp_ptr);
            if (!hit)
                continue;
            vt = (void **)(p + 8);
            for (n = 0; n < 1024 && (const uint8_t *)&vt[n] + 8 <= s->end; n++)
                if (!in_exe_code(vt[n]))
                    break;
            if (n == 0)
                continue;
            ci = push_cls();
            if (!ci)
                break;
            ci->col = *hit;
            ci->mangled = td_name((*hit)->td_rva);
            gb_demangle(ci->mangled, ci->name, sizeof ci->name);
            ci->vt = vt;
            ci->nslots = n;
            ci->offset = (*hit)->offset;
            if (ci->offset == 0) {
                if (strcmp(ci->name, "eGUI_DynamicCampaignView") == 0)
                    ci->kind = K_VIEW;
                else if (strcmp(ci->name, "Scene::eManager") == 0)
                    ci->kind = K_SCENE;
            }
        }
    }
    qsort(g_cls, (size_t)g_ncls, sizeof *g_cls, cmp_cls_vt);
    gb_log("rtti: %d locators, %d vtables, %lu ms", g_ncols, g_ncls, (unsigned long)(GetTickCount() - t0));
}

static ClassInfo *class_by_vt(const void *vt)
{
    ClassInfo key;
    key.vt = (void **)vt;
    return (ClassInfo *)bsearch(&key, g_cls, (size_t)g_ncls, sizeof *g_cls, cmp_cls_vt);
}

/* class of a live object (reads its first qword) */
static ClassInfo *class_of(const void *obj)
{
    const void *vt;
    if (!readable(obj, 8))
        return NULL;
    vt = *(const void *const *)obj;
    if (!in_image(vt))
        return NULL;
    return class_by_vt(vt);
}

static ClassInfo *find_class(const char *name, int kind)
{
    int i;
    for (i = 0; i < g_ncls; i++)
        if (g_cls[i].offset == 0 && (kind ? g_cls[i].kind == kind : strcmp(g_cls[i].name, name) == 0))
            return &g_cls[i];
    return NULL;
}

/* ------------------------------------------------------------------ tracked objects + diff */
typedef struct {
    uint8_t *ptr;
    uint32_t size;
    uint8_t *snap;
    char name[96];
} Tracked;

static Tracked g_trk[32];
static int g_ntrk;
static DWORD g_last_diff_tick;

static int track_add(void *p, uint32_t size, const char *name)
{
    int i;
    for (i = 0; i < g_ntrk; i++)
        if (g_trk[i].ptr == p)
            return 0;
    if (g_ntrk >= 32 || !readable(p, size))
        return 0;
    g_trk[g_ntrk].ptr = (uint8_t *)p;
    g_trk[g_ntrk].size = size;
    g_trk[g_ntrk].snap = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!g_trk[g_ntrk].snap)
        return 0;
    memcpy(g_trk[g_ntrk].snap, p, size);
    snprintf(g_trk[g_ntrk].name, sizeof g_trk[g_ntrk].name, "%s", name);
    g_ntrk++;
    gb_log("[track] %s %p (%u bytes), %d tracked", name, p, size, g_ntrk);
    return 1;
}

static void track_clear(void)
{
    int i;
    for (i = 0; i < g_ntrk; i++)
        HeapFree(GetProcessHeap(), 0, g_trk[i].snap);
    g_ntrk = 0;
}

static void describe_qword(char *out, size_t cap, uint64_t v)
{
    char a[96];
    const ClassInfo *ci;
    if (v == 0) {
        snprintf(out, cap, "0");
        return;
    }
    if (in_image((void *)v)) {
        ci = class_by_vt((void *)v);
        if (ci) {
            snprintf(out, cap, "vtable %s", ci->name);
            return;
        }
        if (in_exe_code((void *)v)) {
            snprintf(out, cap, "fn %s", fmt_addr(a, sizeof a, (void *)v));
            return;
        }
        snprintf(out, cap, "%s", fmt_addr(a, sizeof a, (void *)v));
        return;
    }
    if (v >= 0x10000 && v < 0x00007FFFFFFFFFFFull && (v & 7) == 0 && readable((void *)v, 8)) {
        ci = class_of((void *)v);
        if (ci) {
            snprintf(out, cap, "-> %s", ci->name);
            return;
        }
        snprintf(out, cap, "ptr 0x%llX", (unsigned long long)v);
        return;
    }
    if (v < 0x100000000ull)
        snprintf(out, cap, "0x%llX (%lld)", (unsigned long long)v, (long long)(int32_t)(uint32_t)v);
    else
        snprintf(out, cap, "0x%llX", (unsigned long long)v);
}

static void diff_all(const char *reason)
{
    int i;
    DWORD now = GetTickCount();
    uint8_t tmp[0x1000];
    int any = 0;

    if (g_ntrk == 0)
        return;
    if (now - g_last_diff_tick < (DWORD)g_cfg.diff_min_ms && strcmp(reason, "update") == 0)
        return;
    g_last_diff_tick = now;
    EnterCriticalSection(&g_obj_cs);
    for (i = 0; i < g_ntrk; i++) {
        Tracked *t = &g_trk[i];
        uint32_t off, lines = 0;
        if (t->size > sizeof tmp)
            t->size = sizeof tmp;
        if (!readable(t->ptr, t->size)) {
            gb_log("[diff] %s %p unreadable now", t->name, t->ptr);
            continue;
        }
        memcpy(tmp, t->ptr, t->size);
        if (memcmp(tmp, t->snap, t->size) == 0)
            continue;
        for (off = 0; off + 8 <= t->size; off += 8) {
            uint64_t a = *(uint64_t *)(t->snap + off), b = *(uint64_t *)(tmp + off);
            char da[128], db[128];
            if (a == b)
                continue;
            if (!any) {
                gb_log("[diff] after %s:", reason);
                any = 1;
            }
            if (++lines > 48) {
                gb_log("  %s: ... more changes omitted", t->name);
                break;
            }
            describe_qword(da, sizeof da, a);
            describe_qword(db, sizeof db, b);
            gb_log("  %s +0x%X: %s -> %s", t->name, off, da, db);
        }
        memcpy(t->snap, tmp, t->size);
    }
    LeaveCriticalSection(&g_obj_cs);
}

/* ------------------------------------------------------------------ object dump */
static int dump_lines;

static void dump_object(uint8_t *obj, uint32_t size, int depth, const char *label)
{
    uint32_t off;
    char d[160], str[200];
    ClassInfo *ci = class_of(obj);

    if (!readable(obj, size)) {
        gb_log("[dump] %s %p: not readable (%u bytes)", label, obj, size);
        return;
    }
    gb_log("[dump] %s %p class=%s (%u bytes, depth %d)", label, obj, ci ? ci->name : "?", size, depth);
    dump_lines = 0;
    for (off = 0; off + 8 <= size; off += 8) {
        uint64_t v = *(uint64_t *)(obj + off);
        if (v == 0)
            continue;
        if (++dump_lines > 400) {
            gb_log("  ... dump line cap");
            break;
        }
        /* std::string? */
        if (off + 32 <= size && read_std_string(obj + off, str, sizeof str)) {
            gb_log("  +0x%X string \"%s\"", off, str);
            off += 24;
            continue;
        }
        /* std::vector {begin,end,cap}? */
        if (off + 24 <= size) {
            uint64_t e = *(uint64_t *)(obj + off + 8), c = *(uint64_t *)(obj + off + 16);
            if (!in_image((void *)v) && v >= 0x10000 && e >= v && c >= e && c - v < 0x2000000 &&
                (v & 7) == 0 && readable((void *)v, e > v ? (size_t)(e - v) : 8)) {
                ClassInfo *ec = NULL;
                const char *kind = "";
                if (e - v >= 8) {
                    uint64_t first = *(uint64_t *)v;
                    ec = class_of((void *)first);
                    if (ec)
                        kind = "*";
                    else
                        ec = class_of((void *)v);
                }
                gb_log("  +0x%X vector %llu bytes (cap %llu) elems: %s%s", off, (unsigned long long)(e - v),
                       (unsigned long long)(c - v), ec ? ec->name : "?", kind);
                off += 16;
                continue;
            }
        }
        describe_qword(d, sizeof d, v);
        gb_log("  +0x%X %s", off, d);
    }
    /* recurse into interesting objects */
    if (depth < g_cfg.dump_depth) {
        for (off = 0; off + 8 <= size; off += 8) {
            uint64_t v = *(uint64_t *)(obj + off);
            ClassInfo *sub;
            char lab[128];
            if (v < 0x10000 || in_image((void *)v) || (v & 7))
                continue;
            sub = class_of((void *)v);
            if (!sub || !gb_list_match(sub->name, g_cfg.dump_classes))
                continue;
            snprintf(lab, sizeof lab, "%s+0x%X", label, off);
            if (track_add((void *)v, 0x400, sub->name))
                dump_object((uint8_t *)v, 0x400, depth + 1, lab);
        }
    }
}

/* eDialog: children vector +0xD8/+0xE0, name std::string +0x58, flags +0x48 */
static int child_lines;

static void list_children(uint8_t *dlg, int depth)
{
    uint8_t **beg, **end;
    char pad[16], name[96];
    int i, n;

    if (!readable(dlg, 0xE8))
        return;
    beg = *(uint8_t ***)(dlg + 0xD8);
    end = *(uint8_t ***)(dlg + 0xE0);
    if (!beg || end < beg || end - beg > 2048 || !readable(beg, (size_t)(end - beg) * 8))
        return;
    n = (int)(end - beg);
    memset(pad, ' ', sizeof pad);
    pad[depth * 2 < 15 ? depth * 2 : 15] = 0;
    for (i = 0; i < n; i++) {
        uint8_t *c = beg[i];
        ClassInfo *ci = class_of(c);
        uint32_t flags;
        if (!ci || !readable(c, 0xE8))
            continue;
        if (++child_lines > 600) {
            gb_log("  ... child line cap");
            return;
        }
        if (!read_std_string(c + 0x58, name, sizeof name))
            strcpy(name, "?");
        flags = *(uint32_t *)(c + 0x48);
        gb_log("  %s%s \"%s\" %p%s", pad, ci->name, name, c, (flags & 2) ? "" : " (hidden)");
        if (depth < 4)
            list_children(c, depth + 1);
    }
}

/* .data scan: static objects of traced classes and global pointers to them */
static void scan_globals(void)
{
    int i, found = 0;
    for (i = 0; i < g_exe.nsec; i++) {
        const Section *s = &g_exe.sec[i];
        const uint8_t *p;
        if (!(s->chr & IMAGE_SCN_MEM_WRITE) || !(s->chr & IMAGE_SCN_MEM_READ) || (s->chr & IMAGE_SCN_MEM_EXECUTE))
            continue;
        for (p = s->beg; p + 8 <= s->end; p += 8) {
            uint64_t v = *(const uint64_t *)p;
            ClassInfo *ci;
            if (v < 0x10000)
                continue;
            if (in_image((void *)v)) {
                ci = class_by_vt((void *)v);
                if (ci && ci->traced) {
                    gb_log("[global] static %s object at exe+0x%llX", ci->name,
                           (unsigned long long)(p - g_exe.base));
                    found++;
                }
                continue;
            }
            if ((v & 7) || v > 0x00007FFFFFFFFFFFull)
                continue;
            ci = class_of((void *)v);
            if (ci && ci->traced) {
                gb_log("[global] pointer at exe+0x%llX -> %s object %p", (unsigned long long)(p - g_exe.base),
                       ci->name, (void *)v);
                found++;
            }
            if (found > 200)
                return;
        }
    }
    gb_log("[global] scan done, %d hits", found);
}

/* ------------------------------------------------------------------ tracer */
typedef struct {
    void *orig;
    ClassInfo *cls;
    uint32_t slot;
    volatile LONG count;
} TraceSlot;

static TraceSlot *g_slots;
static uint32_t g_nslots;
static uint8_t *g_stubs;
static ClassInfo *g_view_cls, *g_scene_cls;
static volatile LONG g_pending_children;
static int g_globals_done;

static void trace_summary(const char *why)
{
    uint32_t i, shown = 0;
    gb_log("[summary] call counts (%s):", why);
    for (i = 0; i < g_nslots; i++) {
        if (g_slots[i].count == 0)
            continue;
        if (++shown > 200) {
            gb_log("  ... more");
            break;
        }
        gb_log("  %s::vt[%u] = %ld", g_slots[i].cls->name, g_slots[i].slot, (long)g_slots[i].count);
    }
}

static void on_view_bind(uint8_t *view, void *caller)
{
    char a[96];
    gb_log("[view] Bind this=%p from %s", view, fmt_addr(a, sizeof a, caller));
    EnterCriticalSection(&g_obj_cs);
    track_clear();
    track_add(view, (uint32_t)g_cfg.dump_bytes, "view");
    dump_object(view, (uint32_t)g_cfg.dump_bytes, 0, "view");
    LeaveCriticalSection(&g_obj_cs);
    InterlockedExchange(&g_pending_children, 1);
}

static void on_view_update(uint8_t *view)
{
    if (InterlockedExchange(&g_pending_children, 0)) {
        gb_log("[view] first Update after Bind: children of the view:");
        child_lines = 0;
        list_children(view, 1);
        if (!g_globals_done) {
            g_globals_done = 1;
            scan_globals();
        }
        diff_all("bind->update");
        return;
    }
    diff_all("update");
}

static void on_view_notify(uint8_t *view, uint64_t *saved, void *caller)
{
    char a[96], d2[128], d3[128], d4[128], nm[96];
    ClassInfo *ci;
    describe_qword(d2, sizeof d2, saved[2]);
    describe_qword(d3, sizeof d3, saved[1]);
    describe_qword(d4, sizeof d4, saved[0]);
    gb_log("[view] Notify this=%p a2=%s a3=%s a4=%s from %s", view, d2, d3, d4, fmt_addr(a, sizeof a, caller));
    ci = class_of((void *)saved[2]);
    if (ci && readable((void *)saved[2], 0x80) && read_std_string((uint8_t *)saved[2] + 0x58, nm, sizeof nm))
        gb_log("[view]   a2 is dialog %s \"%s\"", ci->name, nm);
    diff_all("notify");
}

static void on_scene_load(uint8_t *scene, uint8_t *desc, uint64_t flag, void *caller)
{
    char a[96], nm[128], d0[128], d1[128], d2[128];
    if (!read_std_string(scene + 0x860, nm, sizeof nm))
        strcpy(nm, "?");
    if (!desc || !read_std_string(desc, d0, sizeof d0))
        strcpy(d0, "?");
    if (!desc || !read_std_string(desc + 0x28, d1, sizeof d1))
        strcpy(d1, "?");
    if (!desc || !read_std_string(desc + 0x50, d2, sizeof d2))
        strcpy(d2, "?");
    gb_log("[scene] Load name \"%s\" desc \"%s\" / \"%s\" / \"%s\" flag %llu from %s", nm, d0, d1, d2,
           (unsigned long long)flag, fmt_addr(a, sizeof a, caller));
    diff_all("scene load");
    trace_summary("scene load");
}

/* Called by the trampoline with the slot index, the caller's return address and
 * a pointer to the saved registers {r9, r8, rdx, rcx}. Returns the original. */
__attribute__((used)) void *gb_trace_entry(uint32_t idx, void *caller, uint64_t *saved)
{
    TraceSlot *s = &g_slots[idx];
    LONG n = InterlockedIncrement(&s->count);
    uint8_t *self = (uint8_t *)saved[3];
    int handled = 0;

    if (s->cls->kind == K_VIEW) {
        switch (s->slot) {
        case 69:
            on_view_bind(self, caller);
            handled = 1;
            break;
        case 34:
            on_view_notify(self, saved, caller);
            handled = 1;
            break;
        case 20:
            on_view_update(self);
            handled = 1;
            break;
        case 0:
            gb_log("[view] dtor this=%p", self);
            trace_summary("view dtor");
            EnterCriticalSection(&g_obj_cs);
            track_clear();
            LeaveCriticalSection(&g_obj_cs);
            handled = 1;
            break;
        default:
            break;
        }
    } else if (s->cls->kind == K_SCENE && s->slot == 5) {
        on_scene_load(self, (uint8_t *)saved[2], saved[1], caller);
        handled = 1;
    }
    if (!handled && (n <= g_cfg.trace_first || (g_cfg.trace_every > 0 && n % g_cfg.trace_every == 0))) {
        char a[96];
        gb_log("[trace] %s::vt[%u] #%ld this=%p a2=0x%llX a3=0x%llX a4=0x%llX from %s", s->cls->name, s->slot,
               (long)n, (void *)self, (unsigned long long)saved[2], (unsigned long long)saved[1],
               (unsigned long long)saved[0], fmt_addr(a, sizeof a, caller));
    }
    return s->orig;
}

/* Common trampoline. Entered by a per-slot stub that set r10d = slot index.
 * Stack on entry: [rsp] = return address into the caller (rsp == 8 mod 16). */
__attribute__((naked)) static void gb_trace_thunk(void)
{
    __asm__ volatile(
        "movq (%rsp), %rax\n\t"
        "pushq %rcx\n\t"
        "pushq %rdx\n\t"
        "pushq %r8\n\t"
        "pushq %r9\n\t"
        "subq $0x68, %rsp\n\t"
        "movups %xmm0, 0x20(%rsp)\n\t"
        "movups %xmm1, 0x30(%rsp)\n\t"
        "movups %xmm2, 0x40(%rsp)\n\t"
        "movups %xmm3, 0x50(%rsp)\n\t"
        "movl %r10d, %ecx\n\t"
        "movq %rax, %rdx\n\t"
        "leaq 0x68(%rsp), %r8\n\t"
        "call gb_trace_entry\n\t"
        "movups 0x20(%rsp), %xmm0\n\t"
        "movups 0x30(%rsp), %xmm1\n\t"
        "movups 0x40(%rsp), %xmm2\n\t"
        "movups 0x50(%rsp), %xmm3\n\t"
        "addq $0x68, %rsp\n\t"
        "popq %r9\n\t"
        "popq %r8\n\t"
        "popq %rdx\n\t"
        "popq %rcx\n\t"
        "jmp *%rax\n\t");
}

static int trace_class(ClassInfo *c)
{
    uint32_t i, n = 0, installed = 0;
    DWORD old;
    char a[96];

    /* recount slots accepting other modules' hooks (executable anywhere) */
    for (n = 0; n < 1024; n++)
        if (!executable(c->vt[n]))
            break;
    if (n == 0)
        return 0;
    if (!VirtualProtect(c->vt, n * sizeof(void *), PAGE_READWRITE, &old)) {
        gb_log("[trace] %s: VirtualProtect failed %lu", c->name, GetLastError());
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint8_t *stub;
        TraceSlot *s;
        if (g_nslots >= (uint32_t)g_cfg.max_slots)
            break;
        if (c->kind == K_SCENE && i != 5 && i != 0)
            continue;
        s = &g_slots[g_nslots];
        s->orig = c->vt[i];
        s->cls = c;
        s->slot = i;
        s->count = 0;
        if (!in_image(s->orig))
            gb_log("[trace] %s::vt[%u] already hooked by %s", c->name, i, fmt_addr(a, sizeof a, s->orig));
        stub = g_stubs + (size_t)g_nslots * 32;
        stub[0] = 0x41; stub[1] = 0xBA;                 /* mov r10d, imm32 */
        memcpy(stub + 2, &g_nslots, 4);
        stub[6] = 0xFF; stub[7] = 0x25;                 /* jmp [rip+0] */
        memset(stub + 8, 0, 4);
        {
            void *t = (void *)gb_trace_thunk;
            memcpy(stub + 12, &t, 8);
        }
        InterlockedExchangePointer(&c->vt[i], stub);
        g_nslots++;
        installed++;
    }
    VirtualProtect(c->vt, n * sizeof(void *), old, &old);
    c->traced = 1;
    gb_log("[trace] %s vtable exe+0x%llX: %u slots, %u wrapped", c->name,
           (unsigned long long)((uint8_t *)c->vt - g_exe.base), n, installed);
    return (int)installed;
}

static void install_tracer(void)
{
    int i, ncls = 0;
    DWORD old;

    g_slots = (TraceSlot *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)g_cfg.max_slots * sizeof *g_slots);
    g_stubs = (uint8_t *)VirtualAlloc(NULL, (size_t)g_cfg.max_slots * 32, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!g_slots || !g_stubs) {
        gb_log("[trace] allocation failed");
        return;
    }
    /* forced classes first so their slots exist even if the filter misses them */
    for (i = 0; i < g_ncls; i++) {
        ClassInfo *c = &g_cls[i];
        if (c->offset != 0 || c->traced)
            continue;
        if (c->kind == K_VIEW || c->kind == K_SCENE) {
            trace_class(c);
            ncls++;
        }
    }
    for (i = 0; i < g_ncls; i++) {
        ClassInfo *c = &g_cls[i];
        if (c->offset != 0 || c->traced)
            continue;
        if (!gb_list_match(c->name, g_cfg.trace_classes))
            continue;
        if (gb_list_match(c->name, g_cfg.trace_exclude)) {
            VLOG("[trace] skip %s (excluded), vtable exe+0x%llX, %u slots", c->name,
                 (unsigned long long)((uint8_t *)c->vt - g_exe.base), c->nslots);
            continue;
        }
        if (g_nslots >= (uint32_t)g_cfg.max_slots) {
            gb_log("[trace] max_slots reached at %s", c->name);
            break;
        }
        trace_class(c);
        ncls++;
    }
    FlushInstructionCache(GetCurrentProcess(), g_stubs, (size_t)g_cfg.max_slots * 32);
    VirtualProtect(g_stubs, (size_t)g_cfg.max_slots * 32, PAGE_EXECUTE_READ, &old);
    gb_log("[trace] %d classes, %u slots wrapped", ncls, g_nslots);
}

/* ------------------------------------------------------------------ CreateFileW (exe IAT) */
typedef HANDLE(WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t g_orig_CreateFileW;

static int path_interesting(const wchar_t *p)
{
    wchar_t low[520];
    int i;
    for (i = 0; p[i] && i < 519; i++)
        low[i] = (p[i] >= L'A' && p[i] <= L'Z') ? p[i] + 32 : p[i];
    low[i] = 0;
    if (wcsstr(low, L".log"))
        return 0;
    return wcsstr(low, L"profiles") || wcsstr(low, L"campaign") || wcsstr(low, L"save") || wcsstr(low, L".sav");
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_orig_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    if (name && path_interesting(name)) {
        char a[96], u8[1024];
        WideCharToMultiByte(CP_UTF8, 0, name, -1, u8, sizeof u8, NULL, NULL);
        gb_log("[file] %s %s access=%s%s disp=%lu from %s", h == INVALID_HANDLE_VALUE ? "FAIL" : "open", u8,
               (access & GENERIC_READ) ? "R" : "", (access & GENERIC_WRITE) ? "W" : "", (unsigned long)disp,
               fmt_addr(a, sizeof a, __builtin_return_address(0)));
        if (h != INVALID_HANDLE_VALUE)
            diff_all("file open");
    }
    return h;
}

static void install_file_hook(void)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_exe.base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_exe.base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int hooked = 0;

    if (!dir.VirtualAddress)
        return;
    imp = (IMAGE_IMPORT_DESCRIPTOR *)(g_exe.base + dir.VirtualAddress);
    for (; imp->Name; imp++) {
        IMAGE_THUNK_DATA64 *o = (IMAGE_THUNK_DATA64 *)(g_exe.base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *f = (IMAGE_THUNK_DATA64 *)(g_exe.base + imp->FirstThunk);
        if (!imp->OriginalFirstThunk)
            continue;
        for (; o->u1.AddressOfData; o++, f++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (o->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(g_exe.base + o->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, "CreateFileW") != 0)
                continue;
            if (!VirtualProtect(&f->u1.Function, 8, PAGE_READWRITE, &old))
                continue;
            if (!g_orig_CreateFileW)
                g_orig_CreateFileW = (CreateFileW_t)(uintptr_t)f->u1.Function;
            f->u1.Function = (ULONGLONG)(uintptr_t)hook_CreateFileW;
            VirtualProtect(&f->u1.Function, 8, old, &old);
            hooked++;
        }
    }
    gb_log("[file] CreateFileW IAT entries hooked: %d (orig %p)", hooked, (void *)g_orig_CreateFileW);
}

/* ------------------------------------------------------------------ startup */
static void read_ini(void)
{
    wchar_t ini[MAX_PATH];
    snprintf(g_cfg.trace_classes, sizeof g_cfg.trace_classes, "*Campaign*;*Conquest*");
    snprintf(g_cfg.trace_exclude, sizeof g_cfg.trace_exclude,
             "*Portrait*;*Icon*;*Line*;*Panel*;*ResearchTree*;*Library*;*Button*");
    snprintf(g_cfg.dump_classes, sizeof g_cfg.dump_classes, "*Campaign*;*Conquest*;*Research*;*Region*;*Army*");
    g_cfg.log = 1;
    g_cfg.verbose = 1;
    g_cfg.trace = 1;
    g_cfg.file_hooks = 1;
    g_cfg.game_only = 1;
    g_cfg.trace_first = 10;
    g_cfg.trace_every = 500;
    g_cfg.max_slots = 4096;
    g_cfg.install_delay_ms = 5000;
    g_cfg.dump_bytes = 0xA40;
    g_cfg.dump_depth = 2;
    g_cfg.diff_min_ms = 100;
    g_cfg.max_log_mb = 32;

    _snwprintf(ini, MAX_PATH, L"%s\\" L"gem_battalions.ini", g_dir);
    if (GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES)
        return;
    g_cfg.log = GetPrivateProfileIntW(L"gem_battalions", L"log", g_cfg.log, ini);
    g_cfg.verbose = GetPrivateProfileIntW(L"gem_battalions", L"verbose", g_cfg.verbose, ini);
    g_cfg.trace = GetPrivateProfileIntW(L"trace", L"enabled", g_cfg.trace, ini);
    g_cfg.trace_first = GetPrivateProfileIntW(L"trace", L"first", g_cfg.trace_first, ini);
    g_cfg.trace_every = GetPrivateProfileIntW(L"trace", L"every", g_cfg.trace_every, ini);
    g_cfg.max_slots = GetPrivateProfileIntW(L"trace", L"max_slots", g_cfg.max_slots, ini);
    g_cfg.install_delay_ms = GetPrivateProfileIntW(L"trace", L"install_delay_ms", g_cfg.install_delay_ms, ini);
    g_cfg.dump_bytes = GetPrivateProfileIntW(L"dump", L"view_bytes", g_cfg.dump_bytes, ini);
    g_cfg.dump_depth = GetPrivateProfileIntW(L"dump", L"depth", g_cfg.dump_depth, ini);
    g_cfg.diff_min_ms = GetPrivateProfileIntW(L"dump", L"diff_min_ms", g_cfg.diff_min_ms, ini);
    g_cfg.file_hooks = GetPrivateProfileIntW(L"files", L"enabled", g_cfg.file_hooks, ini);
    g_cfg.game_only = GetPrivateProfileIntW(L"gem_battalions", L"game_only", g_cfg.game_only, ini);
    g_cfg.max_log_mb = GetPrivateProfileIntW(L"gem_battalions", L"max_log_mb", g_cfg.max_log_mb, ini);
    if (g_cfg.max_slots < 64)
        g_cfg.max_slots = 64;
    if (g_cfg.max_slots > 65536)
        g_cfg.max_slots = 65536;
    if (g_cfg.dump_bytes < 0x100)
        g_cfg.dump_bytes = 0x100;
    if (g_cfg.dump_bytes > 0x1000)
        g_cfg.dump_bytes = 0x1000;
    if (g_cfg.dump_depth > 3)
        g_cfg.dump_depth = 3;
}

/* GetPrivateProfileStringA with the ini path built from the wide dir */
static void read_ini_strings(void)
{
    char ini[MAX_PATH * 3];
    WideCharToMultiByte(CP_ACP, 0, g_dir, -1, ini, sizeof ini - 32, NULL, NULL);
    strcat(ini, "\\gem_battalions.ini");
    GetPrivateProfileStringA("trace", "classes", g_cfg.trace_classes, g_cfg.trace_classes,
                             sizeof g_cfg.trace_classes, ini);
    GetPrivateProfileStringA("trace", "exclude", g_cfg.trace_exclude, g_cfg.trace_exclude,
                             sizeof g_cfg.trace_exclude, ini);
    GetPrivateProfileStringA("dump", "classes", g_cfg.dump_classes, g_cfg.dump_classes, sizeof g_cfg.dump_classes,
                             ini);
}

static void open_log(void)
{
    wchar_t path[MAX_PATH];
    const wchar_t *suffix = g_exe.is_editor ? L"_editor" : (g_exe.is_server ? L"_server" : L"");
    if (!g_cfg.log)
        return;
    _snwprintf(path, MAX_PATH, L"%s\\gem_battalions%s.log", g_dir, suffix);
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_log_max = (LONG)g_cfg.max_log_mb * 1024 * 1024;
}

static void log_class_list(void)
{
    int i, matched = 0;
    gb_log("classes matching [trace] classes (excluded ones marked):");
    for (i = 0; i < g_ncls; i++) {
        ClassInfo *c = &g_cls[i];
        if (c->offset != 0)
            continue;
        if (!gb_list_match(c->name, g_cfg.trace_classes) && c->kind == K_NONE)
            continue;
        matched++;
        gb_log("  %-48s vtable exe+0x%-8llX %3u slots%s", c->name,
               (unsigned long long)((uint8_t *)c->vt - g_exe.base), c->nslots,
               gb_list_match(c->name, g_cfg.trace_exclude) && c->kind == K_NONE ? "  (excluded)" : "");
    }
    gb_log("  %d classes matched", matched);
}

static void check_known(void)
{
    char a[96];
    if (g_exe.timestamp != 0x6AB3DCC7)
        return;
    gb_log("exe is game 1.067 (6AB3DCC7): cross-check against ConquestUI notes");
    if (g_view_cls)
        gb_log("  eGUI_DynamicCampaignView vtable %s (expected exe+0x100EA50) %s",
               fmt_addr(a, sizeof a, g_view_cls->vt),
               (uint8_t *)g_view_cls->vt == g_exe.base + 0x100EA50 ? "OK" : "MISMATCH");
    if (g_scene_cls)
        gb_log("  Scene::eManager vtable %s (expected exe+0x1100548) %s", fmt_addr(a, sizeof a, g_scene_cls->vt),
               (uint8_t *)g_scene_cls->vt == g_exe.base + 0x1100548 ? "OK" : "MISMATCH");
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    if (g_cfg.install_delay_ms > 0)
        Sleep((DWORD)g_cfg.install_delay_ms);
    if (g_cfg.trace)
        install_tracer();
    if (g_cfg.file_hooks)
        install_file_hook();
    gb_log("ready: tracing %s, file hooks %s", g_cfg.trace ? "on" : "off", g_cfg.file_hooks ? "on" : "off");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t *bs;
        g_self = (HMODULE)inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_obj_cs);
        GetModuleFileNameW(inst, g_dir, MAX_PATH);
        bs = wcsrchr(g_dir, L'\\');
        if (bs)
            *bs = 0;
        if (!parse_exe())
            return TRUE;
        read_ini();
        read_ini_strings();
        open_log();
        gb_log("GEM Battalions " GB_VERSION " probe | exe %s | PE timestamp %08X | base %p | size 0x%X",
               g_exe.exe_name, g_exe.timestamp, (void *)g_exe.base, g_exe.size);
        if (g_cfg.game_only && (g_exe.is_editor || g_exe.is_server)) {
            gb_log("editor/server: no campaign here, DLL inert (game_only=1)");
            return TRUE;
        }
        rtti_scan();
        g_view_cls = find_class(NULL, K_VIEW);
        g_scene_cls = find_class(NULL, K_SCENE);
        gb_log("eGUI_DynamicCampaignView: %s | Scene::eManager: %s", g_view_cls ? "found" : "NOT FOUND",
               g_scene_cls ? "found" : "NOT FOUND");
        check_known();
        log_class_list();
        gb_log("hooks install in %d ms (after the other payload DLLs)", g_cfg.install_delay_ms);
        CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_nslots)
            trace_summary("process detach");
        gb_log("detach");
    }
    return TRUE;
}
