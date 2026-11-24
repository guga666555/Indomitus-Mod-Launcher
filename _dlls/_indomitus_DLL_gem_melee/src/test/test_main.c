/* gem_melee_test.exe driver (spec 8.1). No args (or the three exe paths): runs every step as a child process with inherited
 * stdout / stderr, prints "== <step>: exit <n>" after each and "ALL OK" / "SOME FAILED" last (exit 0 / 1).
 * Step routing (the children land here too):
 *   --core <g> <e> <s>     test_core_main          --stance <g> <e> <s>   stance_test_main (sink DISCARD, epoch frozen)
 *   --pen <g> <e> <s>      pen_test_main (same)    --linked <g> <e> <s>   test_linked_main (WP4)
 *   --chase <x>            test_chase_main         --amtag                stance's own second pass (ms:4110-4113)
 * The itest steps run gem_melee_itest.exe next to this exe. */
#include "../core/core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_exe[3] = { "call_to_arms.exe", "call_to_arms_ed.exe", "call_to_arms_server.exe" };
int test_linked_amtag(void);                                            /* src\test\test_linked.c (review F8) */

static const char *base_name(const char *p)
{
    const char *n = p + strlen(p);
    while (n > p && n[-1] != '\\' && n[-1] != '/') n--;
    return n;
}

/* run "<exe>" <args> with this process's std handles, wait, print the step line; 1 = exit code 0 */
static int run_step(const char *exe, const char *args, const char *step)
{
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    fflush(stdout); fflush(stderr);
    DWORD ec = 1;
    if (CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        if (!GetExitCodeProcess(pi.hProcess, &ec)) ec = 1;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else printf("%s: could not start \"%s\" (%lu)\n", step, exe, GetLastError());
    printf("== %s: exit %lu\n", step, ec);
    fflush(stdout);
    return ec == 0;
}

int main(int argc, char **argv)
{
    /* ---- routing: one step in this process ---- */
    if (argc > 1 && !strcmp(argv[1], "--amtag")) {
        core_test_sink(CORE_SINK_DISCARD); core_test_freeze_epoch(1);
        int linked = test_linked_amtag();                               /* started from --linked: link attached here too (review F8) */
        int r = stance_test_main(argc, argv);
        if (linked) {                                                    /* the pass must have reached link: else the parent prints FAILED */
            long notes = 0, traces = 0; link_test_counts(&notes, &traces);
            if (!r && (notes <= 0 || traces <= 0)) { fprintf(stderr, "--amtag under --linked: link saw notes %ld, traces %ld\n", notes, traces); r = 1; }
        }
        return r;
    }
    if (argc > 1 && (!strcmp(argv[1], "--stance") || !strcmp(argv[1], "--pen") || !strcmp(argv[1], "--linked") || !strcmp(argv[1], "--core"))) {
        char *av[8]; int ac = 0;
        av[ac++] = argv[0];
        for (int i = 2; i < argc && ac < 7; i++) av[ac++] = argv[i];
        av[ac] = NULL;
        if (!strcmp(argv[1], "--core")) return test_core_main(ac, av);
        if (!strcmp(argv[1], "--linked")) return test_linked_main(ac, av);
        core_test_sink(CORE_SINK_DISCARD); core_test_freeze_epoch(1);
        return !strcmp(argv[1], "--stance") ? stance_test_main(ac, av) : pen_test_main(ac, av);
    }
    if (argc > 2 && !strcmp(argv[1], "--chase")) {
        char *av[3] = { argv[0], argv[2], NULL };
        return test_chase_main(2, av);
    }
    if (argc != 1 && argc != 4) {
        printf("usage: gem_melee_test.exe [<call_to_arms.exe> <call_to_arms_ed.exe> <call_to_arms_server.exe>]\n"
               "       gem_melee_test.exe --core|--stance|--pen|--linked <g> <e> <s> | --chase <exe>\n");
        return 2;
    }

    /* ---- driver ---- */
    char exes[3][MAX_PATH];
    if (argc == 4) for (int i = 0; i < 3; i++) snprintf(exes[i], MAX_PATH, "%s", argv[i + 1]);
    else {
        const char *bin = getenv("GOH_BIN");
        if (!bin || !*bin) bin = "C:/Program Files (x86)/Steam/steamapps/common/Call to Arms - Gates of Hell/binaries/x64";
        for (int i = 0; i < 3; i++) snprintf(exes[i], MAX_PATH, "%s/%s", bin, k_exe[i]);
    }
    char me[MAX_PATH], dir[MAX_PATH], gm[MAX_PATH], itest[MAX_PATH + 32];
    DWORD n = GetModuleFileNameA(NULL, me, MAX_PATH);
    if (!n || n >= MAX_PATH) { printf("cannot read my own path\nSOME FAILED\n"); return 1; }
    snprintf(dir, sizeof dir, "%s", me);
    char *bs = strrchr(dir, '\\'); if (bs) *bs = 0;
    snprintf(itest, sizeof itest, "%s\\gem_melee_itest.exe", dir);
    snprintf(gm, sizeof gm, "%s", dir);                                   /* <GM>\out\test -> <GM> */
    for (int up = 0; up < 2; up++) { bs = strrchr(gm, '\\'); if (bs) *bs = 0; }
    char three[3 * MAX_PATH + 16], args[MAX_PATH * 4], step[MAX_PATH + 32];
    snprintf(three, sizeof three, "\"%s\" \"%s\" \"%s\"", exes[0], exes[1], exes[2]);
    int all = 1;
    snprintf(args, sizeof args, "--core %s", three);   all &= run_step(me, args, "core");
    snprintf(args, sizeof args, "--stance %s", three); all &= run_step(me, args, "stance");
    snprintf(args, sizeof args, "--pen %s", three);    all &= run_step(me, args, "pen");
    snprintf(args, sizeof args, "--linked %s", three); all &= run_step(me, args, "linked");
    for (int i = 0; i < 3; i++) {
        snprintf(args, sizeof args, "--chase \"%s\"", exes[i]);
        snprintf(step, sizeof step, "chase %s", base_name(exes[i]));
        all &= run_step(me, args, step);
    }
    for (int i = 0; i < 3; i++) {
        snprintf(args, sizeof args, "--install \"%s\"", exes[i]);
        snprintf(step, sizeof step, "install %s", base_name(exes[i]));
        all &= run_step(itest, args, step);
    }
    all &= run_step(itest, "--epoch", "epoch");
    snprintf(args, sizeof args, "--replay \"%s\\inputs\\live_payload_a1d6ed3c\\gem_meleestance_editor.log\"", gm);
    all &= run_step(itest, args, "replay");
    all &= run_step(itest, "--link", "link");
    printf(all ? "ALL OK\n" : "SOME FAILED\n");
    fflush(stdout);
    return all ? 0 : 1;
}
