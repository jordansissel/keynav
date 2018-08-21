CFLAGS+=$(shell pkg-config --cflags cairo-xlib 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags xinerama 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags glib-2.0 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags x11 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags xrandr 2> /dev/null)

LDFLAGS+=$(shell pkg-config --libs cairo-xlib 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs xinerama 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs glib-2.0 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs x11 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs xrandr 2> /dev/null)
LDFLAGS+=-Xlinker -rpath=/usr/local/lib

PREFIX=/usr

OTHERFILES=README.md CHANGELIST COPYRIGHT keynav.pod \
           keynavrc Makefile version.sh VERSION

VERSION=$(shell sh version.sh)

.PHONY: all uninstall

all: keynav

clean:
	rm -f *.o keynav keynav_version.h keynav.1.gz

keynav.o: keynav_version.h
keynav_version.h: version.sh

debug:CFLAGS+=-DPROFILE_THINGS
#debug:CFLAGS+=-pg
debug:CFLAGS+=-g
#debug:LDFLAGS+=-lrt
debug: keynav.o
	$(CC) keynav.o -o keynav $(CFLAGS) $(LDFLAGS) -lxdo

keynav:CFLAGS+=-O2
keynav: keynav.o
	$(CC) keynav.o -o keynav $(CFLAGS) $(LDFLAGS) -lxdo
	strip keynav

keynav_version.h:
	sh version.sh --header > $@

VERSION:
	sh version.sh --shell > $@

pre-create-package:
	rm -f keynav_version.h VERSION
	$(MAKE) VERSION keynav_version.h

create-package: clean pre-create-package keynav_version.h
	NAME=keynav-$(VERSION); \
	mkdir $${NAME}; \
	rsync --exclude '.*' -av *.c $(OTHERFILES) $${NAME}/; \
	tar -zcf $${NAME}.tar.gz $${NAME}/; \
	rm -rf $${NAME}/

package: create-package test-package-build

test-package-build: create-package
	@NAME=keynav-$(VERSION); \
	tmp=$$(mktemp -d); \
	echo "Testing package $$NAME"; \
	tar -C $${tmp} -zxf $${NAME}.tar.gz; \
	make -C $${tmp}/$${NAME} keynav; \
	(cd $${tmp}/$${NAME}; ./keynav version); \
	rm -rf $${NAME}/
	rm -f $${NAME}.tar.gz

keynav.1: keynav.pod
	pod2man -c "" -r "" $< > $@

install: keynav keynav.1
	mkdir -p $(PREFIX)/bin
	install ./keynav $(PREFIX)/bin/keynav
	rm -f keynav.1.gz
	gzip keynav.1
	mkdir -p $(PREFIX)/share/man/man1
	install ./keynav.1.gz $(PREFIX)/share/man/man1/

uninstall:
	rm -f $(PREFIX)/bin/keynav
	rm -f $(PREFIX)/share/man/man1/keynav.1.gz
