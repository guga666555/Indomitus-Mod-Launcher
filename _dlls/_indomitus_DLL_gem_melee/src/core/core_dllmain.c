/* gem_melee DllMain and installation order (spec 3.1). Linked only into the dll and gem_melee_itest.exe (it calls the modules). */
#include "core_int.h"
#include <stdio.h>
#include <wchar.h>

/* a [meleechase] key the way chase reads it (mc/core:31-36: GetPrivateProfileIntW on the wide ini path; review N1) */
static int chase_key(const wchar_t *key, int def) { return (int)GetPrivateProfileIntW(L"meleechase", key, def, core_ini_pathw()); }

/* dry: 0 = DllMain (the real attach, with the attach batch), 1 = the itest's install dry run (no loader lock, so no batch), 2 = a
 * dry run with the attach batch, for the start-up measurement only (gem_melee_itest --attach-time <exe> batch) */
void core_attach_all(HMODULE self, int dry)
{
    /* 1. paths, [Core] keys, sink, banner (a dll path too long for the rule of ms:3290 / pf:1940: nothing at all) */
    core_init(self);
    if (!core_init_ok()) return;
    /* 2. the core's clock finder */
    core_clock_setup();
    /* 3. crash log: MeleeChase's logger, so also MeleeChase's own conditions (exe gate and [meleechase] enabled, mc/main:21-24, ahead of
     *    mc/main:30-33); [meleechase] enabled=0 removes its VEH and exit hooks as it did in 0.5.4 */
    if (!dry && core_ini_int("Core", "CrashLog", 1) && chase_key(L"crash_log", 1) && chase_key(L"enabled", 1)
        && _wcsnicmp(core_exe_namew(), L"call_to_arms", 12) == 0) core_crash_install();
    /* 4. guard against the old dlls (3.4) */
    if (!dry) {
        core_guard_early();
        if (core_guard_skip[CORE_MOD_STANCE]) stance_set_guarded();     /* S19: our export passes registrations on to the old dll */
    }
    int sc = 0, ss = 0, sp = 0;
    const char *cs, *sts, *ps;
    /* 5-7: the module attaches inside the attach batch (2.2): under the loader lock no thread created now runs user code before DllMain
     *    returns, so one thread walk serves every code write of the three modules */
    int batch = !dry || dry == 2;
    if (batch) core_patch_batch_begin();
    /* 5. chase (its own exe gate, mc/main:21-23) */
    if (core_guard_skip[CORE_MOD_CHASE]) {
        cs = "OFF (gem_meleechase.dll already loaded)";
        core_log(CORE_MOD_CORE, "chase module off: gem_meleechase.dll is already loaded");
    } else if (_wcsnicmp(core_exe_namew(), L"call_to_arms", 12) != 0) {
        cs = "OFF (not a call_to_arms exe)";
        core_log(CORE_MOD_CORE, "chase module off: %s is not a call_to_arms exe", core_exe_name());
    } else { sc = chase_attach(); cs = sc ? "ON" : "OFF"; }
    /* 6. stance, 7. pen */
    if (core_guard_skip[CORE_MOD_STANCE]) sts = "OFF (gem_meleestance.dll already loaded)";
    else { ss = stance_attach(); sts = ss ? "ON" : "OFF"; }
    if (core_guard_skip[CORE_MOD_PEN]) ps = "OFF (gem_penfilter.dll already loaded)";
    else { sp = pen_attach(); ps = sp ? "ON" : "OFF"; }
    if (batch) core_patch_batch_end();
    /* 7b. clock bind (also dry): a module whose clock is not the core's keeps only its own new-battle rule. Chase is never bound. */
    if (ss) core_epoch_bind(CORE_MOD_STANCE, stance_clock_ptr());
    if (sp) core_epoch_bind(CORE_MOD_PEN, pen_clock_ptr());
    /* 8. link, 9. patch table + how the code writes were made */
    link_attach(sc, ss, sp);
    core_patch_report();
    core_patch_suspend_line();
    /* 10. summary, 11. background thread */
    char bg[64];
    int nbg = core_bg_count();
    if (dry) snprintf(bg, sizeof bg, "OFF (dry run, %d step%s registered)", nbg, nbg == 1 ? "" : "s");
    else if (nbg > 0 || core_late_guard_secs() > 0) snprintf(bg, sizeof bg, "ON (%d step%s + late guard %d s)", nbg, nbg == 1 ? "" : "s", core_late_guard_secs());
    else snprintf(bg, sizeof bg, "OFF (nothing to run)");
    core_log(CORE_MOD_CORE, "modules: chase %s, stance %s, pen %s | background thread %s", cs, sts, ps, bg);
    if (!dry) core_bg_start();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(inst); core_attach_all(inst, 0); }
    else if (reason == DLL_PROCESS_DETACH && !res) core_log(CORE_MOD_CORE, "DLL detach (FreeLibrary)");   /* mc/main:127-128 */
    return TRUE;                                                   /* always: the launcher kills the game if a payload dll is missing */
}
