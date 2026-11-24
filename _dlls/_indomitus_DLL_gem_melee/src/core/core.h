/* gem_melee core API. Included by every TU of the dll and its tests (module ports include it after their system headers).
 * Every non-static symbol outside the three module ports starts with core_, link_ or test_; macros start with CORE_ or GEM_MELEE_.
 * No type, macro or function here may use a name the three snapshots use (W, PATCH, MATCH, R, VTP, MAXT, TLEN, OFF, D, GI, Sec,
 * Chase, Track, sites_t, img_t, link_holder, ...). */
#ifndef GEM_MELEE_CORE_H
#define GEM_MELEE_CORE_H
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#define GEM_MELEE_NAME     "GEM Melee"
#define GEM_MELEE_VERSION  "0.1.20"
#define GEM_MELEE_DLL      "gem_melee.dll"
#define GEM_MELEE_MODULES  "stance 0.7.42, chase 0.5.4, pen 0.1.1"

enum { CORE_MOD_CORE = 0, CORE_MOD_CHASE = 1, CORE_MOD_STANCE = 2, CORE_MOD_PEN = 3, CORE_MOD_LINK = 4, CORE_MOD_N = 5 };

/* ---- log sink: one file for the whole dll ---- */
void core_log(int mod, const char *fmt, ...);
void core_vlog(int mod, const char *fmt, va_list ap);
enum { CORE_SINK_FILE = 0, CORE_SINK_STDOUT = 1, CORE_SINK_DISCARD = 2 };
void core_test_sink(int mode);                       /* tests: route every line (default before core_attach_all: DISCARD) */
void core_test_sink_file(const char *log_path);      /* tests: FILE mode on this path */

/* ---- paths, exe image ---- */
const char    *core_ini_path(void);                  /* "<dll dir>\gem_melee.ini" (file name derived from the dll's own name) */
const wchar_t *core_ini_pathw(void);
uint8_t       *core_exe_base(void);                  /* GetModuleHandleW(NULL), or the test override */
const char    *core_exe_name(void);                  /* file name only, e.g. "call_to_arms_ed.exe" */
const wchar_t *core_exe_namew(void);
uint32_t       core_exe_timestamp(void);
void core_test_set_exe(uint8_t *base, const char *exe_file_name);   /* itest dry run: an exe mapped by the test */
void core_test_set_ini(const char *ini_path);

/* ---- ini: GetPrivateProfile*A on core_ini_path() ---- */
int  core_ini_int(const char *section, const char *key, int def);
void core_ini_str(const char *section, const char *key, const char *def, char *out, int cap);

/* ---- simulation clock (the engine's brain-timer clock, ed exe+0x13DF988) ---- */
volatile uint32_t *core_clock_ptr(void);             /* NULL when the core finder failed */
uint32_t core_now(void);                             /* *clock, 0 without one */
void core_test_set_clock(volatile uint32_t *clock);

/* ---- new battle (2.2): one detector on the CORE clock; stance and pen OR it into their own rule, chase does not ---- */
uint32_t core_epoch_poll(int mod);                   /* reads the core clock itself (never the caller's); returns the epoch counter the
                                                        module acts on: 0 while frozen or without a core clock, the value frozen at
                                                        core_epoch_bind time for a detached module, else the live counter (0 at start) */
uint32_t core_epoch(void);
void core_epoch_bind(int mod, const volatile void *module_clock);  /* attached only when module_clock == core_clock_ptr(); else
                                                        detached + one WARNING line; nothing when the core has no clock */
int  core_epoch_listen(void (*fn)(uint32_t epoch, uint32_t now));   /* max 4; called by the detecting thread, in registration order */
void core_on_quant(void);                            /* stance frame_hook, first thing each quant: core_epoch_poll(CORE_MOD_CORE) */
void core_test_freeze_epoch(int on);                 /* module self-tests: poll returns 0, never fires */

/* ---- patch registry: every engine write of the dll goes through these ---- */
enum { CORE_PATCH_CODE = 1, CORE_PATCH_DATA = 2, CORE_PATCH_PTR = 3 };
enum { CORE_PATCH_WRITTEN = 1, CORE_PATCH_REFUSED_OVERLAP = 2, CORE_PATCH_REFUSED_BYTES = 3,
       CORE_PATCH_REFUSED_FULL = 4, CORE_PATCH_FAILED = 5 };
#define CORE_PATCH_MAXLEN 32
#define CORE_PATCH_MAX    64
typedef struct {
    int mod, kind, state; const char *name; uint8_t *addr; uint32_t len;
    uint8_t orig[CORE_PATCH_MAXLEN], now[CORE_PATCH_MAXLEN];
} core_patch_t;
/* the three writers return 1 when the write happened (state WRITTEN), else 0 (never a state code: every caller tests truthiness);
 * the state is read back with core_patch_get */
int  core_patch_code(int mod, const char *name, void *addr, size_t len, const void *expected, const void *bytes);
int  core_patch_data(int mod, const char *name, void *addr, size_t len, const void *expected, const void *bytes);
int  core_patch_ptr (int mod, const char *name, void **slot, const void *expected, const void *value, void **old);
int  core_patch_count(void);
int  core_patch_get(int i, core_patch_t *out);
void core_patch_report(void);

/* ---- one background thread (wall clock; log / interop only, never a simulation decision) ---- */
typedef void (*core_bg_fn)(void);
int  core_bg_register(int mod, core_bg_fn fn);       /* max 8; each called every ~100 ms, in registration order */

/* ---- engine weapon cease-fire (R3): ed exe+0x699CC0 = chase ClearTarget = pen weapon stop ---- */
int  core_ceasefire(int mod, void *weapon, void *engine_stop, uint64_t *ret);
void core_ceasefire_stats(long calls[CORE_MOD_N], long vetoed[CORE_MOD_N], long *collisions);

/* ---- cost accounting (R8) ---- */
enum { CORE_T_PEN_A = 0, CORE_T_N = 4 };
extern volatile int core_timing_on;                  /* = [Link] CostLine && [MeleeStance] HookTiming; CostLine ships 0 */
void core_time_add(int id, int64_t qpc_ticks);       /* InterlockedAdd64 + InterlockedIncrement (the core thread reads them) */
void core_time_read(int id, int64_t *qpc_ticks, long *calls);
static inline int64_t core_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
#define CORE_TIME_BEGIN(v)   int64_t v = core_timing_on ? core_qpc() : 0
#define CORE_TIME_END(id, v) do { if (v) core_time_add((id), core_qpc() - (v)); } while (0)

/* ---- shared unit answers (read once per entity per sim clock value; link only in 0.1.0) ---- */
typedef struct {
    const void *e; uint32_t clock;
    uint8_t human, actor, vehicle, owner, player, dc_sim, linked, traced;
    uint32_t id;
} core_unit_t;
int core_unit(const void *entity, core_unit_t *out);         /* 0 = entity NULL */
int core_vec_has_ci(const void *tag_vector, const char *word); /* stance vec_has (ms:123-137) for one word: 1 / 0 / -1 broken */
int core_vec_names(const void *tag_vector, char *out, int cap); /* stance vec_str (ms:140-153) + str_get (ms:104-111), exact port:
                                                        "name1 name2 ..." copied into out (cap >= 1; "" for NULL / broken); returns
                                                        the number of names copied. Every engine string link reads goes through it */

/* ---- integration hooks (R4 flip detector, R5 trace); link_attach fills them, NULL = off ---- */
enum { CORE_PICK_PASS = 0, CORE_PICK_REJECT = 1, CORE_PICK_MELEE = 2,      /* = stance enum ms:196 */
       CORE_PICK_DOWNED = 0x10, CORE_PICK_FAST = 0x20, CORE_PICK_NOMELEE = 0x100 };
enum { CORE_PEN_ALLOW = 1, CORE_PEN_REJECT = 2, CORE_CHASE_HOLD = 1 };
extern volatile int core_flip_on, core_trace_on;
/* R4 suspects: a unit with 2 front-order changes inside [Link] FlipWindowMs (note_front marks it, link's new-battle listener clears
 * the array). Pick / verdict notes reach link only for suspects: a pick by any other unit costs one slot load + pointer compare and
 * one load of the test switch, no call. */
typedef struct { const void *unit; uint32_t id, until; } core_suspect_t;   /* id = entity +0xC0, until = sim ms */
#define CORE_SUSPECT_N 1024
extern core_suspect_t core_suspect[CORE_SUSPECT_N];   /* written by link only (engine thread), zero until then */
extern volatile int core_suspect_all;                /* link_test_force only; 0 in every real run */
static inline uint32_t core_suspect_slot(const void *u) { return ((uint32_t)((uintptr_t)u >> 4) * 2654435761u) >> 22; }
#define CORE_IS_SUSPECT(u) ((u) && (core_suspect[core_suspect_slot(u)].unit == (const void *)(u) || core_suspect_all))
typedef struct {
    void (*note_front)(const void *unit, const void *order_vt, const char *name, uint32_t now);
    void (*note_pick)(int mod, const uint8_t *ctx, int code, uint32_t now);
    void (*note_verdict)(int mod, const void *unit, const void *target, int code, uint32_t now);
    void (*trace_pick)(int mod, const uint8_t *ctx, int code, uint32_t now);
    int  (*trace_unit)(const void *unit);
    void (*trace_vline)(int mod, const void *unit, const char *fmt, va_list ap);
    int  (*ceasefire_veto)(int mod, void *weapon);
} core_link_hooks_t;
void core_set_link_hooks(const core_link_hooks_t *h);
void core_note_front(const void *unit, const void *order_vt, const char *name, uint32_t now);
void core_note_pick(int mod, const uint8_t *ctx, int code);
void core_note_verdict(int mod, const void *unit, const void *target, int code, uint32_t now);
void core_trace_pick(int mod, const uint8_t *ctx, int code);
int  core_trace_unit(const void *unit);
void core_trace_line(int mod, const void *unit, const char *fmt, ...);
#define CORE_NOTE_FRONT(u, vt, nm, now)      do { if (core_flip_on) core_note_front((u), (vt), (nm), (now)); } while (0)
#define CORE_NOTE_PICK(m, ctx, code)         do { if (core_flip_on && ((code) & 0xFF) && CORE_IS_SUSPECT(*(void *const *)((ctx) + 8))) \
                                                  core_note_pick((m), (ctx), (code)); } while (0)
#define CORE_NOTE_VERDICT(m, u, t, code, now) do { if (core_flip_on && (code) && CORE_IS_SUSPECT(u)) \
                                                  core_note_verdict((m), (u), (t), (code), (now)); } while (0)
#define CORE_TRACE_PICK(m, ctx, code)        do { if (core_trace_on) core_trace_pick((m), (ctx), (code)); } while (0)
#define CORE_TRACE(m, u, ...)                do { if (core_trace_on && core_trace_unit(u)) core_trace_line((m), (u), __VA_ARGS__); } while (0)

/* ---- attach (core_dllmain.c) ---- */
void core_attach_all(HMODULE self, int dry);

/* ---- module entry points and APIs (implemented in the module TUs; section 4) ---- */
int  stance_attach(void);
void stance_cost_read(int64_t *hook_qpc, int64_t *orig_qpc, long *timed, int64_t *poll_qpc);
void *stance_clock_ptr(void);
uint32_t stance_name_vec_off(void);                  /* cfg_namtags + TAGS_VEC (0xB80): ammo name tag vector */
uint32_t stance_enttags_off(void);                   /* cfg_enttags (0x4E8): entity tag vector */
void stance_set_guarded(void);                       /* core guard: an old gem_meleestance.dll runs instead (S19) */
uint32_t stance_epoch_probe(uint32_t now);           /* tests: runs log_epoch(now), returns stance's flush count */
int  stance_order_poll_state(void);                  /* S21, read-only: 1 = order_poll can run; 0 = [AutoDraw] OrderPoll=0, -1 = auto-draw off
                                                        ([AutoDraw] Enabled=0 or the charge finders failed), -2 = no eAttack / eAttackBasic vtable */
int  chase_attach(void);
int  chase_layout(uint32_t *weap_item, uint32_t *tag_beg, uint32_t *tag_end);
void *chase_clock_ptr(void);
void *chase_ceasefire_fn(void);
uint32_t chase_enttags_off(void);
int  chase_status(void);
void chase_test_hull_fns(void **sel, void **evr);    /* gem_melee 0.1.14: test-only view of the two hull-melee wrappers */
void chase_test_rwt_fn(void **rwt);                  /* gem_melee 0.1.16: the range(weapon, target) wrapper */
void chase_test_ast_fns(void **ast, void **blk);     /* gem_melee 0.1.17: the aim status / fire line wrappers */
void chase_test_spt_fn(void **spt);                  /* gem_melee 0.1.18: the shooter point test wrapper */
int  pen_attach(void);
void *pen_clock_ptr(void);
void *pen_stop_fn(void);
void *pen_matchammo_fn(void);                        /* S.ma_fn (pf:1718): I-R2 */
int  pen_status(void);
/* gem_melee 0.1.10: 2D distance from `from` to the nearest volume centre of `target`, that centre in pt (pen's volume frames);
   NULL until the pen module attached, -1 = not known. Entity memory only, no lock: stance (veh_shift) and chase (tgt_dist) call it. */
extern float (*core_hull_point)(const uint8_t *from, const uint8_t *target, float *pt);
uint32_t pen_epoch_probe(uint32_t now);              /* tests: epoch_check(now) under g_lock, returns ST.flushes */
int  link_attach(int chase_on, int stance_on, int pen_on);
int  link_flip_count(void);
int  link_word_check(const char *ini_path, char *out, int cap);
/* link test entry points (compiled in the dll too; never called there) */
void link_test_force(int suspect_all, int trace_all);   /* every unit is a suspect / traced (the tag read still runs) */
void link_test_counts(long *notes, long *traces);
int  link_test_replay(const char *stance_log, char *units_out, int cap);   /* 8.4 --replay: flip blocks found */
/* self-test entry points (module TUs built with their test define + -Dmain=...) */
int stance_test_main(int argc, char **argv);
int pen_test_main(int argc, char **argv);
int test_chase_main(int argc, char **argv);
int test_core_main(int argc, char **argv);
int test_linked_main(int argc, char **argv);         /* src\test\test_linked.c (WP4) */
#endif
