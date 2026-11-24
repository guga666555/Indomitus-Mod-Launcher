/* gem_melee private core declarations (spec 2.4). Included by src\core\*.c and src\test\test_core.c only. */
#ifndef GEM_MELEE_CORE_INT_H
#define GEM_MELEE_CORE_INT_H
#include "core.h"
void    core_init(HMODULE self);                     /* 3.1 step 1 */
void    core_crash_install(void);                    /* 4.4 */
HMODULE core_self(void);                             /* the dll's own HMODULE (core_init), NULL in tests */
void    core_text_range(uint8_t **lo, uint8_t **hi); /* exe .text of core_exe_base() */
int     core_in_text(const void *a);                 /* lo <= a < hi */
void    core_log_fixed(int mod, const char *fmt, ...);   /* crash-path writer (2.2 Sink) */
extern int core_guard_skip[CORE_MOD_N];              /* 3.4 early guard result per module */

/* WP0 helpers behind 2.4 (private: core_dllmain.c and test_core.c use them; no module or link code may) */
int     core_init_ok(void);                          /* core_init had a usable dll path (else core_attach_all does nothing) */
const char *core_mod_name(int mod);                  /* "core" "chase" "stance" "pen" "link" */
void    core_rva_str(const volatile void *p, char *out, size_t cap);   /* "exe+0x.." inside core_exe_base()'s image, else "0x.."/"NULL" */
volatile uint32_t *core_clock_find(const uint8_t *base, int *hits);    /* the core clock finder (stance's, ms:2601-2614) on any mapped
                                                        PE image; NULL = fewer than 5 getter calls; *hits = calls of the winner */
void    core_clock_setup(void);                      /* 3.1 step 2: finder on core_exe_base(), sets the core clock, logs */
int     core_patch_suspend_on(void);                 /* [Core] PatchSuspendThreads (default 1) */
/* attach batch (review INST-1, spec 2.2 step 4): between begin and end, the code writes of the calling thread share ONE thread walk
 * (taken at the first one that suspends); end closes the handles it kept. Only core_attach_all's real attach (DllMain, loader lock)
 * opens it, and the itest's start-up measurement (dry 2) */
void    core_patch_batch_begin(void);
void    core_patch_batch_end(void);
typedef struct {
    long walks;                                      /* Toolhelp thread snapshots taken (every attempt of suspend_others + batch walks) */
    long susp, failed, batched;                      /* code writes done with the other threads suspended / FAILED for it / of both, through a batch */
    long plain, exited, batches;                     /* code writes with PatchSuspendThreads=0 / batch threads seen exited / batches opened */
    int  others, open;                               /* most other threads one walk found / batch handles open now */
    int64_t qpc;                                     /* QPC ticks spent suspending + resuming (the write itself included) */
} core_susp_stats_t;
void    core_patch_suspend_stats(core_susp_stats_t *out);   /* counters since process start */
void    core_patch_suspend_line(void);               /* core_attach_all: the "code writes: ..." line after core_patch_report */
int     core_late_guard_secs(void);                  /* [Core] LateGuardSeconds (default 60) */
void    core_guard_early(void);                      /* 3.4 early guard: fills core_guard_skip[], one line per old dll */
int     core_bg_count(void);                         /* registered background steps */
void    core_bg_run_once(void);                      /* one pass of the core thread: the steps in order, then the late guard */
int     core_bg_start(void);                         /* 3.1 step 11: 1 = thread started (or already running) */
#endif
