/*
 * Visual user-directed binary or grid search for something to point your mouse
 * at.
 *
 * TODO:
 * XXX: Merge 'wininfo' and 'wininfo_history'. The latest history entry is the
 *      same as wininfo, so use that instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>

#include "xdotool/xdo.h"

/* These come from xdo.o */
extern keysymcharmap_t keysymcharmap[];
extern char *symbol_map[];

static Display *dpy;
static Window root;
static XWindowAttributes rootattr;
static Window zone;
static xdo_t *xdo;
static int appstate = 0;

static int drag_button = 0;
static char drag_modkeys[128];

#define STATE_ACTIVE 0x0001 
#define STATE_DRAGGING 0x0002

typedef struct wininfo {
  int x;
  int y;
  int w;
  int h;
  int grid_x;
  int grid_y;
  int border_thickness;
  int pen_size;
} wininfo_t;

static wininfo_t wininfo;

/* history tracking */
#define WININFO_MAXHIST (100)
static wininfo_t wininfo_history[WININFO_MAXHIST]; /* XXX: is 100 enough? */
static int wininfo_history_pivot = 0;

void defaults();
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
void cmd_grid(char *args);
void cmd_cell_select(char *args);
void cmd_start(char *args);
void cmd_end(char *args);
void cmd_history_back(char *args);
void cmd_quit(char *args); /* XXX: Is this even necessary? */

void update();
void drawborderline(struct wininfo *info, Window win, GC gc, XRectangle *clip);
void handle_keypress(XKeyEvent *e);
void handle_commands(char *commands);
void parse_config();
void parse_config_line(char *line);
void save_history_point();
void restore_history_point(int moves_ago);

int percent_of(int num, char *args, float default_val);

struct dispatch {
  char *command;
  void (*func)(char *args);
} dispatch[] = {
  "cut-up", cmd_cut_up,
  "cut-down", cmd_cut_down,
  "cut-left", cmd_cut_left,
  "cut-right", cmd_cut_right,
  "move-up", cmd_move_up,
  "move-down", cmd_move_down,
  "move-left", cmd_move_left,
  "move-right", cmd_move_right,

  // Grid commands
  "grid", cmd_grid,
  "cell-select", cmd_cell_select,

  // Mouse activity
  "warp", cmd_warp,
  "click", cmd_click,     
  "doubleclick", cmd_doubleclick,
  "drag", cmd_drag,

  // Other commands.
  "start", cmd_start,
  "end", cmd_end, 
  "history-back", cmd_history_back,
  "quit", cmd_quit,
  NULL, NULL,
};

struct keybinding {
  char *commands;
  int keycode;
  int mods;
} *keybindings = NULL;

int nkeybindings = 0;
int keybindings_size = 10;

int parse_keycode(char *keyseq) {
  char *tokctx;
  char *strptr;
  char *tok;
  char *last_tok;
  char *dup;
  int keycode;
  int keysym;

  strptr = dup = strdup(keyseq);
  //printf("finding keycode for %s\n", keyseq);
  while ((tok = strtok_r(strptr, "+", &tokctx)) != NULL) {
    last_tok = tok;
    strptr = NULL;
  }

  keysym = XStringToKeysym(last_tok);
  if (keysym == NoSymbol)
    fprintf(stderr, "No kesym found for %s\n", last_tok);
  keycode = XKeysymToKeycode(dpy, keysym);
  if (keycode == 0)
    fprintf(stderr, "Unable to lookup keycode for %s\n", last_tok);

  free(dup);
  return keycode;
}

int parse_mods(char *keyseq) {
  char *tokctx;
  char *strptr;
  char *tok;
  char *last_tok;
  char *dup;
  char **mods  = NULL;
  int modmask = 0;
  int nmods = 0;
  int mod_size = 10;

  mods = malloc(mod_size * sizeof(char *));

  //printf("finding mods for %s\n", keyseq);

  strptr = dup = strdup(keyseq);
  while ((tok = strtok_r(strptr, "+", &tokctx)) != NULL) {
    strptr = NULL;
    //printf("mod: %s\n", tok);
    mods[nmods] = tok;
    nmods++;
    if (nmods == mod_size) {
      mod_size *= 2;
      mods = realloc(mods, mod_size * sizeof(char *));
    }
  }


  int i = 0;
  int j = 0;
  /* Use all but the last token as modifiers */
  for (i = 0; i < nmods; i++) {
    char *mod = mods[i];
    KeySym keysym = 0;

    //printf("mod: keysym for %s = %d\n", mod, keysym);
    // from xdo_util: Map "shift" -> "Shift_L", etc.
    for (j = 0; symbol_map[j] != NULL; j+=2) {
      if (!strcasecmp(mod, symbol_map[j])) {
        mod = symbol_map[j + 1];
      }
    }

    keysym = XStringToKeysym(mod);
    //printf("%s => %d\n", mod, keysym);
    if ((keysym == XK_Shift_L) || (keysym == XK_Shift_R))
      modmask |= ShiftMask;
    if ((keysym == XK_Control_L) || (keysym == XK_Control_R))
      modmask |= ControlMask;
    if ((keysym == XK_Alt_L) || (keysym == XK_Alt_R))
      modmask |= Mod1Mask;
    if ((keysym == XK_Super_L) || (keysym == XK_Super_R)
        || (keysym == XK_Hyper_L) || (keysym == XK_Hyper_R))
      modmask |= Mod4Mask;

    /* 'xmodmap' will output the current modN:KeySym mappings */
  }

  free(dup);
  return modmask;
}

void addbinding(int keycode, int mods, char *commands) {
  int i;

  if (nkeybindings == keybindings_size) {
    keybindings_size *= 2;
    keybindings = realloc(keybindings, keybindings_size * sizeof(struct keybinding));
  }

  // Check if we already have a binding for this, if so, override it.
  for (i = 0; i <= nkeybindings; i++) {
    if (keybindings[i].keycode == keycode
        && keybindings[i].mods == mods) {
      free(keybindings[i].commands);
      keybindings[i].commands = strdup(commands);
      return;
    }
  }

  keybindings[nkeybindings].commands = strdup(commands);
  keybindings[nkeybindings].keycode = keycode;
  keybindings[nkeybindings].mods = mods;
  nkeybindings++;

  if (!strncmp(commands, "start", 5)) {
    XGrabKey(dpy, keycode, mods, root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, mods | LockMask, root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, mods | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, keycode, mods | LockMask | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);
  }
}

void parse_config() {
  char *homedir;

  keybindings = malloc(keybindings_size * sizeof(struct keybinding));

  defaults();

  homedir = getenv("HOME");

  if (homedir != NULL) {
    char *rcfile = NULL;
    FILE *fp = NULL;
#define LINEBUF_SIZE 512
    char line[LINEBUF_SIZE];
    asprintf(&rcfile, "%s/.keynavrc", homedir);
    fp = fopen(rcfile, "r");
    if (fp != NULL) {
      /* fopen succeeded */
      while (fgets(line, LINEBUF_SIZE, fp) != NULL) {
        /* Kill the newline */
        *(line + strlen(line) - 1) = '\0';
        parse_config_line(line);
      }
      free(rcfile);
      return;
    }
  }
  fprintf(stderr, "No ~/.keynavrc found.\n");
}

void defaults() {
  char *tmp;
  int i;
  char *default_config[] = {
    "ctrl+semicolon start",
    "Escape end",
    "ctrl+bracketleft end", /* for vi people who use */
    "h cut-left",
    "j cut-down",
    "k cut-up",
    "l cut-right",
    "shift+h move-left",
    "shift+j move-down",
    "shift+k move-up",
    "shift+l move-right",
    "space warp,click 1,end",
    "semicolon warp,end",
    "w warp",
    "e end",
    "1 click 1",
    "2 click 2",
    "3 click 3",
    "ctrl+h cut-left",
    "ctrl+j cut-down",
    "ctrl+k cut-up",
    "ctrl+l cut-right",
    "y cut-left,cut-up",
    "u cut-right,cut-up",
    "b cut-left,cut-down",
    "n cut-right,cut-down",
    "shift+y move-left,move-up",
    "shift+u move-right,move-up",
    "shift+b move-left,move-down",
    "shift+n move-right,move-down",
    "ctrl+y cut-left,cut-up",
    "ctrl+u cut-right,cut-up",
    "ctrl+b cut-left,cut-down",
    "ctrl+n cut-right,cut-down",

    NULL,
  };
  for (i = 0; default_config[i]; i++) {
    tmp = strdup(default_config[i]);
    parse_config_line(tmp);
    free(tmp);
  }
}

void parse_config_line(char *line) {
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

  char *comment;

  /* Ignore everything after a '#' */
  comment = strchr(line, '#');
  if (comment != NULL)
    *comment = '\0';

  /* Ignore leading whitespace */
  while (*line == ' ')
    line++;

  /* Ignore empty lines */
  if (*line == '\n' || *line == '\0')
    return;

  tokctx = line;
  keyseq = strdup(strtok_r(line, " ", &tokctx));
  commands = strdup(tokctx);

  if (strcmp(keyseq, "clear") == 0) {
    /* Reset keybindings */
    free(keybindings);
    nkeybindings = 0;
    keybindings_size = 10;
    keybindings = malloc(keybindings_size * sizeof(struct keybinding));
  }
  else {
    keycode = parse_keycode(keyseq);
    mods = parse_mods(keyseq);

    addbinding(keycode, mods, commands);
  }

  free(keyseq);
  free(commands);
}

int percent_of(int num, char *args, float default_val) {
  static float precision = 100000.0;
  float pct = 0.0;
  int value = 0;

  /* Parse a float. If this fails, assume the default value */
  if (sscanf(args, "%f", &pct) <= 0)
    pct = default_val;

  if (pct > 1.0)
    return (int)pct;

  value = (int)((num * (pct * precision)) / precision);
  return value;
}


GC creategc(Window win) {
  GC gc;
  XGCValues values;

  gc = XCreateGC(dpy, win, 0, &values);
  XSetForeground(dpy, gc, BlackPixel(dpy, 0));
  XSetBackground(dpy, gc, WhitePixel(dpy, 0));
  XSetFillStyle(dpy, gc, FillSolid);

  return gc;
}

void drawgrid(Window win, struct wininfo *info) {
  GC gc;
  XRectangle clip[30];
  int idx = 0;
  Colormap colormap;
  XColor red, unused_xcolor;
  XColor white;
  int w = info->w;
  int h = info->h;
  int cell_width;
  int cell_height;
  int i;

  gc = creategc(win);
  colormap = DefaultColormap(dpy, 0);

  XAllocNamedColor(dpy, colormap, "darkred", &red, &unused_xcolor);
  XSetLineAttributes(dpy, gc, info->pen_size, LineSolid, CapButt, JoinBevel);

  // Fill it red.
  XSetForeground(dpy, gc, red.pixel);
  XFillRectangle(dpy, win, gc, 0, 0, w, h);

  // Draw white lines.
  XSetForeground(dpy, gc, WhitePixel(dpy, 0));


  /*left*/ 
  clip[idx].x = 0;
  clip[idx].y = 0;
  clip[idx].width = info->border_thickness;
  clip[idx].height = h;
  idx++;

  /*right*/
  clip[idx].x = w - info->border_thickness;
  clip[idx].y = 0;
  clip[idx].width = info->border_thickness;
  clip[idx].height = h;
  idx++;

  /*top*/
  clip[idx].x = 0;
  clip[idx].y = 0;
  clip[idx].width = w;
  clip[idx].height = info->border_thickness;
  idx++;

  /*bottom*/
  clip[idx].x = 0;
  clip[idx].y = h - info->border_thickness;
  clip[idx].width = w;
  clip[idx].height = info->border_thickness;
  idx++;
  
  cell_width = (w / info->grid_y);
  cell_height = (h / info->grid_x);

  /* clip vertically */
  for (i = 1; i < info->grid_y; i++) {
    clip[idx].x = cell_width * i - (info->border_thickness / 2); 
    clip[idx].y = 0;
    clip[idx].width = info->border_thickness;
    clip[idx].height = h;
    idx++;
  }

  /* clip horizontally */
  for (i = 1; i < info->grid_x; i++) {
    clip[idx].x = 0;
    clip[idx].y = cell_height * i - (info->border_thickness / 2);
    clip[idx].width = w;
    clip[idx].height = info->border_thickness;
    idx++;
  }

  XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, clip, idx, ShapeSet, 0);

#define CUTSIZE 5
  /* Cut out a hole in the center */
  clip[idx].x = (w/2 - (CUTSIZE/2));
  clip[idx].y = (h/2 - (CUTSIZE/2));
  clip[idx].width = CUTSIZE;
  clip[idx].height = CUTSIZE;
  idx++;
  XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, clip + idx - 1, 1, ShapeSubtract, 0);

  for (i = 0; i < idx; i++) {
    drawborderline(info, win, gc, &(clip[i]));
  }

}

void cmd_start(char *args) {
  XSetWindowAttributes winattr;

  wininfo.x = 0;
  wininfo.y = 0;
  wininfo.w = rootattr.width;
  wininfo.h = rootattr.height;

  /* Default start with 4 cells, 2x2 */
  wininfo.grid_x = 2;
  wininfo.grid_y = 2;

  wininfo.border_thickness = 5;
  wininfo.pen_size = 1;

  if ((appstate & STATE_ACTIVE) == 0) {
    appstate |= STATE_ACTIVE;
    wininfo_history_pivot = 0;
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);

    zone = XCreateSimpleWindow(dpy, root, wininfo.x, wininfo.y, 
                               1, 1, 0, 
                               BlackPixel(dpy, 0), WhitePixel(dpy, 0));

    /* Tell the window manager not to manage us */
    winattr.override_redirect = 1;
    XChangeWindowAttributes(dpy, zone, CWOverrideRedirect, &winattr);
    XSelectInput(dpy, zone, StructureNotifyMask);
  }
}

void cmd_end(char *args) {
  if (!(appstate & STATE_ACTIVE))
    return;

  appstate &= ~(STATE_ACTIVE);

  /* kill drag state too */
  if (appstate & STATE_DRAGGING)
    cmd_drag(NULL);

  XDestroyWindow(dpy, zone);
  XUngrabKeyboard(dpy, CurrentTime);
}

void cmd_history_back(char *args) {
  if (!(appstate & STATE_ACTIVE))
    return;

  restore_history_point(1);
}

void cmd_quit(char *args) {
  exit(0);
}

void cmd_cut_up(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.h = percent_of(wininfo.h, args, .5);
}

void cmd_cut_down(char *args) {
  int orig = wininfo.h;
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.h = percent_of(wininfo.h, args, .5);
  wininfo.y += orig - wininfo.h;
}

void cmd_cut_left(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.w = percent_of(wininfo.w, args, .5);
}

void cmd_cut_right(char *args) {
  int orig = wininfo.w;
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.w = percent_of(wininfo.w, args, .5);
  wininfo.x += orig - wininfo.w;
}

void cmd_move_up(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.y -= percent_of(wininfo.h, args, 1);
}

void cmd_move_down(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.y += percent_of(wininfo.h, args, 1);
}

void cmd_move_left(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.x -= percent_of(wininfo.w, args, 1);
}

void cmd_move_right(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  wininfo.x += percent_of(wininfo.w, args, 1);
}

void cmd_warp(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  //printf("Warp\n");
  xdo_mousemove(xdo, wininfo.x + wininfo.w / 2, wininfo.y + wininfo.h / 2);

  /* Some apps (window managers) don't acknowledge a drag unless there's been
   * some wiggle. Let's wiggle if we're dragging. */
  if (appstate & STATE_DRAGGING) {
    xdo_mousemove_relative(xdo, 1, 1);
    xdo_mousemove_relative(xdo, -1, 0);
    xdo_mousemove_relative(xdo, 0, -1);
  }
}

void cmd_click(char *args) {
  int button;
  if (appstate & STATE_ACTIVE == 0)
    return;

  button = atoi(args);
  if (button > 0)
    xdo_click(xdo, button);
  else
    fprintf(stderr, "Negative mouse button is invalid: %d\n", button);
}

void cmd_doubleclick(char *args) {
  if (appstate & STATE_ACTIVE == 0)
    return;
  cmd_click(args);
  cmd_click(args);
}

void cmd_drag(char *args) {
  int button;

  if (appstate & STATE_ACTIVE == 0)
    return;

  if (args == NULL) {
    button = drag_button;
  } else {
    int count = sscanf(args, "%d %127s", &button, drag_modkeys);
    if (count == 0) {
      button = 1; /* Default to left mouse button */
      drag_modkeys[0] = '\0';
    } else if (count == 1) {
      drag_modkeys[0] = '\0';
    }

    //printf("modkeys: %s\n", drag_modkeys);
  }

  if (button <= 0) {
    fprintf(stderr, "Negative or no mouse button given. Not valid. Button read was '%d'\n", button);
    return;
  }

  drag_button = button;

  cmd_warp(NULL);
  if (appstate & STATE_DRAGGING) { /* End dragging */
    appstate &= ~(STATE_DRAGGING);
    xdo_mouseup(xdo, button);
  } else { /* Start dragging */
    appstate |= STATE_DRAGGING;
    xdo_keysequence_down(xdo, drag_modkeys);
    //printf("down: %s\n", drag_modkeys);
    xdo_mousedown(xdo, button);

    /* Sometimes we need to move a little to tell the app we're dragging */
    xdo_mousemove_relative(xdo, 1, 0);
    xdo_mousemove_relative(xdo, 100, 0);
    //xdo_mousemove_relative(xdo, 1, 0);
    xdo_keysequence_up(xdo, drag_modkeys);
  }
}

void cmd_grid(char *args) {
  int grid_x, grid_y;

  // Try to parse 'NxN' where N is a number.
  if (sscanf(args, "%dx%d", &grid_x, &grid_y) <= 0) {
    // Otherwise, try parsing a number.
    grid_x = grid_y = atoi(args);
  }

  if (grid_x <= 0 || grid_y <= 0) {
    fprintf(stderr, "Invalid grid segmentation: %dx%d\n", grid_x, grid_y);
    fprintf(stderr, "Grid x and y must both be greater than 0.\n");
    return;
  }

  wininfo.grid_x = grid_x;
  wininfo.grid_y = grid_y;
}

void cmd_cell_select(char *args) {
  int x, y, z;
  int cell_width, cell_height;
  x = y = z = 0;

  // Try to parse 'NxM' where N and M are a number.
  if (sscanf(args, "%dx%d", &x, &y) < 2) {
    // Otherwise, try parsing just number.
    z = atoi(args);
  }

  // if z > 0, then this means we said "cell-select N"
  if (z > 0) {
    double dx = (double)z / (double)wininfo.grid_y;
    x = (z / wininfo.grid_y);
    if ( (double)x != dx ) {
      x++;
    }
    y = (z % wininfo.grid_y);
    if ( 0 == y ) {
      y = wininfo.grid_y;
    }
  }

  if (x <= 0 && y <= 0) {
    fprintf(stderr, "Cell number cannot be zero or negative. I was given"
            "x=%d and y=%d\n", x, y);
    return;
  }

  if (x > wininfo.grid_x && y > wininfo.grid_y) {
    fprintf(stderr, "The active grid is %dx%d, and you selected %dx%d which "
            "does not exist.\n", wininfo.grid_x, wininfo.grid_y, x, y);
    return;
  }

  // else, then we said cell-select NxM
  wininfo.w = wininfo.w / wininfo.grid_y;
  wininfo.h = wininfo.h / wininfo.grid_x;
  wininfo.x = wininfo.x + (wininfo.w * (y - 1));
  wininfo.y = wininfo.y + (wininfo.h * (x - 1));
}

void update() {
  if (appstate & STATE_ACTIVE == 0)
    return;

  if (wininfo.x < 0)
    wininfo.x = 0;
  if (wininfo.x + wininfo.w > rootattr.width)
    wininfo.x = rootattr.width - wininfo.w;
  if (wininfo.y < 0)
    wininfo.y = 0;
  if (wininfo.y + wininfo.h > rootattr.height)
    wininfo.y = rootattr.height - wininfo.h;

  if (wininfo.w <= 1 || wininfo.h <= 1) {
    cmd_end(NULL);
    return;
  }

  XMoveResizeWindow(dpy, zone, wininfo.x, wininfo.y, wininfo.w, wininfo.h);

  /* XXX: If I don't call drawgrid here twice, sometimes it fails to paint
   * properly.  I haven't put any time investigating why. */
  drawgrid(zone, &wininfo);
  XMapWindow(dpy, zone);
}

void drawborderline(struct wininfo *info, Window win, GC gc, XRectangle *rect) {
  int penoffset = ((info->border_thickness / 2) - (info->pen_size / 2));
  XDrawLine(dpy, win, gc,
            (rect->x + (rect->width / 2)),
            (rect->y + penoffset),
            (rect->x + (rect->width / 2)),
            ((rect->y + rect->height) - (penoffset*2))
           );
  XDrawLine(dpy, win, gc,
            (rect->x + penoffset),
            (rect->y + (rect->height / 2)),
            (rect->x + rect->width - penoffset),
            (rect->y + (rect->height / 2))
           );
}

void handle_keypress(XKeyEvent *e) {
  int i;

  /* If a mouse button is pressed (like, when we're dragging),
   * then the 'mods' will include values like Button1Mask. 
   * Let's remove those, as they cause breakage */
  e->state &= ~(Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask);
  e->state &= ~(LockMask | Mod2Mask);

  /* Loop over known keybindings */
  for (i = nkeybindings - 1; i >= 0; i--) {
    //printf("e->state:%d bindmods:%d and:%d\n", e->state, keybindings[i].mods, e->state & keybindings[i].mods);
    int keycode = keybindings[i].keycode;
    int mods = keybindings[i].mods;
    char *commands = keybindings[i].commands;
    if ((keycode == e->keycode) && (mods == e->state)) {
      //printf("Calling '%s' from %d/%d\n", commands, keycode, mods);
      handle_commands(commands);
    }
  }
}

void handle_commands(char *commands) {
  char *cmdcopy;
  char *tokctx, *tok, *strptr;

  cmdcopy = strdup(commands);
  strptr = cmdcopy;
  while ((tok = strtok_r(strptr, ",", &tokctx)) != NULL) {
    int i;
    int found = 0;

    /* Ignore leading whitespace */
    while (*tok == ' ' || *tok == '\t')
      tok++;

    strptr = NULL;
    for (i = 0; dispatch[i].command; i++) {

      /* XXX: This approach means we can't have one command be a subset of
       * another. For example, 'grid' and 'grid-foo' will fail because when you
       * use 'grid-foo' it'll match 'grid' first. 
       * I don't care about this yet.
       */

      /* If this command starts with a dispatch function, call it */
      if (!strncmp(tok, dispatch[i].command, strlen(dispatch[i].command))) {
        /* tok + len + 1 is
         * "command arg1 arg2"
         *          ^^^^^^^^^ <-- this 
         */

        char *args = tok + strlen(dispatch[i].command);
        if (*args == '\0')
          args = "";
        else
          args++;

        found = 1;
        dispatch[i].func(args);

        if (appstate & STATE_DRAGGING)
          cmd_warp(NULL);
      }
    }

    if (!found)
      fprintf(stderr, "No such command: '%s'\n", tok);
  }
  if (appstate & STATE_ACTIVE) {
    update();
    save_history_point();
  }

  free(cmdcopy);
}

void save_history_point() {

  /* If the history is full, drop the oldest entry */
  while (wininfo_history_pivot >= WININFO_MAXHIST) {
    int i;
    for (i = 1; i < wininfo_history_pivot; i++) {
      memcpy(&(wininfo_history[i - 1]),
             &(wininfo_history[i]),
             sizeof(wininfo_t));
    }
    wininfo_history_pivot--;
  }

  memcpy(&(wininfo_history[wininfo_history_pivot]),
         &(wininfo),
         sizeof(wininfo));

  wininfo_history_pivot++;
}

void restore_history_point(int moves_ago) {
  wininfo_history_pivot -= moves_ago + 1;
  if (wininfo_history_pivot < 0)
    wininfo_history_pivot = 0;

  memcpy(&(wininfo),
         &(wininfo_history[wininfo_history_pivot]),
         sizeof(wininfo));
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

  XGetWindowAttributes(dpy, root, &rootattr);

  /* Parse config */
  parse_config();

  while (1) {
    XEvent e;
    XNextEvent(dpy, &e);
    switch (e.type) {
      case KeyPress:
        handle_keypress((XKeyEvent *)&e);
        break;

      // Map and Configure events mean the window was changed or is now mapped.
      case MapNotify:
      case ConfigureNotify:
        update();
        break;

      // Ignorable events.
      case KeyRelease:
      case DestroyNotify:
      case UnmapNotify:
        break;
      default:
        //printf("Event: %d\n", e.type);
        break;
    }
  }

  xdo_free(xdo);
}
