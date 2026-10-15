.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
LDLIBS ?= -ljson-c
PYTHON ?= python3

.PHONY: all test clean help

all: cenere

cenere: cenere.c cenere_install.c cenere_install.h Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ cenere.c cenere_install.c $(LDLIBS)

test: cenere
	$(PYTHON) tests/test_cenere.py $(abspath $<)
	CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" LDLIBS="$(LDLIBS)" $(PYTHON) tests/test_runner.py
	CC="$(CC)" CPPFLAGS="$(CPPFLAGS)" LDLIBS="$(LDLIBS)" $(PYTHON) tests/test_storage.py

clean:
	$(RM) cenere

help:
	@printf '%s\n' \
	  'make       Build ./cenere (requires a C compiler and json-c development files).' \
	  '           ./cenere --demo opens terminal menus and simulates the installation.' \
	  '           CENERE_DEFAULT_USERNAME=name ./cenere --demo sets the username default.' \
	  '           doas ./cenere starts the guided offline disk installer.' \
	  'make test  Build and test in temporary filesystems; no privilege escalation needed.' \
	  '           Requires Python 3, bubblewrap, mount, umount, and rsync.' \
	  '           Linux must allow user namespaces and tmpfs mounts.' \
	  '           Set RSYNC=/path/to/rsync to use a local rsync binary.' \
	  'make clean Remove build output.'
