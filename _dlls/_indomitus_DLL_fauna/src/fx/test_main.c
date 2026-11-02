/* GEM Fauna - offline finder test
 * Part of the unity build: #included by gem_fauna.c, not compiled on its own.
 * FX_TEST build only: maps an exe at its image base and runs every finder (tests/run_offline_check.sh).
 */

#ifdef FX_TEST
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: fx_test <call_to_arms.exe>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); if (fread(file, 1, n, f) != (size_t)n) return 2; fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56);
    g_base = mmap((void *)base, imgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_base != (uint8_t *)base) { printf("mmap at image base failed\n"); return 1; }
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s; else if (!strncmp((char *)sh, ".pdata", 8)) g_pdata = s;
    }
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(file + pe + 8));
    int ok = 1;
    printf("--- fauna finders (required)\n");
    ok &= find_tag_ctor(); ok &= find_prop_cond(); ok &= find_vitality();
    printf("stamina fields: %s\n", find_stamina_fields() ? "found" : "NOT found");
    printf("--- squad finders (required: simulation, every exe)\n");
    ok &= find_squad_select(); ok &= find_squad_elect(); ok &= find_squad_pace(); ok &= find_follow();
    ok &= find_fauna_sprint();
    printf("SetSelected: %s\n", g_setSelected ? "found" : "not found (optional)");
    printf("box create finder: %s\n", find_box_create() ? "found" : "NOT found");
    printf("--- bandage finders (simulation)\n");
    ok &= find_bandage();
    printf("--- ui finders (game + editor only)\n");
    int ui = find_ui();
    printf("ui finders: %s\n", ui ? "found" : "not found (expected for the dedicated server)");
    if (ui) printf("head icon finders: %s\n", find_sqview() ? "found" : "NOT found");
    if (ui) printf("direct-control HUD finders: %s\n", find_dcview() ? "found" : "NOT found");
    if (ui) {
        printf("CreateItem prologue (relocated into the stub): "); for (int i = 0; i < CI_PROLOGUE_LEN; i++) printf("%02X ", g_createItemFn[i]); printf("\n");
        printf("resume @ exe+0x%llX, \"%s\"\n", EXE_OFF(g_createItemResume), g_strPanelHuman);
    }
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
