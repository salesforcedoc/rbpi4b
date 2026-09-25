/*
 * envutil.h — how the shims read their configuration.
 *
 * Every shim variable comes from rb.conf via start-rb.sh's SHIM_VARS loop, which
 * exports *all* of them — including the ones nobody set. So **empty means
 * unset** is not a style preference here, it is the contract: getenv() never
 * returns NULL for a variable that appears in that list, it returns "". A bare
 * `getenv("POINT_SWAP_XY") != NULL` test would therefore be always-true.
 *
 * These helpers encode the contract once, so no shim has to remember it.
 */
#ifndef RBLIVE4_ENVUTIL_H
#define RBLIVE4_ENVUTIL_H

#include <stdlib.h>
#include <string.h>

/* NULL, empty, or "0" -> 0. Anything else -> 1. */
static inline int env_flag(const char *name, int dflt)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0')
        return dflt;
    if (strcmp(v, "0") == 0)
        return 0;
    return 1;
}

/* NULL or empty -> dflt. A value that parses to nothing -> dflt, so a typo in
 * rb.conf degrades to the documented default instead of to zero. */
static inline int env_int(const char *name, int dflt)
{
    const char *v = getenv(name);
    char *end;
    long n;
    if (v == NULL || v[0] == '\0')
        return dflt;
    n = strtol(v, &end, 10);
    if (end == v || *end != '\0')
        return dflt;
    return (int)n;
}

static inline double env_double(const char *name, double dflt)
{
    const char *v = getenv(name);
    char *end;
    double d;
    if (v == NULL || v[0] == '\0')
        return dflt;
    d = strtod(v, &end);
    if (end == v || *end != '\0')
        return dflt;
    return d;
}

/* The string itself, or dflt when unset/empty. The returned pointer is into the
 * environment (never freed, never modified). */
static inline const char *env_str(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0')
        return dflt;
    return v;
}

#endif /* RBLIVE4_ENVUTIL_H */
