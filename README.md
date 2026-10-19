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

The binary is named `terminator`. To clean the compiled binary:

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
- **Headless test mode:** `./terminator --shell` runs only the shell engine on stdin/stdout (no X11), e.g. `printf 'sort < in.txt > out.txt\n' | ./terminator --shell`.
- **Multi-Stage Pipelines (`cmd1 | cmd2 | ... | cmdN`):** Creates $N - 1$ pipes for $N$ stages, properly wiring each command's stdout to the next command's stdin.
- **Pipe Closure Rule:** Unused pipe ends are closed in each child and in the parent shell so readers receive `EOF`.
- **Process Group Alignment (`setpgid`):** All pipeline children share a process group for unified signal dispatching.
- **Reaping & Status:** Waits for all pipeline stages and captures the last command's return status in `$?`.

### Act 4: Many Things at Once — Tabs, Shared State & multiWatch
- **Two execution modes, identical behaviour:**
  - `./terminator` (default, **thread mode**): the process hosts one `pthread` per tab.
  - `./terminator useProcess=True` (**process mode**): the GUI is the parent and each tab is a `fork()`ed child.
  - In both modes a tab talks to the GUI through a pair of pipes, so the GUI code is the same.
- **Per-tab state:** `$?`, working directory, input line and output buffer are private. State is thread-local in thread mode (`chdir()` is process-wide, so each tab tracks its own `cwd`, child processes `chdir()` to it before `exec`, and `cd` is resolved against it).
- **Shared state (history, clipboard buffer):**
  - Thread mode: heap memory guarded by a `pthread_mutex_t`.
  - Process mode: `shm_open` + `mmap` guarded by a POSIX named semaphore (`sem_open`). Both names are unlinked immediately, so nothing is left behind.
  - History is a ring of the last 10,000 commands. A command typed in one tab is available in every other tab via `history` and the Up/Down arrows.
- **`exit` is per tab:** it ends that tab's shell loop only, so a thread-mode tab can never take the whole app down.
- **I/O multiplexing:** one `poll()` sleeps on the X11 connection and every tab's output pipe (near 0% CPU when idle).
- **`multiWatch ["cmd1", "cmd2", ...]`:** one `pipe()` + `fork()` per command, each running through the normal shell engine (so pipes and redirection work inside), and a single `poll()` over all read ends. Output is printed as it arrives in the graded format:
  ```text
  "date +%T", 1790860020:
  ----------------------------------------------------
  18:37:00
  ----------------------------------------------------
  ```
  It returns by itself when every command has finished. `Ctrl+C` sends the tab an ETX byte (there is no TTY), and the shell then `kill()`s every child process group and returns to the prompt.
- **`history` built-in:** runs in a child like an external command, so `history | grep make` and `history > file` work.
- **Known limitations:** history is not saved to disk yet; in process mode a tab killed while holding the history semaphore would block the others; `Ctrl+C` outside `multiWatch` only clears the input line until Act 5 adds signal delivery to foreground jobs; the clipboard buffer exists but has no copy/paste keys yet; `;` and `&&` are not supported.

---

## Run

```bash
./terminator                     # thread mode (default)
./terminator useProcess=True     # process mode
```
