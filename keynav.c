/*
 * Visual user-directed binary or grid search for something to point your mouse
 * at.
 *
 * XXX: Merge 'wininfo' and 'wininfo_history'. The latest history entry is the
 *      same as wininfo, so use that instead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xinerama.h>

#include <xdo.h>

#ifndef GLOBAL_CONFIG_FILE
#define GLOBAL_CONFIG_FILE "/etc/keynavrc"
#endif /* GLOBAL_CONFIG_FILE */

extern char **environ;

#define ISACTIVE (appstate.active)
#define ISDRAGGING (appstate.dragging)

struct appstate {
  int active;
  int dragging;
  enum { record_getkey, record_ing, record_off } recording;
};

struct recording {
  int keycode;
  char **commands;
  int ncommands;
  int commands_size;
};

int nrecordings = 0;
int recordings_size = 0;
struct recording *recordings = NULL;

typedef struct colors {
  XColor red;
  XColor white;
  XColor dummy;
  Colormap colormap;
  GC gc;
} colors_t;

typedef struct wininfo {
  int x;
  int y;
  int w;
  int h;
  int grid_x;
  int grid_y;
  int border_thickness;
  int pen_size;
  int center_cut_size;
  int curviewport;
} wininfo_t;

typedef struct mouseinfo {
  int x;
  int y;
} mouseinfo_t;

typedef struct viewport {
  int x;
  int y;
  int w;
  int h;
  int screen_num;
  Screen *screen;
  Window root;
} viewport_t;

static wininfo_t wininfo;
static mouseinfo_t mouseinfo;
static viewport_t *viewports;
static int nviewports = 0;
static int xinerama = 0;
static int daemonize = 0;

static Display *dpy;
static Window zone;
static Pixmap canvas;
static xdo_t *xdo;
static struct appstate appstate = {
  .active = 0,
  .dragging = 0,
  .recording = record_off,
};
static colors_t colors;

static int drag_button = 0;
static char drag_modkeys[128];

/* history tracking */
#define WININFO_MAXHIST (100)
static wininfo_t wininfo_history[WININFO_MAXHIST]; /* XXX: is 100 enough? */
static int wininfo_history_cursor = 0;

void defaults();
void cmd_cell_select(char *args);
void cmd_click(char *args);
void cmd_cut_down(char *args);
void cmd_cut_left(char *args);
void cmd_cut_right(char *args);
void cmd_cut_up(char *args);
void cmd_doubleclick(char *args);
void cmd_drag(char *args);
void cmd_end(char *args);
void cmd_grid(char *args);
void cmd_history_back(char *args);
void cmd_move_down(char *args);
void cmd_move_left(char *args);
void cmd_move_right(char *args);
void cmd_move_up(char *args);
void cmd_cursorzoom(char *args);
void cmd_windowzoom(char *args);
void cmd_quit(char *args);
void cmd_shell(char *args);
void cmd_start(char *args);
void cmd_warp(char *args);
void cmd_record(char *args);

void update();
void drawborderline(struct wininfo *info, Window win, GC gc, XRectangle *clip);
void handle_keypress(XKeyEvent *e);
void handle_commands(char *commands);
void parse_config();
void parse_config_line(char *line);
void save_history_point();
void restore_history_point(int moves_ago);

void query_screens();
void query_screen_xinerama();
void query_screen_normal();
int viewport_sort(const void *a, const void *b);
int query_current_screen();
void viewport_left();
void viewport_right();
int pointinrect(int px, int py, int rx, int ry, int rw, int rh);
int percent_of(int num, char *args, float default_val);
void sigchld(int sig);

struct dispatch {
  char *command;
  void (*func)(char *args);
};
typedef struct dispatch dispatch_t;

dispatch_t dispatch[] = {
  "cut-up", cmd_cut_up,
  "cut-down", cmd_cut_down,
  "cut-left", cmd_cut_left,
  "cut-right", cmd_cut_right,
  "move-up", cmd_move_up,
  "move-down", cmd_move_down,
  "move-left", cmd_move_left,
  "move-right", cmd_move_right,
  "cursorzoom", cmd_cursorzoom,
  "windowzoom", cmd_windowzoom,

  // Grid commands
  "grid", cmd_grid,
  "cell-select", cmd_cell_select,

  // Mouse activity
  "warp", cmd_warp,
  "click", cmd_click,     
  "doubleclick", cmd_doubleclick,
  "drag", cmd_drag,

  // Other commands.
  "sh", cmd_shell,
  "start", cmd_start,
  "end", cmd_end, 
  "history-back", cmd_history_back,
  "quit", cmd_quit,
  "record", cmd_record,
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
    fprintf(stderr, "No keysym found for %s\n", last_tok);
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
  const char **symbol_map = xdo_symbol_map();
  for (i = 0; i < nmods; i++) {
    const char *mod = mods[i];
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
  free(mods);
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
    int i = 0;
    /* Grab on all screen roots */
    for (i = 0; i < ScreenCount(dpy); i++) {
      Window root = RootWindow(dpy, i);
      XGrabKey(dpy, keycode, mods, root, False, GrabModeAsync, GrabModeAsync);
      XGrabKey(dpy, keycode, mods | LockMask, root, False, GrabModeAsync, GrabModeAsync);
      XGrabKey(dpy, keycode, mods | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);
      XGrabKey(dpy, keycode, mods | LockMask | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);
    }
  }
}

void parse_config_file(const char* file) {
  FILE *fp = NULL;
#define LINEBUF_SIZE 512
  char line[LINEBUF_SIZE];
  fp = fopen(file, "r");
  if (fp != NULL) {
    /* fopen succeeded */
    while (fgets(line, LINEBUF_SIZE, fp) != NULL) {
      /* Kill the newline */
      *(line + strlen(line) - 1) = '\0';
      parse_config_line(line);
    }
    fclose(fp);
  }
}

void parse_config() {
  char *homedir;

  keybindings = malloc(keybindings_size * sizeof(struct keybinding));
  recordings_size = 10;
  nrecordings = 0;
  recordings = calloc(sizeof(struct recording), recordings_size);

  parse_config_file(GLOBAL_CONFIG_FILE);
  homedir = getenv("HOME");

  if (homedir != NULL) {
    char *rcfile = NULL;
    asprintf(&rcfile, "%s/.keynavrc", homedir);
    parse_config_file(rcfile);
    free(rcfile);
  }
}

void defaults() {
  char *tmp;
  int i;
  char *default_config[] = {
    "ctrl+semicolon start",
    "Escape end",
    "ctrl+bracketleft end", /* for vi people who use ^[ */
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

  /* A special config option that will clear all keybindings */
  if (strcmp(keyseq, "clear") == 0) {
    /* Reset keybindings */
    free(keybindings);
    nkeybindings = 0;
    keybindings_size = 10;
    keybindings = malloc(keybindings_size * sizeof(struct keybinding));
  } else if (strcmp(keyseq, "daemonize") == 0) {
    daemonize = 1;
  } else {
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


GC creategc(Drawable drawable) {
  GC gc;
  XGCValues gcv;

  gc = XCreateGC(dpy, drawable, 0, NULL);
  XSetForeground(dpy, gc, BlackPixel(dpy, 0));
  XSetBackground(dpy, gc, WhitePixel(dpy, 0));
  XSetFillStyle(dpy, gc, FillSolid);

  return gc;
}

void updategrid(Window win, struct wininfo *info, int apply_clip, int draw) {
  XRectangle clip[30];
  int idx = 0;
  int w = info->w;
  int h = info->h;
  int cell_width;
  int cell_height;
  int i;

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

  if (apply_clip) {
    XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, clip, idx, ShapeSet, 0);

    /* Cut out a hole in the center */
    clip[idx].x = (w/2 - (info->center_cut_size/2));
    clip[idx].y = (h/2 - (info->center_cut_size/2));
    clip[idx].width = info->center_cut_size;
    clip[idx].height = info->center_cut_size;
    idx++;

    /* Cut out where the mouse is */
    int mousecut = 1; /* try 1 pixel cut */
    clip[idx].x = (mouseinfo.x - wininfo.x) - (mousecut / 2);
    clip[idx].y = (mouseinfo.y - wininfo.y) - (mousecut / 2);
    clip[idx].width = mousecut;
    clip[idx].height = mousecut;
    idx++;

    XShapeCombineRectangles(dpy, win, ShapeBounding, 0, 0, clip + idx - 2, 2,
                            ShapeSubtract, 0);
  } /* if apply_clip */

  if (draw) {
    XSetLineAttributes(dpy, colors.gc,
                       info->pen_size,LineSolid, CapButt, JoinBevel);

    // Fill it red.
    XSetForeground(dpy, colors.gc, colors.red.pixel);
    XFillRectangle(dpy, canvas, colors.gc, 0, 0, w, h);

    // Draw white lines.
    XSetForeground(dpy, colors.gc, WhitePixel(dpy, 0));

    for (i = 0; i < idx; i++) {
      drawborderline(info, canvas, colors.gc, &(clip[i]));
    }

    XCopyArea(dpy, canvas, win, colors.gc, 0, 0, /* wininfo.x, wininfo.y,  */
              wininfo.w, wininfo.h, 0, 0);
  } /* if draw */
}

void cmd_start(char *args) {
  XSetWindowAttributes winattr;
  int i;
  int screen;

  screen = query_current_screen();
  wininfo.curviewport = screen;

  wininfo.x = viewports[wininfo.curviewport].x;
  wininfo.y = viewports[wininfo.curviewport].y;
  wininfo.w = viewports[wininfo.curviewport].w;
  wininfo.h = viewports[wininfo.curviewport].h;
  
  /* Default start with 4 cells, 2x2 */
  wininfo.grid_x = 2;
  wininfo.grid_y = 2;

  wininfo.border_thickness = 5;
  wininfo.pen_size = 1;
  wininfo.center_cut_size = 5;

  if (ISACTIVE)
    return;

  int depth;
  int grabstate;
  int grabtries = 0;

  /* This loop is to work around the following scenario:
   * xbindkeys invokes XGrabKeyboard when you press a bound keystroke and
   * doesn't Ungrab until you release a key.
   * Example: (xbindkey '(Control semicolon) "keynav 'start, grid 2x2'")
   * This will only invoke XUngrabKeyboard when you release 'semicolon'
   *
   * The problem is that keynav would be launched as soon as the keydown
   * event 'control + semicolon' occurs, but we could only get the grab on
   * the release.
   *
   * This sleepyloop will keep trying to grab the keyboard until it succeeds.
   *
   * Reported by Colin Shea
   */
  grabstate = XGrabKeyboard(dpy, viewports[wininfo.curviewport].root, False,
                            GrabModeAsync, GrabModeAsync, CurrentTime);
  while (grabstate != GrabSuccess) {
    usleep(10000); /* sleep for 10ms */
    grabstate = XGrabKeyboard(dpy, viewports[wininfo.curviewport].root, False,
                              GrabModeAsync, GrabModeAsync, CurrentTime);
    grabtries += 1;
    if (grabtries >= 20) {
      fprintf(stderr, "XGrabKeyboard failed %d times, giving up...\n",
              grabtries);

      /* Returning from here will result in the appstate.active still
       * being false. */
      return;
    }
  }
  //printf("Got grab!\n");

  appstate.active = True;

  if (zone == 0) { /* Create our window for the first time */
    depth = viewports[wininfo.curviewport].screen->root_depth;
    wininfo_history_cursor = 0;

    zone = XCreateSimpleWindow(dpy, viewports[wininfo.curviewport].root,
                               wininfo.x, wininfo.y, 1, 1, 0, 
                               ((unsigned long) 1) << depth - 1,
                               ((unsigned long) 1) << depth - 1);
    xdo_window_setclass(xdo, zone, "keynav", "keynav");

    canvas = XCreatePixmap(dpy, zone,
                           viewports[wininfo.curviewport].w,
                           viewports[wininfo.curviewport].h,
                           viewports[wininfo.curviewport].screen->root_depth);

    colors.gc = creategc(canvas);

    /* Tell the window manager not to manage us */
    winattr.override_redirect = 1;
    XChangeWindowAttributes(dpy, zone, CWOverrideRedirect, &winattr);

    XSelectInput(dpy, zone, 
                 StructureNotifyMask | ExposureMask | PointerMotionMask);
  } /* if zone == 0 */
}

void cmd_end(char *args) {
  if (!ISACTIVE)
    return;

  /* kill drag state too */
  if (ISDRAGGING)
    cmd_drag(NULL);

  /* Stop recording if we're in that mode */
  if (appstate.recording != record_off) {
    cmd_record(NULL);
  }

  appstate.active = False;

  //XDestroyWindow(dpy, zone);
  XUnmapWindow(dpy, zone);
  XUngrabKeyboard(dpy, CurrentTime);
}

void cmd_history_back(char *args) {
  if (!ISACTIVE)
    return;

  restore_history_point(1);
}

void cmd_shell(char *args) {
  // Trim leading and trailing quotes if they exist
  if (*args == '"') {
    args++;
    *(args + strlen(args) - 1) = '\0';
  }

  if (fork() == 0) { /* child */
    int ret;
    char *const shell = "/bin/sh";
    char *const argv[4] = { shell, "-c", args, NULL };
    //printf("Exec: %s\n", args);
    //printf("Shell: %s\n", shell);
    ret = execvp(shell, argv);
    perror("execve");
    exit(1);
  }
}

void cmd_quit(char *args) {
  exit(0);
}

void cmd_cut_up(char *args) {
  if (!ISACTIVE)
    return;
  wininfo.h = percent_of(wininfo.h, args, .5);
}

void cmd_cut_down(char *args) {
  if (!ISACTIVE)
    return;

  int orig = wininfo.h;
  wininfo.h = percent_of(wininfo.h, args, .5);
  wininfo.y += orig - wininfo.h;
}

void cmd_cut_left(char *args) {
  if (!ISACTIVE)
    return;
  wininfo.w = percent_of(wininfo.w, args, .5);
}

void cmd_cut_right(char *args) {
  int orig = wininfo.w;
  if (!ISACTIVE)
    return;
  wininfo.w = percent_of(wininfo.w, args, .5);
  wininfo.x += orig - wininfo.w;
}

void cmd_move_up(char *args) {
  if (!ISACTIVE)
    return;
  wininfo.y -= percent_of(wininfo.h, args, 1);
}

void cmd_move_down(char *args) {
  if (!ISACTIVE)
    return;
  wininfo.y += percent_of(wininfo.h, args, 1);
}

void cmd_move_left(char *args) {
  if (!ISACTIVE)
    return;
  wininfo.x -= percent_of(wininfo.w, args, 1);
}

void cmd_move_right(char *args) {
  if (!ISACTIVE)
    return;
  wininfo.x += percent_of(wininfo.w, args, 1);
}

void cmd_cursorzoom(char *args) {
  int xradius = 0, yradius = 0, width = 0, height = 0;
  int xloc, yloc;
  if (!ISACTIVE)
    return;

  sscanf(args, "%d %d %d %d", &xradius, &yradius, &width, &height);

  xdo_mouselocation(xdo, &xloc, &yloc, NULL);

  //xloc -= xradius;
  //yloc -= yradius;

  wininfo.x = xloc - (width / 2);
  wininfo.y = yloc - (height / 2);
  wininfo.w = width;
  wininfo.h = height;
}

void cmd_windowzoom(char *args) {
  Window curwin;
  Window rootwin;
  Window dummy_win;
  int x, y, width, height, border_width, depth;

  xdo_window_get_active(xdo, &curwin);
  XGetGeometry(xdo->xdpy, curwin, &rootwin, &x, &y, &width, &height,
               &border_width, &depth);
  XTranslateCoordinates(xdo->xdpy, curwin, rootwin, 
                        -border_width, -border_width, &x, &y, &dummy_win);

  wininfo.x = x;
  wininfo.y = y;
  wininfo.w = width;
  wininfo.h = height;
}

void cmd_warp(char *args) {
  if (!ISACTIVE)
    return;
  xdo_mousemove(xdo, wininfo.x + wininfo.w / 2, wininfo.y + wininfo.h / 2,
                viewports[wininfo.curviewport].screen_num);
}

void cmd_click(char *args) {
  if (!ISACTIVE)
    return;

  int button;
  button = atoi(args);
  if (button > 0)
    xdo_click(xdo, CURRENTWINDOW, button);
  else
    fprintf(stderr, "Negative mouse button is invalid: %d\n", button);
}

void cmd_doubleclick(char *args) {
  if (!ISACTIVE)
    return;
  cmd_click(args);
  cmd_click(args);
}

void cmd_drag(char *args) {
  if (!ISACTIVE)
    return;

  int button;
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
  }

  if (button <= 0) {
    fprintf(stderr, "Negative or no mouse button given. Not valid. Button read was '%d'\n", button);
    return;
  }

  drag_button = button;

  if (ISDRAGGING) { /* End dragging */
    appstate.dragging = False;
    xdo_mouseup(xdo, CURRENTWINDOW, button);
  } else { /* Start dragging */
    appstate.dragging = True;
    xdo_keysequence_down(xdo, 0, drag_modkeys);
    xdo_mousedown(xdo, CURRENTWINDOW, button);

    /* Sometimes we need to move a little to tell the app we're dragging */
    /* TODO(sissel): Make this a 'mousewiggle' command */
    xdo_mousemove_relative(xdo, 1, 0);
    xdo_mousemove_relative(xdo, -1, 0);
    xdo_keysequence_up(xdo, 0, drag_modkeys);
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

void cmd_record(char *args) {
  if (!ISACTIVE)
    return;

  if (appstate.recording != record_off) {
    //printf("Finish recording\n");
    appstate.recording = record_off;

    /* Bump our recording count */
    nrecordings++;
    if (nrecordings >= recordings_size) {
      recordings_size *= 2;
      recordings = realloc(recordings, sizeof(struct recording) * recordings_size);
    }
  } else {
    //printf("Start recording\n");
    appstate.recording = record_getkey;
  }
}

void update() {
  if (!ISACTIVE)
    return;

  if (wininfo.x < viewports[wininfo.curviewport].x)
    viewport_left();

  if (wininfo.x + wininfo.w >
      viewports[wininfo.curviewport].x + viewports[wininfo.curviewport].w)
    viewport_right();

  /* Fix positioning if we went out of bounds (off the screen) */
  if (wininfo.x < 0)
    wininfo.x = 0;
  if (wininfo.x + wininfo.w > 
      viewports[wininfo.curviewport].x + viewports[wininfo.curviewport].w)
    wininfo.x = viewports[wininfo.curviewport].x + viewports[wininfo.curviewport].w - wininfo.w;

  /* XXX: We don't currently understand how to move around if displays are
   * vertically stacked. */
  if (wininfo.y < 0)
    wininfo.y = 0;
  if (wininfo.y + wininfo.h > 
      viewports[wininfo.curviewport].y + viewports[wininfo.curviewport].h)
    wininfo.y = viewports[wininfo.curviewport].h - wininfo.h;

  if (wininfo.w <= 1 || wininfo.h <= 1) {
    cmd_end(NULL);
    return;
  }

  updategrid(zone, &wininfo, True, True);
  XMoveResizeWindow(dpy, zone, wininfo.x, wininfo.y, wininfo.w, wininfo.h);
  XMapRaised(dpy, zone);
}

void viewport_right() {
  int expand_w = 0, expand_h = 0;

  /* Expand if the current window is the size of the current viewport */
  //printf("right %d] %d,%d vs %d,%d\n", wininfo.curviewport,
         //wininfo.w, wininfo.h,
         //viewports[wininfo.curviewport].w, viewports[wininfo.curviewport].h);
  if (wininfo.curviewport == nviewports - 1)
    return;

  if (wininfo.w == viewports[wininfo.curviewport].w)
    expand_w = 1;
  if (wininfo.h == viewports[wininfo.curviewport].h) {
    expand_h = 1;
  }

  wininfo.curviewport++;

  if ((expand_w) || wininfo.w > viewports[wininfo.curviewport].w) {
    wininfo.w = viewports[wininfo.curviewport].w;
  }
  if ((expand_h) || wininfo.h > viewports[wininfo.curviewport].h) {
    wininfo.h = viewports[wininfo.curviewport].h;
  }
  wininfo.x = viewports[wininfo.curviewport].x;
  //wininfo.y = viewports[wininfo.curviewport].y;
}

void viewport_left() {
  int expand_w = 0, expand_h = 0;

  /* Expand if the current window is the size of the current viewport */
  //printf("left %d] %d,%d vs %d,%d\n", wininfo.curviewport,
         //wininfo.w, wininfo.h,
         //viewports[wininfo.curviewport].w, viewports[wininfo.curviewport].h);
  if (wininfo.curviewport == 0)
    return;

  if (wininfo.w == viewports[wininfo.curviewport].w)
    expand_w = 1;
  if (wininfo.h == viewports[wininfo.curviewport].h) {
    expand_h = 1;
  }

  wininfo.curviewport--;
  if (expand_w || wininfo.w > viewports[wininfo.curviewport].w) {
    wininfo.w = viewports[wininfo.curviewport].w;
  }
  if (expand_h || wininfo.h > viewports[wininfo.curviewport].h) {
    wininfo.h = viewports[wininfo.curviewport].h;
  }
  wininfo.x = viewports[wininfo.curviewport].w - wininfo.w;
  //wininfo.y = viewports[wininfo.curviewport].h - wininfo.h;
}

void drawborderline(struct wininfo *info, Drawable drawable,
                    GC gc, XRectangle *rect) {
  int penoffset = ((info->border_thickness / 2) - (info->pen_size / 2));
  XDrawLine(dpy, drawable, gc,
            (rect->x + (rect->width / 2)),
            (rect->y + penoffset),
            (rect->x + (rect->width / 2)),
            ((rect->y + rect->height) - (penoffset*2))
           );
  XDrawLine(dpy, drawable, gc,
            (rect->x + penoffset),
            (rect->y + (rect->height / 2)),
            (rect->x + rect->width - penoffset),
            (rect->y + (rect->height / 2))
           );
}

void handle_keypress(XKeyEvent *e) {
  int i;
  int key_found = 0;

  /* If a mouse button is pressed (like, when we're dragging),
   * then the 'mods' will include values like Button1Mask. 
   * Let's remove those, as they cause breakage */
  e->state &= ~(Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask);

  /* Ignore LockMask (Numlock, etc) and Mod2Mask (shift, etc) */
  e->state &= ~(LockMask | Mod2Mask);

  if (appstate.recording == record_getkey) {
    appstate.recording = record_ing; /* start recording actions */
    /* TODO(sissel): support keys with keystrokes like shift+a */

    /* check existing recording keycodes if we need to override it */
    for (i = 0; i < nrecordings; i++) {
      if (recordings[i].keycode == e->keycode) {
        int j;
        for (j = 0; j < recordings[i].ncommands; j++) {
          free(recordings[i].commands[j]);
        }
        recordings[i].keycode = 0;
      }
    }

    //printf("Recording as keycode:%d\n", e->keycode);
    recordings[nrecordings].keycode = e->keycode;
    recordings[nrecordings].ncommands = 0;
    recordings[nrecordings].commands_size = 10;
    recordings[nrecordings].commands = calloc(sizeof(char *), recordings[nrecordings].commands_size);
    return;
  }

  /* Loop over known keybindings */
  for (i = nkeybindings - 1; i >= 0; i--) {
    //printf("e->state:%d bindmods:%d and:%d\n", e->state, keybindings[i].mods, e->state & keybindings[i].mods);
    int keycode = keybindings[i].keycode;
    int mods = keybindings[i].mods;
    char *commands = keybindings[i].commands;
    if ((keycode == e->keycode) && (mods == e->state)) {
      handle_commands(commands);
      key_found = 1;
    }
  }

  /* Break now if this is a normal command */
  if (key_found)
    return;

  /* Loop over known recordings */
  for (i = nrecordings - 1; i >= 0; i--) {
    struct recording *rec = &(recordings[i]);
    //printf("Comparing: %d vs %d\n", rec->keycode, e->keycode);
    if (e->keycode == rec->keycode) {
      int j = 0;
      for (j = 0; j < rec->ncommands; j++) {
        handle_commands(rec->commands[j]);
      }
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
    if (appstate.recording == record_ing) {
      //printf("Command: %s\n", tok);
      struct recording *rec = &(recordings[nrecordings]);
      rec->commands[rec->ncommands] = strdup(tok);
      rec->ncommands++;

      if (rec->ncommands >= rec->commands_size) {
        rec->commands_size *= 2;
        rec->commands = realloc(rec->commands, rec->commands_size * sizeof(char *));
      }
    }

    for (i = 0; dispatch[i].command; i++) {

      /* XXX: This approach means we can't have one command be a subset of
       * another. For example, 'grid' and 'grid-foo' will fail because when you
       * use 'grid-foo' it'll match 'grid' first. 
       * This hasn't been a problem yet...
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

        if (ISDRAGGING)
          cmd_warp(NULL);
      }
    }

    if (!found)
      fprintf(stderr, "No such command: '%s'\n", tok);
  }

  if (ISACTIVE) {
    update();
    save_history_point();
  }

  free(cmdcopy);
}

void save_history_point() {
  /* If the history is full, drop the oldest entry */
  while (wininfo_history_cursor >= WININFO_MAXHIST) {
    int i;
    for (i = 1; i < wininfo_history_cursor; i++) {
      memcpy(&(wininfo_history[i - 1]),
             &(wininfo_history[i]),
             sizeof(wininfo_t));
    }
    wininfo_history_cursor--;
  }

  memcpy(&(wininfo_history[wininfo_history_cursor]),
         &(wininfo),
         sizeof(wininfo));

  wininfo_history_cursor++;
}

void restore_history_point(int moves_ago) {
  wininfo_history_cursor -= moves_ago + 1;
  if (wininfo_history_cursor < 0)
    wininfo_history_cursor = 0;

  memcpy(&(wininfo),
         &(wininfo_history[wininfo_history_cursor]),
         sizeof(wininfo));
}

/* Sort viewports, left to right.
 * This may perform undesirably for vertically-stacked viewports or
 * for other odd configurations */
int viewport_sort(const void *a, const void *b) {
  viewport_t *va = (viewport_t *)a;
  viewport_t *vb = (viewport_t *)b;

  return va->x - vb->x;
}

void query_screens() {
  int dummyint;
  if (XineramaQueryExtension(dpy, &dummyint, &dummyint)
      && XineramaIsActive(dpy)) {
    xinerama = True;
    query_screen_xinerama();
  } else { /* No xinerama */
    query_screen_normal();
  }

  qsort(viewports, nviewports, sizeof(viewport_t), viewport_sort);
}

void query_screen_xinerama() {
  int i;
  XineramaScreenInfo *screeninfo;

  screeninfo = XineramaQueryScreens(dpy, &nviewports);
  viewports = calloc(nviewports, sizeof(viewport_t));
  for (i = 0; i < nviewports; i++) {
    viewports[i].x = screeninfo[i].x_org;
    viewports[i].y = screeninfo[i].y_org;
    viewports[i].w = screeninfo[i].width;
    viewports[i].h = screeninfo[i].height;
    viewports[i].screen_num = 0;
    viewports[i].screen = ScreenOfDisplay(dpy, 0);
    viewports[i].root = DefaultRootWindow(dpy);
  }
  XFree(screeninfo);
}

void query_screen_normal() {
  int i;
  Screen *s;
  nviewports = ScreenCount(dpy);
  viewports = calloc(nviewports, sizeof(viewport_t));

  for (i = 0; i < nviewports; i++) {
    s = ScreenOfDisplay(dpy, i);
    viewports[i].x = 0;
    viewports[i].y = 0;
    viewports[i].w = s->width;
    viewports[i].h = s->height;
    viewports[i].root = RootWindowOfScreen(s);
    viewports[i].screen_num = i;
    viewports[i].screen = s; }
}

int query_current_screen() {
  int i;
  if (xinerama) {
    return query_current_screen_xinerama();
  } else { 
    return query_current_screen_normal();
  }
}

int query_current_screen_xinerama() {
  int i = 0, dummyint;
  unsigned int dummyuint;
  int x, y;
  Window dummywin;
  Window root = viewports[0].root;
  XQueryPointer(dpy, root, &dummywin, &dummywin,
                &x, &y, &dummyint, &dummyint, &dummyuint);

  /* Figure which display the cursor is on */
  for (i = 0; i < nviewports; i++) {
    if (pointinrect(x, y, viewports[i].x, viewports[i].y,
                    viewports[i].w, viewports[i].h)) {
      return i;
    }
  }

  return -1;
}

int query_current_screen_normal() {
  int i = 0, dummyint;
  unsigned int dummyuint;
  int x, y;
  Window dummywin;
  Window root = viewports[0].root;
  /* Query each Screen's root window to ask if the pointer is in it.
   * I don't know of any other better way to ask what Screen is
   * active (where is the cursor) */
  for (i = 0; i < nviewports; i++) {
    if (!XQueryPointer(dpy, viewports[i].root, &dummywin, &dummywin,
                      &x, &y, &dummyint, &dummyint, &dummyuint))
      continue;

    if (pointinrect(x, y, viewports[i].x, viewports[i].y,
                    viewports[i].w, viewports[i].h)) {
      return i;
    }
  }

  return -1;
}

int pointinrect(int px, int py, int rx, int ry, int rw, int rh) {
  return (px >= rx)
          && (px <= rx + rw)
          && (py >= ry)
          && (py <= ry + rh);
}

void sigchld(int sig) {
  int status;
  while (waitpid(-1, &status, WNOHANG) > 0) {
    /* Do nothing, but we needed to waitpid() to collect dead children */
  }
}

int main(int argc, char **argv) {
  char *pcDisplay;
  int ret;

  if ( (pcDisplay = getenv("DISPLAY")) == NULL) {
    fprintf(stderr, "Error: DISPLAY environment variable not set\n");
    exit(1);
  }

  if ( (dpy = XOpenDisplay(pcDisplay)) == NULL) {
    fprintf(stderr, "Error: Can't open display: %s\n", pcDisplay);
    exit(1);
  }

  signal(SIGCHLD, sigchld);
  xdo = xdo_new_with_opened_display(dpy, pcDisplay, False);

  parse_config();
  query_screens();
  colors.colormap = DefaultColormap(dpy, 0);
  XAllocNamedColor(dpy, colors.colormap, "darkred", &colors.red, &colors.dummy);

  /* Sync with the X server.
   * This ensure we errors about XGrabKey and other failures
   * before we try to daemonize */
  XSync(dpy, 0);

  if (daemonize) {
    printf("Daemonizing now...\n");
    daemon(0, 0);
  }

  if (argc == 2) {
    handle_commands(argv[1]);
  } else if (argc > 2) {
    fprintf(stderr, "Usage: %s [command string]\n", argv[0]);
    fprintf(stderr, "Did you quote your command string?\n");
    exit(1);
  }

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
        updategrid(zone, &wininfo, False, True);
        break;

      case Expose:
        XCopyArea(dpy, canvas, zone, colors.gc, e.xexpose.x, e.xexpose.y,
                  e.xexpose.width, e.xexpose.height,
                  e.xexpose.x, e.xexpose.y);

        break;

      case MotionNotify:
        mouseinfo.x = e.xmotion.x_root;
        mouseinfo.y = e.xmotion.y_root;
        updategrid(zone, &wininfo, True, False);
        break;

      // Ignorable events.
      case GraphicsExpose:
      case NoExpose:
      case KeyRelease:    // key was released
      case DestroyNotify: // window was destroyed
      case UnmapNotify:   // window was unmapped (hidden)
      case MappingNotify: // when keyboard mapping changes
        break;
      default:
        printf("Unexpected X11 event: %d\n", e.type);
        break;
    }
  }

  xdo_free(xdo);
}
