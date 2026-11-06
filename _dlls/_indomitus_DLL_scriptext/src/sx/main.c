/* GEM ScriptExt - startup
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * dbghelp.dll proxy forwarders (the real dbghelp is loaded only when this file IS dbghelp.dll), ini, the
 * finder/installer sequence (init), the watch thread (condition registration, require_mod) and DllMain.
 */

#ifndef SX_TEST
/* ------------------------------------------------------------------------------------ */
/* dbghelp forwarders                                                                   */
/* ------------------------------------------------------------------------------------ */
FARPROC gsx_SymInitialize, gsx_SymCleanup, gsx_SymFromAddr, gsx_SymGetLineFromAddr64,
        gsx_ImageNtHeader, gsx_SymSetOptions, gsx_SymGetOptions, gsx_SymLoadModule64,
        gsx_SymGetModuleBase64, gsx_SymFunctionTableAccess64, gsx_StackWalk64, gsx_MiniDumpWriteDump;

#define FWD(name) \
    __declspec(dllexport) __attribute__((naked)) void name(void) { \
        __asm__ volatile("jmp *gsx_" #name "(%rip)"); }

FWD(SymInitialize)
FWD(SymCleanup)
FWD(SymFromAddr)
FWD(SymGetLineFromAddr64)
FWD(ImageNtHeader)
FWD(SymSetOptions)
FWD(SymGetOptions)
FWD(SymLoadModule64)
FWD(SymGetModuleBase64)
FWD(SymFunctionTableAccess64)
FWD(StackWalk64)
FWD(MiniDumpWriteDump)

static HMODULE g_realDbghelp;

/* only when this DLL was loaded AS dbghelp.dll (the proxy install): the forwarders then need the real one. Injected by
   the launcher under its own name nothing calls them, so no LoadLibrary runs inside DllMain (loader lock). */
static void load_real_dbghelp(HMODULE self)
{
    wchar_t path[MAX_PATH], *nm;
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return;
    nm = wcsrchr(path, L'\\'); nm = nm ? nm + 1 : path;
    if (_wcsicmp(nm, L"dbghelp.dll")) return;
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n) return;
    wcscat(path, L"\\dbghelp.dll");
    g_realDbghelp = LoadLibraryW(path);
    if (!g_realDbghelp) return;
#define RES(name) gsx_##name = GetProcAddress(g_realDbghelp, #name)
    RES(SymInitialize); RES(SymCleanup); RES(SymFromAddr); RES(SymGetLineFromAddr64);
    RES(ImageNtHeader); RES(SymSetOptions); RES(SymGetOptions); RES(SymLoadModule64);
    RES(SymGetModuleBase64); RES(SymFunctionTableAccess64); RES(StackWalk64); RES(MiniDumpWriteDump);
#undef RES
}

/* ------------------------------------------------------------------------------------ */
/* init                                                                                 */
/* ------------------------------------------------------------------------------------ */
static int g_wantHaveEnemy = 1, g_wantEvents = 1, g_okCond, g_okEvents;

static DWORD WINAPI watch_thread(LPVOID arg)
{
    (void)arg;
    int gateState = -2, gated = g_requireMod[0] != 0, waitedLogged = 0, stable = 0, verify = 0;
    void *lastHead = NULL;
    for (;;) {
        if (gated) {
            int st = mod_enabled();
            if (st != gateState) { gateState = st; g_modActive = st > 0; logf_("Mod gate: \"%s\" is %s", g_requireMod, st > 0 ? "enabled - events on" : st == 0 ? "not enabled - events off" : "unreadable - events off"); }
        }
        if (g_okCond && !g_condInstalled) {                     /* registry built and its head unchanged for 3 polls */
            void *h = cond_registry_built() ? *(void *volatile *)g_condHead : NULL;
            stable = h && h == lastHead ? stable + 1 : 0; lastHead = h;
            if (stable >= 3) { install_conditions(); verify = 100; }
            else if (!waitedLogged++) logf_("{if ...}: waiting for the condition registry to initialise");
        } else if (verify > 0) verify = check_conditions() ? verify - 1 : 0;   /* ~2 s at 20 ms */
        if (!gated && (!g_okCond || (g_condInstalled && verify == 0))) return 0;   /* nothing left to watch */
        Sleep((g_okCond && (!g_condInstalled || verify > 0)) || (gated && gateState < 0) ? 20 : 2000);
    }
    return 0;
}

static void init(HMODULE self)
{
    g_self = self;
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;
    if (!ini_int("enabled", 1)) return;
    g_logfile = ini_int("log", 1); g_console = ini_int("console", 0); g_verbose = ini_int("verbose", 1);
    if (g_logfile) { wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, SX_LOG); g_log = _wfopen(lp, L"w"); }
    if (g_console) { AllocConsole(); SetConsoleTitleA(SX_NAME); freopen("CONOUT$", "w", stdout); }
    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(SX_NAME " " SX_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);
    if (ini_int("crash_log", 1)) {
        if (AddVectoredExceptionHandler(1, sx_veh)) logf_("crash log                      ON (fatal exceptions are written here)");
        install_exit_hooks();
    }

    g_wantHaveEnemy = ini_int("have_enemy", 1);
    g_wantEvents    = ini_int("events", 1);
    g_evMoveOrder   = ini_int("move_order_event", 1);
    g_evSelection   = ini_int("selection_event", 1);
    g_evTalk        = ini_int("talk_events", 1);
    g_evInMp        = ini_int("events_in_multiplayer", 1);
    ini_str("move_order_name", "move_order", g_evMoveName, sizeof g_evMoveName);
    ini_str("selection_name", "selection_of_units", g_evSelName, sizeof g_evSelName);
    ini_str("require_mod", "", g_requireMod, sizeof g_requireMod);
    int wantHealthIf = ini_int("health_condition", 1), wantHdc = ini_int("health_damage_ext", 1);
    int wantPlayerIf = ini_int("player_condition", 1), wantTeamIf = ini_int("team_condition", 1);

    int okMp = find_mp_core();
    if (g_wantHaveEnemy || wantHealthIf || wantPlayerIf || wantTeamIf) g_okCond = find_cond_registry();
    if (g_wantHaveEnemy) {
        if (!g_okCond || !find_sensor()) logf_("{if have_enemy}               OFF (engine code not recognised)");
        else {
            g_conds[COND_HAVE_ENEMY].enabled = 1;
            if (find_flag_cond() && install_flag_cond()) logf_("{if \"have_enemy\"}             ON (quoted spelling)");
            else logf_("{if \"have_enemy\"}             OFF (engine code not recognised) - use the unquoted form");
        }
    } else logf_("{if have_enemy}               off (ini)");

    /* health: {if health X} and {health_damage_count value max_health|set_health [cap]} (simulation state: every peer
       of a multiplayer match must run this DLL, like any script using the new words) */
    if (wantHealthIf || wantHdc) {
        g_okVit = find_vitality();
        int okRd = g_okVit && find_hdc();
        if (wantHdc) {
            g_okHdc = okRd && install_hdc();
            logf_("{health_damage_count v max_health|set_health [cap]} %s", g_okHdc ? "ON" : "OFF (engine code not recognised)");
        } else logf_("health_damage_count extension  off (ini)");
        if (wantHealthIf) {
            g_okHealthCond = g_okCond && okRd;
            g_conds[COND_HEALTH].enabled = g_okHealthCond;
            logf_("{if health <fraction>}         %s", g_okHealthCond ? "ON" : "OFF (engine code not recognised)");
        } else logf_("{if health}                    off (ini)");
    } else logf_("health extensions              off (ini)");

    /* {if player N} / {if team N}: read only (the owning player byte, the session's teams, the diplomacy matrix) */
    if (wantPlayerIf || wantTeamIf) {
        int okBase = g_okCond && find_reader() && find_diplomacy();
        if (wantPlayerIf) {
            g_conds[COND_PLAYER].enabled = okBase;
            int okEff = okBase && find_ctx_effector();
            logf_("{if player <number>}           %s", okBase ? "ON" : "OFF (engine code not recognised)");
            if (okBase) logf_("{if player ally|neutral|enemy} ON, compared with %s",
                              okEff && g_localPlayer ? "the effector, else the local player" :
                              okEff ? "the effector (local player not found: false without one)" :
                              g_localPlayer ? "the local player only (effector access not recognised)" : "nothing found - always false");
        } else logf_("{if player}                    off (ini)");
        if (wantTeamIf) {
            g_conds[COND_TEAM].enabled = okBase;
            InitializeCriticalSection(&g_teamLock);
            g_okMpTeams = okBase && !g_mpNever && find_mp_teams();
            logf_("{if team <number|name>}        %s", !okBase ? "OFF (engine code not recognised)" :
                  g_okMpTeams ? "ON (multiplayer: session team a = 1, b = 2...; elsewhere: alliances)" :
                  "ON (alliances only: no multiplayer session in this exe)");
        } else logf_("{if team}                      off (ini)");
    } else logf_("player / team conditions       off (ini)");

    /* {multiplier X} in {health_damage_pierce} / {health_damage_blast} (simulation state: every peer must run it) */
    if (ini_int("damage_multiplier", 1)) {
        g_okMult = find_mult() && install_mult();
        logf_("{multiplier X} (health_damage_pierce/blast) %s", g_okMult ? "ON" : "OFF (engine code not recognised)");
    } else logf_("{multiplier X}                 off (ini)");

    if (g_wantEvents) {
        if (!okMp && !g_evInMp) logf_("mp gate: cannot detect multiplayer - events will fire everywhere");
        g_okEvents = find_talk() & find_fire_named();
        if (g_okEvents && !install_events()) g_okEvents = 0;
        if (g_okEvents) {
            logf_("{on \"%s\"}               %s", g_evMoveName, g_evMoveOrder ? "ON" : "off (ini)");
            logf_("{on \"%s\"}       %s", g_evSelName, g_evSelection ? "ON" : "off (ini)");
            logf_("{on \"talk_<name>\"}            %s", g_evTalk ? "ON" : "off (ini)");
            logf_("events in multiplayer          %s", g_evInMp ? "ON - every player must run this DLL" : "off (ini: not fired while a network game runs)");
        } else logf_("events                         OFF (engine code not recognised)");
    } else logf_("events                         off (ini)");

    /* talk condition {seen enemy <prop|tag> N}: read only (sensor lists, def props, runtime tags) */
    if (ini_int("seen_tags", 1)) {
        if (!g_tagCtorCStr || !g_tagDtor) find_fire_named();
        int okSeen = g_tagCtorCStr && g_tagDtor && find_seen() && install_seen();
        logf_("{seen ... <prop|tag> N} (talk)  %s", okSeen ? "ON (other words = def prop or runtime tag)" : "OFF (engine code not recognised)");
    } else logf_("{seen} prop/tag words          off (ini)");

    if (g_requireMod[0]) {
        if (!find_mod_list()) { logf_("Mod gate: cannot read the mod list - events stay OFF (require_mod=%s)", g_requireMod); g_modActive = 0; g_requireMod[0] = 0; }
        else { g_modActive = 0; logf_("Mod gate: events wait for an enabled mod matching \"%s\"", g_requireMod); }
    }
    if (g_okCond || g_requireMod[0]) { HANDLE t = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        load_real_dbghelp(h);
        init(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf_("DLL detach (%s)", reserved ? "process is terminating" : "FreeLibrary");
    }
    return TRUE;
}
#endif /* SX_TEST */
