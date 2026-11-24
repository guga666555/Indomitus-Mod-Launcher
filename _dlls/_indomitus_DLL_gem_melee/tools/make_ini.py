"""Build / check src\\gem_melee.ini (gem_melee spec section 5.1, WP4).

usage:
  python tools\\make_ini.py            write src\\gem_melee.ini (CRLF, ASCII)
  python tools\\make_ini.py --check    verify the written file, print "INI OK (n keys)" or the problems (exit 1)

The ini = a header + [Core] + [Link], then the three dlls' own inis, unchanged apart from the values of section 5.1, the keys of table ADDED
(keys gem_melee adds to a module section) and added note lines that start with "; gem_melee": inputs\\meleestance_0.7.42\\src\\gem_meleestance.ini (every section and comment),
inputs\\meleechase_0.5.4\\gem_meleechase.ini, inputs\\penfilter_0.1.1\\gem_penfilter.ini. The inputs are only read.

--check reads the file with GetPrivateProfile rules (first section / first key wins, names case-insensitive, value = the rest
of the line after '=' trimmed, one pair of surrounding quotes removed) and, on Windows, again through the real
kernel32 GetPrivateProfileStringA, and verifies:
  (a) every key of the 5.1 table has exactly its value;
  (b) every key the three snapshots' code reads is present (80 stance + 28 chase + 19 pen; pen's two warn-only keys
      HookApproach / BlockStreak are not in the 19: HookApproach is in the table, BlockStreak must stay absent or -1);
  (c) no section name and no key inside a section appears twice;
  (d) CRLF line ends only, ASCII only, no BOM, ends with CRLF;
  (e) every literal core_ini_int / core_ini_str / chase_key key in gem_melee's own code (src\\core, src\\link) is present;
  (f) the file is what this script writes now (rerun it after editing the script);
  (g) no key outside the 5.1 table (a misspelt key would silently fall back to the code default).
"""
import os
import re
import sys

GM = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INP = os.path.join(GM, "inputs")
SRC_STANCE_INI = os.path.join(INP, "meleestance_0.7.42", "src", "gem_meleestance.ini")
SRC_CHASE_INI = os.path.join(INP, "meleechase_0.5.4", "gem_meleechase.ini")
SRC_PEN_INI = os.path.join(INP, "penfilter_0.1.1", "gem_penfilter.ini")
CODE_STANCE = os.path.join(INP, "meleestance_0.7.42", "src", "gem_meleestance.c")
CODE_CHASE = os.path.join(INP, "meleechase_0.5.4", "src", "mc", "main.c")
CODE_PEN = os.path.join(INP, "penfilter_0.1.1", "gem_penfilter.c")
OUT = os.path.join(GM, "src", "gem_melee.ini")

AIM_GUNS = "+gun,+gun1,+gun2,+gun3,+gun4,+gun5,+gun6,+gun7"    # gem_melee 0.1.9: +tag = offered by the dll though no_target
AIM_PRIORITY = ("body,cabin,bodyback;turret,mantlet;aux_turret1,aux_turret2,aux_turret3,aux_turret4,aux_turret5,aux_turret6,"
                "aux_turret7,aux_turret8,aux_turret9;arm_left,arm_right,gun_left,gun_right,hkm_left,hkm_right;leg_left,leg_right,"
                "frontleftleg,frontrightleg,midleftleg,midrightleg,backleftleg,backrightleg;" + AIM_GUNS + ";mover,track,trackleft,trackright,wheel")
AIM_PRIORITY_011 = AIM_PRIORITY.replace(AIM_GUNS + ";", "")      # the value of the 0.1.1 module ini

# spec 5.1: final values (values Alex runs today win; 0.7.42's new key keeps its 0.7.42 default; orchestrator decisions
# Q1 AutoMeleeTag=melee_infantry,melee_auto (Alex, Nov 21 2026), Q2 ProbeAnyRange=0 and CooldownMs=2000). Order inside a section = the file's order.
FINAL = {
    "Core": [("Log", "1"), ("CrashLog", "1"), ("NewBattleBackMs", "1000"), ("PatchSuspendThreads", "1"), ("LateGuardSeconds", "60")],
    "Link": [("Flip", "1"), ("FlipChanges", "5"), ("FlipWindowMs", "3000"), ("FlipLines", "20"), ("Trace", "0"),
             ("TraceTag", "ai_trace"), ("TraceLines", "2000"), ("CeasefireVeto", "1"), ("CostLine", "0")],
    "MeleeStance": [("Enabled", "1"), ("Log", "1"), ("SkipLinked", "1"), ("DrawFromBelt", "1"), ("SkipDirectControl", "1"),
                    ("SkipTags", "tp_jetpack,hidden,melee_disabled,disable_melee,valkyrie,sensor_off"),
                    ("IgnoreTargetTags", "hidden,melee_disabled,valkyrie,sensor_off"), ("DiagManual", "0"), ("HookTiming", "1"),
                    ("FastPass", "1"), ("ClientGate", "1"), ("Tag", "melee_stance"), ("AlwaysTag", "melee_always"),
                    ("AutoMeleeTag", "melee_infantry,melee_auto"),
                    ("MeleeAmmoTags", "melee_light,melee_heavy,medium_inf_melee_target,medium_veh_melee_target,heavy_veh_melee_target"),
                    ("DrawOnToggle", "1"), ("DrawCooldownMs", "3000"), ("DrawButton", "melee_toggle")],
    "Charge": [("Enabled", "1"), ("Tag", "has_melee_weapon"), ("MeleeInHand", "1"), ("PlayerUnits", "1"), ("CooldownMs", "2000"),
               ("RetargetMs", "1500"), ("RefreshMs", "400"), ("SwitchRatio", "0.6"), ("ProgressUnits", "10"), ("CloseDistance", "200"),
               ("RunMode", "2"), ("MoveRefreshUnits", "40"), ("MoveRefreshFraction", "0.4"), ("MoveRefreshMaxUnits", "200"), ("Lead", "1"), ("LeadSeconds", "3"),
               ("LeadKeep", "0.5"), ("LeadRunSpeed", "120"), ("LeadMaxFraction", "0.5"), ("Smooth", "1"), ("SmoothMs", "600"),
               ("SmoothFraction", "0.1"), ("ReplaceOwn", "1"), ("MaxDistance", "2000"),
               ("PlayerMaxDistance", "1000"), ("MinDistance", "60")],
    "Zerg": [("Enabled", "1"), ("Distance", "1600"), ("ReorderMs", "700"), ("Tag", "melee_infantry")],
    "MultiSelect": [("Enabled", "1")],
    "ShieldMelee": [("Enabled", "0"), ("SeeThroughTag", "forcefield")],
    "Downed": [("Enabled", "1"), ("Tag", "start_personage")],
    "BlowReach": [("Enabled", "1"), ("Near", "45"), ("Medium", "45"), ("Far", "45")],
    "AutoDraw": [("Enabled", "1"), ("PlayerUnits", "0"), ("HoldMs", "3000"), ("Distance", "100"), ("Item", "melee"), ("Probe", "1"),
                 ("ProbeMs", "300"), ("ProbeCooldownMs", "4000"), ("NoMeleeMs", "20000"), ("ProbeAnyRange", "0"), ("KnownMs", "300000"),
                 ("OrderPoll", "1"), ("PollMs", "250"), ("BlindCheckMs", "8000"), ("Proximity", "1"), ("EscalateMs", "1500"),
                 ("LeaveFactor", "1.5"), ("Tick", "1"), ("TickMs", "100"), ("FightOrder", "1"), ("FightCooldownMs", "1500"),
                 ("OrderPicks", "1")],
    "Vehicles": [("Enabled", "1"), ("InfantryMs", "500"), ("InfantryRange", "400"), ("CloseDistance", "400"), ("CloseAttackOrder", "1"),
                 ("VehicleMeleeTags", "melee_light,melee_heavy"), ("AdjacentMs", "1500"), ("ContactTag", "hull_contact"),
                 ("AdjacentMax", "120")],
    "ReloadCancel": [("Enabled", "1"),
                     ("KeepTags", "mgun,hmg_shell,stubber,lascannon,plasmacannon,sm_weapon_plasmacannon,sm_weapon_heavybolter,"
                                  "rocketlauncher,heavy_flamethrower,grenadelauncher,at_rifle,ogryn_weapon")],
    "meleechase": [("enabled", "1"), ("log", "1"), ("console", "0"), ("verbose", "1"), ("crash_log", "1"), ("log_chase", "1"),
                   ("melee_tag", "melee"), ("vehicle_melee_tag", "veh_melee"), ("vehicle_chase", "1"), ("repath_fraction", "0.25"),
                   ("repath_min_m", "1"), ("repath_max_m", "10"), ("repath_interval_ms", "500"), ("repath_interval_close_ms", "200"),
                   ("close_range_m", "10"), ("contact_factor", "0.5"), ("lead_seconds", "0.6"), ("lead_max_m", "6"),
                   ("pursue_retreating", "1"), ("retreat_speed_mps", "0.3"), ("chase_always", "1"), ("chase_run_mode", "2"),
                   ("hold_ranged_in_melee", "1"), ("hold_ranged_reach_factor", "1.0"), ("smooth_repath", "1"), ("weapon_priority", "1"),
                   ("prop_prioritize_melee", "ai_prioritize_melee"), ("prop_prioritize_ranged", "ai_prioritize_ranged")],
    "PenFilter": [("Enabled", "1"), ("Log", "1"), ("LogVerdicts", "400"), ("PierceTag", "pierce"), ("ExemptTags", "melee_light,melee_heavy,melee"),
                  ("HookRowFilter", "1"), ("HookAimCheck", "1"), ("HookAimPriority", "1"), ("HookExhaust", "1"), ("ExhaustStop", "1"),
                  ("HookApproach", "0"), ("AimPriority", AIM_PRIORITY), ("VerdictHoldMs", "8000"), ("HoldDistanceM", "15"),
                  ("CacheMs", "60000"), ("UnreachableMs", "3000"), ("PierceRule", "1"), ("PlayerUnits", "1"), ("SkipDirectControl", "1"),
                  ("Facing", "1"), ("MeleeForceAim", "1")],
}

# a value of a module ini that 5.1 changes: the reason, written as a note line above the key (an override without a reason
# here stops the build, so the table and the source inis cannot drift apart silently)
OVERRIDES = {
    ("autodraw", "probeanyrange"): ("1", "0"),
    ("meleestance", "automeleetag"): ("melee_infantry", "melee_infantry,melee_auto"),   # Alex, Nov 21 2026
    ("penfilter", "aimpriority"): (AIM_PRIORITY_011, AIM_PRIORITY),                    # Alex, Nov 22 2026: guns before tracks
}

# a key gem_melee adds to a module section (no snapshot ini has it): written right after the named key, under its note lines
ADDED = {
    ("charge", "moverefreshfraction"): [("MoveRefreshMaxUnits", [
        "gem_melee 0.1.1 (port edit S23; Alex, Nov 21 2026: a stance marine sent in from far kept running to where the enemy had been):",
        "... and never more than this many units (0 = no upper limit, the 0.7.42 rule). With the share alone a run ordered 53 m out",
        "was first re-aimed 5.4 s later, the enemy 15 m from the ordered spot (his log). 200 = after 10 m at any range beyond 25 m;",
        "inside 25 m nothing changes (0.4 x 500 = 200), so no run is re-aimed more often than the 0.7.13 rule already does there.",
    ]), ("Lead", [
        "gem_melee 0.1.2 (port edit S24; Alex, Nov 21 2026: \"Yes, build it\"): intercept lead. 1 = the far charge run at enemy infantry goes",
        "to where the enemy will be: its position + its velocity x the time a runner needs to meet it. The velocity comes from the",
        "enemy's position every 250 ms (taken whenever a unit's scan looks at it), averaged with the sample before; an enemy that stands,",
        "jumps or was not looked at for 1.5 s has none and the run goes to its position, as before. A stall re-order runs straight at",
        "the enemy and keeps that unit unled for 15 s. Vehicles are never led. 0 = off (0.1.1). How often the goal follows: Smooth below.",
    ]), ("LeadSeconds", [
        "gem_melee 0.1.2: ... never further ahead than this many seconds of the enemy's movement (also the lead when a runner at",
        "LeadRunSpeed cannot catch it). 0 = no lead.",
    ]), ("LeadKeep", [
        "gem_melee 0.1.2: ... and never more than this x the time the enemy has kept its heading (a stop or a turn of more than 45 degrees",
        "starts it over): 0.5 = an enemy 2 s on its course is led by 1 s, one that dashes and stops by next to nothing. 0 = no such limit.",
        "Model tools\\lead_sim.py (re-aim cost 0.2-0.6 s, runner 100-135 units/s): against enemies that keep walking or running the",
        "charge closes 10-50 % sooner with a quarter to a third of the re-aims; against ones that dash and stop or only shuffle it is",
        "as 0.1.1. With 0 the first group gains a little more and dash-and-stop is up to 5 % slower. A model, not a game test.",
    ]), ("LeadRunSpeed", [
        "gem_melee 0.1.2: the charging unit's run speed for the meeting time, world units per second (20 = 1 m; marines ran 118-135 in",
        "the logs). 20..1000.",
    ]), ("LeadMaxFraction", [
        "gem_melee 0.1.2: the run goal is never further from the enemy than this share of the distance to it (0..1).",
    ]), ("Smooth", [
        "gem_melee 0.1.3 (port edit S25; Alex, Nov 21 2026 on 0.1.2: \"he only updates his pos once he gets to the original location\"):",
        "1 = a running charge run gets its new goal inside its own order instead of a new order. That is the engine's squad-follow way:",
        "the unit's front eMoveTask takes over the goal of a carrier order and re-plans its path while the unit keeps moving, so there is",
        "no stop and the goal can follow the aim point closely. Used when the unit's front order is a run order the dll gave and it is",
        "moving; otherwise, and for 10 s after an order refused it, a new order as before. The engine methods are checked once per",
        "session (log \"in-place re-aim ON\" or \"OFF\"). 0.1.4: it runs from a weapon-pick scan, inside the engine's quant (from the",
        "dll's tick the engine's path planner asserted: a hitch per re-aim in the editor). 0 = always a new order.",
    ]), ("SmoothMs", [
        "gem_melee 0.1.3: an in-place re-aim at the earliest this many ms after the unit's last order or re-aim (100 or more).",
    ]), ("SmoothFraction", [
        "gem_melee 0.1.3: ... when the aim point (the enemy, or the lead point) is further from the run's goal than this share of the",
        "distance to the enemy, at least MoveRefreshUnits. A NEW order (in place not possible) keeps the limits above (40 units / 40 % /",
        "MoveRefreshMaxUnits) and is given when the enemy leaves the course the run was aimed with or the run's lead time is used up.",
    ]), ("ReplaceOwn", [
        "gem_melee 0.1.8 (Alex, Nov 22 2026 on 0.1.7: \"1.6 was way better, we just need to fix those dangling orders\"): the engine's",
        "AddOrder flag 1, which every order of this dll used, APPENDS the order to the unit's queue; orders piled up there (his log:",
        "33 on one unit) and ran later, one outdated goal after the other. 1 = a new charge order takes the place (flags 5, what a",
        "normal click passes) of the dll's own run order in front, inside the engine's quant; behind any other order (the dll's own",
        "fight order, an order of the player, the bot or the engine) it is appended, but only one order of the dll's waits there,",
        "never more. Switching the stance off cancels the unit's queue while an order of the dll's is in it, then the gun comes",
        "back. 0 = always appended (0.1.5).",
    ])],
    ("penfilter", "facing"): [("MeleeForceAim", [
        "gem_melee 0.1.12 (Alex, Nov 22 2026: dreadnoughts stood in front of a Baneblade and did not swing): the melee weapon of",
        "a vehicle-brain unit (walker, fauna) is a gun on a fixed foresight; against a vehicle the engine's aim solver can refuse",
        "every near part (elevation limit, no ballistic solution), and without an aim nothing fires. 1 = such a pick keeps its",
        "aim when the solver refuses it (the part is inside the picker's range check; a swing needs no firing solution).",
        "0 = the solver's answer stands. The [pen] M log line counts both."])],
}

# note lines written right above a key of a module ini (its own comments stay as they are)
NOTES = {
    ("meleestance", "automeleetag"): [
        "gem_melee 0.1.0 (Alex, Nov 21 2026): melee_infantry = may melee + the AI [Zerg] charge (the 6 TG melee breeds); melee_auto =",
        "may melee only, no [Zerg] (the 12 assault marine breeds: um_assault*, um_vet_assault*, bra_assault*). The code default is",
        "melee_infantry alone, so a gem_melee.ini without this key drops melee_auto: keep this key."],
    ("meleestance", "log"): [
        "gem_melee: the stance lines go to gem_melee.log with the [stance] prefix ([Core] Log=0 stops every module's lines). Log=0 no",
        "longer cuts the link to GEM Teleport's gem_order_listen (it ran only with Log=1 before)."],
    ("charge", "cooldownms"): [
        "gem_melee: the code default is 4000, so keep this key (2000 = the value every payload runs)."],
    ("autodraw", "probeanyrange"): [
        "gem_melee: 0 = the value every payload since Guga's 0.9.15 runs; the 0.7.42 source ini said 1 and the code default is 1,",
        "so keep this key."],
    ("autodraw", "tick"): [
        "gem_melee: the same quant hook gives GEM Teleport its per-quant tick (gem_quant_listen, jetpack aim rings); Tick=0 = no jetpack",
        "aim events at all once Teleport has registered (it then waits for quant ticks that never come)."],
    ("meleechase", "log"): [
        "gem_melee: the chase lines go to gem_melee.log with the [chase] prefix (no separate gem_meleechase.log, no truncation at start)."],
    ("meleechase", "console"): [
        "gem_melee: no longer read (no console window; the one log has every line)."],
    ("meleechase", "crash_log"): [
        "gem_melee: the crash logger belongs to the whole dll now; it runs when this key, enabled above and [Core] CrashLog are all 1, in a",
        "call_to_arms exe (MeleeChase's own conditions: enabled=0 also removes it, as in 0.5.4)."],
    ("penfilter", "aimpriority"): [
        "gem_melee 0.1.9 (Alex, Nov 22 2026: \"add in gun volumes before tracks\"): a tag written +tag names a part the engine never",
        "offers to the AI because it carries no_target (gun, barrel and breech volumes: tags gun, gun1 .. gun7). The dll offers",
        "those itself, on rows without {volume \"x\"} only, when the ammo can pierce them and no group before theirs has a",
        "reachable pierceable part. One the aim solver cannot reach is blocked like an armour miss (CacheMs / 25 degrees /",
        "HoldDistanceM), so the ammo falls through to the next row. Without the + a no_target part is never aimed at."],
    ("penfilter", "log"): [
        "gem_melee: the pen lines go to gem_melee.log with the [pen] prefix; the 10 s stats line comes from the dll's background thread."],
    ("penfilter", "exhauststop"): [
        "gem_melee: [Link] CeasefireVeto=1 refuses this stop for a weapon that MeleeChase counts as melee ([meleechase] melee_tag /",
        "vehicle_melee_tag on the weapon item)."],
}

# a note written right above a section header line of a module ini
SECTION_NOTES = {
    "meleechase": [
        "gem_melee: the lines above are MeleeChase's own header. These keys are read from gem_melee.ini now, and what it says about",
        "gem_meleechase.log goes to gem_melee.log ([chase] prefix)."],
}

HEADER = [
    "GEM Melee 0.1.20 settings: gem_melee.dll = GEM MeleeStance 0.7.42 (stance) + GEM MeleeChase 0.5.4 (chase) + GEM PenFilter 0.1.1",
    "(pen) in one dll. Lives next to gem_melee.dll (the Indomitus launcher payload folder; a hosted MP server reads its copy in",
    "<payload>\\server\\). Read by call_to_arms.exe, call_to_arms_ed.exe and call_to_arms_server.exe.",
    "",
    "Generated by tools\\make_ini.py: [Core] and [Link] are new; every section after them is one of the three old inis with its own",
    "comments, at the values Alex runs today (payload a1d6ed3c); the 0.7.42 key AutoMeleeTag has its 0.7.42 value. Lines that start",
    "with \"; gem_melee\" are notes added for the merge.",
    "GetPrivateProfile rules: the first section / first key of a name wins, names are case-insensitive, no comment after a value.",
    "gem_meleestance.ini, gem_meleechase.ini and gem_penfilter.ini are not read any more (the log says \"... ignored\").",
    "Without this file every module runs on its code defaults: [Charge] CooldownMs 4000 instead of 2000, [AutoDraw] ProbeAnyRange 1",
    "instead of 0, and the 0.7.42 no-melee rule on ([MeleeStance] AutoMeleeTag melee_infantry).",
    "",
    "One log for the whole dll, next to it: gem_melee.log (game), gem_melee_editor.log (editor), gem_melee_server.log (dedicated",
    "server); append mode, readable while the game runs. Each line: wall time, sim clock ms, [core] / [chase] / [stance] / [pen] /",
    "[link], then the module's own text.",
    "It changes AI decisions (simulation): every player of a multiplayer match and the server need the same build.",
    "Do not rename gem_melee.dll: GEM Teleport 0.2.14 finds the per-quant tick by this file name.",
]

CORE_BLOCK = [
    "[Core]",
    "; 1 = write the log (gem_melee.log / gem_melee_editor.log / gem_melee_server.log next to the dll). 0 = no log file for any module",
    ";     (each module's own Log key below picks which of its lines are written while this is 1)",
    "Log=1",
    "; 1 = log fatal exceptions (first chance, with a stack scan) and who ends the process (MeleeChase's crash logger, one copy for the",
    ";     whole dll). Runs only where MeleeChase ran it: [meleechase] crash_log=1 and enabled=1, in a call_to_arms exe.",
    "CrashLog=1",
    "; new battle: the sim clock stepping back by at least this many ms is a restart (editor stop / play, match end, next mission).",
    "; Stance and pen flush their per-battle tables on it as well as on their own rules; chase keeps only its own rule. 1..60000.",
    "NewBattleBackMs=1000",
    "; 1 = each code write suspends the other threads of the process while it writes (matters only when the dll is put into a",
    ";     server that is already running; at game start no other thread exists). At load one walk over the thread list serves",
    ";     every code write of the three modules (the attach batch: the loader lock keeps the set of running threads fixed while",
    ";     the dll loads); the \"[core] code writes:\" line says how many, how many walks and how long. 0 = plain writes, as",
    ";     MeleeStance / PenFilter did.",
    "PatchSuspendThreads=1",
    "; seconds after load during which the dll looks once a second for an old gem_meleestance / gem_meleechase / gem_penfilter dll",
    "; loaded after it (WARNING line: remove it from the payload). 0 = off. An old dll loaded BEFORE gem_melee turns the matching",
    "; module off at load instead (\"... is already loaded in this process\").",
    "LateGuardSeconds=60",
]

LINK_BLOCK = [
    "[Link]",
    "; How the three modules work together (spec section 6). Log only, except CeasefireVeto. Nothing here changes which weapon a",
    "; unit picks or which order it gets.",
    "; R4 flip detector: a unit whose front order changes FlipChanges times within FlipWindowMs gets one \"[link] flip:\" block (its",
    "; last order changes and the last stance / pen / chase verdicts on it) and is then muted for 10 s. A unit becomes a suspect at",
    "; its 2nd change inside the window; only suspects' weapon picks and verdicts are noted. Inputs: stance's order poll (human units",
    "; on its poll list; not the human player's own units outside the stance with [AutoDraw] PlayerUnits=0, nor linked, SkipTags or",
    "; directly controlled units) and every chase approach registration (a vehicle in the hold / chase loop trips with the same",
    "; order class each time). FlipLines = blocks per battle for all units, at most 2 per unit; when it is reached one \"FlipLines=..",
    "; reached\" line says so (no block after it does not mean no flip) and the next battle starts with the count not written. 0 = off.",
    "Flip=1",
    "FlipChanges=5",
    "; stance samples a unit's front order once per order poll ([AutoDraw] PollMs, 250-300 ms live), so the window must hold at least",
    "; FlipChanges+1 polls; 6 in 2000 ms never trips on the live twitch log (8.4 --replay)",
    "FlipWindowMs=3000",
    "FlipLines=20",
    "; R5 trace: 1 = a unit that carries the entity tag TraceTag gets a \"[link] trace:\" line for each of these: stance's weapon pick",
    "; (every pick, PASS included; stance's charges, draws and orders are not traced, see their own lines), chase's priority change,",
    "; approach registration and hold, pen's hook A verdict (every call, held ones too) and X. At most TraceLines per battle: a traced",
    "; vehicle under pen fills 2000 within seconds of combat. No mod file sets ai_trace: give one unit the tag for a test (an editor",
    "; script / breed {tags} line). For following one unit; 0 = off (one global load per decision).",
    "Trace=0",
    "TraceTag=ai_trace",
    "TraceLines=2000",
    "; R3: 1 = pen's weapon stop ([PenFilter] HookExhaust / ExhaustStop) is refused for a weapon whose item tags hold [meleechase]",
    "; melee_tag or vehicle_melee_tag (with today's data pen never stops a melee weapon). Chase's hold is never refused. 0 = off.",
    "CeasefireVeto=1",
    "; R8 diagnostic: 1 = time pen's row filter (hook A) and write a \"[link] cost:\" line every 5 s while the numbers change: stance's",
    "; own time, the engine's MatchAmmo time incl. pen A, pen A, engine alone (needs [MeleeStance] HookTiming=1). Costs two timer",
    "; reads per hook A call and inflates stance's \"engine MatchAmmo\" share on the hb: line; pen A is printed without the timer's own",
    "; cost (measured once at load, the line says how much), which stays in \"engine alone\". Set 1 only for a cost run (AT-12).",
    "CostLine=0",
]

WARN_ONLY = {("penfilter", "hookapproach"), ("penfilter", "blockstreak")}   # pen reads them only to print a warning (pf:1894-1895)
EXPECT_CODE_KEYS = {"stance": 80, "chase": 28, "pen": 19}


# ------------------------------------------------------------------------------------------------------------- reading
def read_lines(path):
    with open(path, "rb") as f:
        data = f.read()
    text = data.decode("ascii")
    if text.startswith("﻿"):
        raise SystemExit(f"{path}: BOM")
    return text.replace("\r\n", "\n").split("\n")


def key_of(line):
    """(key, value) of a GetPrivateProfile key line, else None (comments start with ';', sections with '[')."""
    s = line.strip()
    if not s or s[0] in ";[" or "=" not in s:
        return None
    k, v = s.split("=", 1)
    return k.strip(), v.strip()


def final_map():
    return {sec.lower(): {k.lower(): (k, v) for k, v in kv} for sec, kv in FINAL.items()}


# ------------------------------------------------------------------------------------------------------------- building
def module_block(path, title, fm, used, errors):
    """The lines of one module ini with the 5.1 values and the note lines; every key must be in FINAL."""
    out = [f"; {'=' * 40} {title} {'=' * 40}"[:132]]
    lines = read_lines(path)
    while lines and lines[-1] == "":
        lines.pop()
    sec = None
    for line in lines:
        s = line.strip()
        if s.startswith("[") and "]" in s:
            sec = s[1:s.index("]")].strip().lower()
            if sec not in fm:
                errors.append(f"{os.path.basename(path)}: section [{sec}] is not in the 5.1 table")
            for n in SECTION_NOTES.get(sec, []):
                out.append("; " + n)
            out.append(line)
            continue
        kv = key_of(line)
        if kv is None or sec is None:
            out.append(line)
            continue
        k, v = kv
        kk = (sec, k.lower())
        want = fm.get(sec, {}).get(k.lower())
        if want is None:
            errors.append(f"{os.path.basename(path)}: [{sec}] {k} is not in the 5.1 table")
            out.append(line)
            continue
        if kk in used:
            errors.append(f"{os.path.basename(path)}: [{sec}] {k} appears twice")
        used.add(kk)
        for n in NOTES.get(kk, []):
            out.append("; " + n)
        if v != want[1]:
            ov = OVERRIDES.get(kk)
            if not ov or ov != (v, want[1]):
                errors.append(f"{os.path.basename(path)}: [{sec}] {k}={v} but 5.1 says {want[1]} and no OVERRIDES entry explains it")
        out.append(f"{want[0]}={want[1]}")
        for ak, notes in ADDED.get(kk, []):
            aw = fm.get(sec, {}).get(ak.lower())
            if aw is None or (sec, ak.lower()) in used:
                errors.append(f"ADDED key [{sec}] {ak}: not in the 5.1 table, or written twice")
                continue
            used.add((sec, ak.lower()))
            out += ["; " + n for n in notes] + [f"{aw[0]}={aw[1]}"]
    return out


def port_reads(sec, key):
    """True when the module's port reads [sec] key: stance through GetPrivateProfile*A("sec", "key"), pen ([PenFilter]) through
    the GI("key", default) macro of its load_ini or a GetPrivateProfileStringA("PenFilter", "key" call."""
    if sec.lower() == "penfilter":
        with open(os.path.join(GM, "src", "pen", "pen.c"), "rb") as f:
            t = f.read().decode("latin1")
        return re.search(r'(?:\bGI\(\s*|GetPrivateProfileStringA\(\s*"PenFilter"\s*,\s*)"%s"' % re.escape(key), t, re.I) is not None
    with open(os.path.join(GM, "src", "stance", "stance.c"), "rb") as f:
        t = f.read().decode("latin1")
    return re.search(r'GetPrivateProfile(?:Int|String)A\(\s*"%s"\s*,\s*"%s"' % (re.escape(sec), re.escape(key)), t, re.I) is not None


def build():
    fm = final_map()
    errors = []
    used = set()
    lines = ["; " + h if h else ";" for h in HEADER]
    lines.append("")
    lines += CORE_BLOCK
    lines.append("")
    lines += LINK_BLOCK
    for sec in ("core", "link"):
        for k in fm[sec]:
            used.add((sec, k))
    lines.append("")
    lines += module_block(SRC_STANCE_INI, "MeleeStance 0.7.42 (stance module; its own ini, sections [MeleeStance] .. [ReloadCancel])",
                          fm, used, errors)
    lines.append("")
    lines += module_block(SRC_CHASE_INI, "MeleeChase 0.5.4 (chase module; its own ini, section [meleechase])", fm, used, errors)
    lines.append("")
    lines += module_block(SRC_PEN_INI, "PenFilter 0.1.1 (pen module; its own ini, section [PenFilter])", fm, used, errors)
    for sec, kv in fm.items():
        for k in kv:
            if (sec, k) not in used:
                errors.append(f"5.1 key [{sec}] {kv[k][0]} was not written (missing in its module ini)")
    for kk in NOTES:
        if kk not in used:
            errors.append(f"NOTES entry {kk} names no written key")
    for kk, added in ADDED.items():
        if kk not in used:
            errors.append(f"ADDED entry {kk} names no written key")
        for ak, _ in added:                                   # the module's port must read the key it adds
            if not port_reads(kk[0], ak):
                errors.append(f"ADDED key [{kk[0]}] {ak}: no read of it in the module's port (stance.c / pen.c)")
    for kk in OVERRIDES:
        if kk not in used:
            errors.append(f"OVERRIDES entry {kk} names no written key")
    for i, l in enumerate(lines):
        try:
            l.encode("ascii")
        except UnicodeEncodeError:
            errors.append(f"line {i + 1} is not ASCII")
    data = ("\r\n".join(lines) + "\r\n").encode("ascii", "replace")
    return data, errors


# ------------------------------------------------------------------------------------------------------------- checking
def parse_profile(text):
    """GetPrivateProfile view: {sec_lower: {key_lower: value}}, first section / key wins; plus repeats found."""
    out, repeats, cur, seen_sec = {}, [], None, set()
    for i, line in enumerate(text.split("\n"), 1):
        s = line.strip()
        if s.startswith("["):
            name = s[1:s.index("]")].strip() if "]" in s else s[1:].strip()
            nl = name.lower()
            if nl in seen_sec:
                repeats.append(f"section [{name}] repeated at line {i}")
                cur = None
                continue
            seen_sec.add(nl)
            cur = out.setdefault(nl, {})
            continue
        kv = key_of(line)
        if kv is None or cur is None:
            continue
        k, v = kv
        if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
            v = v[1:-1]
        if k.lower() in cur:
            repeats.append(f"key {k} repeated at line {i}")
            continue
        cur[k.lower()] = v
    return out, repeats


def code_keys():
    """(module, section_lower, key_lower) of every key the snapshot code reads (regexes of compat\\scan\\ini_table.py)."""
    keys = []
    with open(CODE_STANCE, "rb") as f:
        t = f.read().decode("latin1")
    for m in re.finditer(r'GetPrivateProfile(?:Int|String)A\(\s*"(\w+)"\s*,\s*"(\w+)"', t):
        keys.append(("stance", m.group(1).lower(), m.group(2).lower()))
    m = re.search(r'bk\[3\]\s*=\s*\{([^}]*)\}', t)
    for k in re.findall(r'"(\w+)"', m.group(1)) if m else []:
        keys.append(("stance", "blowreach", k.lower()))
    with open(CODE_CHASE, "rb") as f:
        t = f.read().decode("latin1")
    for m in re.finditer(r'ini_(?:int|str|float)\(\s*"(\w+)"', t):
        keys.append(("chase", "meleechase", m.group(1).lower()))
    with open(CODE_PEN, "rb") as f:
        t = f.read().decode("latin1")
    for m in re.finditer(r'\bGI\(\s*"(\w+)"', t):
        keys.append(("pen", "penfilter", m.group(1).lower()))
    for m in re.finditer(r'GetPrivateProfileStringA\(\s*"(\w+)"\s*,\s*"(\w+)"', t):
        keys.append(("pen", m.group(1).lower(), m.group(2).lower()))
    uniq, seen = [], set()
    for k in keys:
        if k not in seen:
            seen.add(k)
            uniq.append(k)
    return uniq


def own_code_keys():
    """(file, section_lower, key_lower) of literal core_ini_int / core_ini_str reads in src\\core and src\\link, and of the [meleechase]
    keys core_dllmain.c reads the way chase does (chase_key(L"key", ...), wide path; STARTUP_batch / review N1)."""
    out = []
    for sub in ("core", "link"):
        d = os.path.join(GM, "src", sub)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".c"):
                continue
            with open(os.path.join(d, fn), "rb") as f:
                t = f.read().decode("latin1")
            for m in re.finditer(r'core_ini_(?:int|str)\(\s*"(\w+)"\s*,\s*"(\w+)"', t):
                out.append((f"src/{sub}/{fn}", m.group(1).lower(), m.group(2).lower()))
            for m in re.finditer(r'chase_key\(\s*L"(\w+)"', t):
                out.append((f"src/{sub}/{fn}", "meleechase", m.group(1).lower()))
    return out


def win_profile_get(path, sec, key):
    import ctypes
    buf = ctypes.create_string_buffer(8192)
    miss = b"\x01gem_melee_missing\x01"
    ctypes.windll.kernel32.GetPrivateProfileStringA(sec.encode("ascii"), key.encode("ascii"), miss, buf, len(buf),
                                                    os.path.abspath(path).encode("mbcs"))
    v = buf.value
    return None if v == miss else v.decode("ascii")


def check():
    problems = []
    if not os.path.isfile(OUT):
        print(f"INI FAILED: {OUT} does not exist (run python tools\\make_ini.py)")
        return 1
    with open(OUT, "rb") as f:
        data = f.read()
    # (d) CRLF / ASCII
    if data.startswith(b"\xef\xbb\xbf"):
        problems.append("(d) file starts with a UTF-8 BOM")
    bad = [i for i, b in enumerate(data) if b >= 0x80]
    if bad:
        problems.append(f"(d) {len(bad)} non-ASCII bytes, first at offset {bad[0]}")
    lf_alone = len(re.findall(rb"(?<!\r)\n", data))
    cr_alone = len(re.findall(rb"\r(?!\n)", data))
    if lf_alone or cr_alone:
        problems.append(f"(d) line ends: {lf_alone} LF without CR, {cr_alone} CR without LF")
    if not data.endswith(b"\r\n"):
        problems.append("(d) the file does not end with CRLF")
    text = data.decode("ascii", "replace").replace("\r\n", "\n")
    prof, repeats = parse_profile(text)
    # (c) no repeats
    for r in repeats:
        problems.append("(c) " + r)
    # (a) the 5.1 values, by the Python reader and by kernel32
    use_win = os.name == "nt"
    nkeys_table = 0
    for sec, kv in FINAL.items():
        for k, v in kv:
            nkeys_table += 1
            got = prof.get(sec.lower(), {}).get(k.lower())
            if got != v:
                problems.append(f"(a) [{sec}] {k}: expected {v!r}, file has {got!r}")
            if use_win:
                wg = win_profile_get(OUT, sec, k)
                if wg != v:
                    problems.append(f"(a) [{sec}] {k}: expected {v!r}, kernel32 GetPrivateProfileStringA returns {wg!r}")
    # (b) every key the snapshots' code reads
    counts = {"stance": 0, "chase": 0, "pen": 0}
    for mod, sec, k in code_keys():
        if (sec, k) in WARN_ONLY:
            continue
        counts[mod] += 1
        if k not in prof.get(sec, {}):
            problems.append(f"(b) {mod} code reads [{sec}] {k}: not in the file")
    for mod, n in EXPECT_CODE_KEYS.items():
        if counts[mod] != n:
            problems.append(f"(b) {mod}: {counts[mod]} keys read by the snapshot code, expected {n} (key extraction out of date?)")
    bs = prof.get("penfilter", {}).get("blockstreak")
    if bs is not None and bs != "-1":
        problems.append(f"(b) [PenFilter] BlockStreak={bs}: warn-only key (pf:1895), keep it out of the ini")
    # (g) no key outside the 5.1 table (a misspelt key would silently fall back to the code default)
    fm = final_map()
    for sec, kv in prof.items():
        for k in kv:
            if k not in fm.get(sec, {}):
                problems.append(f"(g) [{sec}] {k} is not a key of the 5.1 table")
    # (e) gem_melee's own code
    for fn, sec, k in own_code_keys():
        if k not in prof.get(sec, {}):
            problems.append(f"(e) {fn} reads [{sec}] {k}: not in the file")
    # (f) up to date
    gen, errors = build()
    for e in errors:
        problems.append("(f) generator: " + e)
    if gen != data:
        problems.append("(f) src\\gem_melee.ini differs from what tools\\make_ini.py writes now: run python tools\\make_ini.py")
    nkeys = sum(len(v) for v in prof.values())
    if problems:
        for p in problems:
            print(p)
        print(f"INI FAILED ({len(problems)} problems)")
        return 1
    print(f"code keys present: {counts['stance']} stance + {counts['chase']} chase + {counts['pen']} pen; "
          f"5.1 table {nkeys_table} keys{' (also via kernel32)' if use_win else ''}; own code keys {len(own_code_keys())}")
    print(f"INI OK ({nkeys} keys)")
    return 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--check":
        return check()
    if len(sys.argv) > 1:
        print(__doc__)
        return 2
    data, errors = build()
    if errors:
        for e in errors:
            print(e)
        print("NOT written")
        return 1
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(data)
    print(f"written {OUT} ({len(data)} bytes, {data.count(b'\r\n')} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
