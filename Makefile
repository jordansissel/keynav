CFLAGS=`pkg-config --cflags x11 xtst`
LDFLAGS=`pkg-config --libs x11 xtst`

all:
	@echo "Please run xmkmf."

keynav: xdo.o keynav.o
	gcc $(LDFLAGS) xdo.o keynav.o -o $@

xdo.o:
	make -C xdotool xdo.o
	cp xdotool/xdo.o .
