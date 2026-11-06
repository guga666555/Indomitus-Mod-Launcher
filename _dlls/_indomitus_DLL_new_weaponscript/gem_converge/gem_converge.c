/*
 * GEM Converge 1.1  -  Call to Arms: Gates of Hell (GEM engine)  -  module of gem_weaponscript.dll (Indomitus Launcher payload)
 * Barrel convergence + multi-shot for the engine's bullet shooter. Split out of the all-in-one GEM Converge 0.5.1.
 *
 * Per-weapon opt-ins, stuff tags on the gun's set file:
 *   "converge"              every barrel of a multi-barrel weapon fires at the point the gun is aimed at,
 *                           instead of all barrels firing parallel to the single foresight bone.
 *   "bullets_per_shot_<N>"  one trigger pull spawns N real bullets inside a cone ("bullet_spread_<deg>",
 *                           default ini BulletSpread). Ammo is still one round per pull; each pellet does the
 *                           ammo's full damage, so tune the ammo down.
 *
 * Engine side (eWeaponShooterBullet.cpp, all addresses found at runtime, see common\gem_common.h):
 *   eBulletShooter::Launch (vtable slot 11) -> ComputeDir builds ONE direction from the aimer (foresight bone
 *   X axis + spread + zeroing), then SpawnBullet(shooter, syncIdx, float3 *zero, float3 *dir) takes the barrel's
 *   "shooterNN" bone position as the start point and calls Bullet::Launch(pos, dir + zero, speed).
 *   GetAimPoint(aimer, out double3) = raycast along the foresight for the gun's range (direct control) or the
 *   aimer's cached target point.
 *
 * Hook: SpawnBullet (chainable, shared with GEM Overcharge and GEM Guidance). For tagged weapons the direction
 * is rotated by (normalize(aimPoint - shooterPos) - normalize(foresightDir)): spread and zeroing survive, only
 * the parallax between the barrel and the sight is taken out. 1.1.1: only when the sight itself is within
 * MaxAngle of the aim point - while the turret is still traversing toward the cursor, no correction.
 */
#define GEM_NAME    "GEM Converge"
#define GEM_VERSION "1.1.1"
#define GEM_SECT    L"converge"
#include "../common/gem_common.h"

static SpawnBulletFn g_spawnBulletOrig;
static char g_tag[64] = "converge";
static size_t g_tagLen;
static double g_minDist = 3.0;
static double g_maxAngle = 10.0;      /* degrees: the sight must be this close to the aim point for a correction */
static double g_cosMax;
static int g_allowNet = 0;
static int g_logShots = 10;
static double g_bulletSpread = 3.0;   /* cone half-angle, degrees */
static int g_bulletsMax = 32;

/* one pass over the weapon's stuff tags: "converge", "bullets_per_shot_<N>", "bullet_spread_<deg>" */
static void scan_tags(const uint8_t *stuff, int *conv, int *pellets, double *spread)
{
    *conv = 0; *pellets = 0; *spread = g_bulletSpread;
    const uint8_t *b = *(const uint8_t *const *)(stuff + g_stuffTagsBeg), *e = *(const uint8_t *const *)(stuff + g_stuffTagsEnd);
    if (!b || e < b || (size_t)(e - b) > TAG_SIZE * 256) return;
    for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
        size_t len; const char *s = tag_str(t, &len);
        if (len >= 64) continue;
        if (len == g_tagLen && !_strnicmp(s, g_tag, len)) { *conv = 1; continue; }     /* engine lowercases tags; ini value may not be */
        if (len > 17 && !_strnicmp(s, "bullets_per_shot_", 17)) { int n = atoi(s + 17); if (n >= 2 && n <= g_bulletsMax) *pellets = n; continue; }
        if (len > 14 && !_strnicmp(s, "bullet_spread_", 14)) { int n = atoi(s + 14); if (n >= 1 && n <= 45) *spread = n; continue; }
    }
}

static char hook_spawn_bullet(void *shooter, int syncIdx, float *zero, float *dir)
{
    const uint8_t *weapon = *(const uint8_t *const *)((const uint8_t *)shooter + g_weaponOff);
    if (!weapon) goto pass;
    const uint8_t *stuff = *(const uint8_t *const *)(weapon + g_stuffOff);
    if (!stuff) goto pass;
    int conv, pellets; double spread;
    scan_tags(stuff, &conv, &pellets, &spread);
    if (!conv && !pellets) goto pass;
    if (!g_allowNet && g_isNetGame && g_isNetGame()) goto pass;
    void *aimer = *(void **)(weapon + g_aimerOff);
    if (!aimer) goto pass;

    double d[3] = { dir[0], dir[1], dir[2] };
    double len = vnorm(d);
    if (len < 1e-9) goto pass;
    double nd[3] = { d[0], d[1], d[2] };
    double aim[3] = { 0, 0, 0 }, dist = 0;
    const char *boneName = "-";
    MsString name = { {{0}}, 0, 15 };

    if (conv) {
        void *bone = NULL;
        char ok = g_getShooterBone(shooter, &bone, &name);
        /* "shooterNN" fits the SSO buffer; a longer name would leak 1 heap block (no engine deallocator binding) */
        if (ok && bone) {
            boneName = name.cap > 15 ? name.u.ptr : name.u.buf;
            uint8_t *b = (uint8_t *)bone;
            if (b[BONE_FLAGS] & BONE_STALE) g_boneRefresh(bone);
            const float *bpos = (const float *)(b + g_bonePosOff);
            g_getAimPoint(aimer, aim);
            double cv[3] = { aim[0] - bpos[0], aim[1] - bpos[1], aim[2] - bpos[2] };
            dist = vnorm(cv);
            const double *fd = ((AimerGetVecFn)(*(void ***)aimer)[g_aimerDirSlot / 8])(aimer);
            const double *sp = ((AimerGetVecFn)(*(void ***)aimer)[g_aimerPosSlot / 8])(aimer);
            double nom[3] = { fd[0], fd[1], fd[2] };
            /* Is the sight actually ON the aim point? Under direct control the aim point is where the gun is
               COMMANDED to point (the cursor), and the engine fires along the foresight while the turret is still
               traversing. Correcting toward a point the sight is not looking at would turn the shot all the way to
               the cursor (140 deg off the barrels was seen), so convergence only takes out parallax: the sight must
               be within MaxAngle of the aim point, which it is as soon as the turret has settled. */
            double sv[3] = { aim[0] - sp[0], aim[1] - sp[1], aim[2] - sp[2] };
            double onTarget = (vnorm(sv) > 1e-9 && vnorm(nom) > 1e-9) ? nom[0]*sv[0] + nom[1]*sv[1] + nom[2]*sv[2] : -1.0;
            if (dist >= g_minDist && onTarget >= g_cosMax) {
                double c[3] = { d[0] + cv[0] - nom[0], d[1] + cv[1] - nom[1], d[2] + cv[2] - nom[2] };
                if (vnorm(c) > 1e-9) { nd[0] = c[0]; nd[1] = c[1]; nd[2] = c[2]; }
            } else if (g_logShots > 0 && onTarget < g_cosMax) {
                g_logShots--;
                logf_("shot: bone %s, sight %.1f deg off the aim point (turret still turning?) - no correction", boneName, acos(onTarget > 1 ? 1 : onTarget) * 57.29578);
            }
        }
    }

    if (g_logShots > 0) {
        g_logShots--;
        double dot = d[0]*nd[0] + d[1]*nd[1] + d[2]*nd[2];
        if (dot > 1) dot = 1;
        logf_("shot: bone %s, aim point %.1f away, converged %.2f deg, pellets %d (cone %.1f deg)%s", boneName, dist, acos(dot) * 57.29578, pellets, pellets ? spread : 0.0, syncIdx ? " [synced]" : "");
    }

    if (!pellets) {
        dir[0] = (float)(nd[0] * len); dir[1] = (float)(nd[1] * len); dir[2] = (float)(nd[2] * len);
        goto pass;
    }

    /* cone basis around nd */
    double up[3] = { 0, 0, 1 };
    if (fabs(nd[2]) > 0.9) { up[0] = 1; up[2] = 0; }
    double ax[3] = { nd[1]*up[2] - nd[2]*up[1], nd[2]*up[0] - nd[0]*up[2], nd[0]*up[1] - nd[1]*up[0] };
    vnorm(ax);
    double ay[3] = { nd[1]*ax[2] - nd[2]*ax[1], nd[2]*ax[0] - nd[0]*ax[2], nd[0]*ax[1] - nd[1]*ax[0] };
    uint64_t seed = mix64(bits_d(nd[0]) ^ mix64(bits_d(nd[1])) ^ mix64(bits_d(nd[2]) ^ 0x5bd1e995) ^ mix64(bits_d(aim[0]) + bits_d(aim[2])));
    double tanS = tan(spread * 0.01745329);
    char result = 0;
    for (int i = 0; i < pellets; i++) {
        uint64_t r1 = mix64(seed + 2 * (uint64_t)i), r2 = mix64(seed + 2 * (uint64_t)i + 1);
        double u1 = (double)(r1 >> 11) * (1.0 / 9007199254740992.0), u2 = (double)(r2 >> 11) * (1.0 / 9007199254740992.0);
        double rad = tanS * sqrt(u1), phi = 6.283185307 * u2;     /* uniform over the cone's cross-section */
        double ox = rad * cos(phi), oy = rad * sin(phi);
        double p[3] = { nd[0] + ax[0]*ox + ay[0]*oy, nd[1] + ax[1]*ox + ay[1]*oy, nd[2] + ax[2]*ox + ay[2]*oy };
        vnorm(p);
        dir[0] = (float)(p[0] * len); dir[1] = (float)(p[1] * len); dir[2] = (float)(p[2] * len);
        int sid = syncIdx;
        if (i > 0 && syncIdx) {
            if (!g_nextSyncId) break;             /* synced weapon on the host with no counter binding: single bullet */
            sid = g_nextSyncId();
        }
        result |= g_spawnBulletOrig(shooter, sid, zero, dir);
    }
    return result;
pass:
    return g_spawnBulletOrig(shooter, syncIdx, zero, dir);
}

/* finders, then the hook; 1 = feature ON */
static int setup(int install)
{
    int ok = find_launch() && find_spawn_bullet_parts() && find_aim_point() && find_stuff_tags();
    if (!ok || (install && !install_hook_at("SpawnBullet", (void *)g_spawnBullet, (void *)hook_spawn_bullet, (void **)&g_spawnBulletOrig))) {
        logf_("barrel convergence / multi-shot   OFF (engine code not recognised)");
        return 0;
    }
    logf_("barrel convergence / multi-shot   ON (stuff tags \"%s\", bullets_per_shot_N, bullet_spread_N; default cone %g deg, min distance %g, sight within %g deg, multiplayer %s)",
          g_tag, g_bulletSpread, g_minDist, g_maxAngle, g_allowNet ? "allowed" : "off");
    log_mp_note(g_allowNet);
    return 1;
}

static void read_config(void)
{
    ini_str("Tag", "converge", g_tag, sizeof g_tag);
    g_tagLen = strlen(g_tag);
    g_minDist = ini_int("MinDistance", 3);
    g_maxAngle = ini_int("MaxAngle", 10);
    if (g_maxAngle < 1) g_maxAngle = 1;
    if (g_maxAngle > 90) g_maxAngle = 90;
    g_cosMax = cos(g_maxAngle * 0.01745329);
    g_allowNet = ini_int("AllowMultiplayer", 0);
    g_logShots = g_verbose ? ini_int("LogShots", 10) : 0;
    g_bulletSpread = ini_int("BulletSpread", 3);
    g_bulletsMax = ini_int("BulletsPerShotMax", 32);
}

#ifndef GEM_TEST
/* called from gem_weaponscript.c DllMain; 1 = feature ON */
int converge_init(void)
{
    int on = 0;
    if (gem_module_open()) {
        read_config();
        if (!g_tagLen) logf_("Tag= is empty - nothing to do");
        else on = setup(1);
    }
    gem_module_close();
    return on;
}
#else
int converge_selftest(uint8_t *base, const char *exePath, void ***hooked, int *nhooked, GemFound *found)
{
    gem_test_reset();
    g_testExe = exePath;
    get_sections(base);
    read_config();
    int r = setup(1);
    *hooked = g_hooked; *nhooked = g_nhooked;
    gem_found(found);
    return r;
}
#endif
