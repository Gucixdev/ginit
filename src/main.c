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
    fputs("usage: ginit [-u] [-q] [-c] [-d DIR[:DIR]] [-p SOCKET] [SERVICE...]\n"
          "  -u  user mode: services from ~/.config/ginit.d and ~/.config/dinit.d (the default when not root/PID 1)\n"
          "  -q  no log lines\n"
          "  -c  check: load the named services (default: all in the directories), start nothing, exit 1 on errors\n"
          "  -d  service directories (also $GINIT_SERVICE_DIR), first match wins\n"
          "  -p  control socket (also $GINIT_SOCKET)\n", f);
    fputs("Starts the named services (default: boot) and runs until told to shut down.\n"
          "Control it with ginitctl.\n", f);
}

/* ginit -c: load services without starting anything and report what would not work. With no names, every file in the
 * service directories is checked. Hard failures (unreadable file, refused key, missing hard dependency, cycle) make the
 * exit status 1; a soft dependency or ordering name that does not exist is only a note, because dinit skips those too. */
static int gi_check(const char **names, int nnames)
{
    static char *all[GI_MAXSVC];
    int nall = 0, i, j, k, idx, rc = 0, loaded = 0;
    DIR *d;
    struct dirent *e;

    if (nnames > 0) {
        for (i = 0; i < nnames; i++) {
            all[nall++] = gi_strdup(names[i]);
        }
    } else {
        for (i = 0; i < gi_ndirs; i++) {
            d = opendir(gi_dirs[i]);
            if (d == NULL) {
                continue;
            }
            while ((e = readdir(d)) != NULL && nall < GI_MAXSVC) {
                if (e->d_name[0] == '.' || !gi_valid_name(e->d_name)) {
                    continue;
                }
                for (j = 0; j < nall && strcmp(all[j], e->d_name) != 0; j++) {
                    ;
                }
                if (j == nall) {
                    all[nall++] = gi_strdup(e->d_name);
                }
            }
            closedir(d);
        }
    }
    for (i = 0; i < nall; i++) {
        idx = gi_load(all[i]);
        if (idx < 0) {
            printf("error: %s: %s\n", all[i], gi_load_err);
            rc = GI_FAIL;
        } else if (gi_cycle_from(idx)) {
            printf("error: %s: dependency cycle\n", all[i]);
            rc = GI_FAIL;
        }
    }
    for (i = 0; i < gi_nsvc; i++) {
        const char *kind[3];
        char **list[3];
        int cnt[3];

        loaded++;
        kind[0] = "waits-for"; list[0] = gi_raw[i].soft; cnt[0] = gi_raw[i].nsoft;
        kind[1] = "after"; list[1] = gi_raw[i].after; cnt[1] = gi_raw[i].nafter;
        kind[2] = "before"; list[2] = gi_raw[i].before; cnt[2] = gi_raw[i].nbefore;
        for (k = 0; k < 3; k++) {
            for (j = 0; j < cnt[k]; j++) {
                if (gi_lookup(list[k][j]) < 0) {
                    printf("note: %s: %s %s: no such service (skipped)\n", gi_svc[i].name, kind[k], list[k][j]);
                }
            }
        }
    }
    printf("%d service%s checked, %s\n", loaded, loaded == 1 ? "" : "s", rc == 0 ? "no errors" : "errors found");
    return rc;
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
    int nboot = 0, check = 0;
    struct pollfd pf[2 + GI_MAXCLIENT];
    int map[2 + GI_MAXCLIENT];
    int idx;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0) {
            user_mode = 1;
        } else if (strcmp(argv[i], "-q") == 0) {
            gi_quiet = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            check = 1;
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
    if (check) {
        gi_dirs_init(user_mode);
        return gi_check(boot, nboot);
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
        /* the kernel starts init with no PATH; services use bare command names */
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0);
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
