/* main.c - ginit: a service manager and init that reads dinit service files.
 *
 *   ginit [-u] [-q] [-d DIR[:DIR]] [-p SOCKET] [SERVICE...]     the daemon (default service: boot)
 *   ginitctl ...                                                the client (same binary, other name; also dinitctl)
 *   reboot, poweroff                                            ginitctl shutdown reboot / poweroff
 *
 * As PID 1 it also reaps orphans and, when the services are down, signals what is left and reboots or powers off.
 * It does not mount anything: that is what the boot service (a scripted one) is for. Everything else is the .c files in src/,
 * included here so the whole program is one translation unit. */

#include "base.h"
#include "svc.c"
#include "engine.c"
#include "ctl.c"
#ifdef __linux__
#include <sys/reboot.h>
#endif

static void gi_usage(FILE *f)
{
    fputs("usage: ginit [-u] [-q] [-d DIR[:DIR]] [-p SOCKET] [SERVICE...]\n"
          "  -u  user mode: services from ~/.config/ginit.d and ~/.config/dinit.d (the default when not root/PID 1)\n"
          "  -q  no log lines\n"
          "  -d  service directories (also $GINIT_SERVICE_DIR), first match wins\n"
          "  -p  control socket (also $GINIT_SOCKET)\n"
          "Starts the named services (default: boot) and runs until told to shut down.\n"
          "Control it with ginitctl.\n", f);
}

static void gi_drain_signals(void)
{
    char b[64];

    while (read(gi_sigpipe[0], b, sizeof(b)) > 0) {
        /* only the wake-up matters */
    }
}

static void gi_begin_shutdown(int mode)
{
    int i;

    gi_shutdown = mode;
    for (i = 0; i < gi_nsvc; i++) {
        gi_svc[i].explicit_start = 0;
        gi_svc[i].timer = gi_svc[i].timer == TM_RESTART ? TM_NONE : gi_svc[i].timer;
    }
    gi_log("shutting down");
}

/* what PID 1 does when the services are down: end every process left, sync, hand the machine back */
static void gi_final(int mode)
{
    kill(-1, SIGTERM);
    kill(-1, SIGCONT);
    sleep(1);
    kill(-1, SIGKILL);
    while (waitpid(-1, NULL, WNOHANG) > 0) {
        /* collect */
    }
    sync();
#ifdef __linux__
    reboot(mode == GI_SHUT_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
#else
    (void)mode;
#endif
}

static int gi_daemon(int argc, char **argv)
{
    int user_mode = getpid() != 1 && getuid() != 0, i, n;
    const char *sock = NULL;
    const char *boot[16];
    int nboot = 0;
    struct pollfd pf[2 + GI_MAXCLIENT];
    int map[2 + GI_MAXCLIENT];
    int idx;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            user_mode = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            gi_quiet = 1;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            setenv("GINIT_SERVICE_DIR", argv[++i], 1);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            sock = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            gi_usage(stdout);
            return GI_OK;
        } else if (argv[i][0] == '-') {
            gi_usage(stderr);
            return GI_USAGE;
        } else if (nboot < 16) {
            boot[nboot++] = argv[i];
        }
    }
    if (nboot == 0) {
        boot[nboot++] = "boot";
    }
    if (sock == NULL) {
        sock = gi_sock_path();
    }
    gi_dirs_init(user_mode);
    gi_cl_init();
    gi_signals_init();
    gi_listen(sock);
    if (getpid() == 1) {
        setsid();
        gi_log("pid 1, service directories: %s ...", gi_ndirs > 0 ? gi_dirs[0] : "(none)");
    }
    for (i = 0; i < nboot; i++) {
        if (gi_command_start(boot[i], &idx) < 0) {
            gi_log("%s: %s", boot[i], gi_load_err);
        }
    }
    for (;;) {
        if (gi_sig_stop != 0) {
            n = gi_sig_stop;
            gi_sig_stop = 0;
            if (!gi_shutdown) {
                gi_begin_shutdown(n);
            }
        }
        gi_settle();
        gi_clients_progress();
        if (gi_shutdown && gi_all_stopped()) {
            break;
        }
        n = 0;
        pf[n].fd = gi_sigpipe[0];
        pf[n].events = POLLIN;
        map[n++] = -1;
        pf[n].fd = gi_listen_fd;
        pf[n].events = POLLIN;
        map[n++] = -1;
        for (i = 0; i < GI_MAXCLIENT; i++) {
            if (gi_cl[i].fd >= 0) {
                pf[n].fd = gi_cl[i].fd;
                pf[n].events = POLLIN;
                map[n++] = i;
            }
        }
        if (poll(pf, (nfds_t)n, gi_next_timeout()) < 0 && errno != EINTR) {
            gi_die("poll failed");
        }
        if (pf[0].revents != 0) {
            gi_drain_signals();
        }
        gi_sig_chld = 0;
        gi_reap();
        if (pf[1].revents & POLLIN) {
            gi_accept();
        }
        for (i = 2; i < n; i++) {
            if (pf[i].revents != 0 && gi_cl[map[i]].fd == pf[i].fd) {
                gi_cl_readable(&gi_cl[map[i]]);
            }
        }
        gi_tick();
    }
    unlink(sock);
    if (getpid() == 1) {
        gi_final(gi_shutdown);
        for (;;) {
            pause();
        }
    }
    return GI_OK;
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');

    base = base != NULL ? base + 1 : argv[0];
    if (strcmp(base, "ginitctl") == 0 || strcmp(base, "dinitctl") == 0) {
        return gi_ctl_main(argc, argv);
    }
    if (strcmp(base, "reboot") == 0 || strcmp(base, "poweroff") == 0) {
        char *v[3];

        v[0] = argv[0];
        v[1] = "shutdown";
        v[2] = (char *)base;
        return gi_ctl_main(3, v);
    }
    return gi_daemon(argc, argv);
}
