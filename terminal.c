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
  int input_pos;          /* cursor position inside input */
  int searching;          /* Ctrl+R "Enter search term:" mode */
  char search[128];
  int search_len;
  int slot;           /* index into the shared per-tab state (fg_pgid) */
  int closing;        /* Ctrl+D sent EOF: remove the tab once it has exited */
  int remove_pending; /* shell output hit EOF after closing */
  int completing;     /* 1 = waiting for user to press 1..9 to pick a file */
  char comp_matches[64][256];
  int comp_count;
  int comp_replace_start;
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
static char hist_path[PATH_MAX];

static void history_init_path(void) {
  const char *e = getenv("TERMINATOR_HISTFILE");
  const char *home = getenv("HOME");
  hist_path[0] = '\0';
  if (e && *e)
    snprintf(hist_path, sizeof(hist_path), "%s", e);
  else if (home && *home)
    snprintf(hist_path, sizeof(hist_path), "%s/.terminator_history", home);
}

/* Store cmd in the ring (newlines become spaces); caller holds the lock */
static const char *history_store(const char *cmd) {
  char *slot = shared_state->history[shared_state->total % MAX_HISTORY];
  size_t n = 0;
  for (; cmd[n] && n < MAX_HIST_LEN - 1; n++)
    slot[n] = (cmd[n] == '\n' || cmd[n] == '\r') ? ' ' : cmd[n];
  slot[n] = '\0';
  shared_state->total++;
  return slot;
}

static void history_add(const char *cmd) {
  const char *p = cmd;
  while (*p == ' ' || *p == '\t' || *p == '\n')
    p++;
  if (!*p || !shared_state)
    return;
  shared_lock();
  const char *stored = history_store(cmd);
  if (hist_path[0]) {
    /* one O_APPEND write under the lock, so lines never interleave */
    int fd = open(hist_path, O_WRONLY | O_APPEND | O_CREAT, 0600);
    if (fd >= 0) {
      char buf[MAX_HIST_LEN + 2];
      int l = snprintf(buf, sizeof(buf), "%s\n", stored);
      write_all(fd, buf, (size_t)l);
      close(fd);
    }
  }
  shared_unlock();
}

/* Rewrite the file with only the ring's contents (the last MAX_HISTORY). */
static void history_rewrite(void) {
  char tmp[PATH_MAX + 8];
  snprintf(tmp, sizeof(tmp), "%s.tmp", hist_path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return;
  FILE *f = fdopen(fd, "w");
  if (!f) {
    close(fd);
    return;
  }
  long total = shared_state->total;
  long first = total > MAX_HISTORY ? total - MAX_HISTORY : 0;
  for (long n = first; n < total; n++)
    fprintf(f, "%s\n", shared_state->history[n % MAX_HISTORY]);
  if (fclose(f) != 0 || rename(tmp, hist_path) != 0)
    remove(tmp);
}

/* Load the history file into the ring once at startup, trimming extras */
static void history_load(void) {
  if (!hist_path[0] || !shared_state)
    return;
  FILE *f = fopen(hist_path, "r");
  if (!f)
    return;
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  long lines = 0;
  while ((n = getline(&line, &cap, f)) >= 0) {
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    const char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p)
      continue;
    history_store(line);
    lines++;
  }
  free(line);
  fclose(f);
  if (lines > MAX_HISTORY)
    history_rewrite();
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

/* History search: exact match, else longest common substring (> 2) */
#define SEARCH_MAX 128
#define SEARCH_RESULTS_MAX 1000
#define NO_MATCH_MSG "No match for search term in history"

static int longest_common_substring(const char *a, const char *b, size_t blen) {
  int row[SEARCH_MAX + 1];
  int best = 0;
  memset(row, 0, sizeof(row));
  for (const char *p = a; *p; p++) {
    for (size_t j = blen; j >= 1; j--) {
      row[j] = (*p == b[j - 1]) ? row[j - 1] + 1 : 0;
      if (row[j] > best)
        best = row[j];
    }
  }
  return best;
}

static int is_search_cmd(const char *cmd) {
  while (*cmd == ' ' || *cmd == '\t')
    cmd++;
  return (strncmp(cmd, "history -s", 10) == 0 ||
          strncmp(cmd, "history search", 14) == 0);
}

/* Returns 0 = no match, 1 = exact, 2 = substring; results go to out */
static int history_search(const char *term, char *out, size_t cap) {
  char t[SEARCH_MAX + 1];
  size_t tlen = strlen(term);
  if (tlen > SEARCH_MAX)
    tlen = SEARCH_MAX;
  memcpy(t, term, tlen);
  t[tlen] = '\0';
  out[0] = '\0';
  if (tlen == 0 || !shared_state)
    return 0;

  int kind = 0;
  shared_lock();
  long total = shared_state->total;
  long first = total > MAX_HISTORY ? total - MAX_HISTORY : 0;

  /* 1. Exact match: find the most recent one */
  for (long n = total - 1; n >= first; n--) {
    const char *e = shared_state->history[n % MAX_HISTORY];
    if (is_search_cmd(e))
      continue;
    if (strcmp(e, t) == 0) {
      snprintf(out, cap, "%s\n", e);
      kind = 1;
      break;
    }
  }

  /* 2. Else: longest common substring if longer than 2 characters */
  if (!kind && total > first) {
    unsigned short *lens = malloc((size_t)(total - first) * sizeof(*lens));
    if (lens) {
      int best = 0;
      for (long n = first; n < total; n++) {
        const char *e = shared_state->history[n % MAX_HISTORY];
        int l = is_search_cmd(e) ? 0 : longest_common_substring(e, t, tlen);
        lens[n - first] = (unsigned short)l;
        if (l > best)
          best = l;
      }
      if (best > 2) {
        const char *found[SEARCH_RESULTS_MAX];
        int nf = 0;
        for (long n = total - 1; n >= first && nf < SEARCH_RESULTS_MAX; n--) {
          if (lens[n - first] != best)
            continue;
          const char *e = shared_state->history[n % MAX_HISTORY];
          int dup = 0;
          for (int k = 0; k < nf && !dup; k++)
            dup = (strcmp(found[k], e) == 0);
          if (!dup)
            found[nf++] = e;
        }
        /* Output chronological (oldest to newest) */
        size_t used = 0;
        int m = 0;
        while (m < nf && used + strlen(found[m]) + 1 < cap) {
          used += strlen(found[m]) + 1;
          m++;
        }
        size_t o = 0;
        for (int k = m - 1; k >= 0; k--) {
          size_t l = strlen(found[k]);
          if (o + l + 1 < cap) {
            memcpy(out + o, found[k], l);
            o += l;
            out[o++] = '\n';
          }
        }
        out[o] = '\0';
        kind = 2;
      }
      free(lens);
    }
  }
  shared_unlock();
  return kind;
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

/* ---- Task 11: Shell variables & script positional parameters ---- */
#define MAX_VARS 128

typedef struct {
  char name[64];
  char val[1024];
} ShellVar;

static _Thread_local ShellVar shell_vars[MAX_VARS];
static _Thread_local int shell_var_count = 0;
static _Thread_local int script_argc = 0;
static _Thread_local char script_argv[16][1024]; /* $0 .. $9+ */

static void set_shell_var(const char *name, const char *val) {
  for (int i = 0; i < shell_var_count; i++) {
    if (strcmp(shell_vars[i].name, name) == 0) {
      snprintf(shell_vars[i].val, sizeof(shell_vars[i].val), "%s", val);
      setenv(name, val, 1);
      return;
    }
  }
  if (shell_var_count < MAX_VARS) {
    snprintf(shell_vars[shell_var_count].name,
             sizeof(shell_vars[shell_var_count].name), "%s", name);
    snprintf(shell_vars[shell_var_count].val,
             sizeof(shell_vars[shell_var_count].val), "%s", val);
    shell_var_count++;
  }
  setenv(name, val, 1);
}

static const char *get_shell_var(const char *name) {
  for (int i = 0; i < shell_var_count; i++) {
    if (strcmp(shell_vars[i].name, name) == 0)
      return shell_vars[i].val;
  }
  return getenv(name);
}

enum { NEED_FI = 5 };

/* Count unclosed 'if' blocks across lines/semicolons while respecting quotes */
static int count_if_depth(const char *s) {
  int depth = 0;
  size_t i = 0;
  int at_cmd_start = 1;
  while (s[i]) {
    while (s[i] == ' ' || s[i] == '\t')
      i++;
    if (!s[i])
      break;
    if (s[i] == '#') {
      while (s[i] && s[i] != '\n')
        i++;
      continue;
    }
    if (s[i] == '\n' || s[i] == ';') {
      at_cmd_start = 1;
      i++;
      continue;
    }
    size_t start = i;
    while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != ';') {
      if (s[i] == '\'') {
        i++;
        while (s[i] && s[i] != '\'')
          i++;
        if (s[i])
          i++;
      } else if (s[i] == '"') {
        i++;
        while (s[i] && s[i] != '"') {
          if (s[i] == '\\' && s[i + 1])
            i += 2;
          else
            i++;
        }
        if (s[i])
          i++;
      } else if (s[i] == '\\' && s[i + 1]) {
        i += 2;
      } else {
        i++;
      }
    }
    size_t len = i - start;
    if (at_cmd_start) {
      if (len == 2 && strncmp(s + start, "if", 2) == 0)
        depth++;
      else if (len == 2 && strncmp(s + start, "fi", 2) == 0 && depth > 0)
        depth--;
      else if ((len == 4 && strncmp(s + start, "then", 4) == 0) ||
               (len == 4 && strncmp(s + start, "else", 4) == 0) ||
               (len == 4 && strncmp(s + start, "elif", 4) == 0)) {
        at_cmd_start = 1;
        continue;
      }
      at_cmd_start = 0;
    }
  }
  return depth;
}

/* Is buf (physical lines joined with '\n') a complete command? */
static int input_status(const char *buf) {
  size_t i = 0;
  int r = scan_unit(buf, &i, 0);
  if (r != INPUT_COMPLETE)
    return r;
  if (count_if_depth(buf) > 0)
    return NEED_FI;
  return INPUT_COMPLETE;
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
  } else if (s[*i + 1] == '#') {
    snprintf(tmp, sizeof(tmp), "%d", script_argc);
    emit_expansion(tl, w, in_word, tmp, quoted);
    *i += 2;
  } else if (s[*i + 1] >= '0' && s[*i + 1] <= '9') {
    int idx = s[*i + 1] - '0';
    const char *val = (idx <= script_argc) ? script_argv[idx] : "";
    emit_expansion(tl, w, in_word, val, quoted);
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
    const char *val = n ? get_shell_var(name) : NULL;
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

/* ---- Task 11: Built-in [ ... ] test, if/elif/else/fi & Script Interpreter
 * ---- */
static int eval_bracket_test(int argc, char **argv) {
  if (strcmp(argv[0], "[") == 0) {
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
      sh_err("terminator: [: missing ']'\n");
      return 2;
    }
    argc--;
  }
  int pos = 1;
  int neg = 0;
  while (pos < argc && strcmp(argv[pos], "!") == 0) {
    neg = !neg;
    pos++;
  }
  int rem = argc - pos;
  int res = 1;
  char pbuf[PATH_MAX * 2];
  struct stat st;

  if (rem <= 0) {
    res = 1;
  } else if (rem == 1) {
    res = (argv[pos][0] != '\0') ? 0 : 1;
  } else if (rem == 2) {
    const char *op = argv[pos];
    const char *arg = argv[pos + 1];
    if (strcmp(op, "-f") == 0)
      res = (stat(resolve_path(arg, pbuf, sizeof(pbuf)), &st) == 0 &&
             S_ISREG(st.st_mode))
                ? 0
                : 1;
    else if (strcmp(op, "-d") == 0)
      res = (stat(resolve_path(arg, pbuf, sizeof(pbuf)), &st) == 0 &&
             S_ISDIR(st.st_mode))
                ? 0
                : 1;
    else if (strcmp(op, "-e") == 0)
      res = (stat(resolve_path(arg, pbuf, sizeof(pbuf)), &st) == 0) ? 0 : 1;
    else if (strcmp(op, "-r") == 0)
      res = (access(resolve_path(arg, pbuf, sizeof(pbuf)), R_OK) == 0) ? 0 : 1;
    else if (strcmp(op, "-w") == 0)
      res = (access(resolve_path(arg, pbuf, sizeof(pbuf)), W_OK) == 0) ? 0 : 1;
    else if (strcmp(op, "-x") == 0)
      res = (access(resolve_path(arg, pbuf, sizeof(pbuf)), X_OK) == 0) ? 0 : 1;
    else if (strcmp(op, "-s") == 0)
      res = (stat(resolve_path(arg, pbuf, sizeof(pbuf)), &st) == 0 &&
             st.st_size > 0)
                ? 0
                : 1;
    else if (strcmp(op, "-z") == 0)
      res = (strlen(arg) == 0) ? 0 : 1;
    else if (strcmp(op, "-n") == 0)
      res = (strlen(arg) > 0) ? 0 : 1;
  } else if (rem >= 3) {
    const char *a = argv[pos];
    const char *op = argv[pos + 1];
    const char *b = argv[pos + 2];
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
      res = (strcmp(a, b) == 0) ? 0 : 1;
    else if (strcmp(op, "!=") == 0)
      res = (strcmp(a, b) != 0) ? 0 : 1;
    else if (strcmp(op, "-eq") == 0)
      res = (atoll(a) == atoll(b)) ? 0 : 1;
    else if (strcmp(op, "-ne") == 0)
      res = (atoll(a) != atoll(b)) ? 0 : 1;
    else if (strcmp(op, "-lt") == 0)
      res = (atoll(a) < atoll(b)) ? 0 : 1;
    else if (strcmp(op, "-le") == 0)
      res = (atoll(a) <= atoll(b)) ? 0 : 1;
    else if (strcmp(op, "-gt") == 0)
      res = (atoll(a) > atoll(b)) ? 0 : 1;
    else if (strcmp(op, "-ge") == 0)
      res = (atoll(a) >= atoll(b)) ? 0 : 1;
  }
  if (neg)
    res = (res == 0) ? 1 : 0;
  return res;
}

/* Split script/command buffer into next logical statement (separated by
 * unquoted \n or ;) */
static size_t next_statement(const char *s, size_t pos, char **out_stmt) {
  while (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == ';')
    pos++;
  if (!s[pos]) {
    *out_stmt = NULL;
    return pos;
  }
  size_t start = pos;
  int if_depth = 0;
  int at_start = 1;

  while (s[pos]) {
    while (s[pos] == ' ' || s[pos] == '\t')
      pos++;
    if (!s[pos])
      break;
    if (s[pos] == '#') {
      while (s[pos] && s[pos] != '\n')
        pos++;
      continue;
    }
    if (s[pos] == '\n' || s[pos] == ';') {
      if (if_depth == 0) {
        size_t len = pos - start;
        char *r = malloc(len + 1);
        if (!r)
          _exit(1);
        memcpy(r, s + start, len);
        r[len] = '\0';
        *out_stmt = r;
        return pos + 1;
      }
      at_start = 1;
      pos++;
      continue;
    }
    size_t wstart = pos;
    while (s[pos] && s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n' &&
           s[pos] != ';') {
      if (s[pos] == '\'') {
        pos++;
        while (s[pos] && s[pos] != '\'')
          pos++;
        if (s[pos])
          pos++;
      } else if (s[pos] == '"') {
        pos++;
        while (s[pos] && s[pos] != '"') {
          if (s[pos] == '\\' && s[pos + 1])
            pos += 2;
          else
            pos++;
        }
        if (s[pos])
          pos++;
      } else if (s[pos] == '$' && s[pos + 1] == '(') {
        pos += 2;
        scan_unit(s, &pos, 1);
      } else if (s[pos] == '\\' && s[pos + 1]) {
        pos += 2;
      } else {
        pos++;
      }
    }
    size_t wlen = pos - wstart;
    if (at_start) {
      if (wlen == 2 && strncmp(s + wstart, "if", 2) == 0)
        if_depth++;
      else if (wlen == 2 && strncmp(s + wstart, "fi", 2) == 0 && if_depth > 0)
        if_depth--;
      else if ((wlen == 4 && strncmp(s + wstart, "then", 4) == 0) ||
               (wlen == 4 && strncmp(s + wstart, "else", 4) == 0) ||
               (wlen == 4 && strncmp(s + wstart, "elif", 4) == 0)) {
        at_start = 1;
        continue;
      }
      at_start = 0;
    }
  }
  size_t len = pos - start;
  char *r = malloc(len + 1);
  if (!r)
    _exit(1);
  memcpy(r, s + start, len);
  r[len] = '\0';
  *out_stmt = r;
  return pos;
}

static int execute_script_text(const char *text);

/* Evaluate an if / elif / else / fi block (supports arbitrary nesting) */
static int execute_if_block(const char *stmt) {
  const char *p = stmt;
  while (*p == ' ' || *p == '\t' || *p == '\n')
    p++;
  if (strncmp(p, "if", 2) != 0 || !isspace((unsigned char)p[2]))
    return 2;
  p += 2;

  /* Parse clauses: condition + body for if/elif, or body for else */
  while (*p) {
    /* Find matching 'then' at depth 0 */
    size_t i = 0;
    int depth = 0;
    int at_start = 1;
    size_t then_pos = (size_t)-1;
    while (p[i]) {
      while (p[i] == ' ' || p[i] == '\t')
        i++;
      if (!p[i])
        break;
      if (p[i] == '#') {
        while (p[i] && p[i] != '\n')
          i++;
        continue;
      }
      if (p[i] == '\n' || p[i] == ';') {
        at_start = 1;
        i++;
        continue;
      }
      size_t ws = i;
      while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' &&
             p[i] != ';') {
        if (p[i] == '\'') {
          i++;
          while (p[i] && p[i] != '\'')
            i++;
          if (p[i])
            i++;
        } else if (p[i] == '"') {
          i++;
          while (p[i] && p[i] != '"') {
            if (p[i] == '\\' && p[i + 1])
              i += 2;
            else
              i++;
          }
          if (p[i])
            i++;
        } else if (p[i] == '\\' && p[i + 1]) {
          i += 2;
        } else {
          i++;
        }
      }
      size_t wl = i - ws;
      if (at_start) {
        if (wl == 2 && strncmp(p + ws, "if", 2) == 0)
          depth++;
        else if (wl == 2 && strncmp(p + ws, "fi", 2) == 0 && depth > 0)
          depth--;
        else if (depth == 0 && wl == 4 && strncmp(p + ws, "then", 4) == 0) {
          then_pos = ws;
          break;
        }
        at_start = 0;
      }
    }
    if (then_pos == (size_t)-1) {
      sh_err("terminator: syntax error: expected 'then'\n");
      last_status = 2;
      return 2;
    }

    char *cond = malloc(then_pos + 1);
    if (!cond)
      _exit(1);
    memcpy(cond, p, then_pos);
    cond[then_pos] = '\0';

    const char *body_start = p + then_pos + 4;
    if (*body_start == ';')
      body_start++;

    /* Find matching elif, else, or fi at depth 0 */
    i = 0;
    depth = 0;
    at_start = 1;
    size_t branch_pos = (size_t)-1;
    int branch_type = 0; /* 1 = elif, 2 = else, 3 = fi */
    while (body_start[i]) {
      while (body_start[i] == ' ' || body_start[i] == '\t')
        i++;
      if (!body_start[i])
        break;
      if (body_start[i] == '#') {
        while (body_start[i] && body_start[i] != '\n')
          i++;
        continue;
      }
      if (body_start[i] == '\n' || body_start[i] == ';') {
        at_start = 1;
        i++;
        continue;
      }
      size_t ws = i;
      while (body_start[i] && body_start[i] != ' ' && body_start[i] != '\t' &&
             body_start[i] != '\n' && body_start[i] != ';') {
        if (body_start[i] == '\'') {
          i++;
          while (body_start[i] && body_start[i] != '\'')
            i++;
          if (body_start[i])
            i++;
        } else if (body_start[i] == '"') {
          i++;
          while (body_start[i] && body_start[i] != '"') {
            if (body_start[i] == '\\' && body_start[i + 1])
              i += 2;
            else
              i++;
          }
          if (body_start[i])
            i++;
        } else if (body_start[i] == '\\' && body_start[i + 1]) {
          i += 2;
        } else {
          i++;
        }
      }
      size_t wl = i - ws;
      if (at_start) {
        if (wl == 2 && strncmp(body_start + ws, "if", 2) == 0) {
          depth++;
        } else if (wl == 2 && strncmp(body_start + ws, "fi", 2) == 0) {
          if (depth == 0) {
            branch_pos = ws;
            branch_type = 3;
            break;
          }
          depth--;
        } else if (depth == 0 && wl == 4 &&
                   strncmp(body_start + ws, "elif", 4) == 0) {
          branch_pos = ws;
          branch_type = 1;
          break;
        } else if (depth == 0 && wl == 4 &&
                   strncmp(body_start + ws, "else", 4) == 0) {
          branch_pos = ws;
          branch_type = 2;
          break;
        } else if (wl == 4 && (strncmp(body_start + ws, "then", 4) == 0 ||
                               strncmp(body_start + ws, "else", 4) == 0 ||
                               strncmp(body_start + ws, "elif", 4) == 0)) {
          at_start = 1;
          continue;
        }
        at_start = 0;
      }
    }

    if (branch_pos == (size_t)-1) {
      free(cond);
      sh_err("terminator: syntax error: expected 'fi'\n");
      last_status = 2;
      return 2;
    }

    int cond_rc = execute_script_text(cond);
    free(cond);

    if (cond_rc == 0) {
      char *body = malloc(branch_pos + 1);
      if (!body)
        _exit(1);
      memcpy(body, body_start, branch_pos);
      body[branch_pos] = '\0';
      last_status = execute_script_text(body);
      free(body);
      return last_status;
    }

    if (branch_type == 3) {
      /* Hit 'fi' without taking a branch: exit status is 0 per POSIX */
      last_status = 0;
      return 0;
    } else if (branch_type == 1) {
      /* elif: advance p past 'elif' and loop */
      p = body_start + branch_pos + 4;
    } else if (branch_type == 2) {
      /* else: find matching 'fi' at depth 0 and run else body */
      const char *else_start = body_start + branch_pos + 4;
      if (*else_start == ';')
        else_start++;
      size_t ei = 0;
      int edepth = 0;
      int eat_start = 1;
      size_t fi_pos = (size_t)-1;
      while (else_start[ei]) {
        while (else_start[ei] == ' ' || else_start[ei] == '\t')
          ei++;
        if (!else_start[ei])
          break;
        if (else_start[ei] == '#') {
          while (else_start[ei] && else_start[ei] != '\n')
            ei++;
          continue;
        }
        if (else_start[ei] == '\n' || else_start[ei] == ';') {
          eat_start = 1;
          ei++;
          continue;
        }
        size_t ws = ei;
        while (else_start[ei] && else_start[ei] != ' ' &&
               else_start[ei] != '\t' && else_start[ei] != '\n' &&
               else_start[ei] != ';') {
          if (else_start[ei] == '\'') {
            ei++;
            while (else_start[ei] && else_start[ei] != '\'')
              ei++;
            if (else_start[ei])
              ei++;
          } else if (else_start[ei] == '"') {
            ei++;
            while (else_start[ei] && else_start[ei] != '"') {
              if (else_start[ei] == '\\' && else_start[ei + 1])
                ei += 2;
              else
                ei++;
            }
            if (else_start[ei])
              ei++;
          } else if (else_start[ei] == '\\' && else_start[ei + 1]) {
            ei += 2;
          } else {
            ei++;
          }
        }
        size_t wl = ei - ws;
        if (eat_start) {
          if (wl == 2 && strncmp(else_start + ws, "if", 2) == 0) {
            edepth++;
          } else if (wl == 2 && strncmp(else_start + ws, "fi", 2) == 0) {
            if (edepth == 0) {
              fi_pos = ws;
              break;
            }
            edepth--;
          } else if (wl == 4 && (strncmp(else_start + ws, "then", 4) == 0 ||
                                 strncmp(else_start + ws, "else", 4) == 0 ||
                                 strncmp(else_start + ws, "elif", 4) == 0)) {
            eat_start = 1;
            continue;
          }
          eat_start = 0;
        }
      }
      if (fi_pos == (size_t)-1) {
        sh_err("terminator: syntax error: expected 'fi'\n");
        last_status = 2;
        return 2;
      }
      char *ebody = malloc(fi_pos + 1);
      if (!ebody)
        _exit(1);
      memcpy(ebody, else_start, fi_pos);
      ebody[fi_pos] = '\0';
      last_status = execute_script_text(ebody);
      free(ebody);
      return last_status;
    }
  }
  return last_status;
}

static int execute_single_line(const char *line);

static int execute_script_text(const char *text) {
  size_t pos = 0;
  char *stmt = NULL;
  while (!shell_exit_requested && (pos = next_statement(text, pos, &stmt)) &&
         stmt != NULL) {
    const char *p = stmt;
    while (*p == ' ' || *p == '\t' || *p == '\n')
      p++;
    if (*p && *p != '#') {
      if (strncmp(p, "if", 2) == 0 &&
          (p[2] == ' ' || p[2] == '\t' || p[2] == '\n')) {
        last_status = execute_if_block(p);
      } else {
        last_status = execute_single_line(p);
      }
    }
    free(stmt);
    stmt = NULL;
  }
  return last_status;
}

/* Read and interpret a script file directly in Terminator (not /bin/bash) */
static int run_script_file(const char *path, int argc, char **argv,
                           int is_sourced) {
  char pbuf[PATH_MAX * 2];
  const char *rpath = resolve_path(path, pbuf, sizeof(pbuf));
  FILE *f = fopen(rpath, "r");
  if (!f) {
    sh_err("terminator: %s: %s\n", path, strerror(errno));
    return 127;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 0) {
    fclose(f);
    return 1;
  }
  char *content = malloc((size_t)sz + 1);
  if (!content) {
    fclose(f);
    return 1;
  }
  size_t nread = fread(content, 1, (size_t)sz, f);
  content[nread] = '\0';
  fclose(f);

  /* Save previous positional parameters & state */
  int saved_argc = script_argc;
  char saved_argv[16][1024];
  memcpy(saved_argv, script_argv, sizeof(saved_argv));
  int saved_exit_req = shell_exit_requested;
  int saved_exit_code = shell_exit_code;

  script_argc = (argc > 1) ? (argc - 1) : 0;
  if (script_argc > 15)
    script_argc = 15;
  for (int i = 0; i < argc && i < 16; i++)
    snprintf(script_argv[i], sizeof(script_argv[i]), "%s", argv[i]);

  execute_script_text(content);
  free(content);

  int rc = shell_exit_requested ? shell_exit_code : last_status;
  /* An 'exit n' inside ./run.sh or bash run.sh exits the script, not the tab!
   */
  /* An 'exit n' inside 'source run.sh' runs in the current shell environment */
  if (!is_sourced && shell_exit_requested) {
    shell_exit_requested = saved_exit_req;
    shell_exit_code = saved_exit_code;
    last_status = rc;
  }

  script_argc = saved_argc;
  memcpy(script_argv, saved_argv, sizeof(saved_argv));
  return last_status;
}

/* Check if a file is a text/shell script (ends in .sh, starts with #!, or ASCII
 * text) */
static int is_text_script(const char *cmd_name) {
  char pbuf[PATH_MAX * 2];
  const char *rpath = resolve_path(cmd_name, pbuf, sizeof(pbuf));
  struct stat st;
  if (stat(rpath, &st) != 0 || !S_ISREG(st.st_mode))
    return 0;
  size_t len = strlen(cmd_name);
  if (len >= 3 && strcmp(cmd_name + len - 3, ".sh") == 0)
    return 1;
  int fd = open(rpath, O_RDONLY);
  if (fd < 0)
    return 0;
  unsigned char buf[64];
  ssize_t n = read(fd, buf, sizeof(buf));
  close(fd);
  if (n >= 2 && buf[0] == '#' && buf[1] == '!')
    return 1;
  /* Check if non-ELF text file when invoked with a path like ./script */
  if (n >= 4 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' &&
      buf[3] == 'F')
    return 0;
  if (n >= 4 && ((buf[0] == 0xcf && buf[1] == 0xfa) ||
                 (buf[0] == 0xfe && buf[1] == 0xed)))
    return 0; /* Mach-O binary */
  if (strchr(cmd_name, '/') != NULL && n > 0) {
    int text_ok = 1;
    for (ssize_t i = 0; i < n; i++) {
      if (buf[i] == 0) {
        text_ok = 0;
        break;
      }
    }
    return text_ok;
  }
  return 0;
}

static int execute_line(const char *line) { return execute_script_text(line); }

/* Parse, expand and run one single command/pipeline line. */
static int execute_single_line(const char *line) {
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

    /* Variable assignment: VAR=value (or multiple VAR1=v1 VAR2=v2) */
    int all_assign = (argc > 0);
    for (int a = 0; a < argc; a++) {
      const char *eq = strchr(argv[a], '=');
      if (!eq || eq == argv[a] ||
          !(isalpha((unsigned char)argv[a][0]) || argv[a][0] == '_')) {
        all_assign = 0;
        break;
      }
      for (const char *k = argv[a]; k < eq; k++) {
        if (!(isalnum((unsigned char)*k) || *k == '_')) {
          all_assign = 0;
          break;
        }
      }
    }
    if (all_assign) {
      for (int a = 0; a < argc; a++) {
        char *eq = strchr(argv[a], '=');
        char vname[64];
        size_t nlen = (size_t)(eq - argv[a]);
        if (nlen >= sizeof(vname))
          nlen = sizeof(vname) - 1;
        memcpy(vname, argv[a], nlen);
        vname[nlen] = '\0';
        set_shell_var(vname, eq + 1);
      }
      last_status = 0;
      tok_free(&tl);
      return 0;
    }

    if (strcmp(argv[0], "export") == 0) {
      for (int a = 1; a < argc; a++) {
        char *eq = strchr(argv[a], '=');
        if (eq && eq > argv[a]) {
          char vname[64];
          size_t nlen = (size_t)(eq - argv[a]);
          if (nlen >= sizeof(vname))
            nlen = sizeof(vname) - 1;
          memcpy(vname, argv[a], nlen);
          vname[nlen] = '\0';
          set_shell_var(vname, eq + 1);
        }
      }
      last_status = 0;
      tok_free(&tl);
      return 0;
    }

    if (strcmp(argv[0], "[") == 0 || strcmp(argv[0], "test") == 0) {
      last_status = eval_bracket_test(argc, argv);
      tok_free(&tl);
      return last_status;
    }

    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
      if (argc < 2) {
        sh_err("terminator: %s: filename argument required\n", argv[0]);
        last_status = 2;
      } else {
        last_status = run_script_file(argv[1], argc - 1, argv + 1, 1);
      }
      tok_free(&tl);
      return last_status;
    }

    if ((strcmp(argv[0], "bash") == 0 || strcmp(argv[0], "sh") == 0) &&
        argc >= 2) {
      last_status = run_script_file(argv[1], argc - 1, argv + 1, 0);
      tok_free(&tl);
      return last_status;
    }

    if (is_text_script(argv[0])) {
      last_status = run_script_file(argv[0], argc, argv, 0);
      tok_free(&tl);
      return last_status;
    }
  }

  if (num_cmds == 1) {
    char **argv = stage_argv[0];
    int argc = stage_argc[0];

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
        if (stage_argc[s] >= 3 && (strcmp(stage_argv[s][1], "-s") == 0 ||
                                   strcmp(stage_argv[s][1], "search") == 0)) {
          char res[MAX_HIST_LEN * 32];
          int kind = history_search(stage_argv[s][2], res, sizeof(res));
          if (kind > 0) {
            write_all(STDOUT_FILENO, res, strlen(res));
          } else {
            write_all(STDOUT_FILENO, NO_MATCH_MSG "\n",
                      strlen(NO_MATCH_MSG) + 1);
          }
        } else {
          print_history(STDOUT_FILENO);
        }
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
      if (strcmp(stage_argv[s][0], "[") == 0 ||
          strcmp(stage_argv[s][0], "test") == 0) {
        _exit(eval_bracket_test(stage_argc[s], stage_argv[s]));
      }
      if (strcmp(stage_argv[s][0], "source") == 0 ||
          strcmp(stage_argv[s][0], ".") == 0) {
        if (stage_argc[s] < 2)
          _exit(2);
        _exit(run_script_file(stage_argv[s][1], stage_argc[s] - 1,
                              stage_argv[s] + 1, 1));
      }
      if ((strcmp(stage_argv[s][0], "bash") == 0 ||
           strcmp(stage_argv[s][0], "sh") == 0) &&
          stage_argc[s] >= 2) {
        _exit(run_script_file(stage_argv[s][1], stage_argc[s] - 1,
                              stage_argv[s] + 1, 0));
      }
      if (is_text_script(stage_argv[s][0])) {
        _exit(
            run_script_file(stage_argv[s][0], stage_argc[s], stage_argv[s], 0));
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
  t->input_pos = 0;
  t->searching = 0;
  t->search[0] = '\0';
  t->search_len = 0;
  t->slot = slot;
  t->closing = 0;
  t->remove_pending = 0;
  t->pending[0] = '\0';
  t->pending_len = 0;
  t->hist_pos = 0;
  t->saved_input[0] = '\0';
  t->completing = 0;
  t->comp_count = 0;
  t->comp_replace_start = 0;
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

/* ---- hotkeys and ~/.terminatorrc bindings ---- */
typedef enum {
  ACT_NONE = 0,
  ACT_MOVE_LEFT,
  ACT_MOVE_RIGHT,
  ACT_START_OF_LINE,
  ACT_END_OF_LINE,
  ACT_HIST_PREV,
  ACT_HIST_NEXT,
  ACT_DEL_TO_START,
  ACT_DEL_TO_END,
  ACT_DEL_WORD,
  ACT_CLEAR_SCREEN,
  ACT_CLOSE_TAB,
  ACT_COPY,
  ACT_PASTE,
  ACT_SEARCH_HIST,
  ACT_NEW_TAB,
  ACT_INTERRUPT,
  ACT_SUSPEND,
  ACT_COUNT
} ActionId;

typedef struct {
  unsigned int mod;
  KeySym sym;
} KeyBinding;

static KeyBinding bindings[ACT_COUNT];

static void init_default_bindings(void) {
  bindings[ACT_MOVE_LEFT] = (KeyBinding){0, XK_Left};
  bindings[ACT_MOVE_RIGHT] = (KeyBinding){0, XK_Right};
  bindings[ACT_START_OF_LINE] = (KeyBinding){ControlMask, XK_a};
  bindings[ACT_END_OF_LINE] = (KeyBinding){ControlMask, XK_e};
  bindings[ACT_HIST_PREV] = (KeyBinding){0, XK_Up};
  bindings[ACT_HIST_NEXT] = (KeyBinding){0, XK_Down};
  bindings[ACT_DEL_TO_START] = (KeyBinding){ControlMask, XK_u};
  bindings[ACT_DEL_TO_END] = (KeyBinding){ControlMask, XK_k};
  bindings[ACT_DEL_WORD] = (KeyBinding){ControlMask, XK_w};
  bindings[ACT_CLEAR_SCREEN] = (KeyBinding){ControlMask, XK_l};
  bindings[ACT_CLOSE_TAB] = (KeyBinding){ControlMask, XK_d};
  bindings[ACT_COPY] = (KeyBinding){ControlMask | ShiftMask, XK_c};
  bindings[ACT_PASTE] = (KeyBinding){ControlMask | ShiftMask, XK_v};
  bindings[ACT_SEARCH_HIST] = (KeyBinding){ControlMask, XK_r};
  bindings[ACT_NEW_TAB] = (KeyBinding){ControlMask, XK_t};
  bindings[ACT_INTERRUPT] = (KeyBinding){ControlMask, XK_c};
  bindings[ACT_SUSPEND] = (KeyBinding){ControlMask, XK_z};
}

static ActionId parse_action_name(const char *name) {
  if (strcasecmp(name, "move-left") == 0 ||
      strcasecmp(name, "backward-char") == 0)
    return ACT_MOVE_LEFT;
  if (strcasecmp(name, "move-right") == 0 ||
      strcasecmp(name, "forward-char") == 0)
    return ACT_MOVE_RIGHT;
  if (strcasecmp(name, "start-of-line") == 0 ||
      strcasecmp(name, "beginning-of-line") == 0)
    return ACT_START_OF_LINE;
  if (strcasecmp(name, "end-of-line") == 0)
    return ACT_END_OF_LINE;
  if (strcasecmp(name, "browse-history-up") == 0 ||
      strcasecmp(name, "history-prev") == 0 ||
      strcasecmp(name, "previous-history") == 0 ||
      strcasecmp(name, "history-up") == 0)
    return ACT_HIST_PREV;
  if (strcasecmp(name, "browse-history-down") == 0 ||
      strcasecmp(name, "history-next") == 0 ||
      strcasecmp(name, "next-history") == 0 ||
      strcasecmp(name, "history-down") == 0)
    return ACT_HIST_NEXT;
  if (strcasecmp(name, "delete-to-start") == 0 ||
      strcasecmp(name, "backward-kill-line") == 0)
    return ACT_DEL_TO_START;
  if (strcasecmp(name, "delete-to-end") == 0 ||
      strcasecmp(name, "kill-line") == 0)
    return ACT_DEL_TO_END;
  if (strcasecmp(name, "delete-word") == 0 ||
      strcasecmp(name, "delete-previous-word") == 0 ||
      strcasecmp(name, "delete-prev-word") == 0 ||
      strcasecmp(name, "backward-kill-word") == 0)
    return ACT_DEL_WORD;
  if (strcasecmp(name, "clear-screen") == 0 || strcasecmp(name, "clear") == 0)
    return ACT_CLEAR_SCREEN;
  if (strcasecmp(name, "close-tab") == 0 || strcasecmp(name, "send-eof") == 0 ||
      strcasecmp(name, "eof") == 0)
    return ACT_CLOSE_TAB;
  if (strcasecmp(name, "copy") == 0)
    return ACT_COPY;
  if (strcasecmp(name, "paste") == 0)
    return ACT_PASTE;
  if (strcasecmp(name, "history-search") == 0 ||
      strcasecmp(name, "search-history") == 0 ||
      strcasecmp(name, "reverse-search") == 0)
    return ACT_SEARCH_HIST;
  if (strcasecmp(name, "new-tab") == 0)
    return ACT_NEW_TAB;
  if (strcasecmp(name, "interrupt") == 0)
    return ACT_INTERRUPT;
  if (strcasecmp(name, "suspend") == 0)
    return ACT_SUSPEND;
  return ACT_NONE;
}

static int parse_key_combo(const char *str, KeyBinding *kb) {
  kb->mod = 0;
  kb->sym = NoSymbol;

  char buf[128];
  snprintf(buf, sizeof(buf), "%s", str);
  char *saveptr = NULL;
  char *tok = strtok_r(buf, "+", &saveptr);
  while (tok) {
    char *next = strtok_r(NULL, "+", &saveptr);
    if (next) {
      if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0) {
        kb->mod |= ControlMask;
      } else if (strcasecmp(tok, "shift") == 0) {
        kb->mod |= ShiftMask;
      } else if (strcasecmp(tok, "alt") == 0 || strcasecmp(tok, "mod1") == 0 ||
                 strcasecmp(tok, "meta") == 0 || strcasecmp(tok, "opt") == 0 ||
                 strcasecmp(tok, "option") == 0) {
        kb->mod |= Mod1Mask;
      } else {
        return -1;
      }
    } else {
      if (strcasecmp(tok, "left") == 0) {
        kb->sym = XK_Left;
      } else if (strcasecmp(tok, "right") == 0) {
        kb->sym = XK_Right;
      } else if (strcasecmp(tok, "up") == 0) {
        kb->sym = XK_Up;
      } else if (strcasecmp(tok, "down") == 0) {
        kb->sym = XK_Down;
      } else if (strcasecmp(tok, "backspace") == 0) {
        kb->sym = XK_BackSpace;
      } else if (strcasecmp(tok, "delete") == 0) {
        kb->sym = XK_Delete;
      } else if (strcasecmp(tok, "return") == 0 ||
                 strcasecmp(tok, "enter") == 0) {
        kb->sym = XK_Return;
      } else if (strcasecmp(tok, "space") == 0) {
        kb->sym = XK_space;
      } else if (strcasecmp(tok, "tab") == 0) {
        kb->sym = XK_Tab;
      } else if (strlen(tok) == 1) {
        char c = tok[0];
        if (c >= 'A' && c <= 'Z')
          kb->sym = (KeySym)(XK_a + (c - 'A'));
        else if (c >= 'a' && c <= 'z')
          kb->sym = (KeySym)(XK_a + (c - 'a'));
        else
          kb->sym = (KeySym)(unsigned char)c;
      } else {
        KeySym ks = XStringToKeysym(tok);
        if (ks == NoSymbol) {
          char cap[64];
          snprintf(cap, sizeof(cap), "%s", tok);
          cap[0] = (char)toupper((unsigned char)cap[0]);
          for (int i = 1; cap[i]; i++)
            cap[i] = (char)tolower((unsigned char)cap[i]);
          ks = XStringToKeysym(cap);
        }
        if (ks == NoSymbol)
          return -1;
        if (ks >= XK_A && ks <= XK_Z)
          ks = ks - XK_A + XK_a;
        kb->sym = ks;
      }
    }
    tok = next;
  }
  return (kb->sym != NoSymbol) ? 0 : -1;
}

static void load_config(void) {
  init_default_bindings();

  char path[PATH_MAX];
  const char *rc = getenv("TERMINATOR_RC");
  const char *home = getenv("HOME");
  if (rc && *rc) {
    snprintf(path, sizeof(path), "%s", rc);
  } else if (home && *home) {
    snprintf(path, sizeof(path), "%s/.terminatorrc", home);
  } else {
    return;
  }

  FILE *f = fopen(path, "r");
  if (!f)
    return;

  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  int line_no = 0;
  while ((n = getline(&line, &cap, f)) >= 0) {
    line_no++;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (!*p || *p == '#')
      continue;

    char cmd[64], act_str[64], key_str[64];
    cmd[0] = act_str[0] = key_str[0] = '\0';
    int items = sscanf(p, "%63s %63s %63s", cmd, act_str, key_str);
    if (items < 3 || strcmp(cmd, "bind") != 0) {
      fprintf(stderr, "terminator: %s:%d: bad syntax '%s', keeping default\n",
              path, line_no, p);
      continue;
    }

    ActionId act = parse_action_name(act_str);
    if (act == ACT_NONE) {
      fprintf(stderr,
              "terminator: %s:%d: unknown action '%s', keeping default\n", path,
              line_no, act_str);
      continue;
    }

    KeyBinding kb;
    if (parse_key_combo(key_str, &kb) != 0) {
      fprintf(stderr, "terminator: %s:%d: bad key '%s', keeping default\n",
              path, line_no, key_str);
      continue;
    }

    bindings[act] = kb;
  }
  free(line);
  fclose(f);
}

/* ---- Task 10: Tab filename auto-completion (Slide 31) ---- */
static int cmp_str_ptr(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static void replace_input_range(Tab *cur, int start, int end,
                                const char *replacement) {
  int rlen = (int)strlen(replacement);
  int tail_len = cur->input_len - end;
  if (start + rlen + tail_len >= (int)sizeof(cur->input) - 1)
    return;
  memmove(cur->input + start + rlen, cur->input + end, (size_t)tail_len + 1);
  memcpy(cur->input + start, replacement, (size_t)rlen);
  cur->input_len = start + rlen + tail_len;
  cur->input_pos = start + rlen;
}

static void handle_tab_completion(Tab *cur) {
  cur->completing = 0;
  cur->comp_count = 0;

  /* Find start of current filename word before cursor */
  int wstart = cur->input_pos;
  while (wstart > 0 && !isspace((unsigned char)cur->input[wstart - 1]) &&
         cur->input[wstart - 1] != '|' && cur->input[wstart - 1] != '<' &&
         cur->input[wstart - 1] != '>' && cur->input[wstart - 1] != '&') {
    wstart--;
  }

  char token[512];
  int tlen = cur->input_pos - wstart;
  if (tlen < 0)
    tlen = 0;
  if (tlen >= (int)sizeof(token))
    tlen = (int)sizeof(token) - 1;
  memcpy(token, cur->input + wstart, (size_t)tlen);
  token[tlen] = '\0';

  /* Split token into optional directory part and filename prefix */
  char dir_path[PATH_MAX * 2];
  const char *prefix = token;
  int replace_start = wstart;
  char *last_slash = strrchr(token, '/');

  if (last_slash) {
    prefix = last_slash + 1;
    replace_start = wstart + (int)(prefix - token);
    size_t dlen = (size_t)(last_slash - token);
    char sub[512];
    if (dlen == 0) {
      snprintf(dir_path, sizeof(dir_path), "/");
    } else {
      if (dlen >= sizeof(sub))
        dlen = sizeof(sub) - 1;
      memcpy(sub, token, dlen);
      sub[dlen] = '\0';
      if (sub[0] == '/')
        snprintf(dir_path, sizeof(dir_path), "%s", sub);
      else
        snprintf(dir_path, sizeof(dir_path), "%s/%s",
                 cur->cwd[0] ? cur->cwd : ".", sub);
    }
  } else {
    snprintf(dir_path, sizeof(dir_path), "%s", cur->cwd[0] ? cur->cwd : ".");
  }

  DIR *d = opendir(dir_path);
  if (!d)
    return;

  size_t plen = strlen(prefix);
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    /* Skip hidden dot-files unless prefix starts with '.' */
    if (de->d_name[0] == '.' && prefix[0] != '.')
      continue;
    if (strncmp(de->d_name, prefix, plen) == 0) {
      if (cur->comp_count < 64) {
        snprintf(cur->comp_matches[cur->comp_count],
                 sizeof(cur->comp_matches[0]), "%s", de->d_name);
        cur->comp_count++;
      }
    }
  }
  closedir(d);

  if (cur->comp_count == 0) {
    return; /* No match: do nothing (Slide 31) */
  }

  qsort(cur->comp_matches, (size_t)cur->comp_count,
        sizeof(cur->comp_matches[0]), cmp_str_ptr);

  if (cur->comp_count == 1) {
    /* One match: complete it. The rest of the line stays as typed (Slide 31) */
    replace_input_range(cur, replace_start, cur->input_pos,
                        cur->comp_matches[0]);
    return;
  }

  /* Several matches: extend to longest common prefix, then show numbered list
   */
  size_t lcp = strlen(cur->comp_matches[0]);
  for (int i = 1; i < cur->comp_count; i++) {
    size_t j = 0;
    while (j < lcp && cur->comp_matches[0][j] == cur->comp_matches[i][j])
      j++;
    lcp = j;
  }

  char lcp_str[256];
  if (lcp >= sizeof(lcp_str))
    lcp = sizeof(lcp_str) - 1;
  memcpy(lcp_str, cur->comp_matches[0], lcp);
  lcp_str[lcp] = '\0';

  replace_input_range(cur, replace_start, cur->input_pos, lcp_str);
  cur->comp_replace_start = replace_start;
  cur->completing = 1;

  /* Echo current prompt + line and the numbered list to the terminal buffer */
  char echo_line[1200];
  snprintf(echo_line, sizeof(echo_line), "%s%s\n",
           cur->pending_len > 0 ? CONT_PROMPT : "user@terminator> ",
           cur->input);
  tab_append_text(cur, echo_line, (int)strlen(echo_line));

  char list_buf[2048];
  size_t off = 0;
  for (int i = 0; i < cur->comp_count; i++) {
    int n = snprintf(list_buf + off, sizeof(list_buf) - off, "%s%d. %s",
                     (i > 0) ? "  " : "", i + 1, cur->comp_matches[i]);
    if (n > 0 && off + (size_t)n < sizeof(list_buf) - 2)
      off += (size_t)n;
  }
  list_buf[off++] = '\n';
  list_buf[off] = '\0';
  tab_append_text(cur, list_buf, (int)off);
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

  if (cur->searching) {
    const char *srch_prompt = "Enter search term: ";
    XSetForeground(dpy, gc, col_prompt_arrow);
    XDrawString(dpy, win, gc, 16, draw_y, srch_prompt, strlen(srch_prompt));
    int spx = 16 + strlen(srch_prompt) * char_width;

    XSetForeground(dpy, gc, col_text);
    XDrawString(dpy, win, gc, spx, draw_y, cur->search, cur->search_len);
    int cursor_x = spx + cur->search_len * char_width;

    XSetForeground(dpy, gc, col_cursor);
    XFillRectangle(dpy, win, gc, cursor_x, draw_y - font_ascent + 2, char_width,
                   char_height);
  } else {
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
    int cursor_x = px + cur->input_pos * char_width;

    XSetForeground(dpy, gc, col_cursor);
    XFillRectangle(dpy, win, gc, cursor_x, draw_y - font_ascent + 2, char_width,
                   char_height);
    if (cur->input_pos < cur->input_len) {
      XSetForeground(dpy, gc, col_bg);
      XDrawString(dpy, win, gc, cursor_x, draw_y, &cur->input[cur->input_pos],
                  1);
    }
  }
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

  history_init_path();
  history_load();
  load_config();

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
        unsigned int mod = ev.xkey.state & (ControlMask | ShiftMask | Mod1Mask);

        KeySym norm_ks = ks;
        if (norm_ks >= XK_A && norm_ks <= XK_Z)
          norm_ks = norm_ks - XK_A + XK_a;

        ActionId action = ACT_NONE;
        for (int a = 1; a < ACT_COUNT; a++) {
          if (bindings[a].mod == mod && bindings[a].sym == norm_ks) {
            action = (ActionId)a;
            break;
          }
        }

        if (cur->searching) {
          if (action == ACT_INTERRUPT || ks == XK_Escape) {
            cur->searching = 0;
            cur->search_len = 0;
            cur->search[0] = '\0';
            tab_append_text(cur, "^C\n", 3);
            redraw(win_w, win_h);
          } else if (ks == XK_BackSpace) {
            if (cur->search_len > 0) {
              cur->search[--cur->search_len] = '\0';
              redraw(win_w, win_h);
            }
          } else if (ks == XK_Return) {
            cur->searching = 0;
            char echo_srch[256];
            snprintf(echo_srch, sizeof(echo_srch), "Enter search term: %s\n",
                     cur->search);
            tab_append_text(cur, echo_srch, strlen(echo_srch));

            char res[MAX_HIST_LEN * 32];
            int kind = history_search(cur->search, res, sizeof(res));
            if (kind == 1) {
              tab_append_text(cur, res, strlen(res));
              size_t rlen = strlen(res);
              while (rlen > 0 &&
                     (res[rlen - 1] == '\n' || res[rlen - 1] == '\r'))
                res[--rlen] = '\0';
              snprintf(cur->input, sizeof(cur->input), "%s", res);
              cur->input_len = (int)strlen(cur->input);
              cur->input_pos = cur->input_len;
            } else if (kind == 2) {
              tab_append_text(cur, res, strlen(res));
            } else {
              const char *nomatch = NO_MATCH_MSG "\n";
              tab_append_text(cur, nomatch, strlen(nomatch));
            }
            cur->search_len = 0;
            cur->search[0] = '\0';
            redraw(win_w, win_h);
          } else if (len > 0 && (unsigned char)kbuf[0] >= 32 &&
                     (unsigned char)kbuf[0] <= 126) {
            if (cur->search_len < (int)sizeof(cur->search) - 2) {
              cur->search[cur->search_len++] = kbuf[0];
              cur->search[cur->search_len] = '\0';
              redraw(win_w, win_h);
            }
          }
        } else {
          if (cur->completing && action == ACT_NONE && len == 1 &&
              kbuf[0] >= '1' && kbuf[0] <= '9' &&
              !(mod & (ControlMask | Mod1Mask))) {
            int pick = kbuf[0] - '1';
            if (pick < cur->comp_count) {
              cur->completing = 0;
              replace_input_range(cur, cur->comp_replace_start, cur->input_pos,
                                  cur->comp_matches[pick]);
              redraw(win_w, win_h);
              continue;
            }
          }
          if (ks != XK_Shift_L && ks != XK_Shift_R && ks != XK_Control_L &&
              ks != XK_Control_R && ks != XK_Alt_L && ks != XK_Alt_R) {
            cur->completing = 0;
          }
          switch (action) {
          case ACT_NEW_TAB:
            create_tab();
            redraw(win_w, win_h);
            break;
          case ACT_INTERRUPT: {
            /* signal the foreground job, else make the shell drop the line */
            int job_signalled = signal_tab_job(active_tab, SIGINT);
            tab_append_text(cur, "^C\n", 3);
            if (!job_signalled) {
              cur->input_len = 0;
              cur->input_pos = 0;
              cur->input[0] = '\0';
              cur->pending_len = 0;
              cur->pending[0] = '\0';
              cur->hist_pos = 0;
              write_all(cur->in_fd, "\003", 1);
            }
            redraw(win_w, win_h);
            break;
          }
          case ACT_SUSPEND: /* stop the foreground job */
            if (signal_tab_job(active_tab, SIGTSTP)) {
              tab_append_text(cur, "^Z\n", 3);
              redraw(win_w, win_h);
            }
            break;
          case ACT_MOVE_LEFT:
            if (cur->input_pos > 0) {
              cur->input_pos--;
              redraw(win_w, win_h);
            }
            break;
          case ACT_MOVE_RIGHT:
            if (cur->input_pos < cur->input_len) {
              cur->input_pos++;
              redraw(win_w, win_h);
            }
            break;
          case ACT_START_OF_LINE:
            cur->input_pos = 0;
            redraw(win_w, win_h);
            break;
          case ACT_END_OF_LINE:
            cur->input_pos = cur->input_len;
            redraw(win_w, win_h);
            break;
          case ACT_HIST_PREV: /* history is shared by every tab */
            if (cur->pending_len == 0) {
              char h[MAX_HIST_LEN];
              if (history_get_back(cur->hist_pos + 1, h, sizeof(h))) {
                if (cur->hist_pos == 0)
                  memcpy(cur->saved_input, cur->input,
                         sizeof(cur->saved_input));
                cur->hist_pos++;
                snprintf(cur->input, sizeof(cur->input), "%s", h);
                cur->input_len = (int)strlen(cur->input);
                cur->input_pos = cur->input_len;
                redraw(win_w, win_h);
              }
            }
            break;
          case ACT_HIST_NEXT:
            if (cur->pending_len == 0 && cur->hist_pos > 0) {
              char h[MAX_HIST_LEN];
              cur->hist_pos--;
              if (cur->hist_pos == 0) {
                memcpy(cur->input, cur->saved_input, sizeof(cur->input));
              } else if (history_get_back(cur->hist_pos, h, sizeof(h))) {
                snprintf(cur->input, sizeof(cur->input), "%s", h);
              }
              cur->input_len = (int)strlen(cur->input);
              cur->input_pos = cur->input_len;
              redraw(win_w, win_h);
            }
            break;
          case ACT_DEL_TO_START:
            if (cur->input_pos > 0) {
              memmove(cur->input, cur->input + cur->input_pos,
                      cur->input_len - cur->input_pos + 1);
              cur->input_len -= cur->input_pos;
              cur->input_pos = 0;
              redraw(win_w, win_h);
            }
            break;
          case ACT_DEL_TO_END:
            if (cur->input_pos < cur->input_len) {
              cur->input[cur->input_pos] = '\0';
              cur->input_len = cur->input_pos;
              redraw(win_w, win_h);
            }
            break;
          case ACT_DEL_WORD:
            if (cur->input_pos > 0) {
              int p = cur->input_pos;
              while (p > 0 && isspace((unsigned char)cur->input[p - 1]))
                p--;
              while (p > 0 && !isspace((unsigned char)cur->input[p - 1]))
                p--;
              memmove(cur->input + p, cur->input + cur->input_pos,
                      cur->input_len - cur->input_pos + 1);
              cur->input_len -= (cur->input_pos - p);
              cur->input_pos = p;
              redraw(win_w, win_h);
            }
            break;
          case ACT_CLEAR_SCREEN:
            cur->line_count = 0;
            cur->lines[0][0] = '\0';
            redraw(win_w, win_h);
            break;
          case ACT_CLOSE_TAB:
            if (cur->input_len == 0 && cur->pending_len == 0) {
              if (cur->out_fd < 0) {
                close_tab(active_tab);
                if (tab_count == 0) {
                  running = 0;
                  break;
                }
                redraw(win_w, win_h);
              } else {
                cur->closing = 1;
                if (cur->in_fd >= 0) {
                  close(cur->in_fd);
                  cur->in_fd = -1;
                }
              }
            } else if (cur->input_pos < cur->input_len) {
              memmove(cur->input + cur->input_pos,
                      cur->input + cur->input_pos + 1,
                      cur->input_len - cur->input_pos);
              cur->input_len--;
              redraw(win_w, win_h);
            }
            break;
          case ACT_COPY:
            if (shared_state) {
              shared_lock();
              if (cur->input_len > 0) {
                snprintf(shared_state->clipboard,
                         sizeof(shared_state->clipboard), "%s", cur->input);
              } else if (cur->line_count > 0) {
                snprintf(shared_state->clipboard,
                         sizeof(shared_state->clipboard), "%s",
                         cur->lines[cur->line_count - 1]);
              }
              shared_unlock();
            }
            break;
          case ACT_PASTE:
            if (shared_state) {
              shared_lock();
              char clip[4096];
              snprintf(clip, sizeof(clip), "%s", shared_state->clipboard);
              shared_unlock();
              size_t clen = strlen(clip);
              if (clen > 0 &&
                  cur->input_len + (int)clen < (int)sizeof(cur->input) - 2) {
                memmove(cur->input + cur->input_pos + clen,
                        cur->input + cur->input_pos,
                        cur->input_len - cur->input_pos + 1);
                memcpy(cur->input + cur->input_pos, clip, clen);
                cur->input_len += (int)clen;
                cur->input_pos += (int)clen;
                redraw(win_w, win_h);
              }
            }
            break;
          case ACT_SEARCH_HIST:
            cur->searching = 1;
            cur->search[0] = '\0';
            cur->search_len = 0;
            redraw(win_w, win_h);
            break;
          case ACT_NONE:
          default:
            if (ks == XK_Tab && !(mod & (ControlMask | Mod1Mask))) {
              handle_tab_completion(cur);
              redraw(win_w, win_h);
            } else if (ks == XK_BackSpace) {
              if (cur->input_pos > 0) {
                memmove(cur->input + cur->input_pos - 1,
                        cur->input + cur->input_pos,
                        cur->input_len - cur->input_pos + 1);
                cur->input_len--;
                cur->input_pos--;
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

              /* track the command like the shell does, to pick the prompt */
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
              cur->input_pos = 0;
              cur->input[0] = '\0';
              cur->hist_pos = 0;
              redraw(win_w, win_h);
            } else if (len > 0 && (unsigned char)kbuf[0] >= 32 &&
                       (unsigned char)kbuf[0] <= 126 &&
                       !(mod & (ControlMask | Mod1Mask))) {
              if (cur->input_len < (int)sizeof(cur->input) - 2) {
                memmove(cur->input + cur->input_pos + 1,
                        cur->input + cur->input_pos,
                        cur->input_len - cur->input_pos + 1);
                cur->input[cur->input_pos++] = kbuf[0];
                cur->input_len++;
                cur->input[cur->input_len] = '\0';
                redraw(win_w, win_h);
              }
            }
            break;
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
