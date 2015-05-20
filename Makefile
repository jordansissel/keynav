CFLAGS+=$(shell pkg-config --cflags cairo-xlib 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags xinerama 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags glib-2.0 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags xext 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags x11 2> /dev/null)
CFLAGS+=$(shell pkg-config --cflags xtst 2> /dev/null)

LDFLAGS+=$(shell pkg-config --libs cairo-xlib 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs xinerama 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs glib-2.0 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs xext 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs x11 2> /dev/null)
LDFLAGS+=$(shell pkg-config --libs xtst 2> /dev/null)
LDFLAGS+=-g

OTHERFILES=README CHANGELIST COPYRIGHT \
           keynavrc Makefile version.sh VERSION
#CFLAGS+=-DPROFILE_THINGS
#LDFLAGS+=-lrt

VERSION=$(shell sh version.sh)

#CFLAGS+=-pg -g
#LDFLAGS+=-pg -g
#LDFLAGS+=-L/usr/lib/debug/usr/lib/ -lcairo -lX11 -lXinerama -LXtst -lXext
#CFLAGS+=-O2

#CFLAGS+=-DPROFILE_THINGS
#LDFLAGS+=-lrt

.PHONY: all

all: keynav

clean:
	rm *.o keynav keynav_version.h || true;

keynav.o: keynav_version.h
keynav_version.h: version.sh

keynav: LDFLAGS+=-Xlinker -rpath=/usr/local/lib
keynav: keynav.o
	$(CC) keynav.o -o $@ $(LDFLAGS) -lxdo; \

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
