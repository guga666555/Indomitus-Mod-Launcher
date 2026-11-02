/* GEM Fauna - startup
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * ini, the finder/installer sequence (init) and DllMain. No dbghelp proxy: the Indomitus launcher
 * injects the DLL by name (any name works).
 */

#ifndef FX_TEST
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
    if (g_logfile) { wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, FX_LOG); g_log = _wfopen(lp, L"w"); }
    if (g_console) { AllocConsole(); SetConsoleTitleA(FX_NAME); freopen("CONOUT$", "w", stdout); }
    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(FX_NAME " " FX_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);
    if (ini_int("crash_log", 1)) {
        if (AddVectoredExceptionHandler(1, fx_veh)) logf_("crash log                      ON (fatal exceptions are written here)");
        install_exit_hooks();
    }

    int wantUi = ini_int("fauna_ui", 1);
    ini_str("fauna_prop", "fauna_main", g_faunaProp, sizeof g_faunaProp);
    ini_str("hp_bar_name", "bar_health", g_barName, sizeof g_barName);
    ini_str("hp_text_name", "txt_health", g_txtName, sizeof g_txtName);
    ini_str("hp_panel_name", "pnl_health", g_pnlName, sizeof g_pnlName);
    ini_str("stamina_bar_name", "bar_stamina", g_stBarName, sizeof g_stBarName);
    ini_str("stamina_text_name", "txt_stamina", g_stTxtName, sizeof g_stTxtName);
    ini_str("stamina_panel_name", "pnl_stamina", g_stPnlName, sizeof g_stPnlName);
    ini_str("fauna_default_icon", "soldier", g_faunaIconDefault, sizeof g_faunaIconDefault);
    g_wantSqSel = ini_int("fauna_squad_select", 1);
    g_wantElect = ini_int("fauna_squad_elect", 1);
    g_wantPace  = ini_int("fauna_squad_pace", 1);
    g_wantFormation = ini_int("fauna_squad_formation", 1);
    g_formationRadiusCm = ini_int("fauna_formation_radius_cm", 150);
    if (g_formationRadiusCm < 50) g_formationRadiusCm = 50; if (g_formationRadiusCm > 1500) g_formationRadiusCm = 1500;
    g_uiDebug   = ini_int("ui_debug", 0);
    g_wantSprint = ini_int("fauna_double_click_sprint", 1);
    g_wantSqvIcon = ini_int("fauna_head_icon", 1);
    g_wantFaunaSprint = ini_int("fauna_sprint", 1);
    g_sprintDebug = ini_int("fauna_sprint_debug", 0);
    g_wantDcView = ini_int("fauna_dc_hud", 1);
    ini_str("dc_health_icon", g_dcHpIcon, g_dcHpIcon, sizeof g_dcHpIcon);
    ini_str("dc_stamina_icon", g_dcStIcon, g_dcStIcon, sizeof g_dcStIcon);
    ini_str("dc_vehicle_icon_1", g_dcFuelIcon, g_dcFuelIcon, sizeof g_dcFuelIcon);
    ini_str("dc_vehicle_icon_2", g_dcSpeedIcon, g_dcSpeedIcon, sizeof g_dcSpeedIcon);
    int wantBandage = ini_int("fauna_bandage", 1);
    g_bandagePct = ini_int("fauna_bandage_heal_percent", 15);
    if (g_bandagePct < 1) g_bandagePct = 1; if (g_bandagePct > 100) g_bandagePct = 100;

    g_okFauna = find_tag_ctor() && find_prop_cond();
    if (!g_okFauna) { logf_("fauna                          OFF (engine code not recognised)"); return; }
    g_tagCtorCStr(g_tagFaunaBuf, g_faunaProp);
    logf_("fauna: tag \"%s\" id %016llX", g_faunaProp, (unsigned long long)TAG_ID(g_tagFaunaBuf));
    logf_("fauna                          ON (prop \"%s\")", g_faunaProp);

    /* simulation: game, editor and server alike - every multiplayer peer must run this DLL */
    if (g_wantElect) {
        g_okElect = find_squad_select() && find_squad_elect() && install_squad_elect();
        logf_("fauna squads get a leader       %s", g_okElect ? "ON (like infantry squads)" : "OFF (engine code not recognised)");
        if (g_okElect && g_wantFormation) {
            g_okFormation = find_follow() && install_follow();
            if (g_okFormation) logf_("fauna squad members keep formation ON (slot radius %d cm)", g_formationRadiusCm);
            else logf_("fauna squad members keep formation OFF (engine code not recognised)");
        } else if (g_okElect) logf_("fauna squad formation          off (ini)");
    } else logf_("fauna squads get a leader       off (ini)");

    /* walk/run (simulation): squad pace needs the squad offsets above; the double-click sprint does not */
    if (!g_okElect) g_wantPace = 0;
    if (g_wantPace || g_wantSprint || g_wantFaunaSprint) {
        g_okPace = find_squad_pace() && redirect_call(g_mcSite, (void *)fx_move_change, NULL, "squad pace (order)")
                                     && redirect_call(g_giveSite, (void *)fx_move_give, NULL, "squad pace (give)");
        if (!g_okPace) logf_("fauna walk/run (squad pace, double-click sprint) OFF (engine code not recognised)");
        else {
            logf_("fauna squad members match the leader's pace %s", g_wantPace ? "ON (walk/run)" : "off (ini / no squad leaders)");
            logf_("fauna double-click run sprints  %s", !g_wantSprint ? "off (ini)" : g_sprintMode >= 0 ? "ON" : "OFF (mode table not recognised)");
        }
    } else logf_("fauna walk/run (squad pace, double-click sprint) off (ini)");

    /* sprint (simulation): the foot chassis keeps a fauna's sprint request - double-click run, Shift in direct control */
    if (!g_wantFaunaSprint) logf_("fauna sprint                   off (ini)");
    else {
        g_okFaunaSprint = find_fauna_sprint() && install_fauna_sprint();
        if (!g_okFaunaSprint) logf_("fauna sprint                   OFF (engine code not recognised)");
        else logf_("fauna sprint                   ON (Shift in direct control%s; stamina limits it)%s",
                   g_okPace ? ", double-click run" : "; double-click run OFF: move order hook missing", g_sprintDebug ? " - sprint debug log on" : "");
    }

    g_okVit = find_vitality();

    /* simulation, single player and editor: a bandage double-clicked in a fauna inventory heals it */
    if (!wantBandage) logf_("fauna bandages                 off (ini)");
    else if (!g_okVit) logf_("fauna bandages                 OFF (health getters not found)");
    else {
        g_okBandage = find_bandage() && install_bandage();
        if (g_okBandage) logf_("fauna bandages                 ON (double-click a bandage in the inventory: +%d%% of max health)", g_bandagePct);
        else logf_("fauna bandages                 OFF (engine code not recognised)");
    }

    /* local: HUD (game and editor; the server has no GUI) */
    if (!wantUi) logf_("fauna UI                       off (ini)");
    else if (!g_okVit) logf_("fauna UI                       OFF (health getters not found)");
    else {
        g_okUi = find_ui() && install_ui();
        if (g_okUi && (ini_int("fauna_stamina_bar", 1) || g_wantDcView)) find_stamina_fields();
        logf_("fauna UI (hp bar, squad strip) %s", g_okUi ? "ON" : "OFF (engine code not recognised / no GUI in this exe)");
        if (g_okUi && g_wantDcView) {
            g_okDcView = find_dcview() && install_dcview();
            logf_("fauna direct-control HUD        %s", g_okDcView ? "ON (health + stamina instead of fuel + speed)" : "OFF (engine code not recognised)");
        } else if (g_okUi) logf_("fauna direct-control HUD        off (ini)");
        if (g_okUi && g_wantSqvIcon) {
            g_okSqvIcon = find_sqview() && install_sqview();
            logf_("fauna over-head icon prop        %s", g_okSqvIcon ? "ON (def prop squad_view_<file> / squad_view_leader_<file>)" : "OFF (engine code not recognised)");
        } else if (g_okUi) logf_("fauna over-head icon prop        off (ini)");
        if (g_okUi && g_wantSqSel) {
            g_okSqSel = (g_sqSelSite || find_squad_select()) && install_squad_select();
            logf_("fauna squads select their leader %s", g_okSqSel ? "ON (box select, like human squads)" : "OFF (engine code not recognised)");
        } else if (g_okUi) logf_("fauna squads select their leader off (ini)");
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        init(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        logf_("DLL detach (%s)", reserved ? "process is terminating" : "FreeLibrary");
    }
    return TRUE;
}
#endif /* FX_TEST */
