/* base.h - the shared pieces of ginit: feature macros, types, the arena, small string helpers, the service tables.
 *
 * ginit is one translation unit (src/main.c includes the rest), C89 plus POSIX.1-2008 calls, no dependencies beyond libc.
 * All memory is one static arena that is never freed: the services of a run are loaded once and live as long as the
 * daemon. There is no malloc in src/. */
#ifndef GI_BASE_H
#define GI_BASE_H

#define _XOPEN_SOURCE 700

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

__extension__ typedef long long i64;

#ifndef GI_ARENA_BYTES
#define GI_ARENA_BYTES (4u * 1024u * 1024u)
#endif
#define GI_MAXSVC 512   /* services loaded at once */
#define GI_MAXDEP 24    /* dependencies of one kind per service */
#define GI_MAXARG 64    /* words in a command */
#define GI_MAXENV 128   /* lines of an env-file */
#define GI_NAMELEN 128
#define GI_LINE 1024    /* a line of a service file or of the control protocol */

/* exit codes of the binaries */
#define GI_OK 0
#define GI_FAIL 1  /* the command failed (service failed to start, unknown service...) */
#define GI_USAGE 2 /* bad command line */
#define GI_NODAEMON 3 /* ginitctl could not reach the daemon */

/* ---- arena ---- */
static char gi_mem[GI_ARENA_BYTES];
static size_t gi_used;

static void gi_die(const char *msg)
{
    fprintf(stderr, "ginit: %s\n", msg);
    exit(GI_FAIL);
}

static void *gi_alloc(size_t n)
{
    void *p;

    n = (n + 15u) & ~(size_t)15u;
    if (n > sizeof(gi_mem) - gi_used) {
        gi_die("out of arena memory (raise GI_ARENA_BYTES)");
    }
    p = gi_mem + gi_used;
    gi_used += n;
    memset(p, 0, n);
    return p;
}

static char *gi_strdup(const char *s)
{
    size_t n = strlen(s) + 1u;
    char *p = (char *)gi_alloc(n);

    memcpy(p, s, n);
    return p;
}

/* trim blanks at both ends in place, return the start */
static char *gi_trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) {
        e--;
    }
    *e = '\0';
    return s;
}

/* milliseconds on the monotonic clock */
static i64 gi_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000 + (i64)(ts.tv_nsec / 1000000);
}

/* ---- services ---- */
enum { T_PROCESS, T_BGPROCESS, T_SCRIPTED, T_INTERNAL };
enum { S_STOPPED, S_STARTING, S_STARTED, S_STOPPING };
enum { R_NO, R_YES, R_FAILURE };               /* restart policy */
enum { TM_NONE, TM_RESTART, TM_POLL };        /* what the per-service timer is for */

typedef struct Svc {
    /* from the file */
    char *name;
    int type;
    char **argv;        /* command, NULL-terminated words, or NULL */
    char **stop_argv;   /* stop-command */
    char *workdir;
    char *logfile;
    char *pidfile;
    char **env;         /* KEY=VALUE lines of env-file, NULL-terminated, or NULL */
    int restart;
    int restart_delay_ms;
    int restart_max;    /* restarts allowed within restart_interval_s */
    int restart_interval_s;
    int stop_timeout_s;
    int start_timeout_s;
    int dep[GI_MAXDEP]; /* depends-on / depends-ms: must be started, stop with it */
    int ndep;
    int soft[GI_MAXDEP]; /* waits-for: started with it, a failure does not matter */
    int nsoft;
    int after[GI_MAXDEP]; /* ordering only: start after these when they are being started */
    int nafter;
    /* run time */
    int state;
    int explicit_start;  /* started by a command */
    int want;            /* explicit, or needed by a service that is wanted */
    int failed;          /* the last start failed or restarts ran out: not retried until started again */
    int pid;             /* main process, 0 when none */
    int stop_pid;        /* the stop-command process, 0 when none */
    int bg;              /* bgprocess: pid is watched by kill(pid, 0), not waitpid */
    int starter_done;    /* scripted/bgprocess: the start command has exited */
    int term_sent;
    int timer;           /* TM_* */
    i64 deadline;        /* ms, valid when timer != TM_NONE */
    i64 limit;           /* ms: STARTING must be over by then (else it fails), STOPPING gets SIGKILL; 0 = none */
    int forced_down;     /* stopped by a command: not wanted even when something soft-depends on it */
    i64 hist[8];         /* times of the recent restarts */
    int nhist;
    int exit_status;     /* of the last exit, as waitpid reports */
} Svc;

static Svc gi_svc[GI_MAXSVC];
static int gi_nsvc;

#endif
