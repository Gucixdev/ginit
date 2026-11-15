# ginit

A small init and service manager that reads **dinit service files**, written in C89 with no dependency beyond libc.
`ginit` is the daemon, `ginitctl` (also callable as `dinitctl`) controls it. One static arena, no malloc, about 2000 lines.

**Status:** works as a user-level service manager and passes its tests (65 checks, also under ASan and UBSan). It has
**not been run as PID 1 on a real machine**. `tests/pid1` runs it as PID 1 in a private user and pid namespace: orphans are
reaped and the shutdown path starts; the final `reboot(2)` call is refused there and stays untested.
Do not point a boot loader at it yet without a rescue entry.

## Use

```
./run                       build, test, then boot the example services
./install                   ginit, ginitctl, dinitctl into ~/.local/bin (PREFIX=/usr/local, DESTDIR=... also work)

ginit [-u] [-q] [-d DIR[:DIR]] [-p SOCKET] [SERVICE...]     SERVICE defaults to "boot"
ginitctl list | status [NAME] | start [-n] NAME | stop [-n] NAME | restart [-n] NAME | shutdown [quit|reboot|poweroff]
```

`start`, `stop` and `restart` return when the service has finished starting or stopping (`-n`: at once). Exit codes of
`ginitctl`: 0 done, 1 refused or failed, 2 bad usage, 3 no daemon. `reboot` and `poweroff` as program names are
`ginitctl shutdown reboot|poweroff`.

Not root and not PID 1 means user mode: services come from `~/.config/ginit.d`, then `~/.config/dinit.d`, and the
socket is `$XDG_RUNTIME_DIR/ginit.socket`. As root or PID 1: `/etc/ginit.d`, `/etc/dinit.d`, `/run/dinit.d`,
`/usr/local/lib/dinit.d`, `/lib/dinit.d`, `/usr/lib/dinit.d` and `/run/ginit.socket`. `-d` / `$GINIT_SERVICE_DIR` and
`-p` / `$GINIT_SOCKET` override both. The socket is mode 0600.

## Service files

A file named after the service, `key = value` lines, `#` comments (at line start or after a blank), `key += value` adds
to a list. `examples/ginit.d` has three.

| key | meaning |
|---|---|
| `type` | `process` (default), `bgprocess` (needs `pid-file`), `scripted`, `internal` |
| `command`, `stop-command` | words with `'single'`, `"double"` quotes and `\` escapes; no shell, no `$VAR` expansion |
| `depends-on`, `depends-ms` | hard: started first; it stops when they leave, and they stop only after it |
| `waits-for` | soft: started and waited for, its failure does not matter |
| `after`, `before` | ordering only, no dependency |
| `restart` | `yes`, `no`, `on-failure`; `restart-delay` (default 0.2 s); `restart-limit-count` / `-interval` (3 in 10 s) |
| `start-timeout`, `stop-timeout` | seconds, 0 = none (60 and 10); at the stop timeout the process group gets SIGKILL |
| `working-directory`, `logfile`, `env-file` | `KEY=VALUE` lines |
| `options`, `smooth-recovery`, `term-signal`, `log-type` | accepted, no effect |

Anything else (`run-as`, `socket-listen`, `rlimit-*`, `cgroup`, `capabilities`...) makes the file **refused with the key
and line number**, so a service never starts with a setting that was silently dropped.

## How it decides

A service is *wanted* when a command started it or a wanted service depends on it (hard or soft), unless a command
stopped it. A wanted service starts when its hard dependencies are started and what it waits for has finished
starting or failed. A running service stops when it is no longer wanted or a hard dependency left the started state,
and only after everything depending on it has stopped. A service that dies unexpectedly stops its dependents; with
`restart` they come back once it is up again. Without it, the commands that started them are forgotten, like dinit.

Dependency cycles (of any kind, `after` included) are refused when a start is asked for.

## Layout

```
src/base.h      types, the arena, the service table
src/svc.c       service files: parser, loading, name resolution, cycle check
src/engine.c    wanted/start/stop/restart rules, spawning, timers, reaping
src/ctl.c       the control socket and the ginitctl client
src/main.c      the daemon loop, PID 1 duties, argv[0] dispatch
tests/run       drives a real daemon in user mode on a temp socket
```

Not done: socket activation, run-as, cgroups, `reload`, `enable`/`disable`, service environment commands, a boot
sequence for a real system (mounting is the job of a scripted `boot` service you write).

## More

- [docs/bench.md](docs/bench.md): `./bench` results (start of 500 services in about 150 ms, memory, graph size) and what they found
