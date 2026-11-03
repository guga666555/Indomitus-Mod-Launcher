/*
 * GEM Fauna  -  Call to Arms: Gates of Hell (GEM engine, 1.067)
 *
 * Fauna = vehicle-class units whose def carries the fauna prop ({props "fauna_main"}). The engine treats
 * them as vehicles everywhere the HUD and squad code decide "human or not", so a fauna unit has no health
 * row, no squad strip item and a fauna squad has no leader. This DLL gives them the infantry behaviour:
 *   ui      health row in the vehicle selection panel (bar_health / txt_health / pnl_health from the
 *           mod's layout), squad strip items with hp bar and circle icon, mixed human + fauna squads
 *   squads  a real squad leader (box-select selects the leader, orders go to the squad), members take
 *           the leader's walk/run pace and keep their formation slot      [simulation: every MP peer]
 *   bandage double-clicking a bandage in a fauna inventory uses it: +15% of max health  [simulation; in MP the host
 *           uses it and sends the result with the engine's inventory-remove and medic-heal commands]
 *   diag    crash log, ui_debug tracing
 *
 * Every address is re-derived at startup from strings, RTTI and byte patterns (nothing is hard-coded
 * per exe); if a finder fails, that feature stays off and gem_fauna.log says why. The same DLL works in
 * the game, the editor and the dedicated server. Injected by the Indomitus launcher (no dbghelp proxy).
 * Split out of GEM ScriptExt 0.7.0 (ScriptExt 0.8.0 patches none of these sites: both load together).
 *
 * Layout (unity build: this file is the only translation unit; the order below is the dependency order):
 *   fx/core.c        logging, ini, PE / .pdata / pattern / RTTI helpers
 *   fx/engine.c      Tag ctor, condition tables, def props, vitality (health) getters
 *   fx/patch.c       thread-safe code patching: inline hooks, call redirection, vtable hooks (ScriptExt 0.8.1 rules)
 *   fx/fauna.c       "is this unit fauna", vitality access
 *   fx/sprint.c      fauna sprint (foot chassis mode commit)
 *   fx/squads.c      squad leaders (ElectLeader entry hook), box-select (whole squad / loose units), pace
 *   fx/follow.c      fauna followers keep their formation slot (vehicle follow handler re-targeted)
 *   fx/bandage.c     double-clicking a bandage in a fauna inventory heals it (UseItem operation wrapped)
 *   fx/ui.c          vehicle panel health row + squad strip, ui_debug selection trace
 *   fx/dcview.c      direct-control HUD rows (health / stamina) for fauna
 *   fx/headicon.c    over-head marker icon from a def prop "icon_<name>"
 *   fx/crashlog.c    crash / process-exit logging
 *   fx/main.c        init sequence, DllMain
 *   fx/test_main.c   FX_TEST only: offline finder check against exe files (tests/run_offline_check.sh)
 *
 * Build: build.bat (zig cc).  Offline check: sh tests/run_offline_check.sh <game> <editor> <server>
 */
#ifndef FX_TEST
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <stdlib.h>
#else
#include <stdlib.h>
#include <sys/mman.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#define FX_VERSION "0.10.0"
#define FX_NAME    "GEM Fauna"
#define FX_INI     L"\\gem_fauna.ini"
#define FX_LOG     L"\\gem_fauna.log"
#define FX_SECT    L"fauna"
#include "fx/core.c"
#include "fx/engine.c"
#include "fx/patch.c"
#include "fx/fauna.c"
#include "fx/squads.c"
#include "fx/sprint.c"
#include "fx/contact.c"
#include "fx/follow.c"
#include "fx/bandage.c"
#include "fx/ui.c"
#include "fx/dcview.c"
#include "fx/headicon.c"
#include "fx/crashlog.c"
#include "fx/main.c"
#include "fx/test_main.c"
