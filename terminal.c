#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_TABS 10
#define MAX_LINES 1000
#define MAX_LINE_LEN 256
#define TAB_BAR_HEIGHT 32
#define TAB_WIDTH 110
#define ADD_BTN_WIDTH 36
#define MAX_CMD_LEN 16384

typedef struct {
  int id;
  pid_t pid;
  int in_fd;
  int out_fd;
  char title[32];
  char input[1024];
  int input_len;
  char pending[MAX_CMD_LEN]; /* earlier lines of a multiline command */
  int pending_len;
  int hist_pos; /* 0 = editing a fresh line, k = k-th most recent command */
  char saved_input[1024]; /* the half-typed line while browsing history */
  int slot;           /* index into the shared per-tab state (fg_pgid) */
  int closing;        /* Ctrl+D sent EOF: remove the tab once it has exited */
  int remove_pending; /* shell output hit EOF after closing */
  char cwd[PATH_MAX];
  char lines[MAX_LINES][MAX_LINE_LEN];
  int line_count;
} Tab;

static Tab tabs[MAX_TABS];
static int tab_count = 0;
static int active_tab = 0;
static Display *dpy = NULL;
static int scr = 0;
static Window win;
static GC gc;
static XFontStruct *font = NULL;
static int char_width = 8;
static int char_height = 16;
static int font_ascent = 12;

static unsigned long col_bg;
static unsigned long col_tab_bar;
static unsigned long col_tab_active_bg;
static unsigned long col_tab_active_fg;
static unsigned long col_tab_inactive_fg;
static unsigned long col_tab_border;
static unsigned long col_tab_add_fg;
static unsigned long col_prompt_user;
static unsigned long col_prompt_arrow;
static unsigned long col_text;
static unsigned long col_cursor;

static unsigned long get_hex_color(const char *hex) {
  Colormap cmap = DefaultColormap(dpy, scr);
  XColor col;
  if (XParseColor(dpy, cmap, hex, &col) && XAllocColor(dpy, cmap, &col)) {
    return col.pixel;
  }
  return WhitePixel(dpy, scr);
}

static void tab_append_text(Tab *t, const char *txt, int len) {
  for (int i = 0; i < len; i++) {
    char c = txt[i];
    if (c == '\r')
      continue;
    if (c == '\n') {
      if (t->line_count < MAX_LINES - 1) {
        t->line_count++;
        t->lines[t->line_count][0] = '\0';
      } else {
        for (int j = 0; j < MAX_LINES - 1; j++) {
          strcpy(t->lines[j], t->lines[j + 1]);
        }
        t->lines[MAX_LINES - 1][0] = '\0';
      }
    } else {
      if (t->line_count == 0 && t->lines[0][0] == '\0') {
        t->line_count = 1;
        t->lines[0][0] = '\0';
      }
      int cur = t->line_count - 1;
      int clen = strlen(t->lines[cur]);
      if (clen < MAX_LINE_LEN - 2) {
        t->lines[cur][clen] = c;
        t->lines[cur][clen + 1] = '\0';
      }
    }
  }
}

#define CONT_PROMPT "> "
#define MAX_STAGES 32
#define MAX_ARGS 64

/* Reasons why an input buffer is not yet a complete command */
enum {
  INPUT_COMPLETE = 0,
  NEED_SQUOTE,    /* unterminated '...'            */
  NEED_DQUOTE,    /* unterminated "..."            */
  NEED_BACKSLASH, /* line ends with a lone '\'     */
  NEED_PAREN      /* unterminated $( ... )         */
};

#define MAX_HISTORY 10000
#define MAX_HIST_LEN 1024
#define MULTIWATCH_MAX 32
#define MW_SEPARATOR "----------------------------------------------------"

/* Shared by all tabs: heap+mutex (threads) or shm+semaphore (processes) */
typedef struct {
  long total; /* commands ever added; entry n lives in slot n % MAX_HISTORY */
  char history[MAX_HISTORY][MAX_HIST_LEN];
  char clipboard[4096];
  /* each tab's foreground process group (0 = none), used atomically */
  int fg_pgid[MAX_TABS];
} SharedState;

static int use_process_mode = 0;
static SharedState *shared_state = NULL;
static pthread_mutex_t shared_mutex = PTHREAD_MUTEX_INITIALIZER;
static sem_t *shared_sem = SEM_FAILED;

/* Per-tab state: thread-local so tabs running as threads stay isolated */
static _Thread_local int last_status = 0; /* $? */
static _Thread_local int in_subshell =
    0; /* inside $(...) or multiWatch: no cwd announce, no new pgroups */
static _Thread_local int shell_exit_requested = 0;
static _Thread_local int shell_exit_code = 0;
static _Thread_local int tab_in_fd = -1; /* the tab's keyboard line pipe */
static _Thread_local int tab_out_fd =
    -1; /* where diagnostics and the prompt text go */
static _Thread_local int cur_stdout_fd = -1; /* where command output goes */
static _Thread_local char tab_cwd[PATH_MAX]; /* cwd of this tab only */

static void write_all(int fd, const char *s, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return;
    }
    s += w;
    n -= (size_t)w;
  }
}

static void sh_err(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  write_all(tab_out_fd, buf, strlen(buf));
}

static void sh_out(const char *fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  write_all(cur_stdout_fd, buf, strlen(buf));
}

static int shared_init(void) {
  if (!use_process_mode) {
    shared_state = calloc(1, sizeof(SharedState));
    return shared_state ? 0 : -1;
  }
  char shm_name[64], sem_name[64];
  snprintf(shm_name, sizeof(shm_name), "/trm_shm_%d", (int)getpid());
  snprintf(sem_name, sizeof(sem_name), "/trm_sem_%d", (int)getpid());

  int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return -1;
  if (ftruncate(fd, sizeof(SharedState)) < 0) {
    close(fd);
    shm_unlink(shm_name);
    return -1;
  }
  shm_unlink(shm_name); /* the mapping stays; no name is left behind */
  void *m = mmap(NULL, sizeof(SharedState), PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd, 0);
  close(fd);
  if (m == MAP_FAILED)
    return -1;
  shared_state = m;

  shared_sem = sem_open(sem_name, O_CREAT | O_EXCL, 0600, 1);
  if (shared_sem == SEM_FAILED)
    return -1;
  sem_unlink(sem_name);
  return 0;
}

static void shared_lock(void) {
  if (use_process_mode) {
    while (sem_wait(shared_sem) < 0 && errno == EINTR) {
    }
  } else {
    pthread_mutex_lock(&shared_mutex);
  }
}

static void shared_unlock(void) {
  if (use_process_mode) {
    sem_post(shared_sem);
  } else {
    pthread_mutex_unlock(&shared_mutex);
  }
}

/* History file: loaded at startup, appended to on every command */
static void history_add(const char *cmd) {
  const char *p = cmd;
  while (*p == ' ' || *p == '\t' || *p == '\n')
    p++;
  if (!*p || !shared_state)
    return;
  shared_lock();
  char *slot = shared_state->history[shared_state->total % MAX_HISTORY];
  size_t n = 0;
  for (; cmd[n] && n < MAX_HIST_LEN - 1; n++)
    slot[n] = (cmd[n] == '\n') ? ' ' : cmd[n];
  slot[n] = '\0';
  shared_state->total++;
  shared_unlock();
}

static long history_total(void) {
  shared_lock();
  long total = shared_state->total;
  shared_unlock();
  return total;
}

/* n is an absolute, zero-based command number. Returns 0 if it is gone. */
static int history_get_abs(long n, char *out, size_t cap) {
  int ok = 0;
  shared_lock();
  long total = shared_state->total;
  long first = total > MAX_HISTORY ? total - MAX_HISTORY : 0;
  if (n >= first && n < total) {
    snprintf(out, cap, "%s", shared_state->history[n % MAX_HISTORY]);
    ok = 1;
  }
  shared_unlock();
  return ok;
}

/* back = 1 is the most recent command. Returns 0 if there is no such entry. */
static int history_get_back(long back, char *out, size_t cap) {
  if (back < 1)
    return 0;
  return history_get_abs(history_total() - back, out, cap);
}

#define MAX_HISTORY_DISPLAY 1000

static void print_history(int fd) {
  long total = history_total();
  long first = total > MAX_HISTORY_DISPLAY ? total - MAX_HISTORY_DISPLAY : 0;
  char entry[MAX_HIST_LEN];
  char line[MAX_HIST_LEN + 32];
  for (long n = first; n < total; n++) {
    if (!history_get_abs(n, entry, sizeof(entry)))
      continue;
    int l = snprintf(line, sizeof(line), "%5ld  %s\n", n + 1, entry);
    write_all(fd, line, (size_t)l);
  }
}

/* ---- per-tab working directory ---- */
static const char *resolve_path(const char *p, char *buf, size_t cap) {
  if (p[0] == '/')
    return p;
  snprintf(buf, cap, "%s/%s", tab_cwd, p);
  return buf;
}

static int do_cd(const char *dest) {
  char joined[PATH_MAX * 2], resolved[PATH_MAX];
  struct stat st;
  const char *path = resolve_path(dest, joined, sizeof(joined));
  if (!realpath(path, resolved)) {
    sh_err("terminator: cd: %s: %s\n", dest, strerror(errno));
    return 1;
  }
  if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
    sh_err("terminator: cd: %s: Not a directory\n", dest);
    return 1;
  }
  if (access(resolved, X_OK) != 0) {
    sh_err("terminator: cd: %s: %s\n", dest, strerror(errno));
    return 1;
  }
  snprintf(tab_cwd, sizeof(tab_cwd), "%s", resolved);
  return 0;
}

/* Child setup: tab cwd and fds, default signals so Ctrl+C / Ctrl+Z work */
static void child_setup(void) {
  signal(SIGPIPE, SIG_DFL);
  signal(SIGINT, SIG_DFL);
  signal(SIGTSTP, SIG_DFL);
  int rc = chdir(tab_cwd);
  (void)rc;
  dup2(tab_in_fd, STDIN_FILENO);
  dup2(cur_stdout_fd, STDOUT_FILENO);
  dup2(tab_out_fd, STDERR_FILENO);
}

/* Job control: the GUI signals the tab's foreground process group */

#define MAX_JOBS 64
#define JOB_CMD_LEN 256

typedef enum { JOB_FOREGROUND, JOB_BACKGROUND, JOB_STOPPED } JobState;

typedef struct {
  int used;
  int id;         /* the n in %n */
  pid_t pgid;     /* the job's process group */
  pid_t last_pid; /* last pipeline stage: its status becomes $? */
  int live;       /* processes not yet reaped */
  int stopped;    /* of those, how many are currently stopped */
  int status;     /* raw wait status of last_pid once it is known */
  int have_status;
  JobState state;
  char cmd[JOB_CMD_LEN];
} Job;

static _Thread_local Job jobs[MAX_JOBS];
static _Thread_local int tab_slot = -1; /* index into SharedState.fg_pgid */

/* Foreground group for headless mode (--shell), which has no GUI */
static volatile sig_atomic_t headless_fg_pgid = 0;

static void forward_to_foreground(int sig) {
  pid_t g = headless_fg_pgid;
  if (g > 1)
    kill(-g, sig);
}

/* Publish this tab's foreground job (0 = none) for the GUI to find. */
static void set_foreground(pid_t pgid) {
  if (tab_slot >= 0 && shared_state)
    __atomic_store_n(&shared_state->fg_pgid[tab_slot], (int)pgid,
                     __ATOMIC_SEQ_CST);
  else
    headless_fg_pgid = pgid;
}

static int jobs_full(void) {
  for (int i = 0; i < MAX_JOBS; i++)
    if (!jobs[i].used)
      return 0;
  return 1;
}

static Job *job_add(pid_t pgid, pid_t last_pid, int nprocs, const char *cmd) {
  int max_id = 0;
  Job *slot = NULL;
  for (int i = 0; i < MAX_JOBS; i++) {
    if (jobs[i].used) {
      if (jobs[i].id > max_id)
        max_id = jobs[i].id;
    } else if (!slot) {
      slot = &jobs[i];
    }
  }
  if (!slot)
    return NULL;
  memset(slot, 0, sizeof(*slot));
  slot->used = 1;
  slot->id = max_id + 1;
  slot->pgid = pgid;
  slot->last_pid = last_pid;
  slot->live = nprocs;
  snprintf(slot->cmd, sizeof(slot->cmd), "%s", cmd);
  return slot;
}

/* Job text: the command on one line, without a trailing '&' */
static void make_job_text(const char *line, int bg, char *out, size_t cap) {
  while (*line == ' ' || *line == '\t' || *line == '\n')
    line++;
  size_t n = 0;
  for (; *line && n < cap - 1; line++)
    out[n++] = (*line == '\n') ? ' ' : *line;
  out[n] = '\0';
  while (n > 0 && isspace((unsigned char)out[n - 1]))
    out[--n] = '\0';
  if (bg && n > 0 && out[n - 1] == '&')
    out[--n] = '\0';
  while (n > 0 && isspace((unsigned char)out[n - 1]))
    out[--n] = '\0';
}

/* Make j the foreground job and wait until it exits or stops; sets $? */
static void wait_foreground(Job *j, int resume) {
  int st;
  j->state = JOB_FOREGROUND;
  j->stopped = 0;
  last_status = 1;
  set_foreground(j->pgid); /* from now on Ctrl+C / Ctrl+Z reach this group */
  if (resume)
    kill(-j->pgid, SIGCONT);

  while (j->live > 0 && j->stopped < j->live) {
    pid_t r = waitpid(-j->pgid, &st, WUNTRACED);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      j->live = 0; /* ECHILD: nothing of this job is left */
      break;
    }
    if (WIFSTOPPED(st)) {
      j->stopped++;
      continue;
    }
    j->live--;
    if (r == j->last_pid) {
      if (WIFEXITED(st))
        last_status = WEXITSTATUS(st);
      else if (WIFSIGNALED(st))
        last_status = 128 + WTERMSIG(st);
    }
  }
  set_foreground(0);

  if (j->live == 0) {
    j->used = 0;
  } else {
    j->state = JOB_STOPPED;
    last_status = 128 + SIGTSTP;
    sh_out("[%d]+ Stopped %s\n", j->id, j->cmd);
  }
}

/* Before each prompt: report background jobs that finished or stopped */
static void reap_jobs(void) {
  for (int i = 0; i < MAX_JOBS; i++) {
    Job *j = &jobs[i];
    if (!j->used || j->state != JOB_BACKGROUND)
      continue;
    for (;;) {
      int st;
      if (j->live <= 0)
        break;
      pid_t r = waitpid(-j->pgid, &st, WNOHANG | WUNTRACED);
      if (r == 0)
        break;
      if (r < 0) {
        if (errno == EINTR)
          continue;
        j->live = 0;
        break;
      }
      if (WIFSTOPPED(st)) {
        if (++j->stopped >= j->live) {
          j->state = JOB_STOPPED;
          sh_out("[%d]+ Stopped %s\n", j->id, j->cmd);
          break;
        }
        continue;
      }
      j->live--;
      if (r == j->last_pid) {
        j->status = st;
        j->have_status = 1;
      }
    }
    if (j->live <= 0) {
      if (j->have_status && WIFSIGNALED(j->status))
        sh_out("[%d]+ Terminated %s\n", j->id, j->cmd);
      else if (j->have_status && WIFEXITED(j->status) &&
               WEXITSTATUS(j->status) != 0)
        sh_out("[%d]+ Exit %d %s\n", j->id, WEXITSTATUS(j->status), j->cmd);
      else
        sh_out("[%d]+ Done %s\n", j->id, j->cmd);
      j->used = 0;
    }
  }
}

/* jobs: `[id]  State  command`; '+' is the current job, '-' the previous */
static void print_jobs(int fd) {
  int cur = 0, prev = 0;
  for (int i = 0; i < MAX_JOBS; i++) {
    if (!jobs[i].used)
      continue;
    if (jobs[i].id > cur) {
      prev = cur;
      cur = jobs[i].id;
    } else if (jobs[i].id > prev) {
      prev = jobs[i].id;
    }
  }
  int last = 0;
  for (;;) {
    Job *n = NULL;
    for (int i = 0; i < MAX_JOBS; i++)
      if (jobs[i].used && jobs[i].id > last && (!n || jobs[i].id < n->id))
        n = &jobs[i];
    if (!n)
      break;
    last = n->id;
    char mark = n->id == cur ? '+' : (n->id == prev ? '-' : ' ');
    char line[JOB_CMD_LEN + 64];
    int l = snprintf(line, sizeof(line), "[%d]%c  %-8s  %s%s\n", n->id, mark,
                     n->state == JOB_STOPPED ? "Stopped" : "Running", n->cmd,
                     n->state == JOB_BACKGROUND ? " &" : "");
    if (l > (int)sizeof(line) - 1)
      l = (int)sizeof(line) - 1;
    write_all(fd, line, (size_t)l);
  }
}

/* Resolve `%n`, `n`, or (no argument) the most recent job. */
static Job *job_lookup(const char *who, int argc, char **argv) {
  Job *found = NULL;
  if (argc < 2) {
    for (int i = 0; i < MAX_JOBS; i++)
      if (jobs[i].used && (!found || jobs[i].id > found->id))
        found = &jobs[i];
    if (!found)
      sh_err("terminator: %s: no current job\n", who);
    return found;
  }
  const char *a = argv[1];
  if (*a == '%')
    a++;
  char *end = NULL;
  long id = strtol(a, &end, 10);
  if (*a == '\0' || *end != '\0') {
    sh_err("terminator: %s: %s: invalid job specification\n", who, argv[1]);
    return NULL;
  }
  for (int i = 0; i < MAX_JOBS; i++)
    if (jobs[i].used && jobs[i].id == id)
      return &jobs[i];
  sh_err("terminator: %s: %%%ld: no such job\n", who, id);
  return NULL;
}

/* fg %n: Stopped or Background -> Foreground (SIGCONT), then wait for it. */
static int builtin_fg(int argc, char **argv) {
  Job *j = job_lookup("fg", argc, argv);
  if (!j)
    return 1;
  sh_out("%s\n", j->cmd);
  wait_foreground(j, 1);
  return last_status;
}

/* bg %n: Stopped -> Background (SIGCONT); the shell goes straight on. */
static int builtin_bg(int argc, char **argv) {
  Job *j = job_lookup("bg", argc, argv);
  if (!j)
    return 1;
  if (j->state == JOB_BACKGROUND) {
    sh_err("terminator: bg: job %d already in background\n", j->id);
    return 0;
  }
  j->state = JOB_BACKGROUND;
  j->stopped = 0;
  kill(-j->pgid, SIGCONT);
  sh_out("[%d]+ %s &\n", j->id, j->cmd);
  return 0;
}

/* The tab is closing: hang up whatever it still has running or stopped. */
static void jobs_hangup(void) {
  for (int i = 0; i < MAX_JOBS; i++) {
    if (!jobs[i].used)
      continue;
    kill(-jobs[i].pgid, SIGHUP);
    kill(-jobs[i].pgid, SIGCONT); /* a stopped job must wake up to see it */
    jobs[i].used = 0;
  }
}

static int multi_watch(const char *args);

static int scan_unit(const char *s, size_t *i, int in_paren);

/* Scan the inside of a "..." string; *i is just after the opening quote. */
static int scan_dquote(const char *s, size_t *i) {
  while (s[*i]) {
    char c = s[*i];
    if (c == '\\') {
      if (!s[*i + 1])
        return NEED_BACKSLASH;
      *i += 2;
    } else if (c == '"') {
      (*i)++;
      return INPUT_COMPLETE;
    } else if (c == '$' && s[*i + 1] == '(') {
      int r;
      *i += 2;
      if ((r = scan_unit(s, i, 1)) != INPUT_COMPLETE)
        return r;
    } else {
      (*i)++;
    }
  }
  return NEED_DQUOTE;
}

/* Scan unquoted text; with in_paren set, stop after the matching ')' */
static int scan_unit(const char *s, size_t *i, int in_paren) {
  while (s[*i]) {
    char c = s[*i];
    int r;
    if (c == '\\') {
      if (!s[*i + 1])
        return NEED_BACKSLASH;
      *i += 2;
    } else if (c == '\'') {
      const char *end = strchr(s + *i + 1, '\'');
      if (!end)
        return NEED_SQUOTE;
      *i = (size_t)(end - s) + 1;
    } else if (c == '"') {
      (*i)++;
      if ((r = scan_dquote(s, i)) != INPUT_COMPLETE)
        return r;
    } else if (c == '$' && s[*i + 1] == '(') {
      *i += 2;
      if ((r = scan_unit(s, i, 1)) != INPUT_COMPLETE)
        return r;
    } else if (in_paren && c == ')') {
      (*i)++;
      return INPUT_COMPLETE;
    } else {
      (*i)++;
    }
  }
  return in_paren ? NEED_PAREN : INPUT_COMPLETE;
}

static int input_status(const char *buf) {
  size_t i = 0;
  return scan_unit(buf, &i, 0);
}

/* ---- growable string used while building words ---- */
typedef struct {
  char *data;
  size_t len, cap;
} Str;

static void str_putc(Str *s, char c) {
  if (s->len + 2 > s->cap) {
    s->cap = s->cap ? s->cap * 2 : 64;
    s->data = realloc(s->data, s->cap);
    if (!s->data)
      _exit(1);
  }
  s->data[s->len++] = c;
  s->data[s->len] = '\0';
}

static char *empty_str(void) {
  char *e = malloc(1);
  if (!e)
    _exit(1);
  e[0] = '\0';
  return e;
}

static void str_puts(Str *s, const char *t) {
  while (*t)
    str_putc(s, *t++);
}

/* ---- tokens ---- */
#define TOK_WORD 0
#define TOK_PIPE '|'
#define TOK_IN '<'
#define TOK_OUT '>'
#define TOK_APPEND 'A'
#define TOK_BG '&' /* trailing '&': run in the background */

typedef struct {
  int type;
  char *text; /* heap string, NULL for operators */
} Token;

typedef struct {
  Token *items;
  int count, cap;
} TokenList;

static void tok_push(TokenList *tl, int type, char *text) {
  if (tl->count == tl->cap) {
    tl->cap = tl->cap ? tl->cap * 2 : 16;
    tl->items = realloc(tl->items, tl->cap * sizeof(Token));
    if (!tl->items)
      _exit(1);
  }
  tl->items[tl->count].type = type;
  tl->items[tl->count].text = text;
  tl->count++;
}

static void tok_free(TokenList *tl) {
  for (int i = 0; i < tl->count; i++)
    free(tl->items[i].text);
  free(tl->items);
  tl->items = NULL;
  tl->count = tl->cap = 0;
}

static int execute_line(const char *line);

/* $(...): run cmd in a child and return its stdout, newlines trimmed */
static char *command_substitution(const char *cmd) {
  Str out = {0};
  int fds[2];
  if (pipe(fds) < 0) {
    char *e = malloc(1);
    if (e)
      e[0] = '\0';
    return e;
  }
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    char *e = malloc(1);
    if (e)
      e[0] = '\0';
    return e;
  }
  if (pid == 0) {
    close(fds[0]);
    in_subshell = 1;
    cur_stdout_fd = fds[1];
    child_setup();
    close(fds[1]);
    cur_stdout_fd = STDOUT_FILENO;
    execute_line(cmd);
    fflush(NULL);
    _exit(last_status);
  }
  close(fds[1]);
  char buf[512];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0 ||
         (n < 0 && errno == EINTR)) {
    for (ssize_t k = 0; k < n; k++)
      str_putc(&out, buf[k]);
  }
  close(fds[0]);
  waitpid(pid, NULL, 0);
  if (!out.data) {
    out.data = malloc(1);
    if (!out.data)
      _exit(1);
    out.data[0] = '\0';
  }
  while (out.len > 0 && out.data[out.len - 1] == '\n')
    out.data[--out.len] = '\0';
  return out.data;
}

/* Append an expansion to w; unquoted values are split into words */
static void emit_expansion(TokenList *tl, Str *w, int *in_word, const char *val,
                           int quoted) {
  if (quoted) {
    str_puts(w, val);
    *in_word = 1;
    return;
  }
  for (const char *p = val; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\n') {
      if (*in_word) {
        tok_push(tl, TOK_WORD, w->data ? w->data : empty_str());
        w->data = NULL;
        w->len = w->cap = 0;
        *in_word = 0;
      }
    } else {
      str_putc(w, *p);
      *in_word = 1;
    }
  }
}

/* Expand the '$' at s[*i] into the current word; a lone '$' is literal */
static void expand_dollar(const char *s, size_t *i, TokenList *tl, Str *w,
                          int *in_word, int quoted) {
  char tmp[32];
  if (s[*i + 1] == '?') {
    snprintf(tmp, sizeof(tmp), "%d", last_status);
    emit_expansion(tl, w, in_word, tmp, quoted);
    *i += 2;
  } else if (s[*i + 1] == '$') {
    snprintf(tmp, sizeof(tmp), "%d", (int)getpid());
    emit_expansion(tl, w, in_word, tmp, quoted);
    *i += 2;
  } else if (s[*i + 1] == '(') {
    /* Find the matching ')', honouring quotes and nesting (innermost first) */
    size_t start = *i + 2;
    size_t j = start;
    scan_unit(s, &j, 1); /* j is just after the matching ')' */
    size_t inner_len = (j > start) ? j - start - 1 : 0;
    char *inner = malloc(inner_len + 1);
    if (!inner)
      _exit(1);
    memcpy(inner, s + start, inner_len);
    inner[inner_len] = '\0';
    char *val = command_substitution(inner);
    free(inner);
    emit_expansion(tl, w, in_word, val ? val : "", quoted);
    free(val);
    *i = j;
  } else if (s[*i + 1] == '{' || isalpha((unsigned char)s[*i + 1]) ||
             s[*i + 1] == '_') {
    char name[128];
    size_t n = 0;
    size_t j = *i + 1;
    int braced = (s[j] == '{');
    if (braced)
      j++;
    while ((isalnum((unsigned char)s[j]) || s[j] == '_') &&
           n < sizeof(name) - 1)
      name[n++] = s[j++];
    name[n] = '\0';
    if (braced) {
      if (s[j] == '}') {
        j++;
      } else { /* "${" without a name/closing brace: literal */
        str_putc(w, '$');
        *in_word = 1;
        (*i)++;
        return;
      }
    }
    const char *val = n ? getenv(name) : NULL;
    emit_expansion(tl, w, in_word, val ? val : "", quoted);
    *i = j;
  } else {
    str_putc(w, '$');
    *in_word = 1;
    (*i)++;
  }
}

/* Split a command into words and operators, honouring quotes */
static void tokenize(const char *s, TokenList *tl) {
  Str w = {0};
  int in_word = 0;
  size_t i = 0;

#define END_WORD()                                                             \
  do {                                                                         \
    if (in_word) {                                                             \
      tok_push(tl, TOK_WORD, w.data ? w.data : empty_str());                   \
      w.data = NULL;                                                           \
      w.len = w.cap = 0;                                                       \
      in_word = 0;                                                             \
    }                                                                          \
  } while (0)

  while (s[i]) {
    char c = s[i];
    if (c == '#' && !in_word) {
      while (s[i] && s[i] != '\n')
        i++;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      END_WORD();
      i++;
    } else if (c == '\\') {
      if (s[i + 1] == '\n') { /* line continuation: vanishes */
        i += 2;
      } else if (s[i + 1]) {
        str_putc(&w, s[i + 1]);
        in_word = 1;
        i += 2;
      } else {
        i++;
      }
    } else if (c == '\'') {
      i++;
      in_word = 1;
      while (s[i] && s[i] != '\'')
        str_putc(&w, s[i++]);
      if (s[i])
        i++;
    } else if (c == '"') {
      i++;
      in_word = 1;
      while (s[i] && s[i] != '"') {
        if (s[i] == '\\' && (s[i + 1] == '"' || s[i + 1] == '\\' ||
                             s[i + 1] == '$' || s[i + 1] == '\n')) {
          if (s[i + 1] != '\n')
            str_putc(&w, s[i + 1]);
          i += 2;
        } else if (s[i] == '$') {
          expand_dollar(s, &i, tl, &w, &in_word, 1);
        } else {
          str_putc(&w, s[i++]);
        }
      }
      if (s[i])
        i++;
    } else if (c == '$') {
      expand_dollar(s, &i, tl, &w, &in_word, 0);
    } else if (c == '|') {
      END_WORD();
      tok_push(tl, TOK_PIPE, NULL);
      i++;
    } else if (c == '&') {
      END_WORD();
      tok_push(tl, TOK_BG, NULL);
      i++;
    } else if (c == '<') {
      END_WORD();
      tok_push(tl, TOK_IN, NULL);
      i++;
    } else if (c == '>') {
      END_WORD();
      if (s[i + 1] == '>') {
        tok_push(tl, TOK_APPEND, NULL);
        i += 2;
      } else {
        tok_push(tl, TOK_OUT, NULL);
        i++;
      }
    } else {
      str_putc(&w, c);
      in_word = 1;
      i++;
    }
  }
  END_WORD();
#undef END_WORD
  free(w.data);
}

static const char *op_name(int type) {
  switch (type) {
  case TOK_PIPE:
    return "|";
  case TOK_IN:
    return "<";
  case TOK_OUT:
    return ">";
  case TOK_APPEND:
    return ">>";
  case TOK_BG:
    return "&";
  default:
    return "newline";
  }
}

static int execute_line(const char *line) {
  /* multiWatch has its own list syntax, so handle it before tokenizing */
  const char *mw = line;
  while (*mw == ' ' || *mw == '\t')
    mw++;
  if (strncmp(mw, "multiWatch", 10) == 0 &&
      (mw[10] == '\0' || mw[10] == ' ' || mw[10] == '\t' || mw[10] == '[')) {
    last_status = multi_watch(mw + 10);
    return last_status;
  }

  TokenList tl = {0};
  tokenize(line, &tl);

  char *stage_argv[MAX_STAGES][MAX_ARGS + 1];
  int stage_argc[MAX_STAGES] = {0};
  const char *infile[MAX_STAGES] = {NULL};
  const char *outfile[MAX_STAGES] = {NULL};
  int append_mode[MAX_STAGES] = {0};
  int num_cmds = 1;
  int parse_err = 0;
  int background = 0; /* command ended with '&' */

  /* Build: group tokens into pipeline stages with redirections */
  for (int t = 0; t < tl.count && !parse_err; t++) {
    Token *tk = &tl.items[t];
    int s = num_cmds - 1;
    if (tk->type == TOK_WORD) {
      if (stage_argc[s] >= MAX_ARGS) {
        sh_err("terminator: too many arguments\n");
        parse_err = 1;
      } else {
        stage_argv[s][stage_argc[s]++] = tk->text;
      }
    } else if (tk->type == TOK_PIPE) {
      if (stage_argc[s] == 0 || num_cmds >= MAX_STAGES) {
        sh_err("terminator: syntax error near unexpected token '|'\n");
        parse_err = 1;
      } else {
        num_cmds++;
      }
    } else if (tk->type == TOK_BG) {
      /* '&' is only valid as the very last token */
      if (t != tl.count - 1) {
        sh_err("terminator: syntax error near unexpected token '&'\n");
        parse_err = 1;
      } else {
        background = 1;
      }
    } else {
      /* <, > or >>: next token must be a word naming the file */
      if (t + 1 >= tl.count || tl.items[t + 1].type != TOK_WORD) {
        sh_err("terminator: syntax error near unexpected token '%s'\n",
               t + 1 < tl.count ? op_name(tl.items[t + 1].type) : "newline");
        parse_err = 1;
      } else {
        const char *target = tl.items[++t].text;
        if (tk->type == TOK_IN) {
          infile[s] = target;
        } else {
          outfile[s] = target;
          append_mode[s] = (tk->type == TOK_APPEND);
        }
      }
    }
  }
  if (!parse_err && num_cmds > 1 && stage_argc[num_cmds - 1] == 0) {
    sh_err("terminator: syntax error near unexpected token '|'\n");
    parse_err = 1;
  }
  if (!parse_err && background && stage_argc[num_cmds - 1] == 0) {
    sh_err("terminator: syntax error near unexpected token '&'\n");
    parse_err = 1;
  }
  for (int s = 0; s < num_cmds; s++)
    stage_argv[s][stage_argc[s]] = NULL;

  if (parse_err) {
    last_status = 2;
    fflush(stderr);
    tok_free(&tl);
    return last_status;
  }

  /* redirection with no command ("> file") just creates the file */
  if (stage_argc[0] == 0) {
    int rc = 0;
    char pbuf[PATH_MAX * 2];
    if (tl.count == 0) { /* blank line: status unchanged */
      tok_free(&tl);
      return last_status;
    }
    if (infile[0]) {
      int ifd = open(resolve_path(infile[0], pbuf, sizeof(pbuf)), O_RDONLY);
      if (ifd < 0) {
        sh_err("terminator: %s: %s\n", infile[0], strerror(errno));
        rc = 1;
      } else {
        close(ifd);
      }
    }
    if (rc == 0 && outfile[0]) {
      int flags = O_WRONLY | O_CREAT | (append_mode[0] ? O_APPEND : O_TRUNC);
      int ofd = open(resolve_path(outfile[0], pbuf, sizeof(pbuf)), flags, 0644);
      if (ofd < 0) {
        sh_err("terminator: %s: %s\n", outfile[0], strerror(errno));
        rc = 1;
      } else {
        close(ofd);
      }
    }
    last_status = rc;
    fflush(stderr);
    tok_free(&tl);
    return last_status;
  }

  /* Single built-in commands (cd, exit, source, VAR=val, [, bash) when no
   * pipeline */
  if (num_cmds == 1 && !infile[0] && !outfile[0] && !background) {
    char **argv = stage_argv[0];
    int argc = stage_argc[0];
    if (argc == 0) {
      tok_free(&tl);
      return last_status;
    }
    if (strcmp(argv[0], "cd") == 0) {
      const char *dest = (argc > 1) ? argv[1] : getenv("HOME");
      char path_buf[1024];
      if (dest && strcmp(dest, "~") == 0) {
        dest = getenv("HOME");
      } else if (dest && strncmp(dest, "~/", 2) == 0) {
        const char *home = getenv("HOME");
        if (home) {
          snprintf(path_buf, sizeof(path_buf), "%s%s", home, dest + 1);
          dest = path_buf;
        }
      }
      if (!dest)
        dest = "/";

      last_status = do_cd(dest);
      if (last_status == 0 && !in_subshell)
        sh_out("[cwd: %s]\n", tab_cwd);
      tok_free(&tl);
      return last_status;
    }

    if (strcmp(argv[0], "exit") == 0) {
      int exit_code = (argc > 1) ? atoi(argv[1]) : last_status;
      /* the tab may be a thread: only stop this tab's shell loop */
      last_status = exit_code;
      shell_exit_code = exit_code;
      shell_exit_requested = 1;
      tok_free(&tl);
      return last_status;
    }

    /* job builtins change the shell's job table, so run them here */
    if (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0) {
      last_status =
          (argv[0][0] == 'f') ? builtin_fg(argc, argv) : builtin_bg(argc, argv);
      tok_free(&tl);
      return last_status;
    }
  }

  /* A job to start needs a free slot in this tab's job table */
  if (!in_subshell && jobs_full()) {
    sh_err("terminator: too many jobs\n");
    last_status = 1;
    tok_free(&tl);
    return last_status;
  }
  int bg = background && !in_subshell; /* '&' means nothing inside $(...) */

  /* Run: pipeline or external command with redirection */
  int pipes[MAX_STAGES][2];
  for (int i = 0; i < num_cmds - 1; i++) {
    if (pipe(pipes[i]) < 0) {
      sh_err("terminator: pipe: %s\n", strerror(errno));
      for (int j = 0; j < i; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
      }
      last_status = 1;
      tok_free(&tl);
      return last_status;
    }
  }

  pid_t cpid[MAX_STAGES];
  pid_t pgid = 0;
  int started = 0;

  fflush(NULL); /* don't let children inherit unflushed stdio buffers */
  for (int s = 0; s < num_cmds; s++) {
    cpid[s] = fork();
    if (cpid[s] < 0) {
      sh_err("terminator: fork: %s\n", strerror(errno));
      break;
    }

    if (cpid[s] == 0) {
      child_setup();

      /* own process group, except inside multiWatch or $(...) */
      if (!in_subshell) {
        if (s == 0) {
          setpgid(0, 0);
        } else {
          setpgid(0, pgid);
        }
      }

      /* Connect pipeline stdin/stdout */
      if (s > 0) {
        dup2(pipes[s - 1][0], STDIN_FILENO);
      }
      if (s < num_cmds - 1) {
        dup2(pipes[s][1], STDOUT_FILENO);
      }

      /* Close all pipeline pipe descriptors in child */
      for (int j = 0; j < num_cmds - 1; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
      }

      /* background jobs get no keyboard input; '<' overrides */
      if (bg && s == 0) {
        int nfd = open("/dev/null", O_RDONLY);
        if (nfd >= 0) {
          dup2(nfd, STDIN_FILENO);
          close(nfd);
        }
      }

      /* File input redirection (<) */
      if (infile[s]) {
        int ifd = open(infile[s], O_RDONLY);
        if (ifd < 0) {
          sh_err("terminator: %s: %s\n", infile[s], strerror(errno));
          _exit(1);
        }
        dup2(ifd, STDIN_FILENO);
        close(ifd);
      }

      /* File output redirection (> or >>) */
      if (outfile[s]) {
        int flags = O_WRONLY | O_CREAT | (append_mode[s] ? O_APPEND : O_TRUNC);
        int ofd = open(outfile[s], flags, 0644);
        if (ofd < 0) {
          sh_err("terminator: %s: %s\n", outfile[s], strerror(errno));
          _exit(1);
        }
        dup2(ofd, STDOUT_FILENO);
        close(ofd);
      }

      /* Handle built-ins inside pipeline if piped */
      if (strcmp(stage_argv[s][0], "exit") == 0) {
        _exit(0);
      }
      if (strcmp(stage_argv[s][0], "cd") == 0) {
        _exit(0);
      }
      /* history runs in the child so it can be piped and redirected */
      if (strcmp(stage_argv[s][0], "history") == 0) {
        print_history(STDOUT_FILENO);
        _exit(0);
      }
      /* jobs, like history, runs in the child so it can be piped */
      if (strcmp(stage_argv[s][0], "jobs") == 0) {
        print_jobs(STDOUT_FILENO);
        _exit(0);
      }
      /* fg / bg are no-ops inside a pipeline, as in bash */
      if (strcmp(stage_argv[s][0], "fg") == 0 ||
          strcmp(stage_argv[s][0], "bg") == 0) {
        _exit(0);
      }

      execvp(stage_argv[s][0], stage_argv[s]);
      sh_err("terminator: %s: command not found\n", stage_argv[s][0]);
      _exit(127);
    }

    /* Parent synchronizes process group */
    if (s == 0) {
      pgid = cpid[0];
    }
    if (!in_subshell)
      setpgid(cpid[s], pgid);
    started++;
  }

  /* Parent closes all pipeline ends so readers receive EOF */
  for (int j = 0; j < num_cmds - 1; j++) {
    close(pipes[j][0]);
    close(pipes[j][1]);
  }

  /* top level: the pipeline is a job; wait unless it runs in background */
  Job *job = NULL;
  if (!in_subshell && started > 0) {
    char text[JOB_CMD_LEN];
    make_job_text(line, bg, text, sizeof(text));
    job = job_add(pgid, started == num_cmds ? cpid[num_cmds - 1] : -1, started,
                  text);
  }
  if (job) {
    if (bg) {
      job->state = JOB_BACKGROUND;
      sh_out("[%d] %d\n", job->id, (int)cpid[started - 1]);
      last_status = 0;
    } else {
      wait_foreground(job, 0);
    }
    fflush(stdout);
    fflush(stderr);
    tok_free(&tl);
    return last_status;
  }

  /* subshell: wait for every child and keep the last command's status */
  last_status = 1;
  for (int s = 0; s < started; s++) {
    int status = 0;
    waitpid(cpid[s], &status, 0);
    if (s == num_cmds - 1) {
      if (WIFEXITED(status)) {
        last_status = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        last_status = 128 + WTERMSIG(status);
      }
    }
  }
  fflush(stdout);
  fflush(stderr);
  tok_free(&tl);
  return last_status;
}

/* ---- multiWatch: many commands, one view ---- */

/* Parse ["cmd1", ...]; *n counts heap strings the caller must free */
static int parse_multiwatch(const char *s, char *cmds[], int *n) {
  *n = 0;
  while (isspace((unsigned char)*s))
    s++;
  if (*s != '[')
    return -1;
  s++;
  for (;;) {
    while (isspace((unsigned char)*s))
      s++;
    if (*s == ']') {
      s++;
      break;
    }
    if (*s != '"' && *s != '\'')
      return -1;
    char q = *s++;
    Str w = {0};
    while (*s && *s != q) {
      if (q == '"' && *s == '\\' && (s[1] == '"' || s[1] == '\\'))
        s++;
      str_putc(&w, *s++);
    }
    if (*s != q || *n >= MULTIWATCH_MAX) {
      free(w.data);
      return -1;
    }
    s++;
    cmds[(*n)++] = w.data ? w.data : empty_str();
    while (isspace((unsigned char)*s))
      s++;
    if (*s == ',') {
      s++;
      continue;
    }
    if (*s == ']') {
      s++;
      break;
    }
    return -1;
  }
  while (isspace((unsigned char)*s))
    s++;
  return (*s == '\0' && *n > 0) ? 0 : -1;
}

static void multiwatch_print(const char *cmd, const char *data, ssize_t len) {
  char head[2 * MAX_HIST_LEN + 128];
  int h = snprintf(head, sizeof(head), "\"%s\", %ld:\n%s\n", cmd,
                   (long)time(NULL), MW_SEPARATOR);
  if (h > (int)sizeof(head) - 1)
    h = (int)sizeof(head) - 1;
  write_all(cur_stdout_fd, head, (size_t)h);
  write_all(cur_stdout_fd, data, (size_t)len);
  if (len > 0 && data[len - 1] != '\n')
    write_all(cur_stdout_fd, "\n", 1);
  write_all(cur_stdout_fd, MW_SEPARATOR "\n", strlen(MW_SEPARATOR) + 1);
}

/* One pipe+fork per command, polled together; 0x03 on input kills all */
static int multi_watch(const char *args) {
  char *cmds[MULTIWATCH_MAX];
  int n = 0;
  if (parse_multiwatch(args, cmds, &n) != 0) {
    for (int i = 0; i < n; i++)
      free(cmds[i]);
    sh_err("terminator: usage: multiWatch [\"cmd1\", \"cmd2\", ...]\n");
    return 2;
  }

  int fds[MULTIWATCH_MAX];
  pid_t pids[MULTIWATCH_MAX];
  int open_count = 0;

  fflush(NULL);
  for (int i = 0; i < n; i++) {
    int pp[2];
    fds[i] = -1;
    pids[i] = -1;
    if (pipe(pp) < 0) {
      sh_err("terminator: pipe: %s\n", strerror(errno));
      continue;
    }
    pid_t pid = fork();
    if (pid < 0) {
      sh_err("terminator: fork: %s\n", strerror(errno));
      close(pp[0]);
      close(pp[1]);
      continue;
    }
    if (pid == 0) {
      setpgid(0, 0);
      close(pp[0]);
      for (int j = 0; j < i; j++) {
        if (fds[j] >= 0)
          close(fds[j]);
      }
      in_subshell = 1;
      cur_stdout_fd = pp[1];
      tab_out_fd = pp[1];
      child_setup();
      close(pp[1]);
      cur_stdout_fd = STDOUT_FILENO;
      tab_out_fd = STDOUT_FILENO;
      execute_line(cmds[i]);
      _exit(last_status);
    }
    close(pp[1]);
    setpgid(pid, pid);
    fds[i] = pp[0];
    pids[i] = pid;
    open_count++;
  }

  int cancelled = 0;
  int input_open = 1; /* stop watching the tab input once it hits EOF */
  while (open_count > 0 && !cancelled) {
    struct pollfd pf[MULTIWATCH_MAX + 1];
    int idx[MULTIWATCH_MAX];
    int np = 0;
    for (int i = 0; i < n; i++) {
      if (fds[i] >= 0) {
        pf[np].fd = fds[i];
        pf[np].events = POLLIN;
        pf[np].revents = 0;
        idx[np++] = i;
      }
    }
    pf[np].fd = input_open ? tab_in_fd : -1;
    pf[np].events = POLLIN;
    pf[np].revents = 0;

    if (poll(pf, (nfds_t)(np + 1), -1) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    for (int k = 0; k < np; k++) {
      if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR)))
        continue;
      int i = idx[k];
      char buf[4096];
      ssize_t got = read(fds[i], buf, sizeof(buf));
      if (got > 0) {
        multiwatch_print(cmds[i], buf, got);
      } else if (got == 0 || (errno != EINTR && errno != EAGAIN)) {
        close(fds[i]);
        fds[i] = -1;
        open_count--;
      }
    }
    if (pf[np].revents & (POLLIN | POLLHUP | POLLERR)) {
      char cb[64];
      ssize_t got = read(tab_in_fd, cb, sizeof(cb));
      if (got <= 0) {
        input_open = 0;
      } else {
        for (ssize_t k = 0; k < got; k++) {
          if (cb[k] == 3)
            cancelled = 1;
        }
      }
    }
  }

  for (int i = 0; i < n; i++) {
    if (fds[i] >= 0)
      close(fds[i]);
    if (cancelled && pids[i] > 0) {
      if (kill(-pids[i], SIGINT) < 0)
        kill(pids[i], SIGINT);
    }
  }
  for (int i = 0; i < n; i++) {
    if (pids[i] > 0)
      waitpid(pids[i], NULL, 0);
    free(cmds[i]);
  }
  return cancelled ? 130 : 0;
}

/* ---- reading a tab's input ---- */
typedef struct {
  char buf[4096];
  size_t pos, len;
} LineReader;

/* Read one line from fd (-1 at EOF); a 0x03 byte drops the line */
static int read_line(LineReader *r, int fd, char *line, size_t cap,
                     int *interrupted) {
  size_t n = 0;
  *interrupted = 0;
  for (;;) {
    if (r->pos == r->len) {
      ssize_t got = read(fd, r->buf, sizeof(r->buf));
      if (got < 0 && errno == EINTR)
        continue;
      if (got <= 0) {
        if (n > 0) {
          line[n] = '\0';
          return (int)n;
        }
        return -1;
      }
      r->pos = 0;
      r->len = (size_t)got;
    }
    char c = r->buf[r->pos++];
    if (c == 3) {
      n = 0;
      *interrupted = 1;
    } else if (c == '\n') {
      line[n] = '\0';
      return (int)n;
    } else if (n < cap - 1) {
      line[n++] = c;
    }
  }
}

/* Read lines until a command is complete, then record and run it */
static void run_shell(void) {
  char line[1024];
  static _Thread_local char cmd[MAX_CMD_LEN];
  size_t cmd_len = 0;
  LineReader rd = {.pos = 0, .len = 0};
  int interrupted = 0;
  int n;

  cmd[0] = '\0';
  while (!shell_exit_requested &&
         (n = read_line(&rd, tab_in_fd, line, sizeof(line), &interrupted)) >=
             0) {
    size_t len = (size_t)n;
    while (len > 0 && line[len - 1] == '\r')
      line[--len] = '\0';
    if (interrupted) {
      cmd_len = 0;
      cmd[0] = '\0';
    }
    /* announce finished background jobs, but not mid multiline command */
    if (cmd_len == 0)
      reap_jobs();
    if (len == 0 && cmd_len == 0)
      continue;

    if (cmd_len + len + 2 > sizeof(cmd)) {
      sh_err("terminator: command too long\n");
      cmd_len = 0;
      cmd[0] = '\0';
      last_status = 1;
      continue;
    }
    if (cmd_len > 0)
      cmd[cmd_len++] = '\n';
    memcpy(cmd + cmd_len, line, len + 1);
    cmd_len += len;

    if (input_status(cmd) != INPUT_COMPLETE)
      continue; /* need more input */

    history_add(cmd);
    execute_line(cmd);
    cmd_len = 0;
    cmd[0] = '\0';
  }

  if (cmd_len > 0 && !shell_exit_requested) {
    sh_err("terminator: syntax error: unexpected end of input\n");
  }
}

/* ---- tabs: one shell per tab, as a thread or as a process ---- */

/* Everything a tab does once it owns its descriptors. */
static void tab_main(int in_fd, int out_fd, int slot) {
  tab_in_fd = in_fd;
  tab_out_fd = out_fd;
  cur_stdout_fd = out_fd;
  tab_slot = slot;
  last_status = 0;
  if (!getcwd(tab_cwd, sizeof(tab_cwd)))
    snprintf(tab_cwd, sizeof(tab_cwd), "/");
  run_shell();
  jobs_hangup();
  set_foreground(0);
  close(in_fd);
  close(out_fd);
}

typedef struct {
  int in_fd;
  int out_fd;
  int slot;
} TabArgs;

static void *tab_thread(void *arg) {
  TabArgs a = *(TabArgs *)arg;
  free(arg);
  tab_main(a.in_fd, a.out_fd, a.slot);
  return NULL;
}

/* The foreground process group of tab idx (0 if it has none) */
static pid_t tab_foreground_pgid(int idx) {
  if (!shared_state || idx < 0 || idx >= tab_count)
    return 0;
  int s = tabs[idx].slot;
  if (s < 0 || s >= MAX_TABS)
    return 0;
  return (pid_t)__atomic_load_n(&shared_state->fg_pgid[s], __ATOMIC_SEQ_CST);
}

/* Signal only tab idx's foreground job group; returns 1 if signalled */
static int signal_tab_job(int idx, int sig) {
  pid_t g = tab_foreground_pgid(idx);
  if (g <= 1 || g == getpgrp())
    return 0;
  return kill(-g, sig) == 0;
}

static void create_tab(void) {
  if (tab_count >= MAX_TABS)
    return;

  int slot = -1;
  for (int s = 0; s < MAX_TABS; s++) {
    int used = 0;
    for (int i = 0; i < tab_count; i++) {
      if (tabs[i].slot == s) {
        used = 1;
        break;
      }
    }
    if (!used) {
      slot = s;
      break;
    }
  }
  if (slot < 0)
    return;

  int in_pipe[2];
  int out_pipe[2];
  if (pipe(in_pipe) < 0)
    return;
  if (pipe(out_pipe) < 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    return;
  }
  /* Programs started by a tab must never inherit the tab plumbing */
  fcntl(in_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
  fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(out_pipe[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = 0;
  if (use_process_mode) {
    /* Process mode: the GUI is the parent, the tab is a forked child */
    pid = fork();
    if (pid < 0) {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      return;
    }
    if (pid == 0) {
      close(in_pipe[1]);
      close(out_pipe[0]);
      tab_main(in_pipe[0], out_pipe[1], slot);
      _exit(shell_exit_requested ? shell_exit_code : 0);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
  } else {
    /* Thread mode: the tab is a thread; it owns the tab-side descriptors */
    TabArgs *a = malloc(sizeof(*a));
    pthread_t th;
    if (!a) {
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      return;
    }
    a->in_fd = in_pipe[0];
    a->out_fd = out_pipe[1];
    a->slot = slot;
    if (pthread_create(&th, NULL, tab_thread, a) != 0) {
      free(a);
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      return;
    }
    pthread_detach(th);
  }

  fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

  Tab *t = &tabs[tab_count];
  t->id = tab_count + 1;
  t->pid = pid;
  t->in_fd = in_pipe[1];
  t->out_fd = out_pipe[0];
  snprintf(t->title, sizeof(t->title), "%d: tab", t->id);
  t->input[0] = '\0';
  t->input_len = 0;
  t->slot = slot;
  t->closing = 0;
  t->remove_pending = 0;
  t->pending[0] = '\0';
  t->pending_len = 0;
  t->hist_pos = 0;
  t->saved_input[0] = '\0';
  if (!getcwd(t->cwd, sizeof(t->cwd)))
    snprintf(t->cwd, sizeof(t->cwd), ".");
  t->line_count = 0;

  char welcome[128];
  if (use_process_mode) {
    snprintf(welcome, sizeof(welcome),
             "Terminator Tab %d (process mode, PID: %d ready)\n", t->id,
             (int)pid);
  } else {
    snprintf(welcome, sizeof(welcome),
             "Terminator Tab %d (thread mode ready)\n", t->id);
  }
  tab_append_text(t, welcome, strlen(welcome));

  active_tab = tab_count;
  tab_count++;
}

static void close_tab(int idx) {
  if (idx < 0 || idx >= tab_count)
    return;
  if (tabs[idx].in_fd >= 0) {
    close(tabs[idx].in_fd);
    tabs[idx].in_fd = -1;
  }
  if (tabs[idx].out_fd >= 0) {
    close(tabs[idx].out_fd);
    tabs[idx].out_fd = -1;
  }
  if (shared_state && tabs[idx].slot >= 0 && tabs[idx].slot < MAX_TABS) {
    __atomic_store_n(&shared_state->fg_pgid[tabs[idx].slot], 0,
                     __ATOMIC_SEQ_CST);
  }
  for (int i = idx; i < tab_count - 1; i++) {
    tabs[i] = tabs[i + 1];
  }
  tab_count--;
  if (active_tab >= tab_count)
    active_tab = tab_count > 0 ? tab_count - 1 : 0;
}

static void redraw(int win_w, int win_h) {
  XClearWindow(dpy, win);

  XSetForeground(dpy, gc, col_tab_bar);
  XFillRectangle(dpy, win, gc, 0, 0, win_w, TAB_BAR_HEIGHT);

  XSetForeground(dpy, gc, col_tab_border);
  XDrawLine(dpy, win, gc, 0, TAB_BAR_HEIGHT, win_w, TAB_BAR_HEIGHT);

  for (int i = 0; i < tab_count; i++) {
    int x = 8 + i * (TAB_WIDTH + 6);
    int y = 4;
    int w = TAB_WIDTH;
    int h = TAB_BAR_HEIGHT - 8;

    if (i == active_tab) {
      XSetForeground(dpy, gc, col_tab_active_bg);
      XFillRectangle(dpy, win, gc, x, y, w, h);
      XSetForeground(dpy, gc, col_tab_active_fg);
      XDrawLine(dpy, win, gc, x, y + h, x + w, y + h);
      XDrawLine(dpy, win, gc, x, y + h - 1, x + w, y + h - 1);
      XSetForeground(dpy, gc, col_tab_active_fg);
    } else {
      XSetForeground(dpy, gc, col_tab_inactive_fg);
    }
    XDrawRectangle(dpy, win, gc, x, y, w, h);
    XDrawString(dpy, win, gc, x + 10, y + font_ascent + 2, tabs[i].title,
                strlen(tabs[i].title));
  }

  int add_x = 8 + tab_count * (TAB_WIDTH + 6);
  int add_y = 4;
  int add_h = TAB_BAR_HEIGHT - 8;
  XSetForeground(dpy, gc, col_tab_border);
  XDrawRectangle(dpy, win, gc, add_x, add_y, ADD_BTN_WIDTH, add_h);
  XSetForeground(dpy, gc, col_tab_add_fg);
  XDrawString(dpy, win, gc, add_x + 13, add_y + font_ascent + 2, "+", 1);

  if (active_tab < 0 || active_tab >= tab_count)
    return;
  Tab *cur = &tabs[active_tab];

  int content_top = TAB_BAR_HEIGHT + 16;
  int max_visible = (win_h - content_top - 36) / char_height;
  if (max_visible < 1)
    max_visible = 1;

  int start_line = 0;
  if (cur->line_count > max_visible) {
    start_line = cur->line_count - max_visible;
  }

  XSetForeground(dpy, gc, col_text);
  int draw_y = content_top;
  for (int i = start_line; i < cur->line_count; i++) {
    XDrawString(dpy, win, gc, 16, draw_y, cur->lines[i], strlen(cur->lines[i]));
    draw_y += char_height;
  }

  /* Continuation lines of a multiline command only show the "> " prompt */
  const char *prompt_user = cur->pending_len > 0 ? "" : "user@terminator";
  const char *prompt_arrow = cur->pending_len > 0 ? CONT_PROMPT : "> ";
  XSetForeground(dpy, gc, col_prompt_user);
  XDrawString(dpy, win, gc, 16, draw_y, prompt_user, strlen(prompt_user));
  int px = 16 + strlen(prompt_user) * char_width;

  XSetForeground(dpy, gc, col_prompt_arrow);
  XDrawString(dpy, win, gc, px, draw_y, prompt_arrow, strlen(prompt_arrow));
  px += strlen(prompt_arrow) * char_width;

  XSetForeground(dpy, gc, col_text);
  XDrawString(dpy, win, gc, px, draw_y, cur->input, cur->input_len);
  int cursor_x = px + cur->input_len * char_width;

  XSetForeground(dpy, gc, col_cursor);
  XFillRectangle(dpy, win, gc, cursor_x, draw_y - font_ascent + 2, char_width,
                 char_height);
}

int main(int argc, char **argv) {
  /* ./terminator = thread mode, ./terminator useProcess=True = process */
  int headless = 0;
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "useProcess=", 11) == 0) {
      const char *v = argv[i] + 11;
      use_process_mode = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
    } else if (strcmp(argv[i], "--shell") == 0) {
      headless = 1;
    }
  }

  if (shared_init() != 0) {
    fprintf(stderr, "terminator: cannot set up shared state: %s\n",
            strerror(errno));
    return 1;
  }


  /* Headless mode: run only the shell engine on stdin/stdout (for testing) */
  if (headless) {
    tab_in_fd = STDIN_FILENO;
    cur_stdout_fd = STDOUT_FILENO;
    tab_out_fd = STDERR_FILENO;
    if (!getcwd(tab_cwd, sizeof(tab_cwd)))
      snprintf(tab_cwd, sizeof(tab_cwd), "/");
    /* no GUI: forward SIGINT / SIGTSTP to the foreground job */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_to_foreground;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    run_shell();
    jobs_hangup();
    return shell_exit_requested ? shell_exit_code : last_status;
  }

  /* A tab that exits must not kill the GUI when it next writes to it */
  signal(SIGPIPE, SIG_IGN);

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "terminator: cannot open X display (is DISPLAY set?)\n");
    return 1;
  }
  fcntl(ConnectionNumber(dpy), F_SETFD, FD_CLOEXEC);

  scr = DefaultScreen(dpy);
  col_bg = get_hex_color("#181825");
  col_tab_bar = get_hex_color("#11111b");
  col_tab_active_bg = get_hex_color("#313244");
  col_tab_active_fg = get_hex_color("#fab387");
  col_tab_inactive_fg = get_hex_color("#6c7086");
  col_tab_border = get_hex_color("#45475a");
  col_tab_add_fg = get_hex_color("#a6e3a1");
  col_prompt_user = get_hex_color("#a6e3a1");
  col_prompt_arrow = get_hex_color("#f38ba8");
  col_text = get_hex_color("#cdd6f4");
  col_cursor = get_hex_color("#f5e0dc");

  font = XLoadQueryFont(dpy, "9x15");
  if (!font)
    font = XLoadQueryFont(dpy, "fixed");
  if (font) {
    char_width = font->max_bounds.width;
    char_height = font->ascent + font->descent + 2;
    font_ascent = font->ascent;
  }

  int win_w = 820;
  int win_h = 520;
  win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, win_w, win_h,
                            1, col_tab_border, col_bg);

  XSelectInput(dpy, win,
               ExposureMask | KeyPressMask | ButtonPressMask |
                   StructureNotifyMask);
  XMapWindow(dpy, win);

  gc = XCreateGC(dpy, win, 0, NULL);
  if (font)
    XSetFont(dpy, gc, font->fid);

  create_tab();

  int running = 1;
  while (running) {
    struct pollfd pfds[MAX_TABS + 1];
    pfds[0].fd = ConnectionNumber(dpy);
    pfds[0].events = POLLIN;

    int pfd_count = 1;
    for (int i = 0; i < tab_count; i++) {
      if (tabs[i].out_fd >= 0) {
        pfds[pfd_count].fd = tabs[i].out_fd;
        pfds[pfd_count].events = POLLIN;
        pfd_count++;
      }
    }

    int poll_timeout = XPending(dpy) ? 0 : 50;
    int poll_res = poll(pfds, pfd_count, poll_timeout);

    if (poll_res > 0) {
      int cur_idx = 1;
      for (int i = 0; i < tab_count; i++) {
        if (tabs[i].out_fd >= 0) {
          if (pfds[cur_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[1024];
            ssize_t n = read(tabs[i].out_fd, buf, sizeof(buf) - 1);
            if (n > 0) {
              buf[n] = '\0';
              tab_append_text(&tabs[i], buf, n);
              if (strstr(buf, "[cwd: ")) {
                char *p = strstr(buf, "[cwd: ") + 6;
                char *end = strchr(p, ']');
                if (end) {
                  *end = '\0';
                  snprintf(tabs[i].cwd, sizeof(tabs[i].cwd), "%s", p);
                  char *slash = strrchr(p, '/');
                  const char *bname = (slash && *(slash + 1)) ? slash + 1 : p;
                  snprintf(tabs[i].title, sizeof(tabs[i].title), "%d: %s",
                           tabs[i].id, bname);
                }
              }
              redraw(win_w, win_h);
            } else if (n == 0) {
              close(tabs[i].out_fd);
              tabs[i].out_fd = -1;
              /* a dead tab has no foreground job */
              if (shared_state && tabs[i].slot >= 0 && tabs[i].slot < MAX_TABS)
                __atomic_store_n(&shared_state->fg_pgid[tabs[i].slot], 0,
                                 __ATOMIC_SEQ_CST);
              if (tabs[i].closing) {
                close_tab(i);
                if (tab_count == 0) {
                  running = 0;
                  break;
                }
              } else {
                const char *closed_msg = "[Process exited]\n";
                tab_append_text(&tabs[i], closed_msg, strlen(closed_msg));
              }
              redraw(win_w, win_h);
            }
          }
          cur_idx++;
        }
      }
    }

    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);

      if (ev.type == ConfigureNotify) {
        win_w = ev.xconfigure.width;
        win_h = ev.xconfigure.height;
        redraw(win_w, win_h);
      } else if (ev.type == Expose) {
        redraw(win_w, win_h);
      } else if (ev.type == ButtonPress) {
        int bx = ev.xbutton.x;
        int by = ev.xbutton.y;
        if (by <= TAB_BAR_HEIGHT) {
          for (int i = 0; i < tab_count; i++) {
            int tx = 8 + i * (TAB_WIDTH + 6);
            if (bx >= tx && bx <= tx + TAB_WIDTH) {
              active_tab = i;
              redraw(win_w, win_h);
              break;
            }
          }
          int add_x = 8 + tab_count * (TAB_WIDTH + 6);
          if (bx >= add_x && bx <= add_x + ADD_BTN_WIDTH) {
            create_tab();
            redraw(win_w, win_h);
          }
        }
      } else if (ev.type == KeyPress) {
        KeySym ks;
        char kbuf[32];
        int len = XLookupString(&ev.xkey, kbuf, sizeof(kbuf) - 1, &ks, NULL);
        Tab *cur = &tabs[active_tab];

        if ((ev.xkey.state & ControlMask) && (ks == XK_t || ks == XK_T)) {
          create_tab();
          redraw(win_w, win_h);
        } else if ((ev.xkey.state & ControlMask) &&
                   (ks == XK_c || ks == XK_C)) {
          if (!signal_tab_job(active_tab, SIGINT)) {
            tab_append_text(cur, "^C\n", 3);
            cur->input_len = 0;
            cur->input[0] = '\0';
            cur->pending_len = 0;
            cur->pending[0] = '\0';
            cur->hist_pos = 0;
            write_all(cur->in_fd, "\003", 1);
            redraw(win_w, win_h);
          }
        } else if ((ev.xkey.state & ControlMask) &&
                   (ks == XK_z || ks == XK_Z)) {
          signal_tab_job(active_tab, SIGTSTP);
        } else if (ks == XK_Up && cur->pending_len == 0) {
          char h[MAX_HIST_LEN];
          if (history_get_back(cur->hist_pos + 1, h, sizeof(h))) {
            if (cur->hist_pos == 0)
              memcpy(cur->saved_input, cur->input, sizeof(cur->saved_input));
            cur->hist_pos++;
            snprintf(cur->input, sizeof(cur->input), "%s", h);
            cur->input_len = (int)strlen(cur->input);
            redraw(win_w, win_h);
          }
        } else if (ks == XK_Down && cur->pending_len == 0 &&
                   cur->hist_pos > 0) {
          char h[MAX_HIST_LEN];
          cur->hist_pos--;
          if (cur->hist_pos == 0) {
            memcpy(cur->input, cur->saved_input, sizeof(cur->input));
          } else if (history_get_back(cur->hist_pos, h, sizeof(h))) {
            snprintf(cur->input, sizeof(cur->input), "%s", h);
          }
          cur->input_len = (int)strlen(cur->input);
          redraw(win_w, win_h);
        } else if (ks == XK_BackSpace) {
          if (cur->input_len > 0) {
            cur->input[--cur->input_len] = '\0';
            redraw(win_w, win_h);
          }
        } else if (ks == XK_Return) {
          char echo_line[1200];
          snprintf(echo_line, sizeof(echo_line), "%s%s\n",
                   cur->pending_len > 0 ? CONT_PROMPT : "user@terminator> ",
                   cur->input);
          tab_append_text(cur, echo_line, strlen(echo_line));
          write(cur->in_fd, cur->input, cur->input_len);
          write(cur->in_fd, "\n", 1);

          if (!(cur->input_len == 0 && cur->pending_len == 0)) {
            if (cur->pending_len + cur->input_len + 2 > MAX_CMD_LEN) {
              cur->pending_len = 0;
            } else {
              if (cur->pending_len > 0)
                cur->pending[cur->pending_len++] = '\n';
              memcpy(cur->pending + cur->pending_len, cur->input,
                     cur->input_len + 1);
              cur->pending_len += cur->input_len;
              if (input_status(cur->pending) == INPUT_COMPLETE)
                cur->pending_len = 0;
            }
            cur->pending[cur->pending_len] = '\0';
          }
          cur->input_len = 0;
          cur->input[0] = '\0';
          cur->hist_pos = 0;
          redraw(win_w, win_h);
        } else if (len > 0 && (unsigned char)kbuf[0] >= 32 &&
                   (unsigned char)kbuf[0] <= 126 &&
                   !(ev.xkey.state & (ControlMask | Mod1Mask))) {
          if (cur->input_len < (int)sizeof(cur->input) - 2) {
            cur->input[cur->input_len++] = kbuf[0];
            cur->input[cur->input_len] = '\0';
            redraw(win_w, win_h);
          }
        }
      }
    }
  }

  if (font)
    XFreeFont(dpy, font);
  XFreeGC(dpy, gc);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return 0;
}
