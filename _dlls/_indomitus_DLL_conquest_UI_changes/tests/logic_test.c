/* Host-side simulation of the portrait logic (pf_apply + reveal queue) against a fake panel / wrap panel / portraits
   that behave like the engine's (ePortraitWrapPanel vt[43]/vt[44] at 0xEACF0/0xEAEB0, StartReveal 0x18DA50).
   Build/run: gcc -O1 -o out/logic_test tests/logic_test.c && ./out/logic_test */
#define CU_TEST
#define main cu_offline_main
#include "../src/gem_conquestui.c"
#undef main
#include <assert.h>

#define NP 12
enum { F_FLAGS = 0x48, F_PARENT = 0xF0, F_CHILD = 0xD8, F_QUEUE = 0x158, F_UNLOCK = 0x2E0, F_REVEAL = 0x2B0,
       P_WRAP = 0x1C8, P_PAIRS = 0x1F8 };

typedef struct Node { struct Node *next, *prev; void *p; void *ctrl; } Node;
static void *vt_wrap[80], *vt_portrait[80], *vt_panel[80];
static _Alignas(16) uint8_t wrapObj[0x400], panelObj[0x400], por[NP][0x400];
static void *children[NP]; static Pair pairs[NP];
static Node qhead; static size_t qsize; static int vischanged, invalidated;

static void start_reveal(uint8_t *p) { if (U32(p, F_REVEAL) == 0) U32(p, F_REVEAL) = 1; }
static void w_enqueue(void *w, const void *p)
{
    Node *n = calloc(1, sizeof *n); n->p = (void *)p;
    n->prev = qhead.prev; n->next = &qhead; qhead.prev->next = n; qhead.prev = n; qsize++;
    if (qsize == 1) start_reveal((uint8_t *)p);
}
static void w_dequeue(void *w, const void *p)
{
    for (Node *n = qhead.next; n != &qhead; n = n->next)
        if (n->p == p) { n->prev->next = n->next; n->next->prev = n->prev; qsize--; free(n); break; }
    if (qsize) start_reveal((uint8_t *)qhead.next->p);
}
static void w_tick(void)    /* wrap vt[30]: pop the head when its animation is done, start the next */
{
    if (qsize && U32(qhead.next->p, F_REVEAL) == 2) { Node *n = qhead.next; qhead.next = n->next; n->next->prev = &qhead; qsize--; free(n);
        if (qsize) start_reveal((uint8_t *)qhead.next->p); }
}
static void p_draw_all(void)   /* portrait vt[30]: a visible portrait in state 1 finishes its animation */
{
    for (int i = 0; i < NP; i++) if (((U32(por[i], F_FLAGS) >> 1) & 1) && U32(por[i], F_REVEAL) == 1) U32(por[i], F_REVEAL) = 2;
}
static void d_vis(void *d) { vischanged++; }
static void d_inv(void *d) { invalidated++; }
static void d_nop(void *d) {}

static void check_invariants(const char *tag)
{
    /* children = permutation of all portraits */
    int seen[NP] = {0};
    for (int i = 0; i < NP; i++) for (int j = 0; j < NP; j++) if (children[i] == por[j]) seen[j]++;
    for (int j = 0; j < NP; j++) assert(seen[j] == 1);
    /* no hidden portrait queued, no duplicates */
    int inq[NP] = {0};
    for (Node *n = qhead.next; n != &qhead; n = n->next) for (int j = 0; j < NP; j++) if (n->p == por[j]) { inq[j]++; assert(((U32(por[j], F_FLAGS) >> 1) & 1)); }
    for (int j = 0; j < NP; j++) assert(inq[j] <= 1);
    /* visibility follows the mode */
    for (int j = 0; j < NP; j++) {
        int vis = (U32(por[j], F_FLAGS) >> 1) & 1;
        assert(vis == (g_mode == MODE_ALL_UNLOCKED_FIRST || U8(por[j], F_UNLOCK)));
        if (vis && U32(por[j], F_REVEAL) == 0) assert(inq[j]);          /* visible + never revealed => queued */
    }
    /* unlocked first in the child vector */
    int seenLocked = 0;
    for (int i = 0; i < NP; i++) { int u = U8(children[i], F_UNLOCK); if (!u) seenLocked = 1; else assert(!seenLocked); }
    /* queued unrevealed entries in display order */
    int last = -1;
    for (Node *n = qhead.next; n != &qhead; n = n->next) {
        if (U32(n->p, F_REVEAL) != 0) continue;
        int pos = -1; for (int i = 0; i < NP; i++) if (children[i] == n->p) pos = i;
        assert(pos > last); last = pos;
    }
    printf("  ok: %s (queue %zu)\n", tag, qsize);
}

static void run_until_revealed(void) { for (int t = 0; t < 4 * NP; t++) { p_draw_all(); w_tick(); } }

int main(void)
{
    O_DLG_PARENT = F_PARENT; O_DLG_CHILD = F_CHILD; O_WRAP_QUEUE = F_QUEUE;
    O_PORTRAIT_UNLOCKED = F_UNLOCK; O_PORTRAIT_REVEAL = F_REVEAL; O_PANEL_WRAP = P_WRAP; O_PANEL_PAIRS = P_PAIRS;
    V_WRAP = vt_wrap; V_PORTRAIT = vt_portrait; V_PANEL[0] = vt_panel; V_PANEL[1] = V_PANEL[2] = (void **)1;
    vt_wrap[SL_REVEAL_ADD] = (void *)w_enqueue; vt_wrap[SL_REVEAL_DEL] = (void *)w_dequeue;
    vt_wrap[SL_INVALIDATE] = vt_portrait[SL_INVALIDATE] = (void *)d_inv; vt_wrap[SL_REPAINT] = (void *)d_nop;
    vt_wrap[SL_VISCHANGED] = vt_portrait[SL_VISCHANGED] = (void *)d_vis;
    *(void ***)wrapObj = vt_wrap; *(void ***)panelObj = vt_panel;
    qhead.next = qhead.prev = &qhead; PTR(wrapObj, F_QUEUE) = (uint8_t *)&qhead;
    PTR(panelObj, P_WRAP) = wrapObj;
    PTR(panelObj, P_PAIRS) = (uint8_t *)pairs; PTR(panelObj, P_PAIRS + 8) = (uint8_t *)(pairs + NP);
    PTR(wrapObj, F_CHILD) = (uint8_t *)children; PTR(wrapObj, F_CHILD + 8) = (uint8_t *)(children + NP);
    const int unlocked[NP] = { 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0 };   /* first created is locked: it starts animating at once */

    /* native Refresh: AddChild of each portrait = visible, state 0, enqueue (first starts) */
    for (int i = 0; i < NP; i++) {
        *(void ***)por[i] = vt_portrait; PTR(por[i], F_PARENT) = wrapObj; U32(por[i], F_FLAGS) = 2 | 1;
        U8(por[i], F_UNLOCK) = (uint8_t)unlocked[i]; children[i] = por[i]; pairs[i].portrait = por[i];
        w_enqueue(wrapObj, por[i]);
    }
    g_verbose = 0;
    printf("mode 1 (unlocked only), first page open:\n");
    g_mode = MODE_UNLOCKED_ONLY; pf_apply(panelObj, "test"); check_invariants("after first refresh");
    for (int j = 1; j < NP; j++) if (!unlocked[j]) assert(U32(por[j], F_REVEAL) == 0);   /* only the natively started head lost its reveal */
    pf_apply(panelObj, "test"); check_invariants("second refresh is stable");
    run_until_revealed(); check_invariants("reveals ran");
    for (int j = 0; j < NP; j++) if (unlocked[j]) assert(U32(por[j], F_REVEAL) == 2);
    assert(qsize == 0);
    printf("toggle -> mode 0 (all, unlocked first):\n");
    g_mode = MODE_ALL_UNLOCKED_FIRST; pf_apply(panelObj, "toggle"); check_invariants("after toggle");
    { int nl = 0; for (int j = 1; j < NP; j++) nl += !unlocked[j]; assert((int)qsize == nl); }   /* the locked ones slide in */
    run_until_revealed(); check_invariants("locked ones revealed");
    for (int j = 0; j < NP; j++) assert(U32(por[j], F_REVEAL) == 2 || (j == 0 && U32(por[j], F_REVEAL) == 1) || U32(por[j], F_REVEAL) == 2);
    assert(qsize == 0);
    printf("toggle back -> mode 1:\n");
    g_mode = MODE_UNLOCKED_ONLY; pf_apply(panelObj, "toggle"); check_invariants("after toggle back");
    printf("mode 0 from the start (fresh panel):\n");
    qhead.next = qhead.prev = &qhead; qsize = 0;
    for (int i = 0; i < NP; i++) { U32(por[i], F_REVEAL) = 0; U32(por[i], F_FLAGS) = 3; children[i] = por[i]; w_enqueue(wrapObj, por[i]); }
    g_mode = MODE_ALL_UNLOCKED_FIRST; pf_apply(panelObj, "test"); check_invariants("grouped first refresh");
    int calls = vischanged; pf_apply(panelObj, "test"); assert(vischanged == calls); check_invariants("idempotent");
    run_until_revealed(); assert(qsize == 0); check_invariants("all revealed");
    printf("ALL LOGIC TESTS PASSED\n");
    return 0;
}
