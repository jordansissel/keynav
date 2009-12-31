CFLAGS=`pkg-config --cflags xext xinerama x11 xtst 2> /dev/null || echo -I/usr/X11R6/include -I/usr/local/include`
LDFLAGS=`pkg-config --libs xext xinerama x11 xtst 2> /dev/null || echo -L/usr/X11R6/lib -L/usr/local/lib -lX11 -lXtst -lXinerama -lXext` 

#CFLAGS+=-g
OTHERFILES=README CHANGELIST COPYRIGHT \
           keynavrc Makefile

MICROVERSION?=00

all: keynav

clean:
	rm *.o || true;
	make -C xdotool clean || true

# We'll try to detect 'libxdo' and use it if we find it.
# otherwise, build monolithic.
keynav: keynav.o
	@set -x; \
	if $(LD) -lxdo > /dev/null 2>&1 ; then \
		$(CC) $(LDFLAGS) -lxdo keynav.o -o $@; \
	else \
		make xdo.o; \
		$(CC) $(LDFLAGS) xdo.o keynav.o -o $@; \
	fi

xdo.o:
	make -C xdotool xdo.o
	cp xdotool/xdo.o .

package: clean
	NAME=keynav-`date +%Y%m%d`.$(MICROVERSION); \
	mkdir $${NAME}; \
	rsync --exclude '.*' -av *.c $(OTHERFILES) xdotool $${NAME}/; \
	tar -zcf $${NAME}.tar.gz $${NAME}/; \
	rm -rf $${NAME}/

