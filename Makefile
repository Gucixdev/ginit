# ginit build. One translation unit (src/main.c includes the rest), C89, no dependencies beyond libc.
#   make          out/ginit and the ginitctl link
#   make test     the pedantic C89 build under gcc and clang, then tests/run against a sanitizer build and the normal one
#   make install  PREFIX (default /usr/local), DESTDIR for packagers: ginit, ginitctl, dinitctl
CC ?= gcc
PREFIX ?= /usr/local
STD = -std=c89 -pedantic -Wall -Wextra -Werror
CFLAGS ?= -O2

SRC = $(wildcard src/*.c src/*.h)

all: out/ginit

out:
	mkdir -p out

out/ginit: $(SRC) | out
	$(CC) $(STD) $(CFLAGS) src/main.c -o $@
	ln -sf ginit out/ginitctl

test: out/ginit
	@command -v clang > /dev/null && clang $(STD) -O2 src/main.c -o out/ginit-clang && echo "clang build ok" || echo "clang not installed: skipped"
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined src/main.c -o out/ginit-san
	ASAN_OPTIONS=detect_leaks=0 tests/run out/ginit-san
	tests/run out/ginit
	sh tests/pid1

install: out/ginit
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 out/ginit $(DESTDIR)$(PREFIX)/bin/ginit
	ln -sf ginit $(DESTDIR)$(PREFIX)/bin/ginitctl
	ln -sf ginit $(DESTDIR)$(PREFIX)/bin/dinitctl

clean:
	rm -rf out

.PHONY: all test install clean
