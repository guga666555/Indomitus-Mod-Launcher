/* GEM LobbyCheck - offline test, included by src/gem_lobbycheck.c when built with -DLC_TEST.
 *   lc_test [folder with steam_api64.dll, call_to_arms.exe, call_to_arms_server.exe] [-v]
 * 1. lobby scenarios: a fake Steam with lobbies, and several "machines" that each run the dll's core
 *    (the code above "windows side") with their own state, exactly as the hook would drive it.
 * 2. with a folder: the export / import lookups the dll does at load, on the real files.
 * Portable C: runs on Windows (build.bat test) and on Linux. */

/* ---------------------------------------------------------------- fake steam */
#define T_MAXM 8
typedef struct { uint64_t id; char pay[64], ver[64]; } TMember;
typedef struct { uint64_t id, owner; char pay[200], ver[200]; int nm; TMember m[T_MAXM]; } TLobby;
typedef struct { uint64_t lobby, member; } TEvent;

static TLobby   T_lobby[3];
static TEvent   T_ev[256]; static int T_nev;
static uint64_t T_self;
static uint32_t T_now;
static int      T_calls, T_popups, T_claim = 1, T_refuse_set, T_verbose, T_fail, T_checks;
static wchar_t  T_last[MSG_LEN];

static TLobby *t_lobby(uint64_t id) { for (int i = 0; i < 3; i++) if (id && T_lobby[i].id == id) return &T_lobby[i]; return NULL; }
static TMember *t_member(TLobby *l, uint64_t id) { if (l) for (int i = 0; i < l->nm; i++) if (l->m[i].id == id) return &l->m[i]; return NULL; }
static void t_event(uint64_t lobby, uint64_t member) { if (T_nev < 256) { T_ev[T_nev].lobby = lobby; T_ev[T_nev].member = member; T_nev++; } }

static uint64_t st_self(void) { T_calls++; return T_self; }
static uint64_t st_owner(uint64_t lobby) { T_calls++; TLobby *l = t_lobby(lobby); return t_member(l, T_self) ? l->owner : 0; }
static int st_member_count(uint64_t lobby) { T_calls++; TLobby *l = t_lobby(lobby); return l ? l->nm : 0; }
static uint64_t st_member_at(uint64_t lobby, int i)
{
    T_calls++;
    TLobby *l = t_lobby(lobby);
    return (t_member(l, T_self) && i >= 0 && i < l->nm) ? l->m[i].id : 0;
}
static const char *st_lobby_data(uint64_t lobby, const char *key)
{
    T_calls++;
    TLobby *l = t_lobby(lobby);
    if (!l) return "";
    return !strcmp(key, KEY_PAYLOAD) ? l->pay : !strcmp(key, KEY_LAUNCHER) ? l->ver : "";
}
static int st_set_lobby_data(uint64_t lobby, const char *key, const char *val)
{
    T_calls++;
    TLobby *l = t_lobby(lobby);
    if (!l || l->owner != T_self || T_refuse_set) return 0;        /* owner only, like Steam */
    char *dst = !strcmp(key, KEY_PAYLOAD) ? l->pay : !strcmp(key, KEY_LAUNCHER) ? l->ver : NULL;
    if (!dst) return 0;
    snprintf(dst, 200, "%s", val);
    t_event(lobby, lobby);
    return 1;
}
static const char *st_member_data(uint64_t lobby, uint64_t user, const char *key)
{
    T_calls++;
    TLobby *l = t_lobby(lobby);
    TMember *m = t_member(l, T_self) ? t_member(l, user) : NULL;
    if (!m) return "";
    return !strcmp(key, KEY_PAYLOAD) ? m->pay : !strcmp(key, KEY_LAUNCHER) ? m->ver : "";
}
static void st_set_member_data(uint64_t lobby, const char *key, const char *val)
{
    T_calls++;
    TLobby *l = t_lobby(lobby);
    TMember *m = t_member(l, T_self);
    if (!m) return;
    char *dst = !strcmp(key, KEY_PAYLOAD) ? m->pay : !strcmp(key, KEY_LAUNCHER) ? m->ver : NULL;
    if (!dst) return;
    snprintf(dst, 64, "%s", val);
    t_event(lobby, T_self);
}
static void st_name(uint64_t user, wchar_t *out, size_t cap)
{
    WBuf w = { out, cap, 0 }; out[0] = 0;
    w_str(&w, L"Player"); wchar_t d[2] = { (wchar_t)(L'0' + (int)(user % 10)), 0 }; w_str(&w, d);
}
static int claim(uint64_t lobby, uint64_t who) { (void)lobby; (void)who; return T_claim; }
static void popup(const wchar_t *text)
{
    T_popups++;
    WBuf w = { T_last, MSG_LEN, 0 }; T_last[0] = 0; w_str(&w, text);
    if (T_verbose) printf("      ---- message box ----\n%ls\n      ---------------------\n", text);
}
static void logf_(const char *fmt, ...)
{
    if (!T_verbose) return;
    va_list ap; va_start(ap, fmt);
    printf("      [%llu @%6u] ", (unsigned long long)T_self, T_now); vprintf(fmt, ap); printf("\n");
    va_end(ap);
}

/* ---------------------------------------------------------------- machines */
typedef struct { uint64_t id; Core core; int popups; wchar_t last[MSG_LEN]; } Machine;
static Machine T_m[4]; static int T_nm;

static void load(Machine *m) { C = m->core; T_self = m->id; T_popups = m->popups; memcpy(T_last, m->last, sizeof T_last); }
static void save(Machine *m) { m->core = C; m->popups = T_popups; memcpy(m->last, T_last, sizeof T_last); }

static Machine *machine(uint64_t id, const char *payload, const char *launcher)   /* "" payload = a game without the dll */
{
    Machine *m = &T_m[T_nm++];
    memset(m, 0, sizeof *m);
    m->id = id;
    clean(m->core.payload, ID_LEN, payload); clean(m->core.launcher, ID_LEN, launcher);
    m->core.host_notice = 1; m->core.grace_ms = 8000;
    return m;
}
static int active(const Machine *m) { return m->core.payload[0] != 0; }   /* without a build code the dll hooks nothing */

static void world_reset(void)
{
    memset(T_lobby, 0, sizeof T_lobby); memset(T_m, 0, sizeof T_m);
    T_nm = 0; T_nev = 0; T_claim = 1; T_refuse_set = 0; T_now += 1000000;      /* also crosses the 32-bit wrap */
}

/* Steam's callbacks reach every member's game; only machines with the dll react */
static void deliver(void)
{
    while (T_nev) {
        TEvent e = T_ev[0];
        memmove(T_ev, T_ev + 1, (size_t)(--T_nev) * sizeof e);
        TLobby *l = t_lobby(e.lobby);
        for (int i = 0; i < T_nm; i++) {
            Machine *m = &T_m[i];
            if (!active(m) || !t_member(l, m->id)) continue;
            load(m); ev_data(e.lobby, e.member, T_now); save(m);
        }
    }
}
static void chat_to_all(uint64_t lobby)
{
    TLobby *l = t_lobby(lobby);
    for (int i = 0; i < T_nm; i++) {
        Machine *m = &T_m[i];
        if (!active(m) || !t_member(l, m->id)) continue;
        load(m); ev_chat(lobby, T_now); save(m);
    }
}
static void w_create(Machine *host, uint64_t lobby)
{
    TLobby *l = NULL;
    for (int i = 0; i < 3 && !l; i++) if (!T_lobby[i].id) l = &T_lobby[i];
    memset(l, 0, sizeof *l);
    l->id = lobby; l->owner = host->id; l->nm = 1; l->m[0].id = host->id;
    if (active(host)) { load(host); ev_created(lobby, 1, T_now); ev_enter(lobby, 1, T_now); ev_data(lobby, lobby, T_now); save(host); }
}
static void w_join(Machine *g, uint64_t lobby)
{
    TLobby *l = t_lobby(lobby);
    memset(&l->m[l->nm], 0, sizeof l->m[0]);
    l->m[l->nm++].id = g->id;
    if (active(g)) { load(g); ev_enter(lobby, 1, T_now); ev_data(lobby, lobby, T_now); save(g); }
    chat_to_all(lobby);
}
static void w_leave(Machine *g, uint64_t lobby)
{
    TLobby *l = t_lobby(lobby);
    TMember *m = t_member(l, g->id);
    if (!m) return;
    l->nm--;
    memmove(m, m + 1, (size_t)(&l->m[l->nm] - m) * sizeof *m);                  /* order kept */
    if (l->owner == g->id && l->nm) { l->owner = l->m[0].id; t_event(lobby, lobby); }
    chat_to_all(lobby);
}
static void run(uint32_t ms)       /* every game runs its Steam callbacks 10 times a second */
{
    for (uint32_t t = 0; t < ms; t += 100) {
        T_now += 100;
        deliver();
        for (int i = 0; i < T_nm; i++) if (active(&T_m[i])) { load(&T_m[i]); tick(T_now); save(&T_m[i]); }
    }
    deliver();
}

#define CHECK(cond, what) do { T_checks++; if (!(cond)) { T_fail++; printf("  FAIL  %s  (line %d)\n", what, __LINE__); } \
                               else if (T_verbose) printf("  ok    %s\n", what); } while (0)
#define HAS(m, text) (wcsstr((m)->last, text) != NULL)

static void scenarios(void)
{
    Machine *h, *a, *b;

    printf("scenario 1: same build everywhere\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "09d070d0", "0.9.13"); b = machine(103, "09d070d0", "0.9.13");
    w_create(h, 9001); run(500);
    CHECK(!strcmp(T_lobby[0].pay, "09d070d0") && !strcmp(T_lobby[0].ver, "0.9.13"), "host wrote build and version into the lobby data");
    w_join(a, 9001); run(300); w_join(b, 9001); run(20000);
    CHECK(!strcmp(t_member(&T_lobby[0], 102)->pay, "09d070d0"), "guest wrote its build into its member data");
    CHECK(h->popups == 0 && a->popups == 0 && b->popups == 0, "no message anywhere");
    CHECK(a->core.lobby[0].guest_done == 1 && h->core.lobby[0].m[0].state == M_SAME && h->core.lobby[0].nm == 2, "everything compared");

    printf("scenario 2: the guest's launcher is older\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "5f3a9c21", "0.9.12");
    w_create(h, 9001); run(500); w_join(a, 9001); run(1000);
    CHECK(a->popups == 1 && HAS(a, L"Host:  version 0.9.13, build 09d070d0") && HAS(a, L"You:   version 0.9.12, build 5f3a9c21"), "guest is told, both builds named");
    CHECK(HAS(a, L"Your launcher is the older one"), "guest is told his is the older one");
    CHECK(h->popups == 1 && HAS(h, L"Player2:  version 0.9.12, build 5f3a9c21") && HAS(h, L"Their launcher is the older one"), "host is told about the player");
    run(120000);
    CHECK(a->popups == 1 && h->popups == 1, "told once, not again while they stay");

    printf("scenario 3: the host's launcher is older; same version number but another build\n");
    world_reset();
    h = machine(101, "5f3a9c21", "0.9.9"); a = machine(102, "09d070d0", "0.9.10"); b = machine(103, "aaaaaaaa", "0.9.9");
    w_create(h, 9001); run(500); w_join(a, 9001); w_join(b, 9001); run(1000);
    CHECK(a->popups == 1 && HAS(a, L"The host's launcher is the older one"), "0.9.10 guest: host is older (numbers, not text order)");
    CHECK(b->popups == 1 && HAS(b, L"Both of you have to use the same launcher"), "same version, other build: neutral advice");
    CHECK(h->popups == 2, "host is told about both players");

    printf("scenario 4: the host has an older launcher without this dll\n");
    world_reset();
    h = machine(101, "", ""); a = machine(102, "09d070d0", "0.9.13");
    w_create(h, 9001); run(500); w_join(a, 9001); run(7000);
    CHECK(a->popups == 0, "nothing before the grace time is over");
    run(1500);
    CHECK(a->popups == 1 && HAS(a, L"does not report an Indomitus Launcher build") && HAS(a, L"You:   version 0.9.13, build 09d070d0"), "then the guest is told the host reports nothing");
    run(60000);
    CHECK(a->popups == 1, "once");

    printf("scenario 5: a player with an older launcher without this dll joins\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "", "");
    w_create(h, 9001); run(500); w_join(a, 9001); run(7000);
    CHECK(h->popups == 0, "host: nothing before the grace time is over");
    run(1500);
    CHECK(h->popups == 1 && HAS(h, L"A player in your lobby does not report") && HAS(h, L"Player2"), "then the host is told, by name");

    printf("scenario 6: the host's data arrives late (within the grace time)\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "09d070d0", "0.9.13");
    w_create(h, 9001);                               /* host's game has not run its callbacks yet */
    w_join(a, 9001);
    for (int i = 0; i < 30; i++) { T_now += 100; load(a); tick(T_now); save(a); }      /* 3 s: only the guest runs */
    CHECK(a->popups == 0 && a->core.lobby[0].guest_done == 0, "guest waits");
    run(2000);
    CHECK(a->popups == 0 && a->core.lobby[0].guest_done == 1 && h->popups == 0, "host publishes, guest compares, no message");

    printf("scenario 7: leave and come back; the 60 s repeat guard\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "5f3a9c21", "0.9.12");
    w_create(h, 9001); run(500); w_join(a, 9001); run(1000);
    CHECK(a->popups == 1 && h->popups == 1, "first join: both told");
    w_leave(a, 9001); run(5000);
    CHECK(h->core.lobby[0].nm == 0, "host: the player is forgotten after leaving");
    w_join(a, 9001); run(2000);
    CHECK(a->popups == 1 && h->popups == 1, "back within a minute: no second message");
    CHECK(!strcmp(t_member(&T_lobby[0], 102)->pay, "5f3a9c21") && a->core.lobby[0].guest_done, "but he wrote his member data again and compared again");
    w_leave(a, 9001); run(70000);
    CHECK(a->core.lobby[0].id == 0, "guest: the lobby is forgotten some time after leaving");
    w_join(a, 9001); run(2000);
    CHECK(a->popups == 2 && h->popups == 2, "back after more than a minute: told again");
    w_leave(a, 9001); run(70000);
    clean(a->core.payload, ID_LEN, "09d070d0"); clean(a->core.launcher, ID_LEN, "0.9.13");      /* he updated */
    w_join(a, 9001); run(2000);
    CHECK(a->popups == 2 && h->popups == 2 && h->core.lobby[0].m[0].state == M_SAME, "back with the host's build: fine");

    printf("scenario 8: the session list (data of lobbies we are not in)\n");
    world_reset();
    a = machine(102, "09d070d0", "0.9.13");
    load(a); T_calls = 0;
    for (uint64_t id = 50000; id < 50500; id++) ev_data(id, id, T_now);
    for (int i = 0; i < 100; i++) { T_now += 100; tick(T_now); }
    save(a);
    CHECK(T_calls == 0, "500 foreign lobby updates cost no Steam call");
    CHECK(a->core.lobby[0].id == 0 && a->popups == 0, "and are not taken for a lobby we are in");

    printf("scenario 9: two lobbies at once with the same owner (party + session)\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "5f3a9c21", "0.9.12");
    w_create(h, 9001); w_create(h, 9002); run(500);
    CHECK(!strcmp(T_lobby[0].pay, "09d070d0") && !strcmp(T_lobby[1].pay, "09d070d0"), "host published in both");
    w_join(a, 9001); w_join(a, 9002); run(2000);
    CHECK(a->popups == 1 && h->popups == 1, "one message each, not one per lobby");
    CHECK(a->core.lobby[0].guest_done && a->core.lobby[1].guest_done, "both lobbies compared");

    printf("scenario 10: the host leaves, a guest becomes the owner\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "09d070d0", "0.9.13"); b = machine(103, "5f3a9c21", "0.9.12");
    w_create(h, 9001); run(500); w_join(a, 9001); w_join(b, 9001); run(1000);
    CHECK(a->popups == 0 && b->popups == 1 && h->popups == 1, "before: only the odd one and the host are told");
    w_leave(h, 9001); run(2000);
    CHECK(T_lobby[0].owner == 102 && a->core.lobby[0].published == 1, "the new owner publishes its build");
    CHECK(a->popups == 1 && HAS(a, L"Player3"), "and is told about the odd one");
    CHECK(b->popups == 2 && HAS(b, L"Host:  version 0.9.13, build 09d070d0"), "the odd one is compared with the new owner (another player: told again)");

    printf("scenario 11: hostile values from another player\n");
    world_reset();
    h = machine(101, "", ""); a = machine(102, "09d070d0", "0.9.13");
    w_create(h, 9001);
    memset(T_lobby[0].pay, 'A', 199); T_lobby[0].pay[199] = 0;
    snprintf(T_lobby[0].ver, 200, "%%s%%n%%ls\n\x01\xff<script>");
    w_join(a, 9001); run(1000);
    CHECK(a->popups == 1 && HAS(a, L"build AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n"), "a 199-character build code is cut to 39");
    CHECK(!HAS(a, L"%") && !HAS(a, L"<") && !HAS(a, L"\x01"), "format and control characters are replaced");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "", "");
    w_create(h, 9001); run(500); w_join(a, 9001);
    snprintf(t_member(&T_lobby[0], 102)->pay, 64, "09d070d0 "); t_event(9001, 102);     /* "almost" the same */
    run(1000);
    CHECK(h->popups == 1 && HAS(h, L"build 09d070d0?"), "a build code with a trailing blank is not the same build");

    printf("scenario 12: switches and edge cases\n");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "5f3a9c21", "0.9.12");
    h->core.host_notice = 0;
    w_create(h, 9001); run(500); w_join(a, 9001); run(20000);
    CHECK(h->popups == 0 && a->popups == 1 && !strcmp(T_lobby[0].pay, "09d070d0"), "HostNotice=0: host still publishes, only the guest is told");
    world_reset();
    h = machine(101, "09d070d0", ""); a = machine(102, "5f3a9c21", "");
    w_create(h, 9001); run(500); w_join(a, 9001); run(1000);
    CHECK(a->popups == 1 && HAS(a, L"Host:  build 09d070d0\nYou:   build 5f3a9c21") && HAS(a, L"Both of you"), "no version known: build codes only");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "5f3a9c21", "0.9.12");
    T_claim = 0;
    w_create(h, 9001); run(500); w_join(a, 9001); run(2000);
    CHECK(a->popups == 0 && h->popups == 0 && a->core.lobby[0].guest_done, "the other process shows the message: this one stays quiet");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "09d070d0", "0.9.13");
    T_refuse_set = 1;
    w_create(h, 9001); run(15000);
    CHECK(h->core.lobby[0].published == 2 && h->core.lobby[0].set_tries == SET_TRIES, "Steam refuses the lobby data: 10 tries, then given up");
    T_refuse_set = 0;
    world_reset();
    a = machine(102, "09d070d0", "0.9.13");
    load(a); ev_enter(9001, 2, T_now); ev_created(9002, 2, T_now); ev_enter(0, 1, T_now); ev_chat(777, T_now); ev_data(778, 5, T_now); save(a);
    CHECK(a->core.lobby[0].id == 0, "failed enter / create and lobbies we are not in are ignored");
    world_reset();
    h = machine(101, "09d070d0", "0.9.13"); a = machine(102, "5f3a9c21", "0.9.12");
    w_create(h, 9001); run(500);
    {   /* the guest's enter callback is lost: a member's data still shows him the lobby */
        TLobby *l = t_lobby(9001); memset(&l->m[l->nm], 0, sizeof l->m[0]); l->m[l->nm++].id = 102;
        load(h); st_set_member_data(9001, KEY_LAUNCHER, "0.9.13"); save(h);
    }
    run(1000);
    CHECK(a->core.lobby[0].id == 9001 && a->popups == 1, "lobby found through a member's data alone");

    CHECK(version_cmp("0.9.10", "0.9.9") > 0 && version_cmp("1.0", "1.0.0") == 0 && version_cmp("0.9.12", "0.10") < 0 &&
          version_cmp("v2", "1.99") > 0 && version_cmp("", "") == 0, "version comparison");
    {
        char c[8]; clean(c, sizeof c, "abcdefghijkl"); CHECK(!strcmp(c, "abcdefg"), "clean() cuts at the buffer");
        clean(c, sizeof c, NULL); CHECK(c[0] == 0, "clean(NULL) is empty");
        wchar_t t[8]; WBuf w = { t, 8, 0 }; t[0] = 0; w_str(&w, L"0123456789"); w_asc(&w, "abc");
        CHECK(wcslen(t) == 7, "text buffer never overruns");
    }
}

/* ---------------------------------------------------------------- the lookups on the real files */
/* a PE file laid out as the loader would (headers + sections at their rva), zero filled */
static uint8_t *map_image(const char *path, uint32_t *img)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc((size_t)n + 1), *m = NULL;
    if (file && n > 0x400 && fread(file, 1, (size_t)n, f) == (size_t)n && file[0] == 'M' && file[1] == 'Z') {
        uint32_t lfa = rd32(file + 0x3C);
        if (lfa < (uint32_t)n - 0x200 && rd32(file + lfa) == 0x4550 && rd16(file + lfa + 24) == 0x20B) {
            const uint8_t *nt = file + lfa;
            uint32_t size = rd32(nt + 24 + 56), hdr = rd32(nt + 24 + 60), ns = rd16(nt + 6);
            const uint8_t *s = nt + 24 + rd16(nt + 20);
            m = (uint8_t *)calloc(1, (size_t)size + 16);
            if (m && hdr <= (uint32_t)n && hdr <= size) {
                memcpy(m, file, hdr);
                for (uint32_t i = 0; i < ns; i++, s += 40) {
                    uint32_t vs = rd32(s + 8), va = rd32(s + 12), rs = rd32(s + 16), raw = rd32(s + 20);
                    uint32_t sz = rs < vs ? rs : vs;
                    if (raw && sz && raw <= (uint32_t)n && sz <= (uint32_t)n - raw && va <= size && sz <= size - va) memcpy(m + va, file + raw, sz);
                }
                *img = size;
            }
        }
    }
    free(file); fclose(f);
    return m;
}

static void lookups(const char *dir)
{
    char path[1024]; uint32_t img = 0;
    printf("lookups on the files in %s\n", dir);
    snprintf(path, sizeof path, "%s/steam_api64.dll", dir);
    uint8_t *steam = map_image(path, &img);
    CHECK(steam != NULL, "steam_api64.dll opened");
    if (steam) {
        int found = 0;
        for (int i = 0; i < STEAM_N; i++) {
            uint8_t *p = (uint8_t *)pe_export(steam, STEAM_NAMES[i], i < STEAM_VERSIONED);
            if (p) found++;
            printf("  %-50s %s", STEAM_NAMES[i], p ? "" : "NOT FOUND\n");
            if (p) printf("steam_api64+0x%X\n", (unsigned)(p - steam));
        }
        CHECK(found == STEAM_N, "every Steam function the dll calls is exported");
        CHECK(pe_export(steam, "SteamAPI_RunCallbacks", 0) != NULL, "SteamAPI_RunCallbacks is exported");
        CHECK(pe_export(steam, "SteamAPI_SteamUser", 1) == NULL && pe_export(steam, "SteamAPI_SteamMatchmaking_v", 0) == NULL &&
              pe_export(steam, "SteamAPI_NoSuchFunction", 0) == NULL, "a wrong name finds nothing");
        uint8_t *a = (uint8_t *)pe_export(steam, "SteamAPI_SteamMatchmaking_v", 1), *b = (uint8_t *)pe_export(steam, "SteamAPI_SteamMatchmakingServers_v", 1);
        CHECK(a && b && a != b, "Matchmaking and MatchmakingServers are told apart");
        free(steam);
    }
    static const char *const exes[] = { "call_to_arms.exe", "call_to_arms_server.exe" };
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, exes[i]);
        uint8_t *exe = map_image(path, &img);
        CHECK(exe != NULL, exes[i]);
        if (!exe) continue;
        uint64_t *slot = pe_import_slot(exe, "steam_api64.dll", "SteamAPI_RunCallbacks");
        printf("  %-24s import slot of SteamAPI_RunCallbacks: ", exes[i]);
        if (slot) printf("exe+0x%X\n", (unsigned)((uint8_t *)slot - exe)); else printf("NOT FOUND\n");
        CHECK(slot != NULL, "the exe imports steam_api64!SteamAPI_RunCallbacks by name");
        CHECK(pe_import_slot(exe, "STEAM_API64.DLL", "SteamAPI_RunCallbacks") == slot, "dll name is matched without case");
        CHECK(pe_import_slot(exe, "steam_api64.dll", "SteamAPI_RunCallback") == NULL &&
              pe_import_slot(exe, "kernel32.dll", "SteamAPI_RunCallbacks") == NULL, "a wrong name finds nothing");
        free(exe);
    }
    uint8_t junk[64] = { 'M', 'Z' };
    CHECK(pe_export(junk, "x", 0) == NULL && pe_import_slot(junk, "a", "b") == NULL && pe_export(NULL, "x", 0) == NULL, "not a PE image: nothing found");
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-v")) T_verbose = 1; else dir = argv[i]; }
    scenarios();
    if (dir) lookups(dir); else printf("(no folder given: the lookups on steam_api64.dll / the exes were not run)\n");
    printf("%d checks, %d failed\n", T_checks, T_fail);
    printf(T_fail ? "FAILED\n" : "ALL OK\n");
    return T_fail ? 1 : 0;
}
