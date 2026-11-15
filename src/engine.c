/* engine.c - the state machine: which services run, starting and stopping them, restarts, timers, child reaping.
 *
 * The rule that drives everything (gi_settle): a service is WANTED when a command started it or a wanted service
 * depends on it (depends-on, waits-for), unless a command stopped it. A wanted service starts when its hard
 * dependencies are STARTED and what it waits for (waits-for, after) has finished starting or failed. A running service
 * stops when it is no longer wanted or a hard dependency left STARTED, and never before everything that depends on it
 * has stopped. Events (a child exited, a timer ran out, a command arrived) change one service, then gi_settle runs
 * until nothing more changes. */

static int gi_shutdown;                 /* 0 running, else GI_SHUT_* */
#define GI_SHUT_QUIT 1                  /* stop everything and exit */
#define GI_SHUT_REBOOT 2
#define GI_SHUT_POWEROFF 3

static volatile sig_atomic_t gi_sig_chld, gi_sig_stop;
static int gi_sigpipe[2] = {-1, -1};
static int gi_quiet;

static void gi_log(const char *fmt, ...)
{
    va_list ap;

    if (gi_quiet) {
        return;
    }
    fputs("ginit: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *gi_state_name(const Svc *S)
{
    switch (S->state) {
    case S_STARTING: return "starting";
    case S_STARTED: return "started";
    case S_STOPPING: return "stopping";
    default: return S->failed ? "failed" : "stopped";
    }
}

/* ---- signals ---- */
static void gi_on_signal(int sig)
{
    int saved = errno;
    char b = 1;

    if (sig == SIGCHLD) {
        gi_sig_chld = 1;
    } else if (sig == SIGINT) {
        gi_sig_stop = getpid() == 1 ? GI_SHUT_REBOOT : GI_SHUT_QUIT;
    } else {
        gi_sig_stop = getpid() == 1 ? GI_SHUT_POWEROFF : GI_SHUT_QUIT;
    }
    if (write(gi_sigpipe[1], &b, 1) < 0) {
        /* pipe full: the loop is going to wake up anyway */
    }
    errno = saved;
}

static void gi_signals_init(void)
{
    struct sigaction sa;
    int i;

    if (pipe(gi_sigpipe) < 0) {
        gi_die("pipe failed");
    }
    for (i = 0; i < 2; i++) {
        fcntl(gi_sigpipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(gi_sigpipe[i], F_SETFL, fcntl(gi_sigpipe[i], F_GETFL) | O_NONBLOCK);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gi_on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGPWR, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ---- spawning ---- */

/* Fork and exec argv for service S in its own process group. 0 and *pid on success; -1 and *err (an errno) when the
 * fork or the exec failed (the exec result comes back through a close-on-exec pipe, so it is known here). */
static int gi_spawn(Svc *S, char **argv, int *pid_out, int *err)
{
    int ep[2], fd;
    pid_t pid;
    int e = 0;
    ssize_t n;
    sigset_t none;

    if (pipe(ep) < 0) {
        *err = errno;
        return -1;
    }
    fcntl(ep[0], F_SETFD, FD_CLOEXEC);
    fcntl(ep[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        *err = errno;
        close(ep[0]);
        close(ep[1]);
        return -1;
    }
    if (pid == 0) {
        char **ev;

        setpgid(0, 0);
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPWR, SIG_DFL);
        fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, 0);
            if (S->logfile == NULL) {
                dup2(fd, 1);
                dup2(fd, 2);
            }
            if (fd > 2) {
                close(fd);
            }
        }
        if (S->logfile != NULL) {
            fd = open(S->logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                e = errno;
                if (write(ep[1], &e, sizeof(e)) < 0) {
                    _exit(127);
                }
                _exit(127);
            }
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) {
                close(fd);
            }
        }
        if (S->workdir != NULL && chdir(S->workdir) < 0) {
            e = errno;
            if (write(ep[1], &e, sizeof(e)) < 0) {
                _exit(127);
            }
            _exit(127);
        }
        for (ev = S->env; ev != NULL && *ev != NULL; ev++) {
            putenv(*ev);
        }
        execvp(argv[0], argv);
        e = errno;
        if (write(ep[1], &e, sizeof(e)) < 0) {
            _exit(127);
        }
        _exit(127);
    }
    close(ep[1]);
    do {
        n = read(ep[0], &e, sizeof(e));
    } while (n < 0 && errno == EINTR);
    close(ep[0]);
    if (n == (ssize_t)sizeof(e)) {
        int st;

        while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
            /* reap the child that never got to exec */
        }
        *err = e;
        return -1;
    }
    *pid_out = (int)pid;
    return 0;
}

static void gi_kill_group(int pid, int sig)
{
    if (pid > 1) {
        kill(-pid, sig);
        kill(pid, sig);
    }
}

/* ---- relations ---- */

static int gi_hard_dependent(int j, int i)
{
    int k;

    for (k = 0; k < gi_svc[j].ndep; k++) {
        if (gi_svc[j].dep[k] == i) {
            return 1;
        }
    }
    return 0;
}

/* clear the command-started mark of everything that depends on i, however far: it cannot run without i */
static void gi_release_dependents(int i, int depth)
{
    int j;

    if (depth > gi_nsvc) {
        return;
    }
    for (j = 0; j < gi_nsvc; j++) {
        if (gi_hard_dependent(j, i) && (gi_svc[j].explicit_start || gi_svc[j].want)) {
            gi_svc[j].explicit_start = 0;
            gi_release_dependents(j, depth + 1);
        }
    }
}

static int gi_dependents_active(int i)
{
    int j;

    for (j = 0; j < gi_nsvc; j++) {
        if (gi_svc[j].state != S_STOPPED && gi_hard_dependent(j, i)) {
            return 1;
        }
    }
    return 0;
}

static void gi_compute_want(void)
{
    int i, j, k, changed;

    for (i = 0; i < gi_nsvc; i++) {
        gi_svc[i].want = gi_svc[i].explicit_start && !gi_svc[i].forced_down;
    }
    do {
        changed = 0;
        for (i = 0; i < gi_nsvc; i++) {
            if (!gi_svc[i].want) {
                continue;
            }
            for (k = 0; (j = gi_edge_at(i, k)) >= 0; k++) {
                /* after = ordering only: it does not pull the other service in */
                if (k >= gi_svc[i].ndep + gi_svc[i].nsoft) {
                    break;
                }
                if (!gi_svc[j].want && !gi_svc[j].forced_down) {
                    gi_svc[j].want = 1;
                    changed = 1;
                }
            }
        }
    } while (changed);
}

/* a service we wait for is done with starting: running, failed, or not going to start at all */
static int gi_settled(int j)
{
    Svc *D = &gi_svc[j];

    return D->state == S_STARTED || D->failed || !D->want;
}

static int gi_can_start(Svc *S)
{
    int k, j;

    for (k = 0; k < S->ndep; k++) {
        if (gi_svc[S->dep[k]].state != S_STARTED) {
            return 0;
        }
    }
    for (k = 0; (j = gi_edge_at((int)(S - gi_svc), k)) >= 0; k++) {
        if (k >= S->ndep && !gi_settled(j)) {
            return 0;
        }
    }
    return 1;
}

static int gi_deps_ok(const Svc *S)
{
    int k;

    for (k = 0; k < S->ndep; k++) {
        if (gi_svc[S->dep[k]].state != S_STARTED) {
            return 0;
        }
    }
    return 1;
}

/* ---- transitions ---- */

static void gi_finish_stop(int i)
{
    Svc *S = &gi_svc[i];

    S->state = S_STOPPED;
    S->pid = 0;
    S->bg = 0;
    S->starter_done = 0;
    S->timer = TM_NONE;
    S->limit = 0;
    gi_log("%s stopped", S->name);
}

static void gi_fail_start(int i, const char *why)
{
    Svc *S = &gi_svc[i];

    if (S->pid > 0) {
        gi_kill_group(S->pid, SIGKILL);
    }
    S->state = S_STOPPED;
    S->pid = 0;
    S->bg = 0;
    S->starter_done = 0;
    S->timer = TM_NONE;
    S->limit = 0;
    S->failed = 1;
    S->explicit_start = 0;
    gi_log("%s failed to start: %s", S->name, why);
}

static void gi_begin_start(int i)
{
    Svc *S = &gi_svc[i];
    int pid = 0, err = 0;
    i64 now = gi_now_ms();

    S->state = S_STARTING;
    S->starter_done = 0;
    S->limit = S->start_timeout_s > 0 ? now + (i64)S->start_timeout_s * 1000 : 0;
    if (S->type == T_INTERNAL) {
        S->state = S_STARTED;
        S->limit = 0;
        gi_log("%s started", S->name);
        return;
    }
    if (gi_spawn(S, S->argv, &pid, &err) < 0) {
        gi_fail_start(i, strerror(err));
        return;
    }
    S->pid = pid;
    if (S->type == T_PROCESS) {
        S->state = S_STARTED;
        S->limit = 0;
        gi_log("%s started (pid %d)", S->name, pid);
    }
}

static void gi_begin_stop(int i)
{
    Svc *S = &gi_svc[i];
    int was = S->state, pid = 0, err = 0;
    i64 now = gi_now_ms();

    S->state = S_STOPPING;
    S->limit = S->stop_timeout_s > 0 ? now + (i64)S->stop_timeout_s * 1000 : 0;
    if (S->type == T_INTERNAL) {
        gi_finish_stop(i);
        return;
    }
    if (was == S_STARTING && S->pid > 0) {         /* the start command still runs: end it */
        gi_kill_group(S->pid, SIGTERM);
        return;
    }
    if (S->type == T_PROCESS || S->type == T_BGPROCESS) {
        if (S->pid <= 0) {
            gi_finish_stop(i);
            return;
        }
        if (S->stop_argv != NULL && S->stop_argv[0] != NULL) {
            if (gi_spawn(S, S->stop_argv, &pid, &err) == 0) {
                S->stop_pid = pid;
            } else {
                gi_kill_group(S->pid, SIGTERM);
            }
        } else {
            gi_kill_group(S->pid, SIGTERM);
            kill(S->pid, SIGCONT);
        }
        if (S->bg) {
            S->timer = TM_POLL;
            S->deadline = now + 50;
        }
        return;
    }
    /* scripted */
    if (S->stop_argv != NULL && S->stop_argv[0] != NULL && gi_spawn(S, S->stop_argv, &pid, &err) == 0) {
        S->stop_pid = pid;
        return;
    }
    gi_finish_stop(i);
}

/* the main process of a STARTED service is gone and nobody asked for it */
static void gi_handle_death(int i, int status)
{
    Svc *S = &gi_svc[i];
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int again = !gi_shutdown && (S->restart == R_YES || (S->restart == R_FAILURE && !ok));
    i64 now = gi_now_ms();
    int k, n = 0;

    S->pid = 0;
    S->bg = 0;
    S->exit_status = status;
    S->state = S_STOPPED;
    S->limit = 0;
    S->timer = TM_NONE;
    if (WIFSIGNALED(status)) {
        gi_log("%s killed by signal %d", S->name, WTERMSIG(status));
    } else {
        gi_log("%s exited with status %d", S->name, WEXITSTATUS(status));
    }
    if (again) {
        for (k = 0; k < S->nhist; k++) {
            if (now - S->hist[k] < (i64)S->restart_interval_s * 1000) {
                S->hist[n++] = S->hist[k];
            }
        }
        S->nhist = n;
        if (S->nhist >= S->restart_max || S->nhist >= 8) {
            gi_log("%s restarted too often, giving up", S->name);
            S->failed = 1;
            again = 0;
        }
    }
    if (again) {
        S->hist[S->nhist++] = now;
        S->timer = TM_RESTART;
        S->deadline = now + S->restart_delay_ms;
        return;
    }
    S->explicit_start = 0;
    gi_release_dependents(i, 0);
}

static void gi_child_exit(int i, int pid, int status)
{
    Svc *S = &gi_svc[i];
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

    if (S->stop_pid == pid) {
        S->stop_pid = 0;
        if (S->state == S_STOPPING && S->type == T_SCRIPTED && S->pid == 0) {
            gi_finish_stop(i);
        }
        return;
    }
    S->pid = 0;
    S->exit_status = status;
    if (S->state == S_STOPPING) {
        if (S->type == T_SCRIPTED && S->stop_pid > 0) {
            return;                                 /* the start command ended; the stop command is still running */
        }
        gi_finish_stop(i);
    } else if (S->state == S_STARTING) {
        if (!ok) {
            gi_fail_start(i, "the start command failed");
        } else if (S->type == T_SCRIPTED) {
            S->state = S_STARTED;
            S->limit = 0;
            gi_log("%s started", S->name);
        } else {                                    /* bgprocess: now wait for the pid file */
            S->starter_done = 1;
            S->timer = TM_POLL;
            S->deadline = gi_now_ms();
        }
    } else if (S->state == S_STARTED) {
        gi_handle_death(i, status);
    }
}

static void gi_poll_one(int i)
{
    Svc *S = &gi_svc[i];
    i64 now = gi_now_ms();
    FILE *f;
    long v = 0;

    S->timer = TM_NONE;
    if (S->state == S_STARTING && S->starter_done) {
        f = S->pidfile != NULL ? fopen(S->pidfile, "r") : NULL;
        if (f != NULL) {
            if (fscanf(f, "%ld", &v) != 1) {
                v = 0;
            }
            fclose(f);
        }
        if (v > 1 && kill((pid_t)v, 0) == 0) {
            S->pid = (int)v;
            S->bg = 1;
            S->state = S_STARTED;
            S->limit = 0;
            S->timer = TM_POLL;
            S->deadline = now + 500;
            gi_log("%s started (pid %d)", S->name, S->pid);
        } else {
            S->timer = TM_POLL;
            S->deadline = now + 50;
        }
    } else if (S->state == S_STARTED && S->bg) {
        if (kill(S->pid, 0) < 0 && errno == ESRCH) {
            gi_handle_death(i, 256);
        } else {
            S->timer = TM_POLL;
            S->deadline = now + 500;
        }
    } else if (S->state == S_STOPPING && S->bg) {
        if (kill(S->pid, 0) < 0 && errno == ESRCH) {
            gi_finish_stop(i);
        } else {
            S->timer = TM_POLL;
            S->deadline = now + 50;
        }
    }
}

static void gi_tick(void)
{
    i64 now = gi_now_ms();
    int i;
    Svc *S;

    for (i = 0; i < gi_nsvc; i++) {
        S = &gi_svc[i];
        if (S->limit != 0 && now >= S->limit) {
            if (S->state == S_STARTING) {
                gi_fail_start(i, "timed out");
            } else if (S->state == S_STOPPING) {
                gi_log("%s did not stop in time: SIGKILL", S->name);
                if (S->pid > 0) {
                    gi_kill_group(S->pid, SIGKILL);
                }
                if (S->stop_pid > 0) {
                    gi_kill_group(S->stop_pid, SIGKILL);
                }
                S->limit = 0;
                if (S->pid == 0 && S->stop_pid == 0) {
                    gi_finish_stop(i);
                }
            } else {
                S->limit = 0;
            }
        }
        if (S->timer != TM_NONE && now >= S->deadline) {
            if (S->timer == TM_RESTART) {
                S->timer = TM_NONE;
            } else {
                gi_poll_one(i);
            }
        }
    }
}

/* wait for every exited child; the ones that are no service's are just reaped (orphans of a PID 1) */
static void gi_reap(void)
{
    int st, i;
    pid_t p;

    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        for (i = 0; i < gi_nsvc; i++) {
            if (gi_svc[i].pid == (int)p || gi_svc[i].stop_pid == (int)p) {
                gi_child_exit(i, (int)p, st);
                break;
            }
        }
    }
}

static void gi_settle(void)
{
    int i, changed, running;
    Svc *S;

    do {
        changed = 0;
        gi_compute_want();
        for (i = 0; i < gi_nsvc; i++) {
            S = &gi_svc[i];
            running = S->state == S_STARTING || S->state == S_STARTED;
            if (running) {
                if ((!S->want || !gi_deps_ok(S)) && !gi_dependents_active(i)) {
                    gi_begin_stop(i);
                    changed = 1;
                }
            } else if (S->state == S_STOPPED && S->want && !S->failed && S->timer != TM_RESTART) {
                int k;

                for (k = 0; k < S->ndep; k++) {
                    if (gi_svc[S->dep[k]].failed) {
                        S->failed = 1;
                        S->explicit_start = 0;
                        gi_log("%s not started: %s failed", S->name, gi_svc[S->dep[k]].name);
                        changed = 1;
                        break;
                    }
                }
                if (!S->failed && gi_can_start(S)) {
                    gi_begin_start(i);
                    changed = 1;
                }
            }
        }
    } while (changed);
}

/* every service stopped and nothing waiting to restart */
static int gi_all_stopped(void)
{
    int i;

    for (i = 0; i < gi_nsvc; i++) {
        if (gi_svc[i].state != S_STOPPED || gi_svc[i].stop_pid > 0) {
            return 0;
        }
    }
    return 1;
}

/* ms until the next timer or limit, -1 when there is none */
static int gi_next_timeout(void)
{
    i64 now = gi_now_ms(), best = -1, t;
    int i;

    for (i = 0; i < gi_nsvc; i++) {
        t = 0;
        if (gi_svc[i].timer != TM_NONE) {
            t = gi_svc[i].deadline;
        }
        if (gi_svc[i].limit != 0 && (t == 0 || gi_svc[i].limit < t)) {
            t = gi_svc[i].limit;
        }
        if (t != 0 && (best < 0 || t < best)) {
            best = t;
        }
    }
    if (best < 0) {
        return -1;
    }
    return best <= now ? 0 : (int)(best - now);
}

/* start a service by command: load it, check it, clear what blocked it before. Returns 0 or -1 (gi_load_err set). */
static int gi_command_start(const char *name, int *idx_out)
{
    int idx = gi_load(name), k, j;

    if (idx < 0) {
        return -1;
    }
    if (gi_cycle_from(idx)) {
        gi_set_err("dependency cycle", name, 0);
        return -1;
    }
    {
        int stack[GI_MAXSVC], sp = 1, seen[GI_MAXSVC], cur;

        memset(seen, 0, sizeof(int) * (size_t)gi_nsvc);
        stack[0] = idx;
        seen[idx] = 1;
        while (sp > 0) {
            cur = stack[--sp];
            gi_svc[cur].failed = 0;
            gi_svc[cur].forced_down = 0;
            for (k = 0; (j = gi_edge_at(cur, k)) >= 0; k++) {
                if (!seen[j]) {
                    seen[j] = 1;
                    stack[sp++] = j;
                }
            }
        }
    }
    gi_svc[idx].explicit_start = 1;
    *idx_out = idx;
    return 0;
}

/* stop by command: the service and everything that depends on it */
static void gi_command_stop(int idx)
{
    gi_svc[idx].explicit_start = 0;
    gi_svc[idx].forced_down = 1;
    gi_svc[idx].failed = 0;
    gi_svc[idx].timer = TM_NONE;
    gi_release_dependents(idx, 0);
}
