/*
 * Visual user-directed binary search for something to point your mouse at.
 */

/* XXX: fine-tuning once you're zoomed in? (done?)
 * XXX: "cancel" action (done)
 * XXX: vi-keys half-split (done)
 * XXX: use XGrabKeyboard instead of XGrabKey for the movement mode?  (done)
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XTest.h>

#include "xdotool/xdo.h"
#include "xdotool/xdo_util.h"

Display *dpy;
Window root;
XWindowAttributes rootattr;
Window zone;
xdo_t *xdo;

struct wininfo {
  int x;
  int y;
  int w;
  int h;
} wininfo;

void cmd_cut_up(char *args);
void cmd_cut_down(char *args);
void cmd_cut_left(char *args);
void cmd_cut_right(char *args);
void cmd_move_up(char *args);
void cmd_move_down(char *args);
void cmd_move_left(char *args);
void cmd_move_right(char *args);
void cmd_warp(char *args);
void cmd_click(char *args);
void cmd_doubleclick(char *args);
void cmd_drag(char *args);
void cmd_start(char *args);
void cmd_end(char *args);

void update();

struct dispatch {
  char *command;
  void (*func)();
} dispatch[] = {
  "cut-up", NULL,
  "cut-down", NULL,
  "cut-left", NULL,
  "cut-right", NULL,
  "move-up", NULL,
  "move-down", NULL,
  "move-left", NULL,
  "move-right", NULL,

  "warp", NULL,
  "click", NULL,     
  "doubleclick", NULL,
  "drag", NULL,

  "start", NULL,
  "end", NULL, 
  NULL, NULL,
};

struct keybinding {
  char *cmd;
  char *args;
  int keycode;
  int mods;
} *keybindings = NULL;

int nkeybindings = 0;
int keybinding_size = 0;

int parse_keycode(char *keyseq) {
  char *tokctx;
  char *strptr;
  char *tok;

  strptr = keyseq;
  while ((tok = strtok_r(strptr, "+", &tokctx)) != NULL) {
    int i;
    char *maptok = NULL;
    strptr = NULL;

    // from xdo_util: Map shift -> Shift_L, etc.
    for (i = 0; symbol_map[i] != NULL; i+=2)
      if (!strcasecmp(tok, symbol_map[i]))
        maptok = symbol_map[i + 1];

    if (maptok == NULL)
      // start from here.. 
      


    
  }
}

int parse_mods(char *keyseq) {

}

void addbinding(int keycode, int mods, char *commands);
  if (nkeybindings == keybinding_size)
    keybindings = realloc(keybindings, nkeybindings * sizeof(struct keybinding))

  keybindings[nkeybindings].commands = strdup(commands);
  keybindings[nkeybindings].keycode = keycode;
  keybindings[nkeybindings].mods = mods;

  /* We don't need to "bind" a key here unless it's for 'start' */
  if (!strcmp(cmd, "start")) {
    XGrabKey(dpy, keycode, mods, root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, mods | LockMask, root, False, GrabModeAsync, 
             GrabModeAsync);
  }
}

void parseconf(char *line) {
  /* syntax:
   * keysequence cmd1,cmd2,cmd3
   *
   * ex: 
   * ctrl+semicolon start
   * space warp
   * semicolon warp,click
   */

  char *tokctx;
  char *keyseq;
  char *commands;
  int keycode, mods;

  tokctx = line;
  keyseq = strdup(strtok(line, " ", &tokctx));
  commands = strdup(tokctx);

  keycode = parse_keycode(keyseq);
  mods = parse_mods(keyseq);

  addbinding(keycode, mods, commands);
}

GC creategc(Window win) {
  GC gc;
  XGCValues values;

  gc = XCreateGC(dpy, win, 0, &values);
  XSetForeground(dpy, gc, BlackPixel(dpy, 0));
  XSetBackground(dpy, gc, WhitePixel(dpy, 0));
  XSetLineAttributes(dpy, gc, 4, LineSolid, CapButt, JoinBevel);
  XSetFillStyle(dpy, gc, FillSolid);

  return gc;
}

void drawquadrants(Window win, int w, int h) {
  GC gc;
  XRectangle clip[20];
  int idx = 0;
  Colormap colormap;
  XColor red;

  gc = creategc(win);
  colormap = DefaultColormap(dpy, 0);

  XAllocNamedColor(dpy, colormap, "darkred", &red, &red);

# define BORDER 6
# define PEN 4

  /*left*/ clip[idx].x = 0; clip[idx].y = 0; clip[idx].width = BORDER; clip[idx].height = h;
  idx++;
  /*right*/ clip[idx].x = w-BORDER; clip[idx].y = 0; clip[idx].width = BORDER; clip[idx].height = h;
  idx++;
  /*top*/ clip[idx].x = 0; clip[idx].y = 0; clip[idx].width = w; clip[idx].height = BORDER;
  idx++;
  /*bottom*/ clip[idx].x = 0; clip[idx].y = h-BORDER; clip[idx].width = w; clip[idx].height = BORDER;
  idx++;
  /*horiz*/
  clip[idx].x = 0; clip[idx].y = h/2 - BORDER/2;
  clip[idx].width = w; clip[idx].height = BORDER;
  idx++;
  /*vert*/
  clip[idx].x = w/2 - BORDER/2; clip[idx].y = 0;
  clip[idx].width = BORDER; clip[idx].height = h;
  idx++;

  XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, clip, idx, ShapeSet, 0);

  //XSetForeground(dpy, gc, WhitePixel(dpy, 0));
  //XFillRectangle(dpy, win, gc, 0, 0, w, h);

  XSetForeground(dpy, gc, red.pixel);
  XDrawLine(dpy, win, gc, w/2, 0, w/2, h); // vert line
  XDrawLine(dpy, win, gc, 0, h/2, w, h/2); // horiz line
  XDrawLine(dpy, win, gc, BORDER - PEN, BORDER - PEN, w - PEN, BORDER - PEN); //top line
  XDrawLine(dpy, win, gc, BORDER - PEN, h - PEN, w - PEN, h - PEN); //bottom line
  XDrawLine(dpy, win, gc, BORDER - PEN, BORDER - PEN, BORDER - PEN, h - PEN); //left line
  XDrawLine(dpy, win, gc, w - PEN, BORDER - PEN, w - PEN, h - PEN); //left line
  XFlush(dpy);
}

/*
 * move/cut window
 * drawquadrants again
 */

void cmd_start(char *args) {
  XSetWindowAttributes winattr;

  XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);

  wininfo.x = 0;
  wininfo.y = 0;
  wininfo.w = rootattr.width;
  wininfo.h = rootattr.height;

  zone = XCreateSimpleWindow(dpy, root, wininfo.x, wininfo.y, 
                             wininfo.w, wininfo.h, 0, 
                             BlackPixel(dpy, 0), WhitePixel(dpy, 0));

  /* Tell the window manager not to manage us */
  winattr.override_redirect = 1;
  XChangeWindowAttributes(dpy, zone, CWOverrideRedirect, &winattr);

  drawquadrants(zone, w, h);
  XMapWindow(dpy, zone);
  drawquadrants(zone, w, h);
}

void cmd_end(char *args) {
  XDestroyWindow(dpy, zone);
  XUngrabKeyboard(dpy, CurrentTime);
  XFlush(dpy);
}

void cmd_cut_up(char *args) {
  wininfo.h /= 2;
  update();
}

void cmd_cut_down(char *args) {
  wininfo.h /= 2;
  wininfo.y += wininfo.h;
  update();
}

void cmd_cut_left(char *args) {
  wininfo.w /= 2;
  update();
}

void cmd_cut_right(char *args) {
  wininfo.w /= 2;
  wininfo.x += wininfo.w;
  update();
}

void cmd_move_up(char *args) {
  wininfo.y -= wininfo.h;
  update();
}

void cmd_move_down(char *args) {
  wininfo.y += wininfo.h;
  update();
}

void cmd_move_left(char *args) {
  wininfo.x -= wininfo.w;
  update();
}

void cmd_move_right(char *args) {
  wininfo.x += wininfo.w;
  update();
}

void cmd_warp(char *args) {
}

void cmd_click(char *args) {
}

void cmd_doubleclick(char *args) {
}

void cmd_drag(char *args) {
}


void update() {
  XMoveResizeWindow(dpy, zone, wininfo.x, wininfo.y, wininfo.w, wininfo.h);
  drawquadrants(zone, wininfo.w, wininfo.h);
}

int main(int argc, char **argv) {
  char *pcDisplay;
  int ret;

  if ( (pcDisplay = getenv("DISPLAY")) == NULL) {
    fprintf(stderr, "Error: DISPLAY environment variable not set\n");
    exit(1);
  }

  if ( (dpy = XOpenDisplay(pcDisplay)) == NULL) {
    fprintf(stderr, "Error: Can't open display: %s", pcDisplay);
    exit(1);
  }

  root = XDefaultRootWindow(dpy);
  xdo = xdo_new_with_opened_display(dpy, pcDisplay, False);

  //grab("semicolon", ControlMask);
  XGetWindowAttributes(dpy, root, &attr);

  while (1) {
    XEvent e;
    XNextEvent(dpy, &e);
    switch (e.type) {
      case KeyPress:
        handle_keypress();
        break;
      case KeyRelease:
      default:
        break;
    }
  }

  xdo_free(xdo);
}
