#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <string.h>

static char buf[4096] = "Hello World";
static int  buf_len;

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;

    int scr = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 10, 10, 400, 200, 1,
                                     BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapWindow(dpy, win);
    GC gc = DefaultGC(dpy, scr);

    buf_len = strlen(buf);
    XEvent ev;

    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == Expose) {
            XClearWindow(dpy, win);
            XDrawString(dpy, win, gc, 20, 50, buf, buf_len);
        } else if (ev.type == KeyPress) {
            KeySym ks;
            char kbuf[32];
            XLookupString(&ev.xkey, kbuf, sizeof(kbuf), &ks, NULL);
            if (ks == XK_BackSpace && buf_len > 0) {
                buf[--buf_len] = '\0';
                XClearWindow(dpy, win);
                XDrawString(dpy, win, gc, 20, 50, buf, buf_len);
            }
        }
    }
    return 0;
}
