/* svc.c - service files: the dinit syntax (a subset), loaded on first use from the service directories.
 *
 * A service file is named after the service and holds "key = value" lines ('#' starts a comment at the start of a
 * line or after a blank; "key += value" adds to a list). Keys:
 *   type             process (default), bgprocess, scripted, internal
 *   command          the program and its words; 'single', "double" quotes and \ escapes group words, no shell
 *   stop-command     run to stop the service (scripted: its exit is the stop); else SIGTERM
 *   depends-on       hard dependency: started first, stopped with it       (one name per line, += adds)
 *   depends-ms       same as depends-on here (a dependency that has to be started)
 *   waits-for        soft dependency: started and waited for, its failure does not matter
 *   after / before   ordering only, no dependency
 *   restart          yes | true | no | false | on-failure
 *   restart-delay    seconds (default 0.2)
 *   restart-limit-count, restart-limit-interval    at most COUNT restarts in INTERVAL seconds (3 in 10)
 *   start-timeout, stop-timeout   seconds, 0 = none (defaults 60 and 10)
 *   working-directory, logfile, pid-file (bgprocess), env-file (KEY=VALUE lines)
 *   options          accepted and ignored: runs-on-console, starts-on-console, shares-console, ...
 * Not supported (the file is refused with the line number): run-as and the socket, cgroup, rlimit and capability keys.
 * Dependencies are resolved after the parse; a service named by a dependency is loaded in turn. */

/* names that still have to be resolved to indices, per service */
typedef struct {
    char *dep[GI_MAXDEP], *soft[GI_MAXDEP], *after[GI_MAXDEP], *before[GI_MAXDEP];
    int ndep, nsoft, nafter, nbefore;
    int resolved;
} Raw;

static Raw gi_raw[GI_MAXSVC];
static char gi_load_err[256];          /* why the last gi_load failed */
static const char *gi_dirs[16];        /* service directories, first match wins */
static int gi_ndirs;

static void gi_set_err(const char *a, const char *b, int line)
{
    size_t la = strlen(a), lb = strlen(b);
    char num[24];

    if (la > sizeof(gi_load_err) - 64u) {
        la = sizeof(gi_load_err) - 64u;
    }
    if (lb > 100u) {
        lb = 100u;
    }
    memcpy(gi_load_err, a, la);
    gi_load_err[la] = '\0';
    if (lb > 0) {
        strcat(gi_load_err, ": ");
        strncat(gi_load_err, b, lb);
    }
    if (line > 0) {
        sprintf(num, " (line %d)", line);
        strcat(gi_load_err, num);
    }
}

/* ---- service directories ---- */
static void gi_dirs_init(int user_mode)
{
    const char *env = getenv("GINIT_SERVICE_DIR"), *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
    char *p, *q;
    char buf[512];

    gi_ndirs = 0;
    if (env != NULL && env[0] != '\0') {
        p = gi_strdup(env);
        while (p != NULL && *p != '\0' && gi_ndirs < 15) {
            q = strchr(p, ':');
            if (q != NULL) {
                *q++ = '\0';
            }
            if (p[0] != '\0') {
                gi_dirs[gi_ndirs++] = p;
            }
            p = q;
        }
        return;
    }
    if (user_mode) {
        if (xdg != NULL && xdg[0] != '\0') {
            sprintf(buf, "%.400s/ginit.d", xdg);
            gi_dirs[gi_ndirs++] = gi_strdup(buf);
            sprintf(buf, "%.400s/dinit.d", xdg);
            gi_dirs[gi_ndirs++] = gi_strdup(buf);
        } else if (home != NULL) {
            sprintf(buf, "%.400s/.config/ginit.d", home);
            gi_dirs[gi_ndirs++] = gi_strdup(buf);
            sprintf(buf, "%.400s/.config/dinit.d", home);
            gi_dirs[gi_ndirs++] = gi_strdup(buf);
        }
        return;
    }
    gi_dirs[gi_ndirs++] = "/etc/ginit.d";
    gi_dirs[gi_ndirs++] = "/etc/dinit.d";
    gi_dirs[gi_ndirs++] = "/run/dinit.d";
    gi_dirs[gi_ndirs++] = "/usr/local/lib/dinit.d";
    gi_dirs[gi_ndirs++] = "/lib/dinit.d";
    gi_dirs[gi_ndirs++] = "/usr/lib/dinit.d";
}

/* ---- parse helpers ---- */

/* split a command into words in place-free arena strings; NULL-terminated argv, or NULL with *err set */
static char **gi_words(const char *s, const char **err)
{
    char *w[GI_MAXARG + 1];
    char cur[GI_LINE];
    int n = 0;
    size_t len;
    char q;
    char **out;

    *err = NULL;
    for (;;) {
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (*s == '\0') {
            break;
        }
        len = 0;
        q = '\0';
        while (*s != '\0' && (q != '\0' || (*s != ' ' && *s != '\t'))) {
            if (len + 1u >= sizeof(cur)) {
                *err = "word too long";
                return NULL;
            }
            if (q == '\0' && (*s == '\'' || *s == '"')) {
                q = *s++;
            } else if (q != '\0' && *s == q) {
                q = '\0';
                s++;
            } else if (*s == '\\' && q != '\'' && s[1] != '\0') {
                cur[len++] = s[1];
                s += 2;
            } else {
                cur[len++] = *s++;
            }
        }
        if (q != '\0') {
            *err = "unterminated quote";
            return NULL;
        }
        if (n >= GI_MAXARG) {
            *err = "too many words in a command";
            return NULL;
        }
        cur[len] = '\0';
        w[n++] = gi_strdup(cur);
    }
    out = (char **)gi_alloc(sizeof(char *) * (size_t)(n + 1));
    memcpy(out, w, sizeof(char *) * (size_t)n);
    return out;
}

static char **gi_words_add(char **old, char **more)
{
    int a = 0, b = 0;
    char **out;

    while (old != NULL && old[a] != NULL) {
        a++;
    }
    while (more != NULL && more[b] != NULL) {
        b++;
    }
    out = (char **)gi_alloc(sizeof(char *) * (size_t)(a + b + 1));
    if (a > 0) {
        memcpy(out, old, sizeof(char *) * (size_t)a);
    }
    if (b > 0) {
        memcpy(out + a, more, sizeof(char *) * (size_t)b);
    }
    return out;
}

/* seconds with an optional fraction ("0.2", "10") to milliseconds; -1 when it is not a number */
static int gi_seconds_ms(const char *s)
{
    long whole = 0, frac = 0, scale = 100;
    int digits = 0;

    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (*s++ - '0');
        digits++;
        if (whole > 1000000L) {
            return -1;
        }
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            frac += (*s++ - '0') * scale;
            scale /= 10;
            digits++;
        }
    }
    return (*s != '\0' || digits == 0) ? -1 : (int)(whole * 1000 + frac);
}

static int gi_add_name(char **list, int *n, const char *val)
{
    char buf[GI_LINE], *tok;

    strncpy(buf, val, sizeof(buf) - 1u);
    buf[sizeof(buf) - 1u] = '\0';
    for (tok = strtok(buf, " \t"); tok != NULL; tok = strtok(NULL, " \t")) {
        if (*n >= GI_MAXDEP) {
            return 0;
        }
        list[(*n)++] = gi_strdup(tok);
    }
    return 1;
}

/* the env-file: KEY=VALUE lines, '#' comments; NULL-terminated array, NULL when empty or unreadable */
static char **gi_read_env(const char *path, const char *dir)
{
    char full[512], line[GI_LINE], *l;
    char *tmp[GI_MAXENV + 1];
    int n = 0;
    FILE *f;
    char **out;

    if (path[0] == '/') {
        strncpy(full, path, sizeof(full) - 1u);
        full[sizeof(full) - 1u] = '\0';
    } else {
        sprintf(full, "%.300s/%.200s", dir, path);
    }
    f = fopen(full, "r");
    if (f == NULL) {
        return NULL;
    }
    while (n < GI_MAXENV && fgets(line, sizeof(line), f) != NULL) {
        l = gi_trim(line);
        if (l[0] == '\0' || l[0] == '#' || strchr(l, '=') == NULL) {
            continue;
        }
        tmp[n++] = gi_strdup(l);
    }
    fclose(f);
    if (n == 0) {
        return NULL;
    }
    out = (char **)gi_alloc(sizeof(char *) * (size_t)(n + 1));
    memcpy(out, tmp, sizeof(char *) * (size_t)n);
    return out;
}

/* ---- one file ---- */

static int gi_valid_name(const char *n)
{
    size_t i, len = strlen(n);

    if (len == 0 || len >= GI_NAMELEN || n[0] == '.') {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (n[i] == '/' || (unsigned char)n[i] < 33u) {
            return 0;
        }
    }
    return 1;
}

/* fill S (already named, index i) from the file at path; 1 on success, else gi_load_err is set */
static int gi_parse_file(Svc *S, Raw *R, const char *path, const char *dir)
{
    FILE *f = fopen(path, "r");
    char line[GI_LINE], key[64], *l, *eq, *val;
    int no = 0, add, ms;
    const char *err;
    char **w;
    size_t kl;

    if (f == NULL) {
        gi_set_err("cannot open", path, 0);
        return 0;
    }
    S->type = T_PROCESS;
    S->restart = R_NO;
    S->restart_delay_ms = 200;
    S->restart_max = 3;
    S->restart_interval_s = 10;
    S->stop_timeout_s = 10;
    S->start_timeout_s = 60;
    while (fgets(line, sizeof(line), f) != NULL) {
        no++;
        if (strchr(line, '\n') == NULL && !feof(f)) {
            gi_set_err("line too long", S->name, no);
            fclose(f);
            return 0;
        }
        /* a comment starts at '#' when it is the first character or follows a blank */
        for (l = line; *l != '\0'; l++) {
            if (*l == '#' && (l == line || l[-1] == ' ' || l[-1] == '\t')) {
                *l = '\0';
                break;
            }
        }
        l = gi_trim(line);
        if (l[0] == '\0') {
            continue;
        }
        eq = l;
        while (*eq != '\0' && *eq != '=' && *eq != ':') {
            eq++;
        }
        if (*eq == '\0') {
            gi_set_err("expected key = value", l, no);
            fclose(f);
            return 0;
        }
        add = (eq > l && eq[-1] == '+' && *eq == '=');
        kl = (size_t)(eq - l) - (add ? 1u : 0u);
        while (kl > 0 && (l[kl - 1] == ' ' || l[kl - 1] == '\t')) {
            kl--;
        }
        if (kl == 0 || kl >= sizeof(key)) {
            gi_set_err("bad key", l, no);
            fclose(f);
            return 0;
        }
        memcpy(key, l, kl);
        key[kl] = '\0';
        val = gi_trim(eq + 1);
        if (strcmp(key, "type") == 0) {
            S->type = strcmp(val, "process") == 0 ? T_PROCESS : strcmp(val, "bgprocess") == 0 ? T_BGPROCESS
                      : strcmp(val, "scripted") == 0 ? T_SCRIPTED : strcmp(val, "internal") == 0 ? T_INTERNAL : -1;
            if (S->type < 0) {
                gi_set_err("unknown type", val, no);
                fclose(f);
                return 0;
            }
        } else if (strcmp(key, "command") == 0 || strcmp(key, "stop-command") == 0) {
            int is_stop = key[0] == 's';

            w = gi_words(val, &err);
            if (w == NULL) {
                gi_set_err(err, key, no);
                fclose(f);
                return 0;
            }
            if (is_stop) {
                S->stop_argv = add ? gi_words_add(S->stop_argv, w) : w;
            } else {
                S->argv = add ? gi_words_add(S->argv, w) : w;
            }
        } else if (strcmp(key, "depends-on") == 0 || strcmp(key, "depends-ms") == 0) {
            if (!gi_add_name(R->dep, &R->ndep, val)) {
                gi_set_err("too many dependencies", key, no);
                fclose(f);
                return 0;
            }
        } else if (strcmp(key, "waits-for") == 0) {
            if (!gi_add_name(R->soft, &R->nsoft, val)) {
                gi_set_err("too many dependencies", key, no);
                fclose(f);
                return 0;
            }
        } else if (strcmp(key, "after") == 0) {
            if (!gi_add_name(R->after, &R->nafter, val)) {
                gi_set_err("too many entries", key, no);
                fclose(f);
                return 0;
            }
        } else if (strcmp(key, "before") == 0) {
            if (!gi_add_name(R->before, &R->nbefore, val)) {
                gi_set_err("too many entries", key, no);
                fclose(f);
                return 0;
            }
        } else if (strcmp(key, "restart") == 0) {
            if (strcmp(val, "yes") == 0 || strcmp(val, "true") == 0) {
                S->restart = R_YES;
            } else if (strcmp(val, "no") == 0 || strcmp(val, "false") == 0) {
                S->restart = R_NO;
            } else if (strcmp(val, "on-failure") == 0) {
                S->restart = R_FAILURE;
            } else {
                gi_set_err("restart is yes, no or on-failure", val, no);
                fclose(f);
                return 0;
            }
        } else if (strcmp(key, "restart-delay") == 0 || strcmp(key, "start-timeout") == 0 ||
                   strcmp(key, "stop-timeout") == 0 || strcmp(key, "restart-limit-interval") == 0 ||
                   strcmp(key, "restart-limit-count") == 0) {
            ms = gi_seconds_ms(val);
            if (ms < 0) {
                gi_set_err("a number of seconds is expected", key, no);
                fclose(f);
                return 0;
            }
            if (strcmp(key, "restart-delay") == 0) {
                S->restart_delay_ms = ms;
            } else if (strcmp(key, "start-timeout") == 0) {
                S->start_timeout_s = ms / 1000;
            } else if (strcmp(key, "stop-timeout") == 0) {
                S->stop_timeout_s = ms / 1000;
            } else if (strcmp(key, "restart-limit-interval") == 0) {
                S->restart_interval_s = ms / 1000;
            } else {
                S->restart_max = ms / 1000;
            }
        } else if (strcmp(key, "working-directory") == 0) {
            S->workdir = gi_strdup(val);
        } else if (strcmp(key, "logfile") == 0) {
            S->logfile = gi_strdup(val);
        } else if (strcmp(key, "pid-file") == 0) {
            S->pidfile = gi_strdup(val);
        } else if (strcmp(key, "env-file") == 0) {
            S->env = gi_read_env(val, dir);
            if (S->env == NULL) {
                gi_set_err("env-file empty or unreadable", val, no);
                fclose(f);
                return 0;
            }
        } else if (strcmp(key, "options") == 0 || strcmp(key, "smooth-recovery") == 0 ||
                   strcmp(key, "term-signal") == 0 || strcmp(key, "log-type") == 0) {
            /* accepted, no effect here */
        } else {
            gi_set_err("unsupported or unknown setting", key, no);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    if ((S->type == T_PROCESS || S->type == T_BGPROCESS || S->type == T_SCRIPTED) &&
        (S->argv == NULL || S->argv[0] == NULL)) {
        gi_set_err("this type needs a command", S->name, 0);
        return 0;
    }
    if (S->type == T_BGPROCESS && S->pidfile == NULL) {
        gi_set_err("bgprocess needs pid-file", S->name, 0);
        return 0;
    }
    return 1;
}

/* index of the loaded service, -1 when it is not loaded */
static int gi_lookup(const char *name)
{
    int i;

    for (i = 0; i < gi_nsvc; i++) {
        if (strcmp(gi_svc[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* load NAME from the first directory that has it; the new index, or -1 with gi_load_err set */
static int gi_load_one(const char *name)
{
    char path[640];
    struct stat st;
    int i, idx;

    if (!gi_valid_name(name)) {
        gi_set_err("bad service name", name, 0);
        return -1;
    }
    for (i = 0; i < gi_ndirs; i++) {
        sprintf(path, "%.400s/%.200s", gi_dirs[i], name);
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            break;
        }
    }
    if (i == gi_ndirs) {
        gi_set_err("no such service", name, 0);
        return -1;
    }
    if (gi_nsvc >= GI_MAXSVC) {
        gi_set_err("too many services", name, 0);
        return -1;
    }
    idx = gi_nsvc;
    memset(&gi_svc[idx], 0, sizeof(Svc));
    memset(&gi_raw[idx], 0, sizeof(Raw));
    gi_svc[idx].name = gi_strdup(name);
    gi_svc[idx].state = S_STOPPED;
    if (!gi_parse_file(&gi_svc[idx], &gi_raw[idx], path, gi_dirs[i])) {
        return -1;
    }
    gi_nsvc++;
    return idx;
}

static int gi_edge(int *list, int *n, int to)
{
    int i;

    for (i = 0; i < *n; i++) {
        if (list[i] == to) {
            return 1;
        }
    }
    if (*n >= GI_MAXDEP) {
        return 0;
    }
    list[(*n)++] = to;
    return 1;
}

/* Load NAME and everything it names, resolve the names to indices. Returns the index, or -1 with gi_load_err set.
 * On failure nothing stays loaded from this call (see gi_load): a half-resolved service must not be found by name later. */
static int gi_load_graph(const char *name)
{
    int idx = gi_lookup(name), start, i, j, t;
    Raw *R;
    Svc *S;

    if (idx >= 0) {
        return idx;
    }
    start = gi_nsvc;
    idx = gi_load_one(name);
    if (idx < 0) {
        return -1;
    }
    for (i = start; i < gi_nsvc; i++) {
        R = &gi_raw[i];
        S = &gi_svc[i];
        if (R->resolved) {
            continue;
        }
        R->resolved = 1;
        for (j = 0; j < R->ndep; j++) {
            if ((t = gi_lookup(R->dep[j])) < 0 && (t = gi_load_one(R->dep[j])) < 0) {
                char why[256];

                strcpy(why, gi_load_err);
                gi_set_err(S->name, why, 0);
                return -1;
            }
            if (!gi_edge(S->dep, &S->ndep, t)) {
                gi_set_err("too many dependencies", S->name, 0);
                return -1;
            }
        }
        for (j = 0; j < R->nsoft; j++) {
            if ((t = gi_lookup(R->soft[j])) < 0 && (t = gi_load_one(R->soft[j])) < 0) {
                /* a soft dependency that does not exist is skipped, like dinit */
                continue;
            }
            gi_edge(S->soft, &S->nsoft, t);
        }
        for (j = 0; j < R->nafter; j++) {
            if ((t = gi_lookup(R->after[j])) < 0 && (t = gi_load_one(R->after[j])) < 0) {
                continue;
            }
            gi_edge(S->after, &S->nafter, t);
        }
        for (j = 0; j < R->nbefore; j++) {
            if ((t = gi_lookup(R->before[j])) < 0 && (t = gi_load_one(R->before[j])) < 0) {
                continue;
            }
            gi_edge(gi_svc[t].after, &gi_svc[t].nafter, i);
        }
    }
    return idx;
}

/* gi_load_graph, and when it fails drop every service it loaded: their dependency lists may be cut short, and a later
 * start of the same name would otherwise find the half-resolved service and start it without the missing dependency.
 * The only edges from older services into the dropped ones are "before" entries in their after lists. */
static int gi_load(const char *name)
{
    int start = gi_nsvc, i, j, k;
    int r = gi_load_graph(name);

    if (r >= 0) {
        return r;
    }
    for (i = 0; i < start; i++) {
        for (j = 0, k = 0; j < gi_svc[i].nafter; j++) {
            if (gi_svc[i].after[j] < start) {
                gi_svc[i].after[k++] = gi_svc[i].after[j];
            }
        }
        gi_svc[i].nafter = k;
    }
    for (i = start; i < gi_nsvc; i++) {
        memset(&gi_svc[i], 0, sizeof(Svc));
        memset(&gi_raw[i], 0, sizeof(Raw));
    }
    gi_nsvc = start;
    return -1;
}

/* k-th edge of svc n over every kind that makes one service wait for another; -1 past the end */
static int gi_edge_at(int n, int k)
{
    Svc *S = &gi_svc[n];

    if (k < S->ndep) {
        return S->dep[k];
    }
    k -= S->ndep;
    if (k < S->nsoft) {
        return S->soft[k];
    }
    k -= S->nsoft;
    return k < S->nafter ? S->after[k] : -1;
}

/* does a wait-for cycle run through the services reachable from i? Such a start would never finish. */
static int gi_cycle_from(int i)
{
    int stack[GI_MAXSVC], pos[GI_MAXSVC], color[GI_MAXSVC];
    int sp, n, c;

    memset(color, 0, sizeof(int) * (size_t)gi_nsvc);
    stack[0] = i;
    pos[0] = 0;
    color[i] = 1;
    sp = 1;
    while (sp > 0) {
        n = stack[sp - 1];
        c = gi_edge_at(n, pos[sp - 1]++);
        if (c < 0) {
            color[n] = 2;
            sp--;
        } else if (color[c] == 1) {
            return 1;
        } else if (color[c] == 0) {
            color[c] = 1;
            stack[sp] = c;
            pos[sp] = 0;
            sp++;
        }
    }
    return 0;
}
