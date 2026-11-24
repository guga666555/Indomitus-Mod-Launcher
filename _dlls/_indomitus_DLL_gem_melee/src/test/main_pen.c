/* pen_test.exe main (spec 8.2): the pen self-test with the core sink discarded and the epoch frozen. */
#include "../core/core.h"
int main(int argc, char **argv)
{
    core_test_sink(CORE_SINK_DISCARD); core_test_freeze_epoch(1);
    return pen_test_main(argc, argv);
}
