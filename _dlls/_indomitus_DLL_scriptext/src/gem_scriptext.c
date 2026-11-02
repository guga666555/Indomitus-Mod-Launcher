/*
 * GEM ScriptExt  -  Call to Arms: Gates of Hell (GEM engine, 1.067)
 *
 * Extends hard-coded engine features for mods. Every address is re-derived at startup from strings,
 * RTTI and byte patterns (nothing is hard-coded per exe); if a finder fails, that feature stays off and
 * the log says why. The same DLL works in the game, the editor and the dedicated server.
 *
 * Features (ini switches in gem_scriptext.ini, details in README.md):
 *   scripts   {if have_enemy}, {if health X}, {if player N|ally|neutral|enemy}, {if team N}, {if "have_enemy"},
 *             {on "move_order"},
 *             {on "selection_of_units"}, {on "talk_<name>"}, {health_damage_count ...},
 *             {multiplier X} in {health_damage_pierce|blast ...}
 *   diag      crash log
 *
 * Injected by the Indomitus launcher under any name, or installed as a dbghelp.dll proxy (twelve exports
 * forwarded to System32\dbghelp.dll, loaded only in that case).
 *
 * Layout (unity build: this file is the only translation unit, so build.bat and tests/ stay simple;
 * the order below is the dependency order):
 *   sx/core.c        logging, ini, PE / pattern / RTTI helpers
 *   sx/engine.c      shared engine bindings and their finders
 *   sx/patch.c       near memory, thread suspension, 5-byte inline hooks, call redirection
 *   sx/health.c      {if health}, {health_damage_count}, vitality access
 *   sx/multiplier.c  {multiplier X} in damage blocks
 *   sx/teams.c       {if player N}, {if team N}
 *   sx/conditions.c  added {if} conditions
 *   sx/modgate.c     require_mod (gates the named events)
 *   sx/events.c      named events from Entity::TalkEvent
 *   sx/crashlog.c    crash / process-exit logging
 *   sx/main.c        dbghelp forwarders, init sequence, DllMain
 *   sx/test_main.c   SX_TEST only: offline finder check against exe files (tests/run_offline_check.sh)
 *
 * Fauna (health row, squad strip, fauna squads) was removed in 0.8.0 to go into a separate DLL; the last
 * source with it is in _bckp/0.7.0_with_fauna.
 *
 * Build: build.bat (zig cc).  Offline check: sh tests/run_offline_check.sh <game> <editor> <server>
 */
#ifndef SX_TEST
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

#define SX_VERSION "0.9.1"
#define SX_NAME    "GEM ScriptExt"
#define SX_INI     L"\\gem_scriptext.ini"
#define SX_LOG     L"\\gem_scriptext.log"
#define SX_SECT    L"scriptext"

#include "sx/core.c"
#include "sx/engine.c"
#include "sx/patch.c"
#include "sx/health.c"
#include "sx/multiplier.c"
#include "sx/teams.c"
#include "sx/conditions.c"
#include "sx/modgate.c"
#include "sx/events.c"
#include "sx/crashlog.c"
#include "sx/main.c"
#include "sx/test_main.c"
