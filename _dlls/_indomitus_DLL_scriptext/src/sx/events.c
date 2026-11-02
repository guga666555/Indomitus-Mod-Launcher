/* GEM ScriptExt - named events
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * {on "move_order"}, {on "selection_of_units"}, {on "talk_<name>"} fired from Entity::TalkEvent.
 */

#ifndef SX_TEST
/* ------------------------------------------------------------------------------------ */
/* events                                                                               */
/* ------------------------------------------------------------------------------------ */
static int g_evMoveOrder = 1, g_evSelection = 1, g_evTalk = 1, g_evInMp = 1, g_evLogged;
static char g_evMoveName[64] = "move_order", g_evSelName[64] = "selection_of_units";

static int mp_active(void) { return !g_mpNever && g_mpCore && *g_mpCore != NULL; }

static void fire_named(void *entity, const char *name)
{
    uint8_t *e = (uint8_t *)entity;
    if (!e || !(e[ENT_FLAGS] & 2)) return;
    uint8_t *def = *(uint8_t **)(e + g_entDefOff);
    if (!def || !*(void **)(def + g_defSetOff)) return;      /* no compiled interaction set -> FireNamed would fault */
    __attribute__((aligned(16))) uint8_t ctx[0x400];
    __attribute__((aligned(16))) uint8_t tag[0x80];
    memset(ctx, 0, sizeof ctx);
    g_ctxCtor(ctx, entity);
    g_tagCtorCStr(tag, name);
    g_fireNamed(ctx, tag);
    g_tagDtor(tag);
    g_ctxDtor(ctx);
    if (g_verbose && g_evLogged < 200) { logf_("event \"%s\" -> entity %p", name, entity); if (++g_evLogged == 200) logf_("(further events not logged)"); }
}

static void talk_name_to_event(const char *talk, char *out, size_t n)
{
    size_t i = 0; const char *pre = "talk_";
    while (*pre && i < n - 1) out[i++] = *pre++;
    for (; *talk && i < n - 1; talk++) out[i++] = (*talk == ' ' || *talk == '(' || *talk == ')') ? '_' : *talk;
    out[i] = 0;
}

/* script handlers run inside Entity::TalkEvent; one that raises talk events again on the same thread would recurse
   without end, so our events nest at most 3 deep per thread (the engine's own talk handling always runs). The active
   hook frames are kept by stack address: an entry at or below the current frame belongs to a call that was left by an
   exception unwinding past it, and is dropped, so the count cannot get stuck. */
#define TALK_MAX_DEPTH 3
static _Thread_local uintptr_t t_talkSp[TALK_MAX_DEPTH];
static _Thread_local int t_talkDepth;
static void hook_talk_event(void *entity, int *ev)
{
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    while (t_talkDepth > 0 && t_talkSp[t_talkDepth - 1] <= sp) t_talkDepth--;
    if (g_modActive && entity && ev && t_talkDepth < TALK_MAX_DEPTH && (g_evInMp || !mp_active())) {
        t_talkSp[t_talkDepth++] = sp;
        int id = *ev;
        if (id == g_talkIdMoveOrder) { if (g_evMoveOrder) fire_named(entity, g_evMoveName); }
        else if (id == g_talkIdSelection) { if (g_evSelection) fire_named(entity, g_evSelName); }
        else if (g_evTalk) {
            for (int i = 0; i < g_talkTableN; i++)
                if (g_talkTable[i].id == id) { char nm[96]; talk_name_to_event(g_talkTable[i].name, nm, sizeof nm); fire_named(entity, nm); break; }
        }
        t_talkDepth--;
    }
    g_talkEventOrig(entity, ev);
}

static int install_events(void)
{
    /* Entity::TalkEvent 1.067 game/server: push r14 ; sub rsp,0x20 ; mov eax,[rip+..]  (6 bytes relocated)
                            editor: mov [rsp+8],rbx ; mov [rsp+18],rbp ...        (5 bytes relocated) */
    return install_jmp5((uint8_t *)g_talkEvent, (void *)hook_talk_event, (void **)&g_talkEventOrig, "Entity::TalkEvent");
}
#endif /* SX_TEST */
