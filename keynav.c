/*
 * Visual user-directed binary search for something to point your mouse at.
 */

/* XXX: fine-tuning once you're zoomed in? (done?)
 * XXX: "cancel" action (done)
 * XXX: vi-keys half-split (done)
 * XXX: use XGrabKeyboard instead of XGrabKey for the movement mode? 
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

Display *dpy;
Window root;
XWindowAttributes attr;

void grab(char *keyname, int mods) {
  int key;

  key = XKeysymToKeycode(dpy, XStringToKeysym(keyname));
  XGrabKey(dpy, key, mods, root, False,
           GrabModeAsync, GrabModeAsync);
}

GC creategc(Window win) {
  GC gc;
  XGCValues values;

  gc = XCreateGC(dpy, win, 0, &values);
  XSetForeground(dpy, gc, BlackPixel(dpy, 0));
  XSetBackground(dpy, gc, WhitePixel(dpy, 0));
  XSetLineAttributes(dpy, gc, 4, LineSolid, CapButt, JoinBevel);
  XSetFillStyle(dpy, gc, FillSolid);
  //XSetFillStyle(dpy, gc, FillStippled);

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

  XSetForeground(dpy, gc, WhitePixel(dpy, 0));
  XFillRectangle(dpy, win, gc, 0, 0, w, h);

  XSetForeground(dpy, gc, red.pixel);
  XDrawLine(dpy, win, gc, w/2, 0, w/2, h); // vert line
  XDrawLine(dpy, win, gc, 0, h/2, w, h/2); // horiz line
  XDrawLine(dpy, win, gc, BORDER - PEN, BORDER - PEN, w - PEN, BORDER - PEN); //top line
  XDrawLine(dpy, win, gc, BORDER - PEN, h - PEN, w - PEN, h - PEN); //bottom line
  XDrawLine(dpy, win, gc, BORDER - PEN, BORDER - PEN, BORDER - PEN, h - PEN); //left line
  XDrawLine(dpy, win, gc, w - PEN, BORDER - PEN, w - PEN, h - PEN); //left line
  XFlush(dpy);
}

void ungrab(char *keyname, int mod) {
  int key;

  key = XKeysymToKeycode(dpy, XStringToKeysym(keyname));
  XUngrabKey(dpy, key, mod, root);
}

void endmousekey() {
  ungrab("h",0); ungrab("j",0); ungrab("k",0); ungrab("l",0);
  ungrab("h",ControlMask); ungrab("j",ControlMask); ungrab("k",ControlMask); ungrab("l",ControlMask);
  ungrab("h",ShiftMask); ungrab("j",ShiftMask); ungrab("k",ShiftMask); ungrab("l",ShiftMask);
  ungrab("space", 0);
  ungrab("semicolon", 0);
  ungrab("Escape", 0);
  XSync(dpy, 0);
  grab("semicolon", ControlMask);
}

int handlekey(int keysym, int mod, int *x, int *y, int *w, int *h) {
  char *keyname;

  keyname = XKeysymToString(keysym);

#if 0
  if (!strcmp(keyname, "a")) { // up and left
    //*x -= *w;
    //*y -= *h;
  } else if (!strcmp(keyname, "s")) { // up and right
    *x += *w;
    //*y += *h;
  } else if (!strcmp(keyname, "d")) { // down and left
    //*x += *w;
    *y += *h;
  } else if (!strcmp(keyname, "f")) { // down and right
    *x += *w;
    *y += *h;
  }
#endif

  if (mod & ControlMask || !mod) {
    if (*keyname == 'h') { // go left or split left
      *w /= 2;
    } else if (*keyname == 'j') { //go down or split down
      *h /= 2;
      *y += *h;
    } else if (*keyname == 'k') { //go up or split up
      *h /= 2;
    } else if (*keyname == 'l') { //go right or split right
      *w /= 2;
      *x += *w;
    }
  } else if (mod & ShiftMask) {
    if (*keyname == 'h') { // shift left
      if (*x > 0) *x -= *w;
    } else if (*keyname == 'j') { //shift down
      if ((*y + *h) < attr.height) *y += *h;
    } else if (*keyname == 'k') { //shift up
      if (*y > 0) *y -= *h;
    } else if (*keyname == 'l') { //shift right
      if ((*x + *w) < attr.width) *x += *w;
    }
  }

  if (*w < 1 || *h < 1) {
    printf( "OOPS. Area too small. Giving up :(\n");
    return 0;
  }

  printf("Box: @(%d,%d) #(%d,%d)\n", *x, *y, *w, *h);
  return 1;
}

void startmousekey() {
  int hits = 0;
  int keysym;
  int done = 0;
  int x,y,w,h;
  int warp = 1;
  int click = 0;

  Window zone;

  grab("h",ShiftMask); grab("j",ShiftMask); grab("k",ShiftMask); grab("l",ShiftMask);
  grab("h",ControlMask); grab("j",ControlMask); grab("k",ControlMask); grab("l",ControlMask);
  grab("h",0); grab("j",0); grab("k",0); grab("l",0);
  grab("semicolon", 0); grab("space", 0); grab("Escape", 0);

  x = y = 0;
  w = attr.width;
  h = attr.height;

  zone = XCreateSimpleWindow(dpy, root, x, y, w, h, 1, BlackPixel(dpy, 0), WhitePixel(dpy, 0));

  { /* Tell the window manager not to manage us */
    unsigned long valuemask;
    XSetWindowAttributes winattr;
    winattr.override_redirect = 1;
    XChangeWindowAttributes(dpy, zone, CWOverrideRedirect, &winattr);
  }

  drawquadrants(zone, w, h);
  XMapWindow(dpy, zone);
  drawquadrants(zone, w, h);

  printf("Starting quadrants...\n");
  while (!done) {
    XEvent e;
    XNextEvent(dpy, &e);
    switch (e.type) {
      case KeyPress:
        keysym = XKeycodeToKeysym(dpy, e.xkey.keycode, 0);

        if (XStringToKeysym("semicolon") == keysym)
          done++;
        else if (XStringToKeysym("space") == keysym) {
          done++;
          click = 1;
        } else if (XStringToKeysym("Escape") == keysym) {
          warp = 0;
          done++;
        } else {
          if (handlekey(keysym, e.xkey.state, &x, &y, &w, &h)) {
            hits++;
            //if (hits > 5)
              //done++;
            //else {
            XMoveResizeWindow(dpy, zone, x, y, w, h);
            drawquadrants(zone, w, h);
            //}
          } else {
            done++;
          }
        }
        break;
      case KeyRelease:
      default:
        break;
    }
  }

  endmousekey();
  XDestroyWindow(dpy, zone);
  if (warp)
    XWarpPointer(dpy, None, root, 0, 0, 0, 0, x + w/2, y + h/2);
  if (click) {
    XTestFakeButtonEvent(dpy, 1, True, CurrentTime); // button down
    XTestFakeButtonEvent(dpy, 1, False, CurrentTime); // button release
  }
}

int main(int argc, char **argv) {
  char *pcDisplay;
  int ret;

  if ( (pcDisplay = getenv("DISPLAY")) == NULL) {
    fprintf(stderr, "Error: DISPLAY environment variable not set\n");
    exit(1);
  }

  printf("Display: %s\n", pcDisplay);

  if ( (dpy = XOpenDisplay(pcDisplay)) == NULL) {
    fprintf(stderr, "Error: Can't open display: %s", pcDisplay);
    exit(1);
  }

  root = XDefaultRootWindow(dpy);
  XGetWindowAttributes(dpy, root, &attr);

  XGrabKey(dpy, XKeysymToKeycode(dpy, XStringToKeysym("semicolon")),
           ControlMask, root, False, GrabModeAsync, GrabModeAsync);

  while (1) {
    XEvent e;
    XNextEvent(dpy, &e);
    switch (e.type) {
      case KeyPress:
        XUngrabKeyboard(dpy, CurrentTime);
        startmousekey();
        break;
      case KeyRelease:
      default:
        break;
    }
  }
}


