/* update_sign - release signing for the Indomitus Launcher auto-updater (ECDSA P-256 / SHA-256, Windows CNG).
 * Built and run by build_zig.bat; you never need to call it by hand.
 *
 *   update_sign pubheader <out.h>
 *       Loads the private signing key (creating it the first time) and writes the matching PUBLIC key as a
 *       C header that is compiled into the launcher. The launcher only installs updates signed by this key.
 *
 *   update_sign sign <launcher.exe> <gem_launcher.ini> <out_manifest> [update_pubkey.h]
 *       Hashes the built exe and writes the update manifest (update.ini) with version, min_version, url,
 *       size, sha256 and the signature. Values come from gem_launcher.ini (version=, update_exe_url=,
 *       update_min_version=). Does nothing (exit 0) when update_exe_url= is empty. With the header path it
 *       refuses to sign when the key is not the one compiled into the launcher.
 *
 * A key is created only when the key file does not exist at all AND the launcher has no key yet; it is never
 * replaced or overwritten.
 *
 *   update_sign stamp <gem_launcher.ini> <.git dir> <out.ini>
 *       Copies the settings with version= taken from the newest git tag (v1.2.3); that copy is compiled in.
 *
 *   update_sign versionrc <gem_launcher.ini> <out.rc>
 *       Writes the exe's Windows version info from version= (same number as the window and update.ini).
 *
 * The private key lives OUTSIDE the project: %INDOMITUS_SIGN_KEY% if set, otherwise
 * %USERPROFILE%\.indomitus\update_signing.key. Back it up. Lose it and installed launchers can no longer be
 * updated (players would have to download a new launcher by hand). Leak it and anyone can push an "update".
 *
 * Signed message (must match VerifyManifest() in launcher\main.cpp byte for byte):
 *   "indomitus-update-v1\n" version "\n" min_version "\n" sha256(lowercase hex) "\n" size(decimal) "\n"
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define KEY_MAGIC_LINE "indomitus-update-key-v1"
#define PRIV_BLOB_LEN  (8 + 3 * 32)     /* BCRYPT_ECCKEY_BLOB + X + Y + d */
#define PUB_BLOB_LEN   (8 + 2 * 32)     /* BCRYPT_ECCKEY_BLOB + X + Y */
#define MAX_EXE        (64u * 1024u * 1024u)

static void hexof(const unsigned char *p, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[2 * i] = hx[p[i] >> 4]; out[2 * i + 1] = hx[p[i] & 15]; }
    out[2 * n] = 0;
}

static int unhex(const char *s, unsigned char *out, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
            char c = s[2 * i + k];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        out[i] = (unsigned char)v;
    }
    return 1;
}

static int key_path(char *out, size_t n)
{
    const char *e = getenv("INDOMITUS_SIGN_KEY");
    if (e && *e) { snprintf(out, n, "%s", e); return 1; }
    const char *u = getenv("USERPROFILE");
    if (!u || !*u) return 0;
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s\\.indomitus", u);
    CreateDirectoryA(dir, NULL);
    snprintf(out, n, "%s\\update_signing.key", dir);
    return 1;
}

static unsigned char *read_all(const char *path, size_t *len, size_t max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > max) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    buf[sz] = 0;
    *len = (size_t)sz;
    return buf;
}

static int sha256(const unsigned char *p, size_t n, unsigned char out[32])
{
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_HASH_HANDLE h = NULL; int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) >= 0) {
        ok = BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) >= 0 && BCryptFinishHash(h, out, 32, 0) >= 0;
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* Load the private key, or create it when it does not exist yet and create != 0. */
static BCRYPT_KEY_HANDLE load_key(BCRYPT_ALG_HANDLE alg, int create)
{
    char path[MAX_PATH];
    if (!key_path(path, sizeof path)) { fprintf(stderr, "update_sign: no USERPROFILE / INDOMITUS_SIGN_KEY\n"); return NULL; }
    BCRYPT_KEY_HANDLE key = NULL;
    unsigned char blob[PRIV_BLOB_LEN];
    size_t len = 0;
    unsigned char *txt = read_all(path, &len, 4096);
    if (!txt) {
        /* create a key ONLY when the file really does not exist - never over one we merely failed to read */
        DWORD attr = GetFileAttributesA(path), e = GetLastError();
        if (attr != INVALID_FILE_ATTRIBUTES || (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)) {
            fprintf(stderr, "update_sign: the signing key %s exists but cannot be read - nothing changed\n", path);
            return NULL;
        }
    }
    if (txt) {
        char *nl = strchr((char *)txt, '\n');
        char *hexs = nl ? nl + 1 : NULL;
        if (strncmp((char *)txt, KEY_MAGIC_LINE, strlen(KEY_MAGIC_LINE)) != 0 || !hexs || strlen(hexs) < 2 * PRIV_BLOB_LEN
            || !unhex(hexs, blob, PRIV_BLOB_LEN)) {
            fprintf(stderr, "update_sign: %s is not a valid signing key\n", path);
            free(txt);
            return NULL;
        }
        free(txt);
        NTSTATUS ist = BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPRIVATE_BLOB, &key, blob, PRIV_BLOB_LEN, 0);
        SecureZeroMemory(blob, sizeof blob);
        if (ist < 0) { fprintf(stderr, "update_sign: could not import the key in %s\n", path); return NULL; }
        return key;
    }
    if (!create) { fprintf(stderr, "update_sign: signing key not found: %s\n", path); return NULL; }

    if (BCryptGenerateKeyPair(alg, &key, 256, 0) < 0 || BCryptFinalizeKeyPair(key, 0) < 0) {
        fprintf(stderr, "update_sign: key generation failed\n");
        return NULL;
    }
    ULONG cb = 0;
    if (BCryptExportKey(key, NULL, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof blob, &cb, 0) < 0 || cb != PRIV_BLOB_LEN) {
        fprintf(stderr, "update_sign: key export failed\n");
        BCryptDestroyKey(key);
        return NULL;
    }
    char hexs[2 * PRIV_BLOB_LEN + 1];
    hexof(blob, PRIV_BLOB_LEN, hexs);
    SecureZeroMemory(blob, sizeof blob);
    char filetext[sizeof KEY_MAGIC_LINE + 2 * PRIV_BLOB_LEN + 4];
    int tl = snprintf(filetext, sizeof filetext, "%s\n%s\n", KEY_MAGIC_LINE, hexs);
    SecureZeroMemory(hexs, sizeof hexs);
    HANDLE fh = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wr = 0;
    BOOL wok = fh != INVALID_HANDLE_VALUE && WriteFile(fh, filetext, (DWORD)tl, &wr, NULL) && wr == (DWORD)tl && FlushFileBuffers(fh);
    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
    SecureZeroMemory(filetext, sizeof filetext);
    if (!wok) {
        fprintf(stderr, "update_sign: cannot write the new key to %s\n", path);
        if (fh != INVALID_HANDLE_VALUE) DeleteFileA(path);
        BCryptDestroyKey(key);
        return NULL;
    }
    printf("\n  *** NEW update signing key created: %s\n", path);
    printf("  *** BACK THIS FILE UP somewhere safe and never share it or put it in the mod/repository.\n");
    printf("  *** Without it, launchers already installed can never be updated again.\n\n");
    return key;
}

static int export_pub(BCRYPT_KEY_HANDLE key, unsigned char pub[64])
{
    unsigned char blob[PUB_BLOB_LEN];
    ULONG cb = 0;
    if (BCryptExportKey(key, NULL, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &cb, 0) < 0 || cb != PUB_BLOB_LEN) return 0;
    memcpy(pub, blob + 8, 64);
    return 1;
}

/* The 64 key bytes stored in an existing update_pubkey.h; 0 when there is no usable header. */
static int header_key(const char *path, unsigned char key[64])
{
    size_t len = 0;
    unsigned char *t = read_all(path, &len, 1 << 16);
    if (!t) return 0;
    const char *p = strchr((char *)t, '{');
    int n = 0;
    while (p && n < 64 && (p = strstr(p, "0x")) != NULL) {
        unsigned char b;
        if (!unhex(p + 2, &b, 1)) break;
        key[n++] = b;
        p += 4;
    }
    free(t);
    return n == 64;
}

static int key_file_exists(void)
{
    char path[MAX_PATH];
    return key_path(path, sizeof path) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int cmd_pubheader(const char *out)
{
    unsigned char existing[64];
    int hasHeader = header_key(out, existing);
    if (hasHeader && !key_file_exists()) {
        char path[MAX_PATH]; key_path(path, sizeof path);
        fprintf(stderr, "\n  *** The launcher already has an update key (%s) but the private key file is missing:\n"
                        "  ***   %s\n"
                        "  *** Copy your backed-up key there (or set INDOMITUS_SIGN_KEY). A new key would lock out every\n"
                        "  *** launcher already installed. To start over on purpose, delete %s first.\n\n", out, path, out);
        return 1;
    }
    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) < 0) { fprintf(stderr, "update_sign: no ECDSA P-256 provider\n"); return 1; }
    BCRYPT_KEY_HANDLE key = load_key(alg, 1);
    if (!key) { BCryptCloseAlgorithmProvider(alg, 0); return 1; }
    unsigned char pub[64];
    int ok = export_pub(key, pub);
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) { fprintf(stderr, "update_sign: public key export failed\n"); return 1; }
    if (hasHeader && memcmp(existing, pub, 64) != 0) {
        fprintf(stderr, "\n  *** The signing key does not match the one in %s.\n"
                        "  *** Launchers built with the old key would reject every update. Restore the right key file,\n"
                        "  *** or delete %s to switch keys on purpose.\n\n", out, out);
        return 1;
    }

    char text[1024];
    int n = snprintf(text, sizeof text, "// generated by build_zig.bat (tools\\update_sign.c) - public half of the update signing key\n"
                                        "#define INDOMITUS_UPDATE_PUBKEY 1\n"
                                        "static const unsigned char kUpdatePubKey[64] = {");
    for (int i = 0; i < 64; ++i) n += snprintf(text + n, sizeof text - n, "%s0x%02X", i == 0 ? "\n    " : (i % 16) ? "," : ",\n    ", pub[i]);
    n += snprintf(text + n, sizeof text - n, "\n};\n");

    /* rewrite only when it changed, so the launcher is not rebuilt for nothing */
    size_t oldLen = 0;
    unsigned char *old = read_all(out, &oldLen, 1 << 16);
    int same = old && oldLen == (size_t)n && memcmp(old, text, n) == 0;
    free(old);
    if (!same) {
        FILE *f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "update_sign: cannot write %s\n", out); return 1; }
        fwrite(text, 1, (size_t)n, f);
        fclose(f);
    }
    char hx[17]; hexof(pub, 8, hx);
    printf("update key: public key %s... -> %s\n", hx, out);
    return 0;
}

/* Value of key= in an ini text, trimmed; empty string when absent. */
static void ini_value(const char *text, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t kl = strlen(key);
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n'); if (!e) e = p + strlen(p);
        const char *s = p;
        while (s < e && (*s == ' ' || *s == '\t')) ++s;
        if (s < e && *s != ';' && *s != '#' && (size_t)(e - s) > kl && _strnicmp(s, key, kl) == 0) {
            const char *q = s + kl;
            while (q < e && (*q == ' ' || *q == '\t')) ++q;
            if (q < e && *q == '=') {
                ++q;
                while (q < e && (*q == ' ' || *q == '\t')) ++q;
                const char *r = e;
                while (r > q && (r[-1] == '\r' || r[-1] == ' ' || r[-1] == '\t')) --r;
                size_t L = (size_t)(r - q); if (L >= n) L = n - 1;
                memcpy(out, q, L); out[L] = 0;
                return;
            }
        }
        p = *e ? e + 1 : e;
    }
}

static int cmd_sign(const char *exePath, const char *iniPath, const char *outPath, const char *headerPath)
{
    size_t iniLen = 0;
    unsigned char *ini = read_all(iniPath, &iniLen, 1 << 20);
    if (!ini) { fprintf(stderr, "update_sign: cannot read %s\n", iniPath); return 1; }
    char version[64], minv[64], url[1024];
    ini_value((char *)ini, "version", version, sizeof version);
    ini_value((char *)ini, "update_min_version", minv, sizeof minv);
    ini_value((char *)ini, "update_exe_url", url, sizeof url);
    free(ini);
    if (!url[0]) { printf("update manifest: skipped (update_exe_url= is empty in %s)\n", iniPath); return 0; }
    if (!version[0]) { fprintf(stderr, "update_sign: version= is empty in %s\n", iniPath); return 1; }
    if (_strnicmp(url, "https://", 8) != 0) { fprintf(stderr, "update_sign: update_exe_url must start with https://\n"); return 1; }
    for (const char *c = version; *c; ++c) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-' && *c != '_') { fprintf(stderr, "update_sign: odd character in version=\n"); return 1; }

    size_t exeLen = 0;
    unsigned char *exe = read_all(exePath, &exeLen, MAX_EXE);
    if (!exe) { fprintf(stderr, "update_sign: cannot read %s (or it is over 64 MB)\n", exePath); return 1; }
    unsigned char digest[32];
    if (!sha256(exe, exeLen, digest)) { free(exe); fprintf(stderr, "update_sign: hashing failed\n"); return 1; }
    free(exe);
    char shaHex[65]; hexof(digest, 32, shaHex);

    char msg[512];
    int mlen = snprintf(msg, sizeof msg, "indomitus-update-v1\n%s\n%s\n%s\n%llu\n", version, minv, shaHex, (unsigned long long)exeLen);
    unsigned char mdig[32];
    if (!sha256((unsigned char *)msg, (size_t)mlen, mdig)) return 1;

    BCRYPT_ALG_HANDLE alg = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) < 0) { fprintf(stderr, "update_sign: no ECDSA P-256 provider\n"); return 1; }
    BCRYPT_KEY_HANDLE key = load_key(alg, 0);
    if (!key) { BCryptCloseAlgorithmProvider(alg, 0); return 1; }
    unsigned char sig[64]; ULONG cb = 0;
    NTSTATUS st = BCryptSignHash(key, NULL, mdig, 32, sig, sizeof sig, &cb, 0);
    /* self-test: verify with the public half, the same way the launcher will */
    int verified = 0;
    if (st >= 0 && cb == 64) {
        unsigned char pub[64], blob[PUB_BLOB_LEN];
        unsigned char hk[64];
        if (export_pub(key, pub) && headerPath && header_key(headerPath, hk) && memcmp(hk, pub, 64) != 0) {
            fprintf(stderr, "update_sign: the signing key does not match %s - manifest not written\n", headerPath);
        } else if (export_pub(key, pub)) {
            BCRYPT_ECCKEY_BLOB *h = (BCRYPT_ECCKEY_BLOB *)blob;
            h->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC; h->cbKey = 32;
            memcpy(blob + 8, pub, 64);
            BCRYPT_KEY_HANDLE pk = NULL;
            if (BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &pk, blob, PUB_BLOB_LEN, 0) >= 0) {
                verified = BCryptVerifySignature(pk, NULL, mdig, 32, sig, 64, 0) >= 0;
                BCryptDestroyKey(pk);
            }
        }
    }
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0 || cb != 64) { fprintf(stderr, "update_sign: signing failed (0x%08lX)\n", (unsigned long)st); return 1; }
    if (!verified) { fprintf(stderr, "update_sign: signature self-test FAILED - manifest not written\n"); return 1; }

    char sigHex[129]; hexof(sig, 64, sigHex);
    FILE *f = fopen(outPath, "wb");
    if (!f) { fprintf(stderr, "update_sign: cannot write %s\n", outPath); return 1; }
    fprintf(f, "; Indomitus Launcher update manifest - generated by build_zig.bat. Upload it next to the exe; do not edit\n"
               "; (any change breaks the signature and launchers will refuse the update).\n"
               "version=%s\nmin_version=%s\nurl=%s\nsize=%llu\nsha256=%s\nsig=%s\n",
            version, minv, url, (unsigned long long)exeLen, shaHex, sigHex);
    fclose(f);
    printf("update manifest: v%s%s%s, %llu bytes, sha256 %.12s... signed -> %s\n", version,
           minv[0] ? ", requires " : "", minv, (unsigned long long)exeLen, shaHex, outPath);
    printf("\n  GitHub: make the release from the tag  v%s  and attach dist\\release\\Indomitus.exe + update.ini\n\n", version);
    return 0;
}

/* ---- version from git: the newest tag named like v1.2.3 (or 1.2.3), read straight from .git (no git.exe needed) */

static int ver_cmp(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isdigit((unsigned char)*a)) ++a;
        while (*b && !isdigit((unsigned char)*b)) ++b;
        if (!*a && !*b) return 0;
        unsigned long long x = 0, y = 0;
        while (isdigit((unsigned char)*a)) { if (x < 100000000ull) x = x * 10 + (unsigned)(*a - '0'); ++a; }
        while (isdigit((unsigned char)*b)) { if (y < 100000000ull) y = y * 10 + (unsigned)(*b - '0'); ++b; }
        if (x != y) return x < y ? -1 : 1;
    }
}

/* tag "v0.9.4" / "0.9.4" -> "0.9.4"; anything else (release names, "latest", "v1.0-beta") is not a version tag */
static int tag_version(const char *tag, char *out, size_t n)
{
    const char *p = tag;
    if (*p == 'v' || *p == 'V') ++p;
    size_t L = strlen(p);
    if (!L || L >= n || !isdigit((unsigned char)p[0]) || p[L - 1] == '.') return 0;
    for (const char *q = p; *q; ++q) if (!isdigit((unsigned char)*q) && *q != '.') return 0;
    memcpy(out, p, L + 1);
    return 1;
}

static void consider_tag(const char *tag, char *best, size_t n, char *bestTag, size_t tn)
{
    char v[64];
    if (strlen(tag) < 60 && tag_version(tag, v, sizeof v) && (!best[0] || ver_cmp(v, best) > 0)) {
        snprintf(best, n, "%s", v);
        snprintf(bestTag, tn, "%s", tag);
    }
}

static void newest_git_tag(const char *gitdir, char *best, size_t n, char *bestTag, size_t tn)
{
    best[0] = 0; bestTag[0] = 0;
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\refs\\tags\\*", gitdir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) consider_tag(fd.cFileName, best, n, bestTag, tn); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* tags git has packed: "<sha> refs/tags/<name>" lines ("^<sha>" lines are peeled objects) */
    char pr[MAX_PATH];
    snprintf(pr, sizeof pr, "%s\\packed-refs", gitdir);
    size_t len = 0;
    unsigned char *t = read_all(pr, &len, 16u << 20);
    if (t) {
        char *line = (char *)t;
        for (;;) {
            char *e = strchr(line, '\n');
            if (e) *e = 0;
            char *r = strstr(line, " refs/tags/");
            if (r && line[0] != '^' && line[0] != '#') {
                char *nm = r + 11;
                size_t L = strlen(nm);
                while (L && (nm[L - 1] == '\r' || nm[L - 1] == ' ')) nm[--L] = 0;
                consider_tag(nm, best, n, bestTag, tn);
            }
            if (!e) break;
            line = e + 1;
        }
        free(t);
    }
}

/* Writes <out>: gem_launcher.ini with version= set to the newest git version tag (when there is one). This copy is what
 * gets compiled into the launcher, so the window, caption, exe properties, update.ini and the release tag all agree. */
static int cmd_stamp(const char *iniPath, const char *gitdir, const char *out)
{
    size_t iniLen = 0;
    unsigned char *ini = read_all(iniPath, &iniLen, 1 << 20);
    if (!ini) { fprintf(stderr, "update_sign: cannot read %s\n", iniPath); return 1; }
    char iniver[64], ver[64], tag[64];
    ini_value((char *)ini, "version", iniver, sizeof iniver);
    newest_git_tag(gitdir, ver, sizeof ver, tag, sizeof tag);
    int fromGit = ver[0] != 0;
    if (!fromGit) snprintf(ver, sizeof ver, "%s", iniver);

    /* copy the ini, replacing the first uncommented version= line */
    size_t cap = iniLen + 128;
    char *o = (char *)malloc(cap);
    if (!o) { free(ini); return 1; }
    size_t on = 0;
    int done = 0;
    const char *p = (const char *)ini, *end = (const char *)ini + iniLen;
    while (p < end) {
        const char *e = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *next = e ? e + 1 : end;
        const char *s = p;
        while (s < next && (*s == ' ' || *s == '\t')) ++s;
        int isVer = 0;
        if (!done && (size_t)(next - s) > 7 && _strnicmp(s, "version", 7) == 0) {
            const char *q = s + 7;
            while (q < next && (*q == ' ' || *q == '\t')) ++q;
            isVer = q < next && *q == '=';
        }
        if (isVer) {
            int crlf = e && e > p && e[-1] == '\r';
            on += (size_t)snprintf(o + on, cap - on, "version=%s%s", ver, e ? (crlf ? "\r\n" : "\n") : "");
            done = 1;
        } else {
            memcpy(o + on, p, (size_t)(next - p));
            on += (size_t)(next - p);
        }
        p = next;
    }
    if (!done) on += (size_t)snprintf(o + on, cap - on, "\nversion=%s\n", ver);
    free(ini);

    size_t oldLen = 0;
    unsigned char *old = read_all(out, &oldLen, 1 << 20);
    int same = old && oldLen == on && memcmp(old, o, on) == 0;
    free(old);
    if (!same) {
        FILE *f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "update_sign: cannot write %s\n", out); free(o); return 1; }
        fwrite(o, 1, on, f);
        fclose(f);
    }
    free(o);
    if (fromGit) printf("version: %s (from the newest git tag, %s)\n", ver, tag);
    else printf("version: %s (from gem_launcher.ini - no version tags like v1.2.3 in git yet)\n", ver[0] ? ver : "(none)");
    return 0;
}

/* launcher\version.rc: the exe's Windows version info (Properties > Details) from version= in gem_launcher.ini,
 * so Explorer, the launcher window, update.ini and the GitHub release tag all carry the same number. */
static int cmd_versionrc(const char *iniPath, const char *out)
{
    size_t iniLen = 0;
    unsigned char *ini = read_all(iniPath, &iniLen, 1 << 20);
    if (!ini) { fprintf(stderr, "update_sign: cannot read %s\n", iniPath); return 1; }
    char version[64];
    ini_value((char *)ini, "version", version, sizeof version);
    free(ini);
    for (const char *c = version; *c; ++c) if (!isalnum((unsigned char)*c) && *c != '.' && *c != '-' && *c != '_') { fprintf(stderr, "update_sign: odd character in version=\n"); return 1; }
    unsigned n[4] = { 0, 0, 0, 0 };
    int g = 0;
    for (const char *c = version; *c && g < 4; ) {
        while (*c && !isdigit((unsigned char)*c)) ++c;
        if (!*c) break;
        unsigned v = 0;
        while (isdigit((unsigned char)*c)) { if (v < 6000) v = v * 10 + (unsigned)(*c - '0'); ++c; }
        n[g++] = v > 65535 ? 65535 : v;
    }
    const char *shown = version[0] ? version : "0";
    char text[2048];
    int len = snprintf(text, sizeof text,
        "// generated by build_zig.bat from version= in gem_launcher.ini - do not edit\n"
        "1 VERSIONINFO\n"
        "FILEVERSION     %u,%u,%u,%u\n"
        "PRODUCTVERSION  %u,%u,%u,%u\n"
        "FILEFLAGSMASK   0x3FL\n"
        "FILEFLAGS       0\n"
        "FILEOS          0x40004L\n"
        "FILETYPE        0x1L\n"
        "FILESUBTYPE     0x0L\n"
        "BEGIN\n"
        "    BLOCK \"StringFileInfo\"\n"
        "    BEGIN\n"
        "        BLOCK \"040904B0\"\n"
        "        BEGIN\n"
        "            VALUE \"CompanyName\",      \"Indomitus\"\n"
        "            VALUE \"FileDescription\",  \"Indomitus Launcher\"\n"
        "            VALUE \"FileVersion\",      \"%s\"\n"
        "            VALUE \"InternalName\",     \"Indomitus\"\n"
        "            VALUE \"OriginalFilename\", \"Indomitus.exe\"\n"
        "            VALUE \"ProductName\",      \"Indomitus Launcher\"\n"
        "            VALUE \"ProductVersion\",   \"%s\"\n"
        "        END\n"
        "    END\n"
        "    BLOCK \"VarFileInfo\"\n"
        "    BEGIN\n"
        "        VALUE \"Translation\", 0x0409, 0x04B0\n"
        "    END\n"
        "END\n",
        n[0], n[1], n[2], n[3], n[0], n[1], n[2], n[3], shown, shown);
    size_t oldLen = 0;
    unsigned char *old = read_all(out, &oldLen, 1 << 16);
    int same = old && oldLen == (size_t)len && memcmp(old, text, (size_t)len) == 0;
    free(old);
    if (!same) {
        FILE *f = fopen(out, "wb");
        if (!f) { fprintf(stderr, "update_sign: cannot write %s\n", out); return 1; }
        fwrite(text, 1, (size_t)len, f);
        fclose(f);
    }
    printf("version: %s (exe file version %u.%u.%u.%u)\n", shown, n[0], n[1], n[2], n[3]);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "versionrc")) return cmd_versionrc(argv[2], argv[3]);
    if (argc == 5 && !strcmp(argv[1], "stamp")) return cmd_stamp(argv[2], argv[3], argv[4]);
    if (argc == 3 && !strcmp(argv[1], "pubheader")) return cmd_pubheader(argv[2]);
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "sign")) return cmd_sign(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : NULL);
    fprintf(stderr, "usage: update_sign stamp <gem_launcher.ini> <.git dir> <out.ini>\n       update_sign versionrc <gem_launcher.ini> <out.rc>\n       update_sign pubheader <out.h>\n       update_sign sign <exe> <gem_launcher.ini> <out_manifest> [update_pubkey.h]\n");
    return 2;
}
