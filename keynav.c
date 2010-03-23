/*
 * keynav - Keyboard navigation tool. 
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
#include <glib.h>
#include <cairo-xlib.h>

#ifdef PROFILE_THINGS
#include <time.h>
#endif

#include <xdo.h>
#include "keynav_version.h"

#ifndef GLOBAL_CONFIG_FILE
#define GLOBAL_CONFIG_FILE "/etc/keynavrc"
#endif /* GLOBAL_CONFIG_FILE */

extern char **environ;

#define ISACTIVE (appstate.active)
#define ISDRAGGING (appstate.dragging)

struct appstate {
  int active;
  int dragging;
  int need_draw;
  enum { record_getkey, record_ing, record_off } recording;
};

typedef struct recording {
  int keycode;
  GPtrArray *commands;
} recording_t;

GPtrArray *recordings;
recording_t *active_recording = NULL;
char *recordings_filename = NULL;

typedef struct wininfo {
  int x;
  int y;
  int w;
  int h;
  int grid_x;
  int grid_y;
  int border_thickness;
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

static GC canvas_gc;
static Pixmap canvas;
static cairo_surface_t *canvas_surface;
static cairo_t *canvas_cairo;
static Pixmap shape;
static cairo_surface_t *shape_surface;
static cairo_t *shape_cairo;

static xdo_t *xdo;
static struct appstate appstate = {
  .active = 0,
  .dragging = 0,
  .recording = record_off,
};

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
void correct_overflow();
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
void recordings_save(const char *filename);
void parse_recordings(const char *filename);

typedef struct dispatch {
  char *command;
  void (*func)(char *args);
} dispatch_t;

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

typedef struct keybinding {
  char *commands;
  int keycode;
  int mods;
} keybinding_t; 

GPtrArray *keybindings = NULL;

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
  GPtrArray *mods;
  int modmask = 0;

  mods = g_ptr_array_new();

  strptr = dup = strdup(keyseq);
  while ((tok = strtok_r(strptr, "+", &tokctx)) != NULL) {
    strptr = NULL;
    g_ptr_array_add(mods, tok);
  }

  int i = 0;
  /* Use all but the last token as modifiers */
  const char **symbol_map = xdo_symbol_map();
  for (i = 0; i < mods->len; i++) {
    KeySym keysym = 0;
    int j = 0;
    const char *mod = g_ptr_array_index(mods, i);

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
  g_ptr_array_free(mods, FALSE);
  return modmask;
}

void addbinding(int keycode, int mods, char *commands) {
  int i;

  // Check if we already have a binding for this, if so, override it.
  for (i = 0; i < keybindings->len; i++) {
    keybinding_t *kbt = g_ptr_array_index(keybindings, i);
    if (kbt->keycode == keycode && kbt->mods == mods) {
      free(kbt->commands);
      kbt->commands = strdup(commands);
      return;
    }
  }

  keybinding_t *keybinding = NULL;
  keybinding = calloc(sizeof(keybinding_t), 1);
  keybinding->commands = strdup(commands);
  keybinding->keycode = keycode;
  keybinding->mods = mods;
  g_ptr_array_add(keybindings, keybinding);

  if (!strncmp(commands, "start", 5)) {
    int i = 0;
    /* Grab on all screen root windows */
    for (i = 0; i < ScreenCount(dpy); i++) {
      Window root = RootWindow(dpy, i);
      XGrabKey(dpy, keycode, mods, root, False, GrabModeAsync, GrabModeAsync);
      XGrabKey(dpy, keycode, mods | LockMask, root, False, GrabModeAsync, GrabModeAsync);
      XGrabKey(dpy, keycode, mods | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);
      XGrabKey(dpy, keycode, mods | LockMask | Mod2Mask, root, False, GrabModeAsync, GrabModeAsync);
    }
  }

  if (!strncmp(commands, "record", 6)) {
    char *path = commands + 6;
    char *newrecordingpath;

    while (isspace(*path))
      path++;

    /* If args is nonempty, try to use it as the file to store recordings in */
    if (path != NULL && path[0] != '\0') {
      /* Handle ~/ swapping in for actual homedir */
      if (!strncmp(path, "~/", 2)) {
        asprintf(&newrecordingpath, "%s/%s", getenv("HOME"), path + 2);
      } else {
        newrecordingpath = strdup(path);
      }

      /* Fail if we try to set the record file to another name than we set
       * previously */
      if (recordings_filename != NULL
          && strcmp(recordings_filename, newrecordingpath)) {
        free(newrecordingpath);
        fprintf(stderr, 
                "Recordings file already set to '%s', you tried to\n"
                "set it to '%s'. Keeping original value.\n",
                recordings_filename, path);
      } else {
        parse_recordings(recordings_filename);
      }
    }
  } /* special config handling for 'record' */
}

void parse_config_file(const char* file) {
  FILE *fp = NULL;
#define LINEBUF_SIZE 512
  char line[LINEBUF_SIZE];
  fp = fopen(file, "r");
  if (fp == NULL)
    return;
  /* fopen succeeded */
  while (fgets(line, LINEBUF_SIZE, fp) != NULL) {
    /* Kill the newline */
    *(line + strlen(line) - 1) = '\0';
    parse_config_line(line);
  }
  fclose(fp);
}

void parse_config() {
  char *homedir;

  keybindings = g_ptr_array_new();
  recordings = g_ptr_array_new();

  defaults();
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
    "clear",
    "ctrl+semicolon start",
    "Escape end",
    "ctrl+bracketleft end", /* for vi people who use ^[ */
    "q record ~/.keynav_macros",
    "a history-back",
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
    "t windowzoom",
    "c cursorzoom 300 300",
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
  while (isspace(*line))
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
    g_ptr_array_free(keybindings, TRUE);
    keybindings = g_ptr_array_new();
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

void updategrid(Window win, struct wininfo *info, int apply_clip, int draw) {
  double w = info->w;
  double h = info->h;
  double cell_width;
  double cell_height;
  double x_off, y_off;
  int i;
  
#ifdef PROFILE_THINGS
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
#endif

  //printf("updategrid: clip:%d, draw:%d\n", apply_clip, draw);

  x_off = info->border_thickness / 2;
  y_off = info->border_thickness / 2;

  if (draw) {
    cairo_new_path(canvas_cairo);
    //cairo_rectangle(canvas_cairo, 0, 0, w, h);
    //cairo_set_source_rgb(canvas_cairo, 1, 1, 0);
    //cairo_fill(canvas_cairo);
    cairo_set_line_width(canvas_cairo, wininfo.border_thickness);
  }

  if (apply_clip) {
    cairo_new_path(shape_cairo);
    cairo_set_operator(shape_cairo, CAIRO_OPERATOR_CLEAR);
    cairo_rectangle(shape_cairo, 0, 0, w, h);
    cairo_fill(shape_cairo);
  }

  w -= info->border_thickness;
  h -= info->border_thickness;
  cell_width = (w / info->grid_y);
  cell_height = (h / info->grid_x);

  h++;
  w++;
  /* clip vertically */
  for (i = 0; i <= info->grid_y; i++) {
    cairo_move_to(canvas_cairo, cell_width * i + x_off, y_off);
    cairo_line_to(canvas_cairo, cell_width * i + x_off, h);
  }

  /* clip horizontally */
  for (i = 0; i <= info->grid_x; i++) {
    cairo_move_to(canvas_cairo, x_off, cell_height * i + y_off);
    cairo_line_to(canvas_cairo, w, cell_height * i + y_off);
  }

  cairo_path_t *path = cairo_copy_path(canvas_cairo);

#ifdef PROFILE_THINGS
  clock_gettime(CLOCK_MONOTONIC, &end);
  printf("updategrid pathbuild time: %ld.%09ld\n",
         end.tv_sec - start.tv_sec,
         end.tv_nsec - start.tv_nsec);
  clock_gettime(CLOCK_MONOTONIC, &start);
#endif

  if (apply_clip) {
    cairo_new_path(shape_cairo);
    cairo_append_path(shape_cairo, path);
    cairo_set_operator(shape_cairo, CAIRO_OPERATOR_OVER);
    cairo_stroke(shape_cairo);
    XShapeCombineMask(dpy, win, ShapeBounding, 0, 0, shape, ShapeSet);

#ifdef PROFILE_THINGS
    XSync(dpy, False);
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("updategrid applyclip time: %ld.%09ld\n",
           end.tv_sec - start.tv_sec,
           end.tv_nsec - start.tv_nsec);
    clock_gettime(CLOCK_MONOTONIC, &start);
#endif
  } /* if apply_clip */

  if (draw) {
    cairo_set_source_rgb(canvas_cairo, 0.5, 0, 0);
    cairo_stroke(canvas_cairo);

    /* cairo_stroke clears the current path, put it back */
    cairo_append_path(canvas_cairo, path);
    cairo_set_line_width(canvas_cairo, 1);
    cairo_set_source_rgba(canvas_cairo, 1, 1, 1, .7);
    cairo_stroke(canvas_cairo);

    XCopyArea(dpy, canvas, win, canvas_gc,
              0, 0, wininfo.w, wininfo.h, 0, 0);
#ifdef PROFILE_THINGS
    XSync(dpy, False);
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("updategrid draw time: %ld.%09ld\n",
           end.tv_sec - start.tv_sec,
           end.tv_nsec - start.tv_nsec);
    clock_gettime(CLOCK_MONOTONIC, &start);
#endif
  } /* if draw */

  cairo_path_destroy(path);
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
  appstate.need_draw = 1;

  if (zone == 0) { /* Create our window for the first time */
    viewport_t *viewport = &(viewports[wininfo.curviewport]);
    
    depth = viewports[wininfo.curviewport].screen->root_depth;
    wininfo_history_cursor = 0;

    zone = XCreateSimpleWindow(dpy, viewport->root,
                               wininfo.x, wininfo.y, wininfo.w, wininfo.h, 0, 0, 0);
    xdo_window_setclass(xdo, zone, "keynav", "keynav");
    canvas_gc = XCreateGC(dpy, zone, 0, NULL);

    canvas = XCreatePixmap(dpy, zone, viewport->w, viewport->h,
                           viewport->screen->root_depth);
    canvas_surface = cairo_xlib_surface_create(dpy, canvas,
                                               viewport->screen->root_visual,
                                               viewport->w, viewport->h);
    canvas_cairo = cairo_create(canvas_surface);
    cairo_set_antialias(canvas_cairo, CAIRO_ANTIALIAS_NONE);
    cairo_set_line_cap(canvas_cairo, CAIRO_LINE_CAP_SQUARE);

    shape = XCreatePixmap(dpy, zone, viewport->w, viewport->h, 1);
    shape_surface = cairo_xlib_surface_create_for_bitmap(dpy, shape,
                                                         viewport->screen,
                                                         viewport->w,
                                                         viewport->h);
    shape_cairo = cairo_create(shape_surface);
    cairo_set_line_width(shape_cairo, wininfo.border_thickness);
    cairo_set_line_cap(shape_cairo, CAIRO_LINE_CAP_SQUARE);

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

  sscanf(args, "%d %d", &width, &height);
  xdo_mouselocation(xdo, &xloc, &yloc, NULL);

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
  char *filename;
  if (!ISACTIVE)
    return;

  if (appstate.recording != record_off) {
    appstate.recording = record_off;
    g_ptr_array_add(recordings, (gpointer) active_recording);

    /* Save to file */
    if (recordings_filename != NULL) {
      recordings_save(recordings_filename);
    }
  } else {
    active_recording = calloc(sizeof(recording_t), 1);
    active_recording->commands = g_ptr_array_new();
    appstate.recording = record_getkey;
  }
}

void update() {
  if (!ISACTIVE)
    return;

  correct_overflow();
  if (wininfo.w <= 1 || wininfo.h <= 1) {
    cmd_end(NULL);
    return;
  }

  wininfo_t *previous = &(wininfo_history[wininfo_history_cursor - 1]);
  //printf("window: %d,%d @ %d,%d\n", wininfo.w, wininfo.h, wininfo.x, wininfo.y);
  //printf("previous: %d,%d @ %d,%d\n", previous->w, previous->h, previous->x, previous->y);
  int draw = 0, move = 0, resize = 0, clip = 0;
  if (previous->x != wininfo.x || previous->y != wininfo.y) {
    move = 1;
  }

  if (previous->w != wininfo.w || previous->h != wininfo.h) {
    clip = 1;
    draw = 1;
    resize = 1;
  }

  if (appstate.need_draw) {
    move = 1;
    clip = 1;
    draw = 1;
    resize = 1;
    appstate.need_draw = 0;
  }

  //printf("move: %d, clip: %d, draw: %d, resize: %d\n", move, clip, draw, resize);

  if (clip || draw) {
    updategrid(zone, &wininfo, clip, draw);
  }

  if (resize) {
    XResizeWindow(dpy, zone, wininfo.w, wininfo.h);
  }

  if (move) {
    XMoveWindow(dpy, zone, wininfo.x, wininfo.y);
  }

  XMapRaised(dpy, zone);
}

void correct_overflow() {
  /* If the window is outside the boundaries of the screen, bump it back
   * or, if possible, move it to the next screen */

  if (wininfo.x < viewports[wininfo.curviewport].x)
    viewport_left();
  if (wininfo.x + wininfo.w >
      viewports[wininfo.curviewport].x + viewports[wininfo.curviewport].w)
    viewport_right();

  /* Fix positioning if we went out of bounds (off the screen) */
  if (wininfo.x < 0) {
    wininfo.x = 0;
  }
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
    for (i = 0; i < recordings->len; i++) {
      recording_t *rec = (recording_t *) g_ptr_array_index(recordings, i);
      if (rec->keycode == e->keycode) {
        g_ptr_array_free(rec->commands, TRUE);
        g_ptr_array_remove_index_fast(recordings, i);
        i--; /* array removal will shift everything down one to make up for the loss 
                we'll need to redo this index */
      }
    }

    //printf("Recording as keycode:%d\n", e->keycode);
    active_recording->keycode = e->keycode;
    return;
  }

  /* Loop over known keybindings */
  for (i = 0; i < keybindings->len; i++) {
    keybinding_t *kbt = g_ptr_array_index(keybindings, i);
    int keycode = kbt->keycode;
    int mods = kbt->mods;
    char *commands = kbt->commands;
    if ((keycode == e->keycode) && (mods == e->state)) {
      handle_commands(commands);
      key_found = 1;
    }
  }

  /* Break now if this is a normal command */
  if (key_found)
    return;

  /* Loop over known recordings */
  for (i = 0; i < recordings->len; i++) {
    recording_t *rec = g_ptr_array_index(recordings, i);
    if (e->keycode == rec->keycode) {
      int j = 0;
      for (j = 0; j < rec->commands->len; j++) {
        handle_commands(g_ptr_array_index(rec->commands, j));
      }
    }
  }
}

void handle_commands(char *commands) {
  char *cmdcopy;
  char *tokctx, *tok, *strptr;

  //printf("Commands; %s\n", commands);
  cmdcopy = strdup(commands);
  strptr = cmdcopy;
  while ((tok = strtok_r(strptr, ",", &tokctx)) != NULL) {
    int i;
    int found = 0;
    strptr = NULL;

    /* Ignore leading whitespace */
    while (isspace(*tok))
      tok++;

    /* Record this command (if the command is not 'record') */
    if (appstate.recording == record_ing && strncmp(tok, "record", 6)) {
      //printf("Record: %s\n", tok);
      g_ptr_array_add(active_recording->commands, (gpointer) strdup(tok));
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

        /* Fix keynav window boundaries if they exceed the screen */
        correct_overflow();
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

void recordings_save(const char *filename) {
  FILE *output = NULL;
  int i = 0;

  output = fopen(filename, "w");
  if (output == NULL) {
    fprintf(stderr, "Failure opening '%s' for write: %s\n", filename, strerror(errno));
    return; /* Should we exit instead? */
  }

  for (i = 0; i < recordings->len; i++) {
    int j;
    recording_t *rec = g_ptr_array_index(recordings, i);
    if (rec->commands->len == 0)
      continue;

    fprintf(output, "%d ", rec->keycode);
    for (j = 0; j < rec->commands->len; j++) {
      fprintf(output, "%s%s", 
              (char *) g_ptr_array_index(rec->commands, j),
              (j + 1 < rec->commands->len ? ", " : ""));
    }

    fprintf(output, "\n");
  }

  fclose(output);
}

void parse_recordings(const char *filename) {
  FILE *fp = fopen(filename, "r");
  if (fp == NULL)
    return;

  static const int bufsize = 8192;
  char line[bufsize];
  /* fopen succeeded */
  while (fgets(line, bufsize, fp) != NULL) {
    /* Kill the newline */
    *(line + strlen(line) - 1) = '\0';

    int keycode = 0;
    char *command = NULL;
    keycode = atoi(line);
    command = line + strcspn(line, " \t");
    //printf("found recording: %d => %s\n", keycode, command);

    recording_t *rec = NULL;
    rec = calloc(sizeof(recording_t), 1);
    rec->keycode = keycode;
    rec->commands = g_ptr_array_new();
    g_ptr_array_add(rec->commands, (gpointer) strdup(command));
    g_ptr_array_add(recordings, (gpointer) rec);
  }
  fclose(fp);
}

int main(int argc, char **argv) {
  char *pcDisplay;
  int ret;

  if ((pcDisplay = getenv("DISPLAY")) == NULL) {
    fprintf(stderr, "Error: DISPLAY environment variable not set\n");
    exit(1);
  }

  if ((dpy = XOpenDisplay(pcDisplay)) == NULL) {
    fprintf(stderr, "Error: Can't open display: %s\n", pcDisplay);
    exit(1);
  }

  if (argc > 1 && (!strcmp(argv[1], "version") 
                   || !strcmp(argv[1], "-v") 
                   || !strcmp(argv[1], "--version"))) {
    printf("keynav %s\n", KEYNAV_VERSION);
    return 0;
  }

  signal(SIGCHLD, sigchld);
  xdo = xdo_new_with_opened_display(dpy, pcDisplay, False);

  parse_config();
  query_screens();

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

      case MapNotify:
        update();
        break;

      // Map and Configure events mean the window was changed or is now mapped.
      case ConfigureNotify:
        update();
        break;

      case Expose:
        XCopyArea(dpy, canvas, zone, canvas_gc, e.xexpose.x, e.xexpose.y,
                  e.xexpose.width, e.xexpose.height,
                  e.xexpose.x, e.xexpose.y);
        break;

      case MotionNotify:
        mouseinfo.x = e.xmotion.x_root;
        mouseinfo.y = e.xmotion.y_root;
        update();
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
