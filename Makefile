CFLAGS+= $(shell pkg-config --cflags xinerama glib-2.0 2> /dev/null || echo -I/usr/X11R6/include -I/usr/local/include)
LDFLAGS+= $(shell pkg-config --libs xinerama glib-2.0 2> /dev/null || echo -L/usr/X11R6/lib -L/usr/local/lib -lX11 -lXtst -lXinerama -lXext -lglib)

#CFLAGS+=-g
OTHERFILES=README CHANGELIST COPYRIGHT \
           keynavrc Makefile version.sh

VERSION=$(shell sh version.sh)

.PHONY: all

all: keynav

clean:
	rm *.o || true;
	$(MAKE) -C xdotool clean || true

keynav.o: keynav_version.h
keynav_version.h: version.sh

# We'll try to detect 'libxdo' and use it if we find it.
# otherwise, build monolithic.
keynav: keynav.o
	@set -x; \
	if $(LD) -lxdo > /dev/null 2>&1 ; then \
		$(CC) keynav.o -o $@ $(LDFLAGS) -lxdo; \
	else \
		$(MAKE) keynav.static; \
	fi

.PHONY: keynav.static
keynav.static: keynav.o xdo.o
	$(CC) xdo.o keynav.o -o keynav `pkg-config --libs xext xtst` $(LDFLAGS)

keynav_version.h:
	sh version.sh --header > $@

xdo.o:
	$(MAKE) -C xdotool xdo.o
	cp xdotool/xdo.o .

pre-create-package:
	rm -f keynav_version.h

package: clean pre-create-package keynav_version.h
	NAME=keynav-$(VERSION); \
	mkdir $${NAME}; \
	rsync --exclude '.*' -av *.c $(OTHERFILES) xdotool $${NAME}/; \
	tar -zcf $${NAME}.tar.gz $${NAME}/; \
	rm -rf $${NAME}/
