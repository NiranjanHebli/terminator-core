#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <errno.h>

#define MAX_TABS 10
#define MAX_LINES 1000
#define MAX_LINE_LEN 256
#define TAB_BAR_HEIGHT 32
#define TAB_WIDTH 110
#define ADD_BTN_WIDTH 36

typedef struct {
    int id;
    pid_t pid;
    int in_fd;
    int out_fd;
    char title[32];
    char input[1024];
    int input_len;
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
        if (c == '\r') continue;
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

static void create_tab(void) {
    if (tab_count >= MAX_TABS) return;

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) return;

    pid_t pid = fork();
    if (pid < 0) return;

    if (pid == 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);

        char line[1024];
        int last_status = 0;
        while (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (len == 0) continue;

            char *argv[64];
            int argc = 0;
            char *tok = strtok(line, " \t");
            while (tok && argc < 63) {
                argv[argc++] = tok;
                tok = strtok(NULL, " \t");
            }
            argv[argc] = NULL;
            if (argc == 0) continue;

            /* Expand $? in arguments */
            char status_str[16];
            snprintf(status_str, sizeof(status_str), "%d", last_status);
            for (int i = 0; i < argc; i++) {
                if (strcmp(argv[i], "$?") == 0) {
                    argv[i] = status_str;
                }
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
                if (!dest) dest = "/";

                if (chdir(dest) == 0) {
                    char cwd[128];
                    if (getcwd(cwd, sizeof(cwd))) {
                        printf("[cwd: %s]\n", cwd);
                    }
                    last_status = 0;
                } else {
                    perror("cd");
                    last_status = 1;
                }
                fflush(stdout);
                fflush(stderr);
                continue;
            }

            if (strcmp(argv[0], "exit") == 0) {
                int exit_code = (argc > 1) ? atoi(argv[1]) : last_status;
                _exit(exit_code);
            }

            pid_t cpid = fork();
            if (cpid == 0) {
                execvp(argv[0], argv);
                fprintf(stderr, "terminator: %s: command not found\n", argv[0]);
                _exit(127);
            } else if (cpid > 0) {
                int status = 0;
                waitpid(cpid, &status, 0);
                if (WIFEXITED(status)) {
                    last_status = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    last_status = 128 + WTERMSIG(status);
                } else {
                    last_status = 1;
                }
                fflush(stdout);
                fflush(stderr);
            } else {
                perror("fork");
                last_status = 1;
            }
        }
        _exit(0);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);

    Tab *t = &tabs[tab_count];
    t->id = tab_count + 1;
    t->pid = pid;
    t->in_fd = in_pipe[1];
    t->out_fd = out_pipe[0];
    snprintf(t->title, sizeof(t->title), "%d: tab", t->id);
    t->input[0] = '\0';
    t->input_len = 0;
    t->line_count = 0;

    char welcome[128];
    snprintf(welcome, sizeof(welcome), "Terminator Tab %d (PID: %d ready)\n", t->id, pid);
    tab_append_text(t, welcome, strlen(welcome));

    active_tab = tab_count;
    tab_count++;
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
        XDrawString(dpy, win, gc, x + 10, y + font_ascent + 2, tabs[i].title, strlen(tabs[i].title));
    }

    int add_x = 8 + tab_count * (TAB_WIDTH + 6);
    int add_y = 4;
    int add_h = TAB_BAR_HEIGHT - 8;
    XSetForeground(dpy, gc, col_tab_border);
    XDrawRectangle(dpy, win, gc, add_x, add_y, ADD_BTN_WIDTH, add_h);
    XSetForeground(dpy, gc, col_tab_add_fg);
    XDrawString(dpy, win, gc, add_x + 13, add_y + font_ascent + 2, "+", 1);

    if (active_tab < 0 || active_tab >= tab_count) return;
    Tab *cur = &tabs[active_tab];

    int content_top = TAB_BAR_HEIGHT + 16;
    int max_visible = (win_h - content_top - 36) / char_height;
    if (max_visible < 1) max_visible = 1;

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

    const char *prompt_user = "user@terminator";
    const char *prompt_arrow = "> ";
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
    XFillRectangle(dpy, win, gc, cursor_x, draw_y - font_ascent + 2, char_width, char_height);
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;

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
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) {
        char_width = font->max_bounds.width;
        char_height = font->ascent + font->descent + 2;
        font_ascent = font->ascent;
    }

    int win_w = 820;
    int win_h = 520;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, win_w, win_h, 1,
                              col_tab_border, col_bg);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);

    gc = XCreateGC(dpy, win, 0, NULL);
    if (font) XSetFont(dpy, gc, font->fid);

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
                                    char *slash = strrchr(p, '/');
                                    const char *bname = (slash && *(slash + 1)) ? slash + 1 : p;
                                    snprintf(tabs[i].title, sizeof(tabs[i].title), "%d: %s", tabs[i].id, bname);
                                }
                            }
                            redraw(win_w, win_h);
                        } else if (n == 0) {
                            close(tabs[i].out_fd);
                            tabs[i].out_fd = -1;
                            const char *closed_msg = "[Process exited]\n";
                            tab_append_text(&tabs[i], closed_msg, strlen(closed_msg));
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
                } else if (ks == XK_BackSpace) {
                    if (cur->input_len > 0) {
                        cur->input[--cur->input_len] = '\0';
                        redraw(win_w, win_h);
                    }
                } else if (ks == XK_Return) {
                    char echo_line[1200];
                    snprintf(echo_line, sizeof(echo_line), "user@terminator> %s\n", cur->input);
                    tab_append_text(cur, echo_line, strlen(echo_line));

                    write(cur->in_fd, cur->input, cur->input_len);
                    write(cur->in_fd, "\n", 1);

                    cur->input_len = 0;
                    cur->input[0] = '\0';
                    redraw(win_w, win_h);
                } else if (len > 0 && (unsigned char)kbuf[0] >= 32 && (unsigned char)kbuf[0] <= 126) {
                    if (cur->input_len < (int)sizeof(cur->input) - 2) {
                        cur->input[cur->input_len++] = kbuf[0];
                        cur->input[cur->input_len] = '\0';
                        redraw(win_w, win_h);
                    }
                }
            }
        }
    }

    if (font) XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
