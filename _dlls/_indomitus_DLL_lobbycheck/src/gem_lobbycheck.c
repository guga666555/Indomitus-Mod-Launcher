/* GEM LobbyCheck 0.1 - does everyone in a multiplayer lobby run the same launcher build?
 * Indomitus Launcher payload dll (gem_lobbycheck.dll + gem_lobbycheck.ini). Call to Arms: Gates of Hell 1.067.
 *
 * The launcher starts the game with two environment variables:
 *   INDOMITUS_PAYLOAD            its build code = the fingerprint of the engine files it carries
 *   INDOMITUS_LAUNCHER_VERSION   its version as shown in the launcher window (may be missing)
 * The game keeps every multiplayer session in a Steam lobby (eSteamLobby / eSessionLobby in the exe).
 * This dll listens to Steam's lobby callbacks and, through Steam only:
 *   - lobby owner (the host): writes both values into the lobby data, keys "indomitus_payload" and
 *     "indomitus_launcher"; reads every other member's values and shows a message for each member
 *     whose build code differs or who reports none (HostNotice=0 turns this half off)
 *   - everyone: writes both values into his own lobby member data
 *   - everyone but the owner: compares the owner's build code with his own and shows a message when
 *     they differ, or when the owner reports none within GraceSeconds
 * It only compares and tells. It never blocks a match, changes nothing in the simulation and writes
 * nothing the game reads (the game reads its lobby keys by name: gamever, hostname, mapname, ...).
 *
 * How it gets there (nothing in the game's own code is patched):
 *   - the exe's import slot of steam_api64!SteamAPI_RunCallbacks is pointed at hook_run_callbacks.
 *     That is the thread Steam delivers callbacks on, so the four listeners are registered there,
 *     the first time Steam is up, and all the work runs there after Steam's own dispatch.
 *   - listeners: LobbyEnter_t 504, LobbyDataUpdate_t 505, LobbyChatUpdate_t 506, LobbyCreated_t 513,
 *     registered with SteamAPI_RegisterCallback like the game's own (CCallbackBase layout: vtable,
 *     flags byte at +8, callback id at +12; vtable = Run(data, ioFailure, call), Run(data), size).
 *   - Steam is called through the flat C exports of steam_api64.dll (SteamAPI_ISteamMatchmaking_...),
 *     found in its export table; the three interface getters carry a version number in their name
 *     (SteamAPI_SteamMatchmaking_v009) and are looked up by prefix.
 *   - the message box runs on its own thread: the game is never stopped by it.
 * Active in the game and in the MP host server (whichever of the two holds the lobby; a named mutex
 * keeps the two from showing the same message twice). Not in the editor.
 * Without INDOMITUS_PAYLOAD (game not started by a launcher that sets it) the dll does nothing.
 *
 * Values that come from other players are untrusted: cut to 39 characters, [0-9A-Za-z._+-] kept,
 * everything else replaced, never used as a format string.
 *
 * Build: build.bat      Offline test (lobby scenarios + the lookups on the real files): build.bat test
 * The part up to "windows side" is plain C and is what the test runs.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#ifndef LC_TEST
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define LC_VERSION    "0.1"
#define KEY_PAYLOAD   "indomitus_payload"
#define KEY_LAUNCHER  "indomitus_launcher"
#define ID_LEN        40          /* a build code or version text, cleaned, with its 0 */
#define MAX_LOBBIES   4           /* session lobby, party lobby, ... at the same time */
#define MAX_MEMBERS   64
#define MAX_RECENT    16
#define REPEAT_MS     60000u      /* the same finding about the same player is not shown again this soon */
#define SET_TRIES     10
#define MSG_LEN       900

/* ---------------------------------------------------------------- what the core needs from outside */
static uint64_t    st_self(void);                                                   /* my steam id, 0 = steam not ready */
static uint64_t    st_owner(uint64_t lobby);                                        /* 0 = I am not in that lobby */
static int         st_member_count(uint64_t lobby);
static uint64_t    st_member_at(uint64_t lobby, int i);
static const char *st_lobby_data(uint64_t lobby, const char *key);                  /* "" when not set */
static int         st_set_lobby_data(uint64_t lobby, const char *key, const char *val);
static const char *st_member_data(uint64_t lobby, uint64_t user, const char *key);  /* "" when not set */
static void        st_set_member_data(uint64_t lobby, const char *key, const char *val);
static void        st_name(uint64_t user, wchar_t *out, size_t cap);                /* never empty */
static int         claim(uint64_t lobby, uint64_t who);                             /* 0 = another process of this game shows it */
static void        popup(const wchar_t *text);
static void        logf_(const char *fmt, ...);

/* ---------------------------------------------------------------- state */
enum { M_WAIT = 0, M_SAME, M_TOLD };
typedef struct { uint64_t id; uint32_t first_ms; uint8_t state; } Member;

typedef struct {
    uint64_t id;            /* 0 = free slot */
    uint64_t owner;         /* owner at the last look */
    uint32_t since_ms;      /* entered / owner changed: start of the wait for a silent host */
    uint32_t next_ms;       /* next timed look */
    uint8_t  dirty;         /* steam reported something about this lobby */
    uint8_t  published;     /* owner: lobby data written (2 = gave up) */
    uint8_t  set_tries;
    uint8_t  announced;     /* own member data written */
    uint8_t  guest_done;    /* not the owner: owner compared, or reported as silent */
    uint8_t  misses;        /* looks in a row without an owner = not in the lobby any more */
    int      nm;
    Member   m[MAX_MEMBERS];
} Lobby;

typedef struct { uint64_t who; uint32_t ms; char payload[ID_LEN]; } Recent;

typedef struct {
    char     payload[ID_LEN];     /* this launcher's build code; "" = unknown, the dll stays off */
    char     launcher[ID_LEN];    /* its version, "" = unknown */
    int      host_notice;
    uint32_t grace_ms;
    uint32_t last_tick;
    int      recent_next;
    Recent   recent[MAX_RECENT];
    Lobby    lobby[MAX_LOBBIES];
} Core;
static Core C;

/* ---------------------------------------------------------------- text helpers */
/* keeps [0-9A-Za-z._+-]; anything else becomes '?'. Other players' values go through here. */
static void clean(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (src)
        for (; src[n] && n + 1 < cap; n++) {
            unsigned char c = (unsigned char)src[n];
            int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     c == '.' || c == '_' || c == '+' || c == '-';
            dst[n] = ok ? (char)c : '?';
        }
    dst[n] = 0;
}

static int has_digit(const char *s)
{
    for (; *s; s++) if (*s >= '0' && *s <= '9') return 1;
    return 0;
}

/* dotted versions, compared number by number: 0.9.10 > 0.9.9, 1.0 == 1.0.0 (the launcher's own rule) */
static int version_cmp(const char *a, const char *b)
{
    for (;;) {
        while (*a && (*a < '0' || *a > '9')) a++;
        while (*b && (*b < '0' || *b > '9')) b++;
        if (!*a && !*b) return 0;
        unsigned long x = 0, y = 0;
        while (*a >= '0' && *a <= '9') { if (x < 100000000ul) x = x * 10 + (unsigned)(*a - '0'); a++; }
        while (*b >= '0' && *b <= '9') { if (y < 100000000ul) y = y * 10 + (unsigned)(*b - '0'); b++; }
        if (x != y) return x < y ? -1 : 1;
    }
}

typedef struct { wchar_t *b; size_t cap, n; } WBuf;
static void w_str(WBuf *w, const wchar_t *s) { while (*s && w->n + 1 < w->cap) w->b[w->n++] = *s++; w->b[w->n] = 0; }
static void w_asc(WBuf *w, const char *s)    { while (*s && w->n + 1 < w->cap) w->b[w->n++] = (wchar_t)(unsigned char)*s++; w->b[w->n] = 0; }

/* "version 0.9.12, build 09d070d0" */
static void w_ident(WBuf *w, const char *launcher, const char *payload)
{
    if (has_digit(launcher)) { w_str(w, L"version "); w_asc(w, launcher); w_str(w, L", "); }
    w_str(w, L"build "); w_asc(w, payload);
}

static const wchar_t TXT_UPDATE[] = L"close the game, start the Indomitus Launcher again and let it update";
static const wchar_t TXT_END[]    = L"\n\nA match between different builds goes out of sync or crashes.";

/* ---------------------------------------------------------------- telling */
/* 1 = this finding about this player was shown less than REPEAT_MS ago (in any lobby) */
static int recently_told(uint64_t who, const char *payload, uint32_t now)
{
    for (int i = 0; i < MAX_RECENT; i++) {
        Recent *r = &C.recent[i];
        if (r->who == who && !strcmp(r->payload, payload) && now - r->ms < REPEAT_MS) return 1;
    }
    Recent *r = &C.recent[C.recent_next];
    C.recent_next = (C.recent_next + 1) % MAX_RECENT;
    r->who = who; r->ms = now;
    clean(r->payload, sizeof r->payload, payload);
    return 0;
}

static void report(const Lobby *lb, uint64_t who, const char *payload, const wchar_t *text, uint32_t now)
{
    if (recently_told(who, payload, now)) {
        logf_("lobby %llu: same finding as less than %u s ago - no second message", (unsigned long long)lb->id, REPEAT_MS / 1000);
        return;
    }
    if (!claim(lb->id, who)) {
        logf_("lobby %llu: the other process of this game (game / host server) shows the message", (unsigned long long)lb->id);
        return;
    }
    popup(text);
}

/* I joined; the owner's build differs (hp) or the owner reports none (hp = "") */
static void report_guest(const Lobby *lb, uint64_t owner, const char *hp, const char *hv, uint32_t now)
{
    wchar_t text[MSG_LEN]; WBuf w = { text, MSG_LEN, 0 }; text[0] = 0;
    if (hp[0]) {
        logf_("lobby %llu: DIFFERENT BUILD - host %llu has version %s build %s, I have version %s build %s",
              (unsigned long long)lb->id, (unsigned long long)owner, hv[0] ? hv : "?", hp,
              C.launcher[0] ? C.launcher : "?", C.payload);
        w_str(&w, L"The host of the lobby you joined uses a different Indomitus Launcher build.\n\nHost:  ");
        w_ident(&w, hv, hp);
        w_str(&w, L"\nYou:   "); w_ident(&w, C.launcher, C.payload);
        w_str(&w, L"\n\n");
        int c = (has_digit(hv) && has_digit(C.launcher)) ? version_cmp(C.launcher, hv) : 0;
        if (c < 0)      { w_str(&w, L"Your launcher is the older one: "); w_str(&w, TXT_UPDATE); w_str(&w, L"."); }
        else if (c > 0) { w_str(&w, L"The host's launcher is the older one. The host has to "); w_str(&w, TXT_UPDATE); w_str(&w, L"."); }
        else            { w_str(&w, L"Both of you have to use the same launcher: "); w_str(&w, TXT_UPDATE);
                          w_str(&w, L". If this message comes back, the host has to do the same."); }
    } else {
        logf_("lobby %llu: NO BUILD REPORTED by host %llu within %u s (older launcher, or no launcher); I have version %s build %s",
              (unsigned long long)lb->id, (unsigned long long)owner, C.grace_ms / 1000,
              C.launcher[0] ? C.launcher : "?", C.payload);
        w_str(&w, L"The host of the lobby you joined does not report an Indomitus Launcher build.\n\nYou:   ");
        w_ident(&w, C.launcher, C.payload);
        w_str(&w, L"\n\nThe host uses an older launcher than you (one without this check) or did not start the game "
                  L"with the Indomitus Launcher. The host has to "); w_str(&w, TXT_UPDATE); w_str(&w, L".");
    }
    w_str(&w, TXT_END);
    report(lb, owner, hp, text, now);
}

/* I am the host; a member's build differs (mp) or he reports none (mp = "") */
static void report_host(const Lobby *lb, uint64_t member, const char *mp, const char *mv, uint32_t now)
{
    wchar_t name[64], text[MSG_LEN]; WBuf w = { text, MSG_LEN, 0 }; text[0] = 0;
    st_name(member, name, 64);
    if (mp[0]) {
        logf_("lobby %llu: DIFFERENT BUILD - player %llu has version %s build %s, I (host) have version %s build %s",
              (unsigned long long)lb->id, (unsigned long long)member, mv[0] ? mv : "?", mp,
              C.launcher[0] ? C.launcher : "?", C.payload);
        w_str(&w, L"A player in your lobby uses a different Indomitus Launcher build.\n\n");
        w_str(&w, name); w_str(&w, L":  "); w_ident(&w, mv, mp);
        w_str(&w, L"\nYou (host):  "); w_ident(&w, C.launcher, C.payload);
        w_str(&w, L"\n\n");
        int c = (has_digit(mv) && has_digit(C.launcher)) ? version_cmp(C.launcher, mv) : 0;
        if (c > 0)      { w_str(&w, L"Their launcher is the older one. They have to "); w_str(&w, TXT_UPDATE); w_str(&w, L"."); }
        else if (c < 0) { w_str(&w, L"Your launcher is the older one: "); w_str(&w, TXT_UPDATE); w_str(&w, L"."); }
        else            { w_str(&w, L"Both of you have to use the same launcher. They have to "); w_str(&w, TXT_UPDATE);
                          w_str(&w, L". If this message comes back, do the same yourself."); }
    } else {
        logf_("lobby %llu: NO BUILD REPORTED by player %llu within %u s (older launcher, or no launcher); I (host) have version %s build %s",
              (unsigned long long)lb->id, (unsigned long long)member, C.grace_ms / 1000,
              C.launcher[0] ? C.launcher : "?", C.payload);
        w_str(&w, L"A player in your lobby does not report an Indomitus Launcher build.\n\n");
        w_str(&w, name);
        w_str(&w, L"\nYou (host):  "); w_ident(&w, C.launcher, C.payload);
        w_str(&w, L"\n\nThey use an older launcher than you (one without this check) or did not start the game "
                  L"with the Indomitus Launcher. They have to "); w_str(&w, TXT_UPDATE); w_str(&w, L".");
    }
    w_str(&w, TXT_END);
    report(lb, member, mp, text, now);
}

/* ---------------------------------------------------------------- looking at a lobby */
/* both return 1 while something is still being waited for */
static int host_look(Lobby *lb, uint64_t me, uint32_t now)
{
    int pending = 0;
    if (!lb->published) {
        int ok = st_set_lobby_data(lb->id, KEY_PAYLOAD, C.payload);
        ok = st_set_lobby_data(lb->id, KEY_LAUNCHER, C.launcher[0] ? C.launcher : "?") && ok;
        if (ok) {
            lb->published = 1;
            logf_("lobby %llu: I am the host - published version %s, build %s", (unsigned long long)lb->id,
                  C.launcher[0] ? C.launcher : "?", C.payload);
        } else if (++lb->set_tries >= SET_TRIES) {
            lb->published = 2;
            logf_("lobby %llu: I am the host but Steam refused the lobby data %d times - given up, "
                  "the players will be told that the host reports no build", (unsigned long long)lb->id, SET_TRIES);
        } else pending = 1;
    }
    if (!C.host_notice) return pending;

    uint64_t ids[MAX_MEMBERS]; int k = 0;
    int n = st_member_count(lb->id);
    if (n > MAX_MEMBERS) n = MAX_MEMBERS;
    for (int i = 0; i < n; i++) {
        uint64_t id = st_member_at(lb->id, i);
        if (id && id != me) ids[k++] = id;
    }
    /* forget the players that left: one who comes back is checked again */
    for (int i = 0; i < lb->nm; ) {
        int here = 0;
        for (int j = 0; j < k; j++) if (ids[j] == lb->m[i].id) { here = 1; break; }
        if (here) i++; else lb->m[i] = lb->m[--lb->nm];
    }
    for (int j = 0; j < k; j++) {
        Member *m = NULL;
        for (int i = 0; i < lb->nm; i++) if (lb->m[i].id == ids[j]) { m = &lb->m[i]; break; }
        if (!m) {
            if (lb->nm >= MAX_MEMBERS) continue;
            m = &lb->m[lb->nm++];
            m->id = ids[j]; m->first_ms = now; m->state = M_WAIT;
        }
        if (m->state != M_WAIT) continue;
        char mp[ID_LEN], mv[ID_LEN];
        clean(mp, sizeof mp, st_member_data(lb->id, m->id, KEY_PAYLOAD));
        clean(mv, sizeof mv, st_member_data(lb->id, m->id, KEY_LAUNCHER));
        if (mp[0]) {
            if (!strcmp(mp, C.payload)) {
                m->state = M_SAME;
                logf_("lobby %llu: player %llu has the same build (%s)", (unsigned long long)lb->id, (unsigned long long)m->id, mp);
            } else { m->state = M_TOLD; report_host(lb, m->id, mp, mv, now); }
        } else if (now - m->first_ms >= C.grace_ms) { m->state = M_TOLD; report_host(lb, m->id, "", "", now); }
        else pending = 1;
    }
    return pending;
}

static int guest_look(Lobby *lb, uint64_t owner, uint32_t now)
{
    if (lb->guest_done) return 0;
    char hp[ID_LEN], hv[ID_LEN];
    clean(hp, sizeof hp, st_lobby_data(lb->id, KEY_PAYLOAD));
    clean(hv, sizeof hv, st_lobby_data(lb->id, KEY_LAUNCHER));
    if (hp[0]) {
        lb->guest_done = 1;
        if (!strcmp(hp, C.payload))
            logf_("lobby %llu: host %llu has the same build (%s)", (unsigned long long)lb->id, (unsigned long long)owner, hp);
        else report_guest(lb, owner, hp, hv, now);
        return 0;
    }
    if (now - lb->since_ms >= C.grace_ms) { lb->guest_done = 1; report_guest(lb, owner, "", "", now); return 0; }
    return 1;
}

static void look(Lobby *lb, uint32_t now)
{
    lb->dirty = 0;
    lb->next_ms = now + 1000;
    uint64_t me = st_self(), owner = st_owner(lb->id);
    if (!me || !owner) {              /* Steam has no "you left" callback: no owner = not in it any more */
        if (++lb->misses >= 3) {
            logf_("lobby %llu: left", (unsigned long long)lb->id);
            memset(lb, 0, sizeof *lb);
        }
        return;
    }
    lb->misses = 0;
    if (owner != lb->owner) {
        if (lb->owner) logf_("lobby %llu: the owner changed to %llu", (unsigned long long)lb->id, (unsigned long long)owner);
        lb->owner = owner; lb->since_ms = now; lb->guest_done = 0; lb->published = 0; lb->set_tries = 0;
    }
    if (!lb->announced) {
        st_set_member_data(lb->id, KEY_PAYLOAD, C.payload);
        st_set_member_data(lb->id, KEY_LAUNCHER, C.launcher[0] ? C.launcher : "?");
        lb->announced = 1;
    }
    int pending = owner == me ? host_look(lb, me, now) : guest_look(lb, owner, now);
    if (!pending) lb->next_ms = now + 30000;      /* settled: only steam's callbacks, and a slow "still in it?" */
}

/* ---------------------------------------------------------------- steam's callbacks, as plain values */
static Lobby *lobby_find(uint64_t id)
{
    if (id) for (int i = 0; i < MAX_LOBBIES; i++) if (C.lobby[i].id == id) return &C.lobby[i];
    return NULL;
}

/* fresh = Steam said "you entered": whatever was known about that lobby is void (leaving wipes the
 * member data, and Steam has no callback for leaving) */
static void lobby_add(uint64_t id, uint32_t now, const char *why, int fresh)
{
    Lobby *lb = lobby_find(id);
    if (lb && !fresh) { lb->dirty = 1; return; }
    for (int i = 0; i < MAX_LOBBIES && !lb; i++) if (!C.lobby[i].id) lb = &C.lobby[i];
    if (!lb) {                        /* all slots taken: the one entered longest ago goes */
        lb = &C.lobby[0];
        for (int i = 1; i < MAX_LOBBIES; i++) if (now - C.lobby[i].since_ms > now - lb->since_ms) lb = &C.lobby[i];
        logf_("lobby %llu: dropped (more than %d lobbies at once)", (unsigned long long)lb->id, MAX_LOBBIES);
    }
    if (lb->id != id || lb->owner) logf_("lobby %llu: in it (%s)", (unsigned long long)id, why);
    memset(lb, 0, sizeof *lb);
    lb->id = id; lb->since_ms = now; lb->dirty = 1;
}

static void ev_enter(uint64_t lobby, uint32_t response, uint32_t now)   /* LobbyEnter_t; 1 = k_EChatRoomEnterResponseSuccess */
{
    if (lobby && response == 1) lobby_add(lobby, now, "entered", 1);
}
static void ev_created(uint64_t lobby, int32_t result, uint32_t now)    /* LobbyCreated_t; 1 = k_EResultOK */
{
    if (lobby && result == 1) lobby_add(lobby, now, "created", 1);
}
static void ev_data(uint64_t lobby, uint64_t member, uint32_t now)      /* LobbyDataUpdate_t */
{
    Lobby *lb = lobby_find(lobby);
    if (lb) { lb->dirty = 1; return; }
    /* member == lobby: lobby-level data. Steam also sends that for every lobby the session list asks
     * about, so it says nothing about being in it. A member's data only reaches the lobby's members. */
    if (lobby && member && member != lobby && st_owner(lobby)) lobby_add(lobby, now, "a member's data arrived", 0);
}
static void ev_chat(uint64_t lobby, uint32_t now)                       /* LobbyChatUpdate_t: someone came or went */
{
    Lobby *lb = lobby_find(lobby);
    if (lb) { lb->dirty = 1; return; }
    if (lobby && st_owner(lobby)) lobby_add(lobby, now, "a member came or went", 0);
}

/* after every SteamAPI_RunCallbacks of the game */
static void tick(uint32_t now)
{
    if (now - C.last_tick < 100) return;
    C.last_tick = now;
    for (int i = 0; i < MAX_LOBBIES; i++) {
        Lobby *lb = &C.lobby[i];
        if (lb->id && (lb->dirty || (int32_t)(now - lb->next_ms) >= 0)) look(lb, now);
    }
}

/* ---------------------------------------------------------------- PE lookups (a loaded 64-bit image) */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* data directory i: its rva (0 = none) and size; img = SizeOfImage */
static uint32_t pe_dir(const uint8_t *b, int i, uint32_t *size, uint32_t *img)
{
    if (!b || b[0] != 'M' || b[1] != 'Z') return 0;
    uint32_t lfa = rd32(b + 0x3C);
    if (lfa > 0x1000) return 0;
    const uint8_t *nt = b + lfa;
    if (rd32(nt) != 0x4550 || rd16(nt + 24) != 0x20B) return 0;             /* "PE", PE32+ */
    *img = rd32(nt + 24 + 56);
    if ((uint32_t)i >= rd32(nt + 24 + 108)) return 0;
    uint32_t rva = rd32(nt + 24 + 112 + 8 * i);
    *size = rd32(nt + 24 + 112 + 8 * i + 4);
    return (rva && rva < *img && *size <= *img - rva) ? rva : 0;
}

/* length of the 0-terminated text at rva, -1 if it runs out of the image or is longer than 255 */
static int pe_strlen(const uint8_t *b, uint32_t rva, uint32_t img)
{
    for (uint32_t n = 0; n < 256 && rva + n < img; n++) if (!b[rva + n]) return (int)n;
    return -1;
}

/* Address of an export. versioned = 0: the exact name. versioned = 1: `name` is a prefix that must be
 * followed by digits only - "SteamAPI_SteamUser_v" finds SteamAPI_SteamUser_v021 whatever number this
 * steam_api64.dll carries (the highest, should there be two). */
static void *pe_export(const uint8_t *b, const char *name, int versioned)
{
    uint32_t size = 0, img = 0, dir = pe_dir(b, 0, &size, &img);
    if (!dir || size < 40) return NULL;
    const uint8_t *d = b + dir;
    uint32_t nfn = rd32(d + 20), nnm = rd32(d + 24), afn = rd32(d + 28), anm = rd32(d + 32), aord = rd32(d + 36);
    if (nfn > 0x10000 || nnm > 0x10000 || afn >= img || anm >= img || aord >= img) return NULL;
    if (4 * nfn > img - afn || 4 * nnm > img - anm || 2 * nnm > img - aord) return NULL;
    size_t len = strlen(name);
    long best = -1; uint32_t best_rva = 0;
    for (uint32_t i = 0; i < nnm; i++) {
        uint32_t nrva = rd32(b + anm + 4 * i);
        int sl = nrva < img ? pe_strlen(b, nrva, img) : -1;
        if (sl < (int)len || memcmp(b + nrva, name, len)) continue;
        const char *t = (const char *)b + nrva + len;
        long ver = 0;
        if (versioned) {
            if (!*t) continue;
            for (; *t >= '0' && *t <= '9'; t++) if (ver < 100000) ver = ver * 10 + (*t - '0');
        }
        if (*t) continue;
        uint32_t ord = rd16(b + aord + 2 * i);
        if (ord >= nfn) continue;
        uint32_t frva = rd32(b + afn + 4 * ord);
        if (!frva || frva >= img || (frva >= dir && frva < dir + size)) continue;   /* none / forwarded */
        if (ver > best) { best = ver; best_rva = frva; }
    }
    return best >= 0 ? (void *)(uintptr_t)(b + best_rva) : NULL;
}

static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* the image's import slot (IAT entry) for dll!name, imported by name; NULL if it does not import it */
static uint64_t *pe_import_slot(uint8_t *b, const char *dll, const char *name)
{
    uint32_t size = 0, img = 0, dir = pe_dir(b, 1, &size, &img);
    if (!dir) return NULL;
    for (uint32_t o = dir; o + 20 <= dir + size; o += 20) {
        uint32_t oft = rd32(b + o), nm = rd32(b + o + 12), ft = rd32(b + o + 16);
        if (!nm) break;
        if (nm >= img || pe_strlen(b, nm, img) < 0 || !ieq((const char *)b + nm, dll)) continue;
        if (!oft || oft >= img || !ft || ft >= img) continue;                       /* no name table */
        for (uint32_t i = 0; oft + 8 * i + 8 <= img && ft + 8 * i + 8 <= img; i++) {
            uint64_t v = rd64(b + oft + 8 * i);
            if (!v) break;
            if (v >> 63) continue;                                                  /* by ordinal */
            uint32_t h = (uint32_t)v;                                               /* hint word, then the name */
            if (h + 2 >= img || pe_strlen(b, h + 2, img) < 0) continue;
            if (!strcmp((const char *)b + h + 2, name)) return (uint64_t *)(uintptr_t)(b + ft + 8 * i);
        }
    }
    return NULL;
}

static const char *const STEAM_NAMES[] = {       /* order = the fields of struct Steam; first three: versioned getters */
    "SteamAPI_SteamMatchmaking_v", "SteamAPI_SteamUser_v", "SteamAPI_SteamFriends_v",
    "SteamAPI_GetHSteamUser", "SteamAPI_RegisterCallback",
    "SteamAPI_ISteamUser_GetSteamID",
    "SteamAPI_ISteamMatchmaking_GetLobbyOwner", "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers",
    "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex", "SteamAPI_ISteamMatchmaking_GetLobbyData",
    "SteamAPI_ISteamMatchmaking_SetLobbyData", "SteamAPI_ISteamMatchmaking_GetLobbyMemberData",
    "SteamAPI_ISteamMatchmaking_SetLobbyMemberData", "SteamAPI_ISteamFriends_GetFriendPersonaName",
};
#define STEAM_N ((int)(sizeof STEAM_NAMES / sizeof STEAM_NAMES[0]))
#define STEAM_VERSIONED 3

#ifndef LC_TEST
/* ================================================================ windows side */
/* ---------------------------------------------------------------- log */
static wchar_t g_ini[MAX_PATH], g_logpath[MAX_PATH];
static FILE   *g_log;
static int     cfg_log = 1;
static void logf_(const char *fmt, ...)
{
    if (!cfg_log) return;
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = _wfopen(g_logpath, L"w");          /* one file per run */
        if (!g_log) return;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02u:%02u:%02u] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- steam_api64.dll, flat api */
typedef struct {
    void       *(*Matchmaking)(void);
    void       *(*User)(void);
    void       *(*Friends)(void);
    int         (*GetHSteamUser)(void);
    void        (*RegisterCallback)(void *cb, int id);
    uint64_t    (*GetSteamID)(void *user);
    uint64_t    (*GetLobbyOwner)(void *mm, uint64_t lobby);
    int         (*GetNumLobbyMembers)(void *mm, uint64_t lobby);
    uint64_t    (*GetLobbyMemberByIndex)(void *mm, uint64_t lobby, int i);
    const char *(*GetLobbyData)(void *mm, uint64_t lobby, const char *key);
    unsigned char (*SetLobbyData)(void *mm, uint64_t lobby, const char *key, const char *val);
    const char *(*GetLobbyMemberData)(void *mm, uint64_t lobby, uint64_t user, const char *key);
    void        (*SetLobbyMemberData)(void *mm, uint64_t lobby, const char *key, const char *val);
    const char *(*GetFriendPersonaName)(void *fr, uint64_t user);
} Steam;
static union { Steam f; void *p[STEAM_N]; } S;
_Static_assert(sizeof(Steam) == sizeof(void *) * STEAM_N, "struct Steam and STEAM_NAMES must list the same functions");

static uint64_t st_self(void)                    { void *u = S.f.User();        return u ? S.f.GetSteamID(u) : 0; }
static uint64_t st_owner(uint64_t lobby)         { void *m = S.f.Matchmaking(); return m ? S.f.GetLobbyOwner(m, lobby) : 0; }
static int      st_member_count(uint64_t lobby)  { void *m = S.f.Matchmaking(); return m ? S.f.GetNumLobbyMembers(m, lobby) : 0; }
static uint64_t st_member_at(uint64_t lobby, int i) { void *m = S.f.Matchmaking(); return m ? S.f.GetLobbyMemberByIndex(m, lobby, i) : 0; }
static const char *st_lobby_data(uint64_t lobby, const char *key)
{
    void *m = S.f.Matchmaking();
    const char *v = m ? S.f.GetLobbyData(m, lobby, key) : NULL;
    return v ? v : "";
}
static int st_set_lobby_data(uint64_t lobby, const char *key, const char *val)
{
    void *m = S.f.Matchmaking();
    return m ? S.f.SetLobbyData(m, lobby, key, val) != 0 : 0;
}
static const char *st_member_data(uint64_t lobby, uint64_t user, const char *key)
{
    void *m = S.f.Matchmaking();
    const char *v = m ? S.f.GetLobbyMemberData(m, lobby, user, key) : NULL;
    return v ? v : "";
}
static void st_set_member_data(uint64_t lobby, const char *key, const char *val)
{
    void *m = S.f.Matchmaking();
    if (m) S.f.SetLobbyMemberData(m, lobby, key, val);
}
static void st_name(uint64_t user, wchar_t *out, size_t cap)
{
    char utf8[96]; size_t n = 0;
    void *f = S.f.Friends();
    const char *s = f ? S.f.GetFriendPersonaName(f, user) : NULL;
    if (s) for (; s[n] && n + 1 < sizeof utf8 && n + 1 < cap; n++) utf8[n] = s[n];   /* n bytes -> at most n wide chars */
    utf8[n] = 0;
    out[0] = 0;
    if (n && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, (int)cap) <= 0) out[0] = 0;
    out[cap - 1] = 0;
    for (wchar_t *p = out; *p; p++) if (*p < 32) *p = L' ';
    if (!out[0]) { WBuf w = { out, cap, 0 }; w_str(&w, L"A player"); }
}

/* ---------------------------------------------------------------- once per finding, across the game and its host server */
static struct { uint64_t lobby, who; HANDLE h; } g_claims[32];
static int g_claim_next;
static int claim(uint64_t lobby, uint64_t who)
{
    for (int i = 0; i < 32; i++)
        if (g_claims[i].h && g_claims[i].lobby == lobby && g_claims[i].who == who) return 1;   /* mine already */
    static const wchar_t hex[] = L"0123456789abcdef";
    wchar_t nm[80]; WBuf w = { nm, 80, 0 };
    w_str(&w, L"Local\\gem_lobbycheck_");
    for (int i = 60; i >= 0; i -= 4) nm[w.n++] = hex[(lobby >> i) & 15];
    nm[w.n++] = L'_';
    for (int i = 60; i >= 0; i -= 4) nm[w.n++] = hex[(who >> i) & 15];
    nm[w.n] = 0;
    HANDLE h = CreateMutexW(NULL, FALSE, nm);
    if (!h) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); return 0; }    /* the other process has it */
    /* kept open so that the other process sees it; the oldest of 32 is let go */
    if (g_claims[g_claim_next].h) CloseHandle(g_claims[g_claim_next].h);
    g_claims[g_claim_next].lobby = lobby; g_claims[g_claim_next].who = who; g_claims[g_claim_next].h = h;
    g_claim_next = (g_claim_next + 1) % 32;
    return 1;
}

/* ---------------------------------------------------------------- message box on its own thread */
static int              cfg_popup = 1;
static CRITICAL_SECTION g_pq_lock;
static wchar_t          g_pq[8][MSG_LEN];
static int              g_pq_head, g_pq_n, g_pq_running;

static DWORD WINAPI popup_thread(LPVOID arg)
{
    (void)arg;
    static wchar_t text[MSG_LEN];               /* one such thread at a time */
    for (;;) {
        EnterCriticalSection(&g_pq_lock);
        if (!g_pq_n) { g_pq_running = 0; LeaveCriticalSection(&g_pq_lock); return 0; }
        memcpy(text, g_pq[g_pq_head], sizeof text);
        g_pq_head = (g_pq_head + 1) % 8; g_pq_n--;
        LeaveCriticalSection(&g_pq_lock);
        MessageBoxW(NULL, text, L"Indomitus Launcher - different builds in this lobby",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
}

static void popup(const wchar_t *text)
{
    if (!cfg_popup) { logf_("(ShowMessage=0: no message box)"); return; }
    int start = 0;
    EnterCriticalSection(&g_pq_lock);
    if (g_pq_n < 8) {
        wchar_t *dst = g_pq[(g_pq_head + g_pq_n) % 8];
        WBuf w = { dst, MSG_LEN, 0 }; dst[0] = 0;
        w_str(&w, text);
        g_pq_n++;
        if (!g_pq_running) { g_pq_running = 1; start = 1; }
    }
    LeaveCriticalSection(&g_pq_lock);
    if (!start) return;
    HANDLE h = CreateThread(NULL, 0, popup_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else {
        EnterCriticalSection(&g_pq_lock); g_pq_running = 0; LeaveCriticalSection(&g_pq_lock);
        logf_("message box thread could not be started (%lu)", GetLastError());
    }
}

/* ---------------------------------------------------------------- steam callback listeners */
/* CCallbackBase as steam_api64.dll (MSVC) lays it out. SteamAPI_RegisterCallback writes flags and id.
 * Vtable order (MSVC puts overloads in reverse): Run(data, ioFailure, call), Run(data), GetCallbackSizeBytes. */
typedef struct CbObj { void **vt; uint8_t flags; int id; int size; void (*fn)(const uint8_t *data); } CbObj;
_Static_assert(offsetof(CbObj, flags) == 8 && offsetof(CbObj, id) == 12, "CCallbackBase layout");

static void cb_run_result(CbObj *o, void *data, int io_failure, uint64_t call) { (void)io_failure; (void)call; if (data) o->fn(data); }
static void cb_run(CbObj *o, void *data)                                       { if (data) o->fn(data); }
static int  cb_size(CbObj *o)                                                  { return o->size; }
static void *g_cb_vt[3] = { (void *)cb_run_result, (void *)cb_run, (void *)cb_size };

/* callback structs, pack 8:
 *   LobbyEnter_t       504, 24 bytes: u64 lobby, u32 permissions, bool locked, u32 response (+16)
 *   LobbyDataUpdate_t  505, 24 bytes: u64 lobby, u64 member, u8 success
 *   LobbyChatUpdate_t  506, 32 bytes: u64 lobby, u64 user changed, u64 user making the change, u32 change
 *   LobbyCreated_t     513, 16 bytes: i32 result, u64 lobby (+8) */
static void h_enter(const uint8_t *d)   { ev_enter(rd64(d), rd32(d + 16), GetTickCount()); }
static void h_data(const uint8_t *d)    { ev_data(rd64(d), rd64(d + 8), GetTickCount()); }
static void h_chat(const uint8_t *d)    { ev_chat(rd64(d), GetTickCount()); }
static void h_created(const uint8_t *d) { ev_created(rd64(d + 8), (int32_t)rd32(d), GetTickCount()); }

static CbObj g_cb[4] = {
    { g_cb_vt, 0, 0, 24, h_enter }, { g_cb_vt, 0, 0, 24, h_data }, { g_cb_vt, 0, 0, 32, h_chat }, { g_cb_vt, 0, 0, 16, h_created },
};
static const int g_cb_id[4] = { 504, 505, 506, 513 };

/* ---------------------------------------------------------------- the hook: SteamAPI_RunCallbacks of the exe */
typedef void (*RunCallbacksFn)(void);
static RunCallbacksFn g_run_orig;
static volatile LONG  g_inside;
static int            g_registered;

static void hook_run_callbacks(void)
{
    /* a second thread or a nested call just runs Steam's function */
    if (InterlockedCompareExchange(&g_inside, 1, 0) != 0) { g_run_orig(); return; }
    if (!g_registered && S.f.GetHSteamUser() != 0) {
        /* here and now: this is the thread that walks Steam's listener table, and it is not in it */
        for (int i = 0; i < 4; i++) S.f.RegisterCallback(&g_cb[i], g_cb_id[i]);
        g_registered = 1;
        logf_("Steam is up: listening to the lobby callbacks 504 / 505 / 506 / 513 (steam id %llu)", (unsigned long long)st_self());
    }
    g_run_orig();
    if (g_registered) tick(GetTickCount());
    InterlockedExchange(&g_inside, 0);
}

/* ---------------------------------------------------------------- install */
static void install(const char *exe_name)
{
    logf_("[GEM LobbyCheck %s] %s | launcher version %s, build %s | HostNotice=%d GraceSeconds=%u ShowMessage=%d",
          LC_VERSION, exe_name, C.launcher[0] ? C.launcher : "?", C.payload[0] ? C.payload : "(none)",
          C.host_notice, C.grace_ms / 1000, cfg_popup);
    if (!C.payload[0]) {
        logf_("OFF: INDOMITUS_PAYLOAD is not set - the game was not started by an Indomitus Launcher that "
              "passes its build code. Nothing hooked.");
        return;
    }
    uint8_t *steam = (uint8_t *)GetModuleHandleW(L"steam_api64.dll");
    if (!steam) { logf_("OFF: steam_api64.dll is not loaded in this process. Nothing hooked."); return; }
    for (int i = 0; i < STEAM_N; i++) {
        S.p[i] = pe_export(steam, STEAM_NAMES[i], i < STEAM_VERSIONED);
        if (!S.p[i]) {
            logf_("OFF: steam_api64.dll does not export %s%s (another Steam SDK than 1.067 ships). Nothing hooked.",
                  STEAM_NAMES[i], i < STEAM_VERSIONED ? "<number>" : "");
            return;
        }
    }
    uint8_t *exe = (uint8_t *)GetModuleHandleW(NULL);
    uint64_t *slot = pe_import_slot(exe, "steam_api64.dll", "SteamAPI_RunCallbacks");
    if (!slot) { logf_("OFF: %s does not import steam_api64!SteamAPI_RunCallbacks. Nothing hooked.", exe_name); return; }
    void *real = pe_export(steam, "SteamAPI_RunCallbacks", 0);

    InitializeCriticalSection(&g_pq_lock);
    HMODULE self;
    int pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    (LPCWSTR)(void *)&hook_run_callbacks, &self) != 0;
    g_run_orig = (RunCallbacksFn)(uintptr_t)*slot;
    DWORD old;
    if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) {
        logf_("OFF: VirtualProtect on the import slot failed (%lu). Nothing hooked.", GetLastError());
        return;
    }
    *slot = (uint64_t)(uintptr_t)&hook_run_callbacks;        /* one aligned pointer: seen whole by any thread */
    VirtualProtect(slot, 8, old, &old);
    logf_("ON: import slot exe+0x%llX (SteamAPI_RunCallbacks) -> this dll%s%s",
          (unsigned long long)((uint8_t *)slot - exe),
          (void *)g_run_orig == real ? "" : "; the slot already led somewhere else than steam_api64 (another hook, kept in the chain)",
          pinned ? "" : " (could not pin the dll)");
    logf_("waiting for Steam and for a lobby. Nothing in the simulation is touched.");
}

static void load_config(void)
{
    char buf[256];
    cfg_log       = GetPrivateProfileIntW(L"LobbyCheck", L"Log", 1, g_ini) != 0;
    cfg_popup     = GetPrivateProfileIntW(L"LobbyCheck", L"ShowMessage", 1, g_ini) != 0;
    C.host_notice = GetPrivateProfileIntW(L"LobbyCheck", L"HostNotice", 1, g_ini) != 0;
    UINT g = GetPrivateProfileIntW(L"LobbyCheck", L"GraceSeconds", 10, g_ini);
    if (g < 2) g = 2;
    if (g > 120) g = 120;
    C.grace_ms = g * 1000;
    buf[0] = 0;
    DWORD n = GetEnvironmentVariableA("INDOMITUS_PAYLOAD", buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) buf[0] = 0;
    clean(C.payload, sizeof C.payload, buf);
    buf[0] = 0;
    n = GetEnvironmentVariableA("INDOMITUS_LAUNCHER_VERSION", buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) buf[0] = 0;
    clean(C.launcher, sizeof C.launcher, buf);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);

    wchar_t exe[MAX_PATH], *nm;
    DWORD ne = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (ne == 0 || ne >= MAX_PATH) return TRUE;
    nm = wcsrchr(exe, L'\\'); nm = nm ? nm + 1 : exe;
    int server = _wcsicmp(nm, L"call_to_arms_server.exe") == 0;
    if (!server && _wcsicmp(nm, L"call_to_arms.exe") != 0) return TRUE;   /* editor, tools, anything else: inert */

    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return TRUE;
    wchar_t *dot = wcsrchr(path, L'.');
    wchar_t *sl = wcsrchr(path, L'\\');
    if (dot && (!sl || dot > sl)) *dot = 0;
    { WBuf w = { g_ini, MAX_PATH, 0 }; w_str(&w, path); w_str(&w, L".ini"); }
    { WBuf w = { g_logpath, MAX_PATH, 0 }; w_str(&w, path); w_str(&w, server ? L"_server.log" : L".log"); }

    load_config();
    if (!GetPrivateProfileIntW(L"LobbyCheck", L"Enabled", 1, g_ini)) {
        logf_("[GEM LobbyCheck %s] Enabled=0 - nothing hooked", LC_VERSION);
        return TRUE;
    }

    /* one copy per process */
    static const wchar_t hex[] = L"0123456789abcdef";
    wchar_t mx[64]; WBuf w = { mx, 64, 0 };
    w_str(&w, L"Local\\gem_lobbycheck_pid_");
    DWORD pid = GetCurrentProcessId();
    for (int i = 28; i >= 0; i -= 4) mx[w.n++] = hex[(pid >> i) & 15];
    mx[w.n] = 0;
    HANDLE h = CreateMutexW(NULL, FALSE, mx);      /* kept open for the process lifetime */
    if (h && GetLastError() == ERROR_ALREADY_EXISTS) {
        logf_("[GEM LobbyCheck %s] another copy is already loaded in this process - this one stays off", LC_VERSION);
        CloseHandle(h);
        return TRUE;
    }

    char exe_a[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, nm, -1, exe_a, sizeof exe_a, NULL, NULL);
    install(exe_a);
    return TRUE;
}
#else
/* ================================================================ offline test (build.bat test) */
#include "../test/lc_test.inc"
#endif
