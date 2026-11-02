/* gb_names.h - pure helpers shared by the DLL and the host-side test.
 * MSVC RTTI name demangling (just enough for class names) and ';'-separated
 * pattern lists with a leading/trailing '*' wildcard. No Windows headers. */
#ifndef GB_NAMES_H
#define GB_NAMES_H

#include <stddef.h>
#include <string.h>

/* ".?AVeManager@Scene@@" -> "Scene::eManager". Returns the number of
 * components, 0 if the string is not an RTTI class/struct name. */
static int gb_demangle(const char *m, char *out, size_t cap)
{
    const char *comps[16];
    size_t lens[16];
    int n = 0;
    const char *p;
    size_t o = 0;
    int i;

    if (!m || strncmp(m, ".?A", 3) != 0 || cap == 0)
        return 0;
    p = m + 4; /* skip ".?AV" / ".?AU" / ".?AW" */
    while (*p && n < 16) {
        const char *q = p;
        while (*q && *q != '@')
            q++;
        if (q == p)
            break;
        comps[n] = p;
        lens[n] = (size_t)(q - p);
        n++;
        if (*q != '@')
            break;
        p = q + 1;
        if (*p == '@')
            break; /* "@@" terminator */
    }
    out[0] = 0;
    for (i = n - 1; i >= 0; i--) {
        if (o && o + 2 < cap) {
            out[o++] = ':';
            out[o++] = ':';
        }
        if (lens[i] >= cap - o - 1)
            lens[i] = cap - o - 1;
        memcpy(out + o, comps[i], lens[i]);
        o += lens[i];
        out[o] = 0;
        if (o + 1 >= cap)
            break;
    }
    return n;
}

static int gb_pat_match(const char *name, const char *pat, size_t plen)
{
    int lead, trail;
    const char *core;
    size_t clen, nlen, k;

    if (!plen)
        return 0;
    lead = pat[0] == '*';
    trail = plen > (size_t)lead && pat[plen - 1] == '*';
    core = pat + lead;
    clen = plen - (size_t)lead - (size_t)trail;
    if (!clen)
        return 0;
    nlen = strlen(name);
    if (nlen < clen)
        return 0;
    if (lead && trail) {
        for (k = 0; k + clen <= nlen; k++)
            if (memcmp(name + k, core, clen) == 0)
                return 1;
        return 0;
    }
    if (lead)
        return memcmp(name + nlen - clen, core, clen) == 0;
    if (trail)
        return memcmp(name, core, clen) == 0;
    return nlen == clen && memcmp(name, core, clen) == 0;
}

/* list: "a;*b*;c*" (spaces around items are ignored) */
static int gb_list_match(const char *name, const char *list)
{
    const char *p = list;
    if (!list)
        return 0;
    while (*p) {
        const char *q;
        size_t len;
        while (*p == ' ' || *p == '\t' || *p == ';')
            p++;
        if (!*p)
            break;
        q = p;
        while (*q && *q != ';')
            q++;
        len = (size_t)(q - p);
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
            len--;
        if (gb_pat_match(name, p, len))
            return 1;
        p = q;
    }
    return 0;
}

#endif
