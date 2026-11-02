/* Host-side checks for gb_names.h (demangling and pattern lists).
 * Build: cc -I../src logic_test.c -o logic_test && ./logic_test */
#include <stdio.h>
#include "../src/gb_names.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void)
{
    char out[128];

    CHECK(gb_demangle(".?AVeGUI_DynamicCampaignView@@", out, sizeof out) == 1);
    CHECK(strcmp(out, "eGUI_DynamicCampaignView") == 0);
    CHECK(gb_demangle(".?AVeManager@Scene@@", out, sizeof out) == 2);
    CHECK(strcmp(out, "Scene::eManager") == 0);
    CHECK(gb_demangle(".?AUeFoo@Bar@Baz@@", out, sizeof out) == 3);
    CHECK(strcmp(out, "Baz::Bar::eFoo") == 0);
    CHECK(gb_demangle("not rtti", out, sizeof out) == 0);
    CHECK(gb_demangle(".?AVeScene@@", out, 4) == 1 && strcmp(out, "eSc") == 0);

    CHECK(gb_list_match("eCampaignResearch", "*Campaign*;*Conquest*"));
    CHECK(!gb_list_match("eCampaignResearch", "*Portrait*;*Icon*"));
    CHECK(gb_list_match("eGUI_CampaignResearchTree", "*ResearchTree*"));
    CHECK(gb_list_match("Scene::eManager", "Scene::eManager"));
    CHECK(!gb_list_match("Scene::eManager2", "Scene::eManager"));
    CHECK(gb_list_match("eCampaignX", "eCampaign*"));
    CHECK(gb_list_match("XCampaign", "*Campaign"));
    CHECK(!gb_list_match("XCampaignY", "*Campaign"));
    CHECK(gb_list_match("abc", " abc ; def "));
    CHECK(!gb_list_match("abc", ""));
    CHECK(!gb_list_match("abc", "*"));
    CHECK(!gb_list_match("abc", NULL));

    printf(fails ? "%d FAILED\n" : "all tests passed\n", fails);
    return fails != 0;
}
