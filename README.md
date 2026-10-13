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

## Features

### Act 1: X11 GUI & Multi-Tab Terminal Foundation
- Custom X11 window rendered via Xlib with modern Catppuccin Mocha-inspired color scheme.
- Tab bar with multi-tab management (click `+` or press `Ctrl+T` to create new tabs, click tabs to switch).
- Multiplexed non-blocking event loop using `poll()` to sleep when idle and handle X11 events and tab pipes concurrently.
- Per-tab in-memory text buffer and interactive command-line editing with backspace and cursor.

### Act 2: Command Execution Engine & Built-ins
- **Command Lifecycle:** Spawns external binaries via `fork()` $\to$ `execvp()` $\to$ `waitpid()`.
- **Built-in `cd`:** Changes directory with `chdir()` directly inside the tab shell, supporting `cd`, `cd ~`, `cd ..`, and relative/absolute paths.
- **Dynamic Tab Titles:** Each tab dynamically updates its title to display its active directory (e.g., `1: terminal_demo`).
- **Built-in `exit`:** Gracefully exits tab process and frees pipe descriptors.
- **Return Status Tracking (`$?`):** Tracks exit codes of commands and built-ins, expanding `$?` expressions (e.g., `echo $?`).
- **Standard Error Integration:** Child `stderr` is multiplexed with `stdout`, routing error diagnostics (such as `command not found`) straight to the terminal window.

---

## Run

```bash
./terminal
```
