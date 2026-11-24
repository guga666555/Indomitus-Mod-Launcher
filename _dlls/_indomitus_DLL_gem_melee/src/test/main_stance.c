/* stance_test.exe main (spec 8.2): the stance self-test with the core sink discarded and the epoch frozen. The test's own
 * "--amtag" child (ms:4110-4113) lands here too. */
#include "../core/core.h"
int main(int argc, char **argv)
{
    core_test_sink(CORE_SINK_DISCARD); core_test_freeze_epoch(1);
    return stance_test_main(argc, argv);
}
