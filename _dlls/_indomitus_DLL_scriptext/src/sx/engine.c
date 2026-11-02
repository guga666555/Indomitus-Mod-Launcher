/* GEM ScriptExt - engine bindings and shared finders
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * Engine types/functions several features use: Tag, interaction context, named-event firing,
 * Entity::TalkEvent, the {if} condition registry, script-flag condition, sensor, talk table, MP core
 * and the vitality (health) extender.
 */

/* ------------------------------------------------------------------------------------ */
/* engine bindings                                                                      */
/* ------------------------------------------------------------------------------------ */
typedef void  (*TagCtorCStrFn)(void *tag, const char *s);          /* Tag::Tag(const char*)           */
typedef void  (*TagDtorFn)(void *tag);                              /* Tag::~Tag                       */
typedef void *(*CtxCtorFn)(void *ctx, void *entity);                /* InteractionContext(entity)      */
typedef void  (*CtxDtorFn)(void *ctx);
typedef void  (*FireNamedFn)(void *ctx, const void *tag);           /* fires {on "name"} handlers      */
typedef void  (*TalkEventFn)(void *entity, int *talkEvent);         /* Entity::TalkEvent               */

static TagCtorCStrFn g_tagCtorCStr;
static TagDtorFn     g_tagDtor;
static CtxCtorFn     g_ctxCtor;
static CtxDtorFn     g_ctxDtor;
static FireNamedFn   g_fireNamed;
static TalkEventFn   g_talkEvent, g_talkEventOrig;
static uint32_t      g_entDefOff, g_defSetOff;                 /* entity->def, def->compiled interaction set (read from FireNamed) */

/* condition registry (eIA_If.cpp) */
static uint8_t *g_ifParser;        /* the {if} parser function */
static void   **g_condHead;        /* -> first node */
/* script flag condition {if "name"} */
typedef uint8_t *(*FlagRegistryFn)(void);
typedef void (*CondParseFn)(void *cond, void *reader);
static FlagRegistryFn g_flagRegistry; static uint32_t g_flagBegOff;
static void **g_flagVt; static CondParseFn g_flagParseOrig;
static int32_t *g_condGuard;       /* MSVC static-init guard of the registry: 0 until constructed, -1 while constructing */
/* engine allocator: registry nodes are freed at exit by the registry's atexit destructor with the engine's sized
   delete, so ours must come from the engine's operator new (not HeapAlloc / static memory) */
typedef void *(*EngineNewFn)(size_t n);
typedef void  (*EngineDeleteFn)(void *p, size_t n);
static EngineNewFn g_engineNew; static EngineDeleteFn g_engineDelete;

/* sensor (eActorTalkScanner.cpp "seen") */
static uint32_t g_sensorOff, g_sensorDummyOff, g_sensorListOff, g_sensorListCount = 4;

/* talk event table: {int id; const char* name; float a, b;} */
typedef struct { int32_t id; int32_t pad; const char *name; float a, b; } TalkDesc;
static const TalkDesc *g_talkTable; static int g_talkTableN;
static int g_talkIdMoveOrder = -1, g_talkIdSelection = -1;

/* multiplayer core pointer (Multiplayer::Start) */
static void **g_mpCore; static int g_mpNever;

#define ENT_FLAGS 0x30           /* entity flags; bit1 = alive / in scene */
/* MSVC std::string (SSO) */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

/* actor block [entity+g_actorOff]: +g_vitOff vitality (Actor::eVitalityExtender) */
static uint32_t g_actorOff, g_vitOff; static int g_okVit;
typedef float (*VitGetFn)(void *vit);                                /* GetHealth / GetMaxHealth (display units) */
static VitGetFn g_getHealth, g_getMaxHealth;

/* script reader (the token stream every parse(reader) slot gets): Peek(reader, kind, 0) is non-consuming (kind 1 =
   word or quoted string, 4 = '{' block, 9 = number), ReadWord(reader, 1) -> std::string*, ReadFloat(reader) */
typedef uint8_t (*RdPeekFn)(void *reader, int kind, int z);        /* bool in al */
typedef MsString *(*RdWordFn)(void *reader, int flag);
typedef float  (*RdFloatFn)(void *reader);
static RdPeekFn g_rdPeek; static RdWordFn g_rdWord; static RdFloatFn g_rdFloat;
static void **g_vtHdc; static uint8_t *g_rdFloatSite, *g_rdCommon;  /* where they were found (health_damage_count) */

/* players: owning player number = byte [entity+g_playerOff] (0 .. g_maxPlayers-1, the editor's "player N"); diplomacy
   matrix: relation of players a, b = (*g_dipData)[b * *g_dipStride + a], 2 ally, 1 enemy, 0 neutral */
static uint32_t g_playerOff; static int g_maxPlayers;
static int32_t **g_dipData; static int32_t *g_dipStride;
static uint32_t g_operOff, g_operBit;          /* [entity+g_operOff] & g_operBit: the engine gives a relation only to such units */
static uint8_t *g_localPlayer;                 /* the local (human) player number, NULL if not found */
/* interaction context: ctx[0] = the unit the script runs on, ctx[1] = the effector ({with_effector} swaps the two);
   [ctx+g_ctxFlagsOff] & g_ctxEffBit = the effector is set */
static uint32_t g_ctxFlagsOff, g_ctxEffBit;

/* multiplayer session (Multiplayer::GamePlay::eGame; the pointer is NULL outside a network or bot battle): a vector of
   player pointers at +g_mpPlayersOff; player: +g_mpIdOff = its player number, team Tag at +KEY whose name ("a", "b",
   "dm"...) is the std::string at +g_mpTeamOff (= KEY + 0x40) */
static uint8_t **g_mpGame; static uint32_t g_mpPlayersOff, g_mpIdOff, g_mpTeamOff;

/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */

/* {if} parser: ... call Registry() ; mov rbx,[rax] ; test rbx,rbx ; ... ; lea r8,"unknown condition" */
static int find_cond_registry(void)
{
    uint8_t *s = find_cstr("unknown condition");
    if (!s) { logf_("conditions: 'unknown condition' string not found"); return 0; }
    uint8_t *lea = find_lea_to(s, 0);
    if (!lea) { logf_("conditions: string xref not found"); return 0; }
    uint8_t *acc = NULL;
    static const uint8_t sig[] = { 0x48,0x8B,0x18, 0x48,0x85,0xDB };     /* mov rbx,[rax] ; test rbx,rbx */
    for (uint8_t *p = lea - 5; p > lea - 0x100; p--)
        if (p[0] == 0xE8 && !memcmp(p + 5, sig, sizeof sig)) { acc = p + 5 + *(int32_t *)(p + 1); break; }
    if (!acc || !in_text(acc)) { logf_("conditions: registry accessor not found"); return 0; }
    /* accessor: ... cmp dword [rip+guard],eax ; jg ; lea rax,[rip+head] ; add rsp,28 ; ret */
    uint8_t *cmp = NULL, *la = NULL;
    for (uint8_t *p = acc; p < acc + 0x40; p++) {
        if (p[0] == 0x39 && p[1] == 0x05 && !cmp) cmp = p;
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05 && cmp && !la) { la = p; break; }
    }
    if (!cmp || !la) { logf_("conditions: registry statics not recognised"); return 0; }
    g_condGuard = (int32_t *)(cmp + 6 + *(int32_t *)(cmp + 2));
    g_condHead  = (void **)(la + 7 + *(int32_t *)(la + 3));
    if (!in_data(g_condGuard) || !in_data(g_condHead)) { logf_("conditions: registry statics not in .data"); return 0; }
    logf_("condition registry: head @ exe+0x%llX guard @ exe+0x%llX (accessor exe+0x%llX)",
          EXE_OFF(g_condHead), EXE_OFF(g_condGuard), EXE_OFF(acc));
    g_ifParser = func_start(lea, 0x400);
    /* operator new: a registration routine 'call Registry() ; mov ecx,0x10 ; mov rbx,rax ; call NEW' */
    g_engineNew = NULL; g_engineDelete = NULL;
    for (size_t i = 0; i + 5 <= g_text.n && !g_engineNew; i++) {
        uint8_t *p = g_text.p + i;
        if (p[0] != 0xE8 || p + 5 + *(int32_t *)(p + 1) != acc) continue;
        uint8_t *q = NULL;                                                              /* mov ecx,0x10 within a few insns */
        for (uint8_t *r = p + 5; r < p + 0x28; r++) { if (r[0] == 0xE8) break; if (r[0] == 0xB9 && *(uint32_t *)(r + 1) == 0x10) { q = r; break; } }
        if (!q) continue;
        uint8_t *c = next_call(q + 5, 0x10, NULL);                                    /* game: mov rbx,rax in between */
        if (c) g_engineNew = (EngineNewFn)c;
    }
    /* sized delete: the registry's atexit destructor (accessor: lea rcx,[rip+dtor] ... call atexit) does
       'mov rbx,[rcx] ; mov edx,0x10 ; call DELETE' for every node */
    for (uint8_t *p = acc; p < acc + 0x80 && !g_engineDelete; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x0D) continue;
        uint8_t *d = p + 7 + *(int32_t *)(p + 3);
        if (!in_text(d)) continue;
        for (uint8_t *q = d; q < d + 0x40; q++)
            if (q[0] == 0xBA && *(uint32_t *)(q + 1) == 0x10 && q[5] == 0xE8) {
                uint8_t *t = q + 10 + *(int32_t *)(q + 6);
                if (in_text(t) && t[0] == 0xE9) t = t + 5 + *(int32_t *)(t + 1);         /* thunk */
                if (in_text(t)) { g_engineDelete = (EngineDeleteFn)t; break; }
            }
    }
    if (!g_engineNew || !g_engineDelete) { logf_("condition registry: engine operator new/delete not found - not registering (would crash at exit)"); return 0; }
    logf_("condition registry: engine new exe+0x%llX delete exe+0x%llX", EXE_OFF(g_engineNew), EXE_OFF(g_engineDelete));
    return 1;
}

/* {if "name"} = script flag condition (eTaggedFlags): 0x10-byte object {vptr, int flagIndex}; parse interns the
   quoted name into a global registry of Tags (0x60 bytes each, max 128) and stores its index; eval tests that bit on
   the entity. The {if} parser loads its vtable with the first 'lea r64,[rip+vt]' at its start (rdi in the game, rbx in the editor). */
static int find_flag_cond(void)
{
    if (!g_ifParser) { logf_("conditions: {if} parser start unknown"); return 0; }
    uint8_t *vt = NULL;
    for (uint8_t *p = g_ifParser; p < g_ifParser + 0x60; p++)
        if ((p[0] & 0xF8) == 0x48 && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {               /* lea r64,[rip+disp] */
            uint8_t *t = p + 7 + *(int32_t *)(p + 3);
            if (in_rdata(t) && in_text(*(uint8_t **)t) && in_text(*(uint8_t **)(t + 8)) && in_text(*(uint8_t **)(t + 16))) { vt = t; break; }
        }
    if (!vt) { logf_("conditions: flag condition vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8), *parse = *(uint8_t **)(vt + 16);
    if (!in_text(*(uint8_t **)vt) || !in_text(ev) || !in_text(parse)) { logf_("conditions: flag condition vtable slots not in .text"); return 0; }
    /* eval: ... mov eax,[rdi+8] ... setne al */
    static const uint8_t idx_sig[] = { 0x8B,0x47,0x08 }, setne_sig[] = { 0x0F,0x95,0xC0 };
    if (!find_bytes(ev, 0x80, idx_sig, 3) || !find_bytes(ev, 0x80, setne_sig, 3)) { logf_("conditions: flag eval not recognised"); return 0; }
    /* parse: call Registry() ; mov r64,[rax+END] ; mov r64,[rax+BEGIN] */
    uint8_t *acc = NULL; uint32_t o1 = 0, o2 = 0;
    for (uint8_t *p = parse; p < parse + 0x80; p++) {
        if (p[0] != 0xE8) continue;
        for (uint8_t *q = p + 5; q < p + 5 + 8 && !acc; q++)            /* optional 'mov r64,rax' in between */
            if (q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x80 && q[7] == 0x48 && q[8] == 0x8B && (q[9] & 0xC7) == 0x80) {
                acc = p + 5 + *(int32_t *)(p + 1); o1 = *(uint32_t *)(q + 3); o2 = *(uint32_t *)(q + 10);
            }
        if (acc) break;
    }
    if (!acc || !in_text(acc) || (o1 != o2 + 8 && o2 != o1 + 8)) { logf_("conditions: flag registry access not recognised"); return 0; }
    g_flagRegistry = (FlagRegistryFn)acc; g_flagBegOff = o1 < o2 ? o1 : o2;
    g_flagVt = (void **)vt; g_flagParseOrig = (CondParseFn)parse;
    logf_("flag condition: vtable exe+0x%llX, registry accessor exe+0x%llX (tags @ +0x%X)", EXE_OFF(vt), EXE_OFF(acc), g_flagBegOff);
    return 1;
}

/* eActorTalkScanner: count-seen function. Reads sensor = [entity+off]; requires [sensor+dummyOff] != 0;
   four vectors at sensor+listOff + k*0x18 of record pointers; record: +0x14 flags (bit0 seen, bit1 enemy), +0x18 entity */
static int find_sensor(void)
{
    uint8_t *s = find_cstr("Seen units can be checked if sensor is not dummy");
    if (!s) { logf_("have_enemy: sensor string not found"); return 0; }
    /* the string is used by several scanner functions (seen, enemy_ratio, ...); all read the same layout */
    for (int k = 0; k < 8; k++) {
        uint8_t *lea = find_lea_to(s, k);
        if (!lea) break;
        uint8_t *f = func_start(lea, 0x200);
        if (!f) continue;
        uint32_t sensor = 0, dummy = 0, list = 0, count = 0; int flagsOk = 0, entOk = 0;
        for (uint8_t *p = f; p < lea - 6; p++) {
            if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC7) == 0x82 && !sensor) sensor = *(uint32_t *)(p + 3);          /* mov r64,[rdx+disp32] */
            if (p[0] == 0x48 && p[1] == 0x83 && (p[2] & 0xF8) == 0xB8 && p[7] == 0 && !dummy) dummy = *(uint32_t *)(p + 3); /* cmp qword [r64+disp32],0 */
        }
        for (uint8_t *p = lea; p < lea + 0x100; p++) {
            if (p[0] == 0x4A && p[1] == 0x8B && (p[2] & 0xC7) == 0x84 && (p[3] & 0xC0) == 0 && !list) list = *(uint32_t *)(p + 4); /* mov r64,[base+r1x*1+disp32] */
            if (p[0] == 0x8B && (p[1] & 0xC7) == 0x40 && p[2] == 0x14) flagsOk = 1;                                       /* mov r32,[r64+0x14] */
            if (p[0] == 0x4C && p[1] == 0x8B && (p[2] & 0xC7) == 0x40 && p[3] == 0x18) entOk = 1;                          /* mov r64,[r64+0x18] */
        }
        for (uint8_t *p = lea; p < lea + 0x400; p++)
            if (p[0] == 0x49 && p[1] == 0x83 && (p[2] & 0xF8) == 0xC0 && p[3] == 0x18 && p[4] == 0x49 && p[5] == 0x83 && (p[6] & 0xF8) == 0xF8) { count = p[7] / 0x18; break; } /* add r1x,0x18 ; cmp r1x,imm8 */
        if (sensor > 0x100 && sensor < 0x2000 && dummy && list > dummy && list < 0x1000 && flagsOk && entOk && count >= 1 && count <= 8) {
            g_sensorOff = sensor; g_sensorDummyOff = dummy; g_sensorListOff = list; g_sensorListCount = count;
            logf_("sensor: entity+0x%X, dummy check +0x%X, %u seen-lists @ +0x%X (from exe+0x%llX)", g_sensorOff, g_sensorDummyOff, g_sensorListCount, g_sensorListOff, EXE_OFF(f));
            return 1;
        }
    }
    logf_("have_enemy: seen-counter layout not recognised");
    return 0;
}

/* talk table + Entity::TalkEvent */
static int find_talk(void)
{
    uint8_t *s = find_cstr("executing_order");
    if (!s) { logf_("events: 'executing_order' not found"); return 0; }
    /* pointer to it in .rdata = TalkDesc.name of the id-2 entry */
    uint8_t *ent = NULL;
    for (size_t i = 0; i + 8 <= g_rdata.n; i += 8)
        if (*(uint8_t **)(g_rdata.p + i) == s) { ent = g_rdata.p + i - 8; break; }
    if (!ent) { logf_("events: talk table entry not found"); return 0; }
    const TalkDesc *t = (const TalkDesc *)ent;
    /* table starts where ids stop being plausible going backwards */
    while ((t - 1)->name && in_rdata((t - 1)->name) && (t - 1)->id > 0 && (t - 1)->id < 64) t--;
    g_talkTable = t; g_talkTableN = 0;
    for (const TalkDesc *q = t; q->name && in_rdata(q->name) && q->id > 0 && q->id < 64 && g_talkTableN < 64; q++) {
        g_talkTableN++;
        if (!strcmp(q->name, "executing_order")) g_talkIdMoveOrder = q->id;
        if (!strcmp(q->name, "selection")) g_talkIdSelection = q->id;
    }
    if (g_talkIdMoveOrder < 0 || g_talkIdSelection < 0) { logf_("events: talk ids not found"); return 0; }
    /* OnTalkEvent(scanner, ev): the function with a lea to the table start that has callers */
    uint8_t *onTalk = NULL;
    for (int k = 0; k < 8 && !onTalk; k++) {
        uint8_t *lea = find_lea_to(t, k);
        if (!lea) break;
        uint8_t *f = func_start(lea, 0x200);
        if (f && count_callers(f) >= 2) onTalk = f;
    }
    if (!onTalk) { logf_("events: OnTalkEvent not found"); return 0; }
    /* Entity::TalkEvent = the caller of OnTalkEvent with the most callers itself */
    uint8_t *best = NULL; int bestN = 0;
    for (size_t i = 0; i + 5 <= g_text.n; i++) {
        if (g_text.p[i] != 0xE8 || g_text.p + i + 5 + *(int32_t *)(g_text.p + i + 1) != onTalk) continue;
        uint8_t *f = func_start(g_text.p + i, 0x200);
        if (!f || f == onTalk) continue;
        int n = count_callers(f);
        if (n > bestN) { bestN = n; best = f; }
    }
    if (!best || bestN < 5) { logf_("events: Entity::TalkEvent not found"); return 0; }
    g_talkEvent = (TalkEventFn)best;
    logf_("talk: %d events in table (move_order id %d, selection id %d), OnTalkEvent exe+0x%llX, Entity::TalkEvent exe+0x%llX (%d callers)",
          g_talkTableN, g_talkIdMoveOrder, g_talkIdSelection, EXE_OFF(onTalk), EXE_OFF(best), bestN);
    return 1;
}

/* a caller that fires a named event with a string literal:
   lea rdx,"start_healing" ; lea rcx,[tag] ; call Tag(const char*) ; ... call Ctx(entity) ; mov rcx,rax ; lea rdx,[tag] ;
   call FireNamed ; lea rcx,[ctx] ; call ~Ctx ; lea rcx,[tag] ; call ~Tag */
static int find_fire_named(void)
{
    uint8_t *s = find_cstr("start_healing");
    if (!s) { logf_("events: 'start_healing' not found"); return 0; }
    uint8_t *ev = find_cstr("EVENT: ");
    static const uint8_t tagdtor_sig[] = { 0x48,0x83,0xC1,0x38,0xE9 };       /* add rcx,0x38 ; jmp */
    static const uint8_t ctxctor_sig[] = { 0x48,0x89,0x11 };                 /* mov [rcx],rdx */
    uint8_t *fn[5] = { 0 }; int ok = 0;
    /* the string has several users; take the xref followed by the 5-call fire sequence */
    for (int k = 0; k < 8 && !ok; k++) {
        uint8_t *lea = find_lea_to(s, k);
        if (!lea) break;
        uint8_t *p = lea, *site; int i;
        for (i = 0; i < 5; i++) {
            fn[i] = next_call(p, 0x40, &site);
            if (!fn[i]) break;
            p = site + 5;
        }
        if (i < 5 || memcmp(fn[4], tagdtor_sig, 5) || !find_bytes(fn[1], 0x20, ctxctor_sig, 3)) continue;
        /* FireNamed must call the dispatcher that prints "EVENT: " */
        for (uint8_t *q = fn[2]; q < fn[2] + 0x100 && !ok; q++)
            if (q[0] == 0xE8) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (in_text(t) && ev && find_rip_ref(t, 0x400, ev)) ok = 1; }
    }
    if (!ok) { logf_("events: FireNamedEvent not recognised"); return 0; }
    /* entity->def and def->set offsets: mov rX,[rax+disp32] ; mov rY,[rX+disp8] */
    for (uint8_t *q = fn[2]; q < fn[2] + 0x80; q++)
        if ((q[0] & 0xF8) == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x80 && (q[7] & 0xF8) == 0x48 && q[8] == 0x8B && (q[9] & 0xC0) == 0x40) {
            uint32_t off = *(uint32_t *)(q + 3);
            if (off > 0x100 && off < 0x1000) { g_entDefOff = off; g_defSetOff = q[10]; break; }
        }
    if (!g_entDefOff || !g_defSetOff) { logf_("events: entity->def / def->set offsets not recognised"); return 0; }
    g_tagCtorCStr = (TagCtorCStrFn)fn[0]; g_ctxCtor = (CtxCtorFn)fn[1]; g_fireNamed = (FireNamedFn)fn[2];
    g_ctxDtor = (CtxDtorFn)fn[3]; g_tagDtor = (TagDtorFn)fn[4];
    logf_("events: Tag(cstr)=exe+0x%llX Ctx=exe+0x%llX FireNamed=exe+0x%llX ~Ctx=exe+0x%llX ~Tag=exe+0x%llX, entity->def +0x%X, def->set +0x%X",
          EXE_OFF(fn[0]), EXE_OFF(fn[1]), EXE_OFF(fn[2]),
          EXE_OFF(fn[3]), EXE_OFF(fn[4]), g_entDefOff, g_defSetOff);
    return 1;
}

/* {name, ctor} 16-byte entry in .data (the {if} / talk condition and interaction action tables) -> ctor; NULL unless unique */
static uint8_t *find_table_ctor(const char *name)
{
    size_t m = strlen(name) + 1; uint8_t *fn = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *f = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(f) || memcmp(str, name, m) || str[-1] != 0) continue;
        fn = f; hits++;
    }
    return hits == 1 ? fn : NULL;
}

/* condition ctor -> vtable (first lea r64,[rip+x] into .rdata whose 3 slots are code) */
static void **cond_vtable_of(uint8_t *ctor)
{
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if ((p[0] & 0xF8) == 0x48 && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
            uint8_t *t = p + 7 + *(int32_t *)(p + 3);
            if (in_rdata(t) && in_text(*(uint8_t **)t) && in_text(*(uint8_t **)(t + 8)) && in_text(*(uint8_t **)(t + 16))) return (void **)t;
        }
    return NULL;
}

/* {if gamemode} eval: call IsMultiplayer() -> 'cmp qword [rip+core],0 ; setne al ; ret' */
static int find_mp_core(void)
{
    uint8_t *ctor = find_table_ctor("gamemode");
    if (!ctor) { logf_("mp gate: {if gamemode} entry not found"); return 0; }
    uint8_t *vt = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if (p[0] == 0x48 && p[1] == 0x8D && (p[2] == 0x05 || p[2] == 0x0D)) { uint8_t *c = p + 7 + *(int32_t *)(p + 3); if (in_rdata(c)) { vt = c; break; } }
    if (!vt) { logf_("mp gate: gamemode vtable not found"); return 0; }
    uint8_t *ev = *(uint8_t **)(vt + 8);
    if (in_text(ev) && ev[0] == 0x32 && ev[1] == 0xC0 && ev[2] == 0xC3) { g_mpNever = 1; logf_("mp gate: this exe has no multiplayer (gamemode is always false)"); return 1; }
    uint8_t *mp = in_text(ev) ? next_call(ev, 0x20, NULL) : NULL;
    static const uint8_t sig[] = { 0x48,0x83,0x3D };            /* cmp qword [rip+disp],imm8 */
    if (!mp || memcmp(mp, sig, 3) || mp[7] != 0 || mp[8] != 0x0F || mp[9] != 0x95) { logf_("mp gate: IsMultiplayer not recognised"); return 0; }
    g_mpCore = (void **)(mp + 8 + *(int32_t *)(mp + 3));
    if (!in_data(g_mpCore)) { logf_("mp gate: core pointer not in .data"); return 0; }
    logf_("mp gate: Multiplayer core pointer @ exe+0x%llX", EXE_OFF(g_mpCore));
    return 1;
}

/* talk condition {health_level}: eval reads vit = [[entity+ACTOR]+VIT] ; call GetHealth ; call GetMax ; divss xmm6,xmm0 */
static int find_vitality(void)
{
    uint8_t *ctor = find_table_ctor("health_level");
    void **vt = ctor ? cond_vtable_of(ctor) : NULL;
    if (!vt) { logf_("health: talk condition health_level not found"); return 0; }
    uint8_t *ev = (uint8_t *)vt[1];
    for (uint8_t *p = ev; p < ev + 0x40; p++)
        if (p[0] == 0x48 && p[1] == 0x8B && (p[2] & 0xC7) == 0x82 && p[7] == 0x48 && p[8] == 0x8B && (p[9] & 0xC7) == 0x40) {
            g_actorOff = *(uint32_t *)(p + 3); g_vitOff = p[10]; break;
        }
    static const uint8_t divsig[] = { 0xF3,0x0F,0x5E,0xF0 };
    uint8_t *dv = find_bytes(ev, 0x200, divsig, 4);
    if (!g_actorOff || !dv) { logf_("health: health_level eval layout not recognised"); return 0; }
    uint8_t *c1 = NULL, *c2 = NULL;
    for (uint8_t *p = dv - 5; p > ev; p--)
        if (p[0] == 0xE8 && in_text(p + 5 + *(int32_t *)(p + 1))) { if (!c2) c2 = p; else { c1 = p; break; } }
    if (!c1) { logf_("health: health getters not found"); return 0; }
    g_getHealth = (VitGetFn)(c1 + 5 + *(int32_t *)(c1 + 1)); g_getMaxHealth = (VitGetFn)(c2 + 5 + *(int32_t *)(c2 + 1));
    const uint8_t *gh = (const uint8_t *)g_getHealth;
    if (gh[0] != 0xF3 || gh[1] != 0x0F || gh[2] != 0x10 || (gh[3] & 0xC7) != 0x81) { logf_("health: GetHealth prologue not recognised"); return 0; }
    logf_("health: vitality = [[entity+0x%X]+0x%X], GetHealth exe+0x%llX GetMax exe+0x%llX",
          g_actorOff, g_vitOff, EXE_OFF(g_getHealth), EXE_OFF(g_getMaxHealth));
    return 1;
}

/* script reader functions, from Interaction::eHealthDamageCount: its parse (vt slot 5) is 'call ReadFloat ; ... ; jmp
   COMMON' and the common optional-block parser does 'mov edx,4 ; call Peek' and 'mov dl,1 ; call ReadWord'.
   {if health}, {if player}, {if team} and the health_damage_count extension use them. Runs once. */
static int find_reader(void)
{
    static int st = -1;
    if (st >= 0) return st;
    st = 0;
    g_vtHdc = find_vtable_rtti(".?AUeHealthDamageCount@Interaction@@");
    if (!g_vtHdc) { logf_("script reader: health_damage_count class not found"); return 0; }
    uint8_t *ps = (uint8_t *)g_vtHdc[5];
    if (!in_text(ps)) { logf_("script reader: health_damage_count vtable not recognised"); return 0; }
    g_rdFloat = (RdFloatFn)next_call(ps, 0x20, &g_rdFloatSite);
    if (!g_rdFloat || !g_rdFloatSite) { logf_("script reader: ReadFloat not found"); return 0; }
    for (uint8_t *q = g_rdFloatSite + 5; q < g_rdFloatSite + 0x30 && !g_rdCommon; q++)
        if (q[0] == 0xE9) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (in_text(t)) g_rdCommon = t; }
    if (!g_rdCommon) { logf_("script reader: common block parser not found"); return 0; }
    for (uint8_t *q = g_rdCommon; q < g_rdCommon + 0x40 && !g_rdPeek; q++)
        if (q[0] == 0xBA && *(uint32_t *)(q + 1) == 4) { uint8_t *t = next_call(q + 5, 8, NULL); if (t) g_rdPeek = (RdPeekFn)t; }
    for (uint8_t *q = g_rdCommon; q < g_rdCommon + 0x100 && !g_rdWord; q++)
        if (q[0] == 0xB2 && q[1] == 0x01) { uint8_t *t = next_call(q + 2, 8, NULL); if (t) g_rdWord = (RdWordFn)t; }
    if (!g_rdPeek || !g_rdWord) { logf_("script reader: Peek / ReadWord not found"); return 0; }
    logf_("script reader: Peek exe+0x%llX Word exe+0x%llX Float exe+0x%llX", EXE_OFF(g_rdPeek), EXE_OFF(g_rdWord), EXE_OFF(g_rdFloat));
    return st = 1;
}

/* Entity::GetRelation(this, other, flag): 0 unless other is operatable; same player -> 2; else the diplomacy matrix.
     mov r9,rcx ; test rdx,rdx ; je ; mov eax,[rdx+OP] ; shr eax,8 ; test al,1 ; je ; test r8b,r8b ; je ;
     movzx ecx,byte [rdx+PLAYER] ; ... movzx eax,byte [r9+PLAYER] ... cmp edx,MAX ... imul eax,[rip+STRIDE] ...
     mov rax,[rip+DATA] ; mov eax,[rax+rcx*4]
   Unique in game, editor and server. */
static int find_diplomacy(void)
{
    static const uint8_t head[] = { 0x4C,0x8B,0xC9, 0x48,0x85,0xD2, 0x74 };
    uint8_t *hit = NULL; int hits = 0;
    for (uint8_t *p = g_text.p; (p = find_bytes(p, g_text.p + g_text.n - p, head, sizeof head)); p++) {
        if (p[8] != 0x8B || p[9] != 0x82 || p[14] != 0xC1 || p[15] != 0xE8 || p[17] != 0xA8 || p[18] != 0x01 || p[19] != 0x74 ||
            p[21] != 0x45 || p[22] != 0x84 || p[23] != 0xC0 || p[24] != 0x74 || p[26] != 0x0F || p[27] != 0xB6 || p[28] != 0x8A) continue;
        hit = p; hits++;
    }
    if (hits != 1) { logf_("players: Entity::GetRelation not recognised (%d matches)", hits); return 0; }
    uint32_t pl = *(uint32_t *)(hit + 29), op = *(uint32_t *)(hit + 10), opShift = hit[16]; int same = 0, max = 0; uint8_t *stride = NULL, *data = NULL;
    for (uint8_t *q = hit + 33; q < hit + 0x70; q++) {
        if (q[0] == 0x41 && q[1] == 0x0F && q[2] == 0xB6 && q[3] == 0x81 && *(uint32_t *)(q + 4) == pl) same = 1;    /* movzx eax,byte [r9+PLAYER] */
        if (q[0] == 0x83 && q[1] == 0xFA && !max) max = q[2];                                                       /* cmp edx,MAX */
        if (q[0] == 0x0F && q[1] == 0xAF && q[2] == 0x05 && !stride) stride = q + 7 + *(int32_t *)(q + 3);          /* imul eax,[rip+STRIDE] */
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x05 && stride && !data) data = q + 7 + *(int32_t *)(q + 3);    /* mov rax,[rip+DATA] */
    }
    if (!same || pl < 0x100 || pl > 0x4000 || op < 0x100 || op > 0x4000 || opShift > 31 || max < 2 || max > 32 || !in_data(stride) || !in_data(data))
        { logf_("players: GetRelation layout not recognised"); return 0; }
    g_playerOff = pl; g_maxPlayers = max; g_dipStride = (int32_t *)stride; g_dipData = (int32_t **)data;
    g_operOff = op; g_operBit = 1u << opShift;
    /* local player: GetLocalPlayer(out) = 'movzx eax,byte [rip+LOCAL] ; mov [rcx],al ; mov rax,rcx ; ret', next to
       GetRelation (the {if fogged} condition compares it with the effector the same way) */
    static const uint8_t lsig[] = { 0x88,0x01, 0x48,0x8B,0xC1, 0xC3 };
    uint8_t *lp = NULL; int lhits = 0;
    for (uint8_t *q = hit - 0x1000; q < hit + 0x1000; q++)
        if (q[0] == 0x0F && q[1] == 0xB6 && q[2] == 0x05 && !memcmp(q + 7, lsig, sizeof lsig)) { lp = q + 7 + *(int32_t *)(q + 3); lhits++; }
    g_localPlayer = lhits == 1 && in_data(lp) ? lp : NULL;
    logf_("players: entity+0x%X (relation only if +0x%X bit %u), %d players, diplomacy matrix @ exe+0x%llX stride @ exe+0x%llX, local player %s exe+0x%llX (GetRelation exe+0x%llX)",
          g_playerOff, g_operOff, opShift, g_maxPlayers, EXE_OFF(g_dipData), EXE_OFF(g_dipStride),
          g_localPlayer ? "@" : "NOT FOUND", g_localPlayer ? EXE_OFF(g_localPlayer) : 0ULL, EXE_OFF(hit));
    return 1;
}

/* the {if effector "tag"} condition's eval: 'test dword [rdx+FLAGS],BIT ; ... ; mov rax,[rdx+8]' (rdx = context) */
static int find_ctx_effector(void)
{
    uint8_t *ctor = find_table_ctor("effector");
    void **vt = ctor ? cond_vtable_of(ctor) : NULL;
    uint8_t *ev = vt ? (uint8_t *)vt[1] : NULL;
    if (!ev || !in_text(ev)) { logf_("context: {if effector} condition not found"); return 0; }
    static const uint8_t eff[] = { 0x48,0x8B,0x42,0x08 };                   /* mov rax,[rdx+8] */
    for (uint8_t *q = ev; q < ev + 0x20; q++)
        if (q[0] == 0xF7 && q[1] == 0x82 && find_bytes(q + 10, 0x10, eff, sizeof eff)) {
            uint32_t off = *(uint32_t *)(q + 2), bit = *(uint32_t *)(q + 6);
            if (off < 0x40 || off > 0x1000 || !bit || (bit & (bit - 1))) break;
            g_ctxFlagsOff = off; g_ctxEffBit = bit;
            logf_("context: effector = ctx[1], set when [ctx+0x%X] & 0x%X (from exe+0x%llX)", off, bit, EXE_OFF(ev));
            return 1;
        }
    logf_("context: {if effector} eval not recognised");
    return 0;
}

/* multiplayer teams, from the bot Lua API bindings (game and server only; the editor has no network game):
     lea r8,[rip+TEAM_GETTER] ; lea rdx,"team" ; ... lea r8,[rip+ENEMY_GETTER] ; lea rdx,"enemyTeam"
   team getter:  mov r8,[rcx+PLR] ; ... lea rdx,[r8+NAME]                  (the player's team name)
   enemyTeam:    mov rbx,[rcx+PLR] ; ... call GetGame ; ... lea r8,[rbx+KEY]   GetGame = 'mov rax,[rip+GAME] ; ret'
   players: eGame's diplomacy setup 'mov rax,[rcx+PL] ; xor r9b,r9b ; mov rdx,[rcx+PL+8] ; ... movzx r8d,byte [rcx+ID]'
   that writes the matrix (allies = same team Tag). Needs find_diplomacy first. */
static int find_mp_teams(void)
{
    uint8_t *s = find_cstr("enemyTeam");
    if (!s) { logf_("teams: no bot API in this exe - no multiplayer teams"); return 0; }
    uint8_t *eg = NULL, *tg = NULL;
    for (int k = 0; k < 4 && !(eg && tg); k++) {
        uint8_t *lea = find_lea_to(s, k);
        if (!lea) break;
        if (lea[-7] != 0x4C || lea[-6] != 0x8D || lea[-5] != 0x05) continue;
        eg = lea + *(int32_t *)(lea - 4);
        for (uint8_t *q = lea - 0x40; q < lea - 7 && !tg; q++)
            if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05 && q[7] == 0x48 && q[8] == 0x8D && q[9] == 0x15) {
                const char *nm = (const char *)(q + 14 + *(int32_t *)(q + 10));
                if (in_rdata(nm) && !strcmp(nm, "team")) tg = q + 7 + *(int32_t *)(q + 3);
            }
    }
    if (!eg || !tg || !in_text(eg) || !in_text(tg)) { logf_("teams: bot API team bindings not recognised"); return 0; }
    uint8_t *gg = NULL; uint32_t plr = 0, plr2 = 0, key = 0, name = 0;
    for (uint8_t *q = eg; q < eg + 0x30; q++) {
        if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x59 && !plr) plr = q[3];                                     /* mov rbx,[rcx+PLR] */
        if (q[0] == 0xE8 && !gg) { uint8_t *t = q + 5 + *(int32_t *)(q + 1);
            if (in_text(t) && t[0] == 0x48 && t[1] == 0x8B && t[2] == 0x05 && t[7] == 0xC3) gg = t; }
        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x43 && !key) key = q[3];                                     /* lea r8,[rbx+KEY] */
    }
    for (uint8_t *q = tg; q < tg + 0x20; q++) {
        if (q[0] == 0x4C && q[1] == 0x8B && q[2] == 0x41 && !plr2) plr2 = q[3];                                   /* mov r8,[rcx+PLR] */
        if (q[0] == 0x49 && q[1] == 0x8D && q[2] == 0x50 && !name) name = q[3];                                   /* lea rdx,[r8+NAME] */
    }
    if (!gg || !plr || plr != plr2 || !key || name != key + 0x40) { logf_("teams: team getters not recognised"); return 0; }
    uint8_t **game = (uint8_t **)(gg + 7 + *(int32_t *)(gg + 3));
    if (!in_data(game)) { logf_("teams: session pointer not in .data"); return 0; }
    static const uint8_t sig[] = { 0x45,0x32,0xC9, 0x48,0x8B,0x91 };       /* xor r9b,r9b ; mov rdx,[rcx+disp32] */
    uint32_t pl = 0, id = 0; int hits = 0;
    for (uint8_t *p = g_text.p + 7; (p = find_bytes(p, g_text.p + g_text.n - p, sig, sizeof sig)); p++) {
        if (p[-7] != 0x48 || p[-6] != 0x8B || p[-5] != 0x81) continue;                                            /* mov rax,[rcx+disp32] */
        uint32_t a = *(uint32_t *)(p - 4), b = *(uint32_t *)(p + 6), i = 0;
        if (b != a + 8 || a > 0x1000) continue;
        for (uint8_t *q = p + 10; q < p + 0x40 && !i; q++)
            if (q[0] == 0x44 && q[1] == 0x0F && q[2] == 0xB6 && q[3] == 0x41) i = q[4];                           /* movzx r8d,byte [rcx+ID] */
        if (!i || !g_dipData || !find_rip_ref(p, 0x200, g_dipData)) continue;
        pl = a; id = i; hits++;
    }
    if (hits != 1) { logf_("teams: session player list not recognised (%d matches)", hits); return 0; }
    g_mpGame = game; g_mpPlayersOff = pl; g_mpIdOff = id; g_mpTeamOff = name;
    logf_("teams: session @ exe+0x%llX, players vector +0x%X, player number +0x%X, team name +0x%X", EXE_OFF(g_mpGame), g_mpPlayersOff, g_mpIdOff, g_mpTeamOff);
    return 1;
}
