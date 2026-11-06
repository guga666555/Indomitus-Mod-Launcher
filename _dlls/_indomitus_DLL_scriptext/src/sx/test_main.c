/* GEM ScriptExt - offline finder test
 * Part of the unity build: #included by gem_scriptext.c, not compiled on its own.
 * SX_TEST build only: maps an exe at its image base and runs every finder (tests/run_offline_check.sh).
 */

/* ------------------------------------------------------------------------------------ */
/* offline finder test: maps the exe at its image base and runs every finder            */
/* ------------------------------------------------------------------------------------ */
#ifdef SX_TEST
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: sx_test <call_to_arms.exe>\n"); return 2; }
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
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s;
    }
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(file + pe + 8));
    int ok = 1;
    ok &= find_cond_registry(); ok &= find_flag_cond(); ok &= find_sensor(); ok &= find_talk(); ok &= find_fire_named(); ok &= find_mp_core();
    printf("--- health\n"); ok &= find_vitality(); ok &= find_hdc();
    printf("--- player / team\n"); ok &= find_reader(); ok &= find_diplomacy(); ok &= g_localPlayer != NULL; ok &= find_ctx_effector();
    if (find_cstr("enemyTeam")) ok &= find_mp_teams(); else printf("(no bot API: alliances only)\n");
    printf("--- damage multiplier\n"); ok &= find_mult();
    printf("--- mod gate\n"); ok &= find_mod_list();
    printf("--- talk {seen}\n"); ok &= find_seen();
    if (g_talkEvent) {
        int pro = prologue_len((const uint8_t *)g_talkEvent);
        printf("Entity::TalkEvent prologue: %02X %02X %02X %02X %02X %02X %02X %02X -> relocate %d bytes%s\n",
               ((uint8_t *)g_talkEvent)[0], ((uint8_t *)g_talkEvent)[1], ((uint8_t *)g_talkEvent)[2], ((uint8_t *)g_talkEvent)[3],
               ((uint8_t *)g_talkEvent)[4], ((uint8_t *)g_talkEvent)[5], ((uint8_t *)g_talkEvent)[6], ((uint8_t *)g_talkEvent)[7], pro, pro ? "" : " (NOT HOOKABLE)");
        ok &= pro > 0;
    }
    if (g_talkTable) { printf("talk ids:"); for (int i = 0; i < g_talkTableN; i++) printf(" %d=%s", g_talkTable[i].id, g_talkTable[i].name); printf("\n"); }
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
