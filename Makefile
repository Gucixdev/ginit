# ginit build. One translation unit (src/main.c includes the rest), C89, no dependencies beyond libc.
#   make          build/ginit and the ginitctl link
#   make test     the pedantic C89 build under gcc and clang, then tests/run against a sanitizer build and the normal one
#   make install  PREFIX (default /usr/local), DESTDIR for packagers: ginit, ginitctl, dinitctl
CC ?= gcc
PREFIX ?= /usr/local
STD = -std=c89 -pedantic -Wall -Wextra -Werror
CFLAGS ?= -O2

SRC = $(wildcard src/*.c src/*.h)

all: build/ginit

build:
	mkdir -p build

build/ginit: $(SRC) | build
	$(CC) $(STD) $(CFLAGS) src/main.c -o $@
	ln -sf ginit build/ginitctl

test: build/ginit
	@command -v clang > /dev/null && clang $(STD) -O2 src/main.c -o build/ginit-clang && echo "clang build ok" || echo "clang not installed: skipped"
	$(CC) $(STD) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined src/main.c -o build/ginit-san
	ASAN_OPTIONS=detect_leaks=0 tests/run build/ginit-san
	tests/run build/ginit

install: build/ginit
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 build/ginit $(DESTDIR)$(PREFIX)/bin/ginit
	ln -sf ginit $(DESTDIR)$(PREFIX)/bin/ginitctl
	ln -sf ginit $(DESTDIR)$(PREFIX)/bin/dinitctl

clean:
	rm -rf build

.PHONY: all test install clean
