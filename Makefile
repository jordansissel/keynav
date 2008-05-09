CFLAGS=`pkg-config --cflags x11 xtst 2> /dev/null || echo -I/usr/X11R6/include -I/usr/local/include`
LDFLAGS=`pkg-config --libs x11 xtst 2> /dev/null || echo -L/usr/X11R6/lib -L/usr/local/lib -lX11 -lXtst` 

#CFLAGS+=-g
OTHERFILES=README CHANGELIST COPYRIGHT \
           keynavrc Makefile

all: keynav

clean:
	rm *.o || true;
	make -C xdotool clean || true

keynav: xdo.o keynav.o
	gcc $(LDFLAGS) xdo.o keynav.o -o $@

xdo.o:
	make -C xdotool xdo.o
	cp xdotool/xdo.o .

package: clean
	NAME=keynav-`date +%Y%m%d`; \
	mkdir $${NAME}; \
	rsync --exclude '.*' -av *.c $(OTHERFILES) xdotool $${NAME}/; \
	tar -zcf $${NAME}.tar.gz $${NAME}/; \
	rm -rf $${NAME}/

