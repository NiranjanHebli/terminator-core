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

### Multiline Input, Quoting & Expansion (Task 3)
- **Read until complete:** if a line ends with `\`, or leaves a `'…'`, `"…"` or `$(…)` open, the tab shows a `> ` continuation prompt and keeps reading. The same `input_status()` check is used by the shell and the GUI so they never disagree.
- **Quoting:** `'single'` is fully literal, `"double"` keeps spaces but still expands `$VAR`, `$?` and `$(…)`, and `\` escapes the next character. `|`, `<`, `>` inside quotes are plain text.
- **Expansion:** `$VAR`, `${VAR}`, `$?`, `$$` and nested `$(…)` (innermost first, run through the same engine in a child process). Unquoted results are word-split; quoted ones are not.
- Example: `echo "Files in $(basename $(pwd)): $(ls | \` ⏎ `> wc -l)"`.
- Not supported: backticks, globbing (`*.txt`), `&&`/`;`.

### Act 3: Plumbing — I/O Redirection & Pipelines
- **I/O Redirection (`<`, `>`, `>>`):** Rewired in child processes via `open()` and `dup2()` without affecting the shell descriptors. Operators work with or without spaces (`sort<in.txt>out.txt`); a missing target reports `syntax error`, an unopenable file reports the `errno` message and sets `$?` to 1, and a bare `> file` creates/truncates the file.
- **Headless test mode:** `./terminal --shell` runs only the shell engine on stdin/stdout (no X11), e.g. `printf 'sort < in.txt > out.txt\n' | ./terminal --shell`.
- **Multi-Stage Pipelines (`cmd1 | cmd2 | ... | cmdN`):** Creates $N - 1$ pipes for $N$ stages, properly wiring each command's stdout to the next command's stdin.
- **Pipe Closure Rule:** Unused pipe ends are closed in each child and in the parent shell so readers receive `EOF`.
- **Process Group Alignment (`setpgid`):** All pipeline children share a process group for unified signal dispatching.
- **Reaping & Status:** Waits for all pipeline stages and captures the last command's return status in `$?`.

---

## Run

```bash
./terminal
```
