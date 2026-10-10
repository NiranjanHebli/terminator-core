# Terminator Demo

A custom X11 terminal emulator and command shell built from scratch in C using Xlib and POSIX system calls.

---

## Prerequisites

- **macOS:** [XQuartz](https://www.xquartz.org/) and `libx11` via Homebrew:
  ```bash
  brew install libx11
  open -a XQuartz
  export DISPLAY=:0
  ```
- **Linux:** X11 development headers:
  ```bash
  sudo apt-get install -y libx11-dev
  ```

---

## Build

Build using [Makefile](Makefile):

```bash
make
```

To clean the compiled binary:

```bash
make clean
```

---

## Run

```bash
./terminal
```

---

## Features

- Creates an X11 window using Xlib.
- Displays an editable `"Hello World"` text buffer.
- Handles keyboard input (`KeyPress`) and backspace (`XK_BackSpace`).
