/* gem_melee_test.exe --linked <g> <e> <s> (spec 8.4b): the stance and pen self-tests in ONE process with link attached, every unit a
 * suspect and traced (link_test_force), so S6-S9 / P4 / P6 / P7 call into link with the modules' own fake units. Link writes only to
 * the DISCARD sink and changes no module result, so stdout must equal the two baselines, followed by one LINKED OK line.
 * Also the chase_* API stubs of core.h 2.1: this exe links no chase module (layout 0, NULL pointers, status 0), so link runs without
 * chase (R3 veto unavailable, as the module tests need). */
#include "../core/core.h"
#include <stdio.h>
#include <string.h>

int chase_attach(void) { return 0; }
int chase_layout(uint32_t *weap_item, uint32_t *tag_beg, uint32_t *tag_end) { (void)weap_item; (void)tag_beg; (void)tag_end; return 0; }
void *chase_clock_ptr(void) { return NULL; }
void *chase_ceasefire_fn(void) { return NULL; }
uint32_t chase_enttags_off(void) { return 0; }
int chase_status(void) { return 0; }

void link_test_sources(long *front, long *pick, long *verdict);   /* link.c test entry (not in core.h) */

/* the --amtag child of stance's self-test runs with link attached too (review F8): test_main's --amtag branch calls this when the
 * environment variable GEM_MELEE_TEST_LINKED (the temp ini path, set below) is there; returns 0 when it is not */
int test_linked_amtag(void)
{
    char ini[MAX_PATH + 16];
    DWORD n = GetEnvironmentVariableA("GEM_MELEE_TEST_LINKED", ini, sizeof ini);
    if (!n || n >= sizeof ini) return 0;
    core_test_set_ini(ini);
    link_attach(0, 1, 1);
    link_test_force(1, 1);
    return 1;
}

int test_linked_main(int argc, char **argv)
{
    core_test_sink(CORE_SINK_DISCARD);
    core_test_freeze_epoch(1);
    /* temp ini "<this exe's folder>\tmp\linked.ini": [Link] Flip=1, Trace=1, CeasefireVeto=1 (every other key: link's defaults) */
    char me[MAX_PATH], tmp[MAX_PATH], ini[MAX_PATH + 16];
    DWORD n = GetModuleFileNameA(NULL, me, MAX_PATH);
    if (!n || n >= MAX_PATH) { printf("LINKED FAILED (cannot read my own path)\n"); return 1; }
    snprintf(tmp, sizeof tmp, "%s", me);
    char *bs = strrchr(tmp, '\\'); if (bs) *bs = 0;
    strncat(tmp, "\\tmp", sizeof tmp - strlen(tmp) - 1);
    CreateDirectoryA(tmp, NULL);
    snprintf(ini, sizeof ini, "%s\\linked.ini", tmp);
    FILE *f = fopen(ini, "wb");
    if (!f) { printf("LINKED FAILED (cannot write %s)\n", ini); return 1; }
    fputs("[Link]\r\nFlip=1\r\nTrace=1\r\nCeasefireVeto=1\r\n", f);
    fclose(f);
    core_test_set_ini(ini);
    link_attach(0, 1, 1);
    link_test_force(1, 1);
    SetEnvironmentVariableA("GEM_MELEE_TEST_LINKED", ini);              /* stance's --amtag child attaches link too (F8) */
    fflush(stdout);
    int rs = stance_test_main(argc, argv);
    fflush(stdout);
    SetEnvironmentVariableA("GEM_MELEE_TEST_LINKED", NULL);
    long f0 = 0, p0 = 0, v0 = 0;
    link_test_sources(&f0, &p0, &v0);                                    /* stance: front-order notes (S9) and pick notes (S8) */
    int rp = pen_test_main(argc, argv);
    fflush(stdout);
    long notes = 0, traces = 0, f1 = 0, p1 = 0, v1 = 0;
    link_test_counts(&notes, &traces);
    link_test_sources(&f1, &p1, &v1);                                    /* pen: verdict notes (P4) */
    /* review F4: every source must reach link (with S9 removed `notes > 0` still held: 20 instead of 27) */
    int src = f0 > 0 && p0 > 0 && v1 - v0 > 0;
    if (rs == 0 && rp == 0 && notes > 0 && traces > 0 && src) { printf("LINKED OK (notes %ld, traces %ld)\n", notes, traces); return 0; }
    printf("LINKED FAILED (stance %d, pen %d, notes %ld, traces %ld; stance front %ld, stance picks %ld, pen verdicts %ld)\n", rs, rp, notes, traces,
           f0, p0, v1 - v0);
    return 1;
}
