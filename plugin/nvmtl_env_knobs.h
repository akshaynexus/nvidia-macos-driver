/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* nvmtl_env_knobs.h - the driver knobs a release plugin forgets when it loads.
   A release plugin drops the NVMTL_/NVK_/NAK_/NVRM_ variables it inherits from the environment: a variable left behind
   in a shell by an earlier experiment must not change how every Metal app launched from that shell draws, and the
   supported route for a setting is the conf file read right after this. Three per-process switches were always kept.
   NVMTL_PROCESS_ENV=1 keeps every knob for this one process: a launcher that sets knobs for one application on purpose
   opts in with that single line, and a process that does not set it behaves exactly as before. The rule is pure over
   the vector it is given, so tools/test_env_knobs.c proves it without loading the plugin. */
#ifndef NVMTL_ENV_KNOBS_H
#define NVMTL_ENV_KNOBS_H
#include <stdlib.h>
#include <string.h>

/* a driver knob: one of our prefixes */
static inline int nvmtl_env_knob_prefixed(const char *kv)
{
    return !strncmp(kv, "NVMTL_", 6) || !strncmp(kv, "NVK_", 4) || !strncmp(kv, "NAK_", 4) || !strncmp(kv, "NVRM_", 5);
}

/* 1 when this process asked to keep its own knobs (the exact value 1; anything else is an ordinary knob and is forgotten) */
static inline int nvmtl_env_knobs_kept(char **envp)
{
    for (char **e = envp; e && *e; e++) if (!strcmp(*e, "NVMTL_PROCESS_ENV=1")) return 1;
    return 0;
}

/* 1 for a variable the release plugin keeps even without the opt-in */
static inline int nvmtl_env_knob_always_kept(const char *kv)
{
    return !strncmp(kv, "NVMTL_UNIFORM_CBUF=", 19)          /* the GL uniform path's per-process override */
        || !strncmp(kv, "NVMTL_UNIFORM_CBUF_STATS=", 25)    /* and its diagnostics */
        || !strncmp(kv, "NVMTL_PRESENT_INLINE=", 21);       /* the switch that restores inline presents */
}

/* The cached answer for this process: 1 when it set NVMTL_PROCESS_ENV=1. Besides keeping the knobs, a release plugin then
   writes its log (nvmtl_log_path) and its fps census for this process, which it never does otherwise. */
static inline int nvmtl_process_env_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_PROCESS_ENV"); on = e && !strcmp(e, "1"); }
    return on;
}

/* The machine-wide log switch for first runs and bug reports: while root has created this file (any content), a release
   plugin logs every line of every process, as if each had set NVMTL_PROCESS_ENV=1 for its log only (no knob is kept):
   to the unified log (syslog "NVMTL: ...", readable with log show) and to nvmtl_log_path() where the process may write
   (/private/tmp/nvmtl-<uid>.log, one file per user, opened only as that user's own regular file and never through a
   link; a sandboxed app writes nvmtl.log in its own temporary directory or nowhere, its lines are in the unified log).
   Removing the file stops it for processes started afterwards. Checked once per process. */
#define NVMTL_LOG_SWITCH "/Library/GPUBundles/nvmtl-log"
#include <unistd.h>
static inline int nvmtl_log_all(void)
{
    static int on = -1;
    if (on < 0) on = access(NVMTL_LOG_SWITCH, F_OK) == 0;
    return on;
}

/* Unsets every driver knob in envp that is not kept and returns how many it forgot. envp is the process environment,
   which unsetenv edits, so the names are collected first and removed after. At most 256 are forgotten, as before. */
static inline unsigned nvmtl_release_forget_env(char **envp)
{
    if (nvmtl_env_knobs_kept(envp)) return 0;
    char *names[256]; unsigned n = 0;
    for (char **e = envp; e && *e && n < 256; e++) {
        if (!nvmtl_env_knob_prefixed(*e) || nvmtl_env_knob_always_kept(*e)) continue;
        const char *eq = strchr(*e, '='); if (!eq) continue;
        names[n] = strndup(*e, (size_t)(eq - *e)); if (names[n]) n++;
    }
    for (unsigned i = 0; i < n; i++) { unsetenv(names[i]); free(names[i]); }
    return n;
}
#endif
