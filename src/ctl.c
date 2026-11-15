/* ctl.c - the control socket (server side, inside the daemon) and the ginitctl client.
 *
 * Protocol: the client sends one line "COMMAND [ARGS]\n"; the daemon answers with text whose first character is '+'
 * (success) or '-' (failure) and closes the connection. start/stop/restart answer when the service has finished
 * starting or stopping, so a waiting client is just a connection that stays open.
 *   list                      one line per loaded service
 *   status NAME               state of one service
 *   start [-n] NAME           start it and what it needs (-n: do not wait)
 *   stop [-n] NAME            stop it and what depends on it
 *   restart [-n] NAME
 *   shutdown [quit|reboot|poweroff]
 *   ping
 * The socket is created owner-only (mode 0600). */

#define GI_MAXCLIENT 16

enum { C_FREE, C_READ, C_START, C_STOP, C_RESTART };

typedef struct {
    int fd;
    int op;
    int idx;
    int len;
    char buf[GI_LINE];
} Client;

static Client gi_cl[GI_MAXCLIENT];
static int gi_listen_fd = -1;
static char gi_sock_buf[128];

static const char *gi_sock_path(void)
{
    const char *e = getenv("GINIT_SOCKET"), *x = getenv("XDG_RUNTIME_DIR");

    if (e != NULL && e[0] != '\0') {
        return e;
    }
    if (getuid() == 0) {
        return "/run/ginit.socket";
    }
    if (x != NULL && x[0] == '/' && strlen(x) < 100) {
        sprintf(gi_sock_buf, "%s/ginit.socket", x);
    } else {
        sprintf(gi_sock_buf, "/tmp/ginit-%lu.socket", (unsigned long)getuid());
    }
    return gi_sock_buf;
}

static int gi_addr(struct sockaddr_un *a, const char *path)
{
    if (strlen(path) >= sizeof(a->sun_path)) {
        return -1;
    }
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, path);
    return 0;
}

/* ---- server ---- */

static void gi_listen(const char *path)
{
    struct sockaddr_un a;
    int fd, probe;
    mode_t old;

    if (gi_addr(&a, path) < 0) {
        gi_die("control socket path too long");
    }
    probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe >= 0) {
        if (connect(probe, (struct sockaddr *)&a, sizeof(a)) == 0) {
            close(probe);
            gi_die("another ginit already answers on that socket");
        }
        close(probe);
        unlink(path);                       /* a stale socket file from a daemon that is gone */
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        gi_die("socket failed");
    }
    old = umask(077);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 8) < 0) {
        umask(old);
        fprintf(stderr, "ginit: cannot listen on %s: %s\n", path, strerror(errno));
        exit(GI_FAIL);
    }
    umask(old);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    gi_listen_fd = fd;
}

static void gi_cl_close(Client *c)
{
    if (c->fd >= 0) {
        close(c->fd);
    }
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void gi_cl_init(void)
{
    int i;

    for (i = 0; i < GI_MAXCLIENT; i++) {
        gi_cl[i].fd = -1;
    }
}

static void gi_reply(Client *c, int ok, const char *fmt, ...)
{
    char out[65536];
    va_list ap;
    int n;

    out[0] = ok ? '+' : '-';
    va_start(ap, fmt);
    n = vsnprintf(out + 1, sizeof(out) - 2u, fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n > sizeof(out) - 3u) {
        n = (int)sizeof(out) - 3;
    }
    out[n + 1] = '\n';
    if (write(c->fd, out, (size_t)n + 2u) < 0) {
        /* the client went away: nothing to do */
    }
    gi_cl_close(c);
}

static void gi_describe(const Svc *S, char *out, size_t size)
{
    if (S->pid > 0) {
        snprintf(out, size, "%s: %s (pid %d)", S->name, gi_state_name(S), S->pid);
    } else {
        snprintf(out, size, "%s: %s", S->name, gi_state_name(S));
    }
}

static void gi_do_list(Client *c)
{
    char out[60000], line[300];
    int i;
    size_t n = 0, l;

    out[0] = '\0';
    for (i = 0; i < gi_nsvc; i++) {
        gi_describe(&gi_svc[i], line, sizeof(line));
        l = strlen(line);
        if (n + l + 2u >= sizeof(out)) {
            break;
        }
        memcpy(out + n, line, l);
        n += l;
        out[n++] = '\n';
        out[n] = '\0';
    }
    if (n > 0) {
        out[n - 1] = '\0';
    }
    gi_reply(c, 1, "%s", n > 0 ? out : "no services loaded");
}

/* start or restart NAME for client c */
static void gi_cmd_start(Client *c, const char *name, int nowait, int restart)
{
    int idx;

    if (name == NULL) {
        gi_reply(c, 0, "%s needs a service name", restart ? "restart" : "start");
        return;
    }
    if (gi_shutdown) {
        gi_reply(c, 0, "shutting down");
        return;
    }
    if (restart) {
        if ((idx = gi_load(name)) < 0) {
            gi_reply(c, 0, "%s", gi_load_err);
            return;
        }
        if (gi_svc[idx].state != S_STOPPED || gi_svc[idx].explicit_start) {
            gi_command_stop(idx);
            gi_settle();
            if (nowait) {
                gi_reply(c, 1, "%s: restarting", gi_svc[idx].name);
            } else {
                c->idx = idx;
                c->op = C_RESTART;
            }
            return;
        }
    }
    if (gi_command_start(name, &idx) < 0) {
        gi_reply(c, 0, "%s", gi_load_err);
        return;
    }
    gi_settle();
    if (nowait) {
        gi_reply(c, 1, "%s: starting", gi_svc[idx].name);
    } else {
        c->idx = idx;
        c->op = C_START;
    }
}

/* act on one request line */
static void gi_serve(Client *c, char *line)
{
    char *cmd = strtok(line, " \t"), *a1, *a2, *name = NULL;
    int nowait = 0, idx;
    char buf[300];

    if (cmd == NULL) {
        gi_reply(c, 0, "empty command");
        return;
    }
    a1 = strtok(NULL, " \t");
    a2 = a1 != NULL ? strtok(NULL, " \t") : NULL;
    if (a1 != NULL && strcmp(a1, "-n") == 0) {
        nowait = 1;
        name = a2;
    } else {
        name = a1;
    }
    if (strcmp(cmd, "ping") == 0) {
        gi_reply(c, 1, "pong");
    } else if (strcmp(cmd, "list") == 0) {
        gi_do_list(c);
    } else if (strcmp(cmd, "status") == 0) {
        if (name == NULL) {
            gi_do_list(c);
        } else if ((idx = gi_load(name)) < 0) {
            gi_reply(c, 0, "%s", gi_load_err);
        } else {
            gi_describe(&gi_svc[idx], buf, sizeof(buf));
            gi_reply(c, 1, "%s", buf);
        }
    } else if (strcmp(cmd, "start") == 0 || strcmp(cmd, "restart") == 0) {
        gi_cmd_start(c, name, nowait, cmd[0] == 'r');
    } else if (strcmp(cmd, "stop") == 0) {
        if (name == NULL) {
            gi_reply(c, 0, "stop needs a service name");
        } else if ((idx = gi_lookup(name)) < 0) {
            gi_reply(c, 0, "%s is not loaded", name);
        } else {
            gi_command_stop(idx);
            gi_settle();
            if (nowait) {
                gi_reply(c, 1, "%s: stopping", gi_svc[idx].name);
            } else {
                c->idx = idx;
                c->op = C_STOP;
            }
        }
    } else if (strcmp(cmd, "shutdown") == 0) {
        int mode = getpid() == 1 ? GI_SHUT_POWEROFF : GI_SHUT_QUIT;

        if (a1 != NULL) {
            mode = strcmp(a1, "reboot") == 0 ? GI_SHUT_REBOOT : strcmp(a1, "poweroff") == 0 ? GI_SHUT_POWEROFF
                   : strcmp(a1, "quit") == 0 ? GI_SHUT_QUIT : 0;
        }
        if (mode == 0) {
            gi_reply(c, 0, "shutdown takes quit, reboot or poweroff");
            return;
        }
        gi_sig_stop = mode;
        gi_reply(c, 1, "shutting down");
    } else {
        gi_reply(c, 0, "unknown command '%s' (list, status, start, stop, restart, shutdown)", cmd);
    }
}

static void gi_accept(void)
{
    int fd, i;

    while ((fd = accept(gi_listen_fd, NULL, NULL)) >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        for (i = 0; i < GI_MAXCLIENT && gi_cl[i].fd >= 0; i++) {
            /* find a free slot */
        }
        if (i == GI_MAXCLIENT) {
            if (write(fd, "-too many clients\n", 18) < 0) {
                /* gone */
            }
            close(fd);
            continue;
        }
        memset(&gi_cl[i], 0, sizeof(gi_cl[i]));
        gi_cl[i].fd = fd;
        gi_cl[i].op = C_READ;
    }
}

static void gi_cl_readable(Client *c)
{
    ssize_t n;
    char *nl;

    n = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1u - (size_t)c->len);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        gi_cl_close(c);
        return;
    }
    if (n < 0) {
        return;
    }
    if (c->op != C_READ) {
        return;                                     /* a waiting client has nothing more to say */
    }
    c->len += (int)n;
    c->buf[c->len] = '\0';
    nl = strchr(c->buf, '\n');
    if (nl != NULL) {
        *nl = '\0';
        gi_serve(c, c->buf);
    } else if ((size_t)c->len >= sizeof(c->buf) - 1u) {
        gi_reply(c, 0, "request too long");
    }
}

/* answer the clients whose service has reached the state they waited for */
static void gi_clients_progress(void)
{
    int i;
    Client *c;
    Svc *S;

    for (i = 0; i < GI_MAXCLIENT; i++) {
        c = &gi_cl[i];
        if (c->fd < 0 || c->op < C_START) {
            continue;
        }
        S = &gi_svc[c->idx];
        if (gi_shutdown && gi_sig_stop == 0 && c->op != C_STOP) {
            gi_reply(c, 0, "%s: shutting down", S->name);
        } else if (c->op == C_STOP) {
            if (S->state == S_STOPPED && S->timer == TM_NONE) {
                gi_reply(c, 1, "%s: stopped", S->name);
            }
        } else if (c->op == C_RESTART) {
            if (S->state == S_STOPPED && S->timer == TM_NONE) {
                int idx;

                if (gi_command_start(S->name, &idx) < 0) {
                    gi_reply(c, 0, "%s", gi_load_err);
                } else {
                    c->op = C_START;
                    gi_settle();
                }
            }
        } else if (c->op == C_START) {
            if (S->state == S_STARTED) {
                gi_reply(c, 1, "%s: started", S->name);
            } else if (S->failed) {
                gi_reply(c, 0, "%s: failed to start", S->name);
            } else if (S->state == S_STOPPED && !S->want && S->timer == TM_NONE) {
                gi_reply(c, 0, "%s: stopped again", S->name);
            }
        }
    }
}

/* ---- client: ginitctl ---- */

static int gi_ctl_main(int argc, char **argv)
{
    const char *path = gi_sock_path();
    struct sockaddr_un a;
    char req[GI_LINE], reply[65536];
    size_t n = 0, l;
    ssize_t r;
    int fd, i, first = 1;

    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        path = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        fputs("usage: ginitctl [-p SOCKET] list | status [NAME] | start [-n] NAME | stop [-n] NAME |\n"
              "                restart [-n] NAME | shutdown [quit|reboot|poweroff]\n"
              "The socket is $GINIT_SOCKET, else /run/ginit.socket (root) or $XDG_RUNTIME_DIR/ginit.socket.\n"
              "Exit 0 done, 1 refused or failed, 2 bad usage, 3 no daemon.\n",
              argc < 2 ? stderr : stdout);
        return argc < 2 ? GI_USAGE : GI_OK;
    }
    req[0] = '\0';
    for (i = 1; i < argc; i++) {
        l = strlen(argv[i]);
        if (strlen(req) + l + 2u >= sizeof(req)) {
            fputs("ginitctl: command too long\n", stderr);
            return GI_USAGE;
        }
        if (!first) {
            strcat(req, " ");
        }
        strcat(req, argv[i]);
        first = 0;
    }
    strcat(req, "\n");
    if (gi_addr(&a, path) < 0) {
        fputs("ginitctl: socket path too long\n", stderr);
        return GI_USAGE;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "ginitctl: cannot reach ginit at %s: %s\n", path, strerror(errno));
        return GI_NODAEMON;
    }
    if (write(fd, req, strlen(req)) < 0) {
        fputs("ginitctl: write failed\n", stderr);
        return GI_NODAEMON;
    }
    while ((r = read(fd, reply + n, sizeof(reply) - 1u - n)) > 0) {
        n += (size_t)r;
    }
    close(fd);
    reply[n] = '\0';
    if (n == 0) {
        fputs("ginitctl: the daemon closed the connection without an answer\n", stderr);
        return GI_FAIL;
    }
    fputs(reply + 1, reply[0] == '+' ? stdout : stderr);
    return reply[0] == '+' ? GI_OK : GI_FAIL;
}
