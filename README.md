# Terminator Demo

[![C11](https://img.shields.io/badge/Language-C11-00599C?logo=c&logoColor=white)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![X11](https://img.shields.io/badge/GUI-X11%20%2F%20Xlib-CC292B)](https://www.x.org/)
[![POSIX](https://img.shields.io/badge/Standard-POSIX-333333)](https://pubs.opengroup.org/)
[![Concurrency](https://img.shields.io/badge/Concurrency-pthreads%20%7C%20fork-orange)](https://pubs.opengroup.org/onlinepubs/9699919799/)
[![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux-lightgrey?logo=apple&logoColor=white)](https://github.com/)
[![Build](https://img.shields.io/badge/Build-Makefile-008080?logo=gnu&logoColor=white)](Makefile)

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

## Documentation

- **System Design Document:** Detailed architecture, concurrency models, synchronization, data structures, IPC, and signals: [docs/DESIGN.md](docs/DESIGN.md).

---

## Quick Start & Running Terminator

Terminator provides two execution architectures presenting identical behaviors:

```bash
./terminator                     # Thread mode (default, one process with POSIX worker threads)
./terminator useProcess=True     # Process mode (multi-process with forked child tabs)
./terminator --shell             # Headless test mode (runs shell engine on stdin/stdout, no X11)
```

---

## Quick Demo Commands

### 1. Test Script Execution
Run the bundled test script [check.sh](check.sh) natively inside Terminator:
```bash
./check.sh terminal.c            # Tests [ -f ] true branch: compiles myprog
./check.sh missing_file.c        # Tests [ -f ] false branch: reports missing file
```

### 2. Multi-Stage Pipelines & Redirection
```bash
ls -la | grep "terminal" > out.txt
cat < out.txt | wc -l
echo "Files in $(basename $(pwd)): $(ls | wc -l)"
```

### 3. Parallel Live Command Monitor (`multiWatch`)
```bash
multiWatch ["date +%T", "ls | wc -l"]
# Press Ctrl+C to stop all parallel jobs and return cleanly to prompt
```

### 4. No-TTY Signal Delivery & Job Control
```bash
sleep 100 &                      # Starts in background: prints [1] <pid>
jobs                             # Lists running and stopped jobs
fg %1                            # Brings job 1 to foreground
# Press Ctrl+Z to stop foreground job: prints [1]+ Stopped sleep 100
bg %1                            # Resumes job in background: prints [1]+ sleep 100 &
# Press Ctrl+C to interrupt active foreground job
```

### 5. Interactive Reverse History Search (`Ctrl+R`)
Press `Ctrl+R` to prompt:
```text
Enter search term: sleep
```
Returns the most recent match, longest common substring (>2 characters), or `No match for search term in history`.

### 6. Tab Filename Auto-Completion
Type a prefix and press `Tab`:
```text
./check.sh ter[Tab]              # Expands to ./check.sh terminal.c
```
If multiple candidates match, extends to Longest Common Prefix (LCP) and presents a numbered list:
```text
1. terminal.c   2. terminator
# Press 1 or 2 to select
```

### 7. Custom Keybindings (`~/.terminatorrc`)
Custom keybindings can be loaded without recompilation:
```bash
cp sample.terminatorrc ~/.terminatorrc
```

---

### X11 GUI & Multi-Tab Terminal Foundation
- Custom X11 window rendered via Xlib with modern Catppuccin Mocha-inspired color scheme.
- Tab bar with multi-tab management (click `+` or press `Ctrl+T` to create new tabs, click tabs to switch).
- Multiplexed non-blocking event loop using `poll()` to sleep when idle and handle X11 events and tab pipes concurrently.
- Per-tab in-memory text buffer and interactive command-line editing with backspace and cursor.

### Command Execution Engine & Built-ins
- **Command Lifecycle:** Spawns external binaries via `fork()` $\to$ `execvp()` $\to$ `waitpid()`.
- **Built-in `cd`:** Changes directory with `chdir()` directly inside the tab shell, supporting `cd`, `cd ~`, `cd ..`, and relative/absolute paths.
- **Dynamic Tab Titles:** Each tab dynamically updates its title to display its active directory (e.g., `1: terminal_demo`).
- **Built-in `exit`:** Gracefully exits tab process and frees pipe descriptors.
- **Return Status Tracking (`$?`):** Tracks exit codes of commands and built-ins, expanding `$?` expressions (e.g., `echo $?`).
- **Standard Error Integration:** Child `stderr` is multiplexed with `stdout`, routing error diagnostics (such as `command not found`) straight to the terminal window.

### Multiline Input, Quoting & Expansion
- **Read until complete:** if a line ends with `\`, or leaves a `'…'`, `"…"` or `$(…)` open, the tab shows a `> ` continuation prompt and keeps reading. The same `input_status()` check is used by the shell and the GUI so they never disagree.
- **Quoting:** `'single'` is fully literal, `"double"` keeps spaces but still expands `$VAR`, `$?` and `$(…)`, and `\` escapes the next character. `|`, `<`, `>` inside quotes are plain text.
- **Expansion:** `$VAR`, `${VAR}`, `$?`, `$$` and nested `$(…)` (innermost first, run through the same engine in a child process). Unquoted results are word-split; quoted ones are not.
- Example: `echo "Files in $(basename $(pwd)): $(ls | \` ⏎ `> wc -l)"`.
- Not supported: backticks, globbing (`*.txt`), `&&`/`;`.

### Plumbing: I/O Redirection & Pipelines
- **I/O Redirection (`<`, `>`, `>>`):** Rewired in child processes via `open()` and `dup2()` without affecting the shell descriptors. Operators work with or without spaces (`sort<in.txt>out.txt`); a missing target reports `syntax error`, an unopenable file reports the `errno` message and sets `$?` to 1, and a bare `> file` creates/truncates the file.
- **Headless test mode:** `./terminator --shell` runs only the shell engine on stdin/stdout (no X11), e.g. `printf 'sort < in.txt > out.txt\n' | ./terminator --shell`.
- **Multi-Stage Pipelines (`cmd1 | cmd2 | ... | cmdN`):** Creates $N - 1$ pipes for $N$ stages, properly wiring each command's stdout to the next command's stdin.
- **Pipe Closure Rule:** Unused pipe ends are closed in each child and in the parent shell so readers receive `EOF`.
- **Process Group Alignment (`setpgid`):** All pipeline children share a process group for unified signal dispatching.
- **Reaping & Status:** Waits for all pipeline stages and captures the last command's return status in `$?`.

### Multi-Tab Concurrency: Tabs, Shared State & multiWatch
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
- **Known limitations:** in process mode a tab killed while holding the history semaphore would block the others; `;` and `&&` are not supported.

### Signals & Job Control
There is no TTY, so the kernel never turns Ctrl+C / Ctrl+Z into signals. The GUI sees the `KeyPress`, finds the active tab's foreground job and `kill()`s that job's **process group**. The shell publishes each tab's foreground process group in the shared state (one atomic `int` per tab, so it works in both thread and process mode without a lock).

| Input | From | To | Signal |
| :--- | :--- | :--- | :--- |
| `Ctrl+C` | Foreground | Ended | `SIGINT` to `-pgid` (sent by the GUI) |
| `Ctrl+Z` | Foreground | Stopped | `SIGTSTP` to `-pgid` (sent by the GUI) |
| `bg %n` | Stopped | Background | `SIGCONT` |
| `fg %n` | Stopped or background | Foreground | `SIGCONT` |
| `cmd &` | (new) | Background | none |

- **A job is one process group**, so a single signal reaches every stage of a pipeline (`sleep 100 | cat` stops and dies as one).
- **Isolation:** only the active tab's foreground group is ever signalled. The GUI, other tabs and background jobs never receive it, and the GUI refuses to signal its own group. With nothing running, `Ctrl+C` just abandons the typed line (and still cancels `multiWatch`); `Ctrl+Z` does nothing.
- **Foreground wait:** `waitpid(-pgid, …, WUNTRACED)` per job, so a stop is noticed as well as an exit. A stopped job goes into the tab's job table and prints `[1]+ Stopped sleep 100`; `$?` is `130` after `Ctrl+C` and `128+SIGTSTP` after `Ctrl+Z`.
- **Built-ins** (run inside the shell, like `cd`): `fg [%n]`, `bg [%n]` (print `sleep 100` / `[1]+ sleep 100 &`). `jobs` prints `[1]+  Stopped   sleep 100` and, like `history`, runs in a child so it can be piped. A bare `fg`/`bg` picks the most recent job.
- **`cmd &`:** starts the job in its own group, prints `[1] <pid>` and returns to the prompt at once. Its stdin is `/dev/null` so it cannot steal the tab's keyboard. Finished jobs are announced before the next prompt (`[1]+ Done sleep 1`, `Exit N`, `Terminated`). A trailing `&` is the only supported form; `&` elsewhere is a syntax error, and a quoted `'&'` stays literal.
- **Per-tab job table:** private to each tab (thread-local), as required by the sharing rules. Closing a tab hangs up whatever it left running or stopped.
- **Headless testing:** in `./terminator --shell` there is no GUI, so `SIGINT`/`SIGTSTP` sent to the process are forwarded to the foreground job and the shell carries on. This is how the job control can be exercised from a script.
- **Known limitations:** background-job completion is announced when the next line is entered (not asynchronously); no `kill %n`, `wait` or `disown`; at most 64 jobs per tab; `2>&1`-style descriptor duplication is not supported.

### Bash-Style Shell: Hotkeys, Persistent History & Search

#### Bash-style Line Editing & Hotkeys
- **Cursor navigation & editing:** in-line cursor movement (`input_pos`), rendering a block cursor with inverted character highlighting at the cursor position.
- **Default Keybindings:**

| Action | Default Key | Description |
| :--- | :--- | :--- |
| Move in line | `←` / `→` | Move cursor backward / forward |
| Line bounds | `Ctrl+A` / `Ctrl+E` | Jump to start / end of line |
| History navigation | `↑` / `↓` | Recall previous / next commands from shared history |
| Delete to bounds | `Ctrl+U` / `Ctrl+K` | Delete from cursor to start / end of line |
| Delete word | `Ctrl+W` | Delete word before cursor |
| Clear screen | `Ctrl+L` | Clear tab display buffer |
| Close tab / EOF | `Ctrl+D` | Send EOF / close tab (or delete char under cursor) |
| Clipboard | `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy current line to / paste from shared clipboard |
| Tab management | `Ctrl+T` | Create new tab |
| Signals | `Ctrl+C` / `Ctrl+Z` | Interrupt / suspend foreground job |

#### Keybinding Configuration (`~/.terminatorrc`)
Custom bindings survive restarts and can be reconfigured without recompiling via `~/.terminatorrc` (or `$TERMINATOR_RC`):
```bash
# ~/.terminatorrc
bind clear-screen Ctrl+Shift+L
bind delete-word Alt+Backspace
```
- Invalid bindings warn on `stderr` and preserve default bindings.

#### Persistent History & Reverse Search
- **Persistent storage:** history is saved to `~/.terminator_history` (or `$TERMINATOR_HISTFILE`), storing up to 10,000 commands across restarts.
- **Shared across tabs:** commands entered in any tab are immediately appended to disk and visible to all tabs (both thread and process modes).
- **`Ctrl+R` Reverse Search:** prompts `Enter search term: ` with prioritized matching rules:
  1. Exact match: displays the most recent matching command.
  2. Longest common substring match: displays commands sharing the longest substring if longer than 2 characters.
  3. Otherwise displays `No match for search term in history`.

#### Tab Filename Auto-Completion
- **Directory-aware matching:** pressing `Tab` scans the current directory for filenames matching the word before the cursor.
- **Completion rules:**
  1. Single match: completes filename immediately, preserving the rest of the command line.
  2. Several matches: extends input to the longest common prefix (LCP), then displays a numbered list of candidates and waits for a numeric selection (`1`..`9`).
  3. No match: does nothing.

#### Shell Scripting Engine & Control Flow
- **Native interpreter:** executes scripts directly within Terminator without delegating to `/bin/bash`.
- **Script invocation:** supports `./run.sh`, `bash run.sh`, `sh run.sh`, and `source run.sh` (or `. run.sh`).
- **Variables & parameters:** supports variable assignment (`VAR=val`), expansion (`$VAR`, `${VAR}`), positional arguments (`$1`..`$9`, `$#`, `$0`), and command status (`$?`).
- **Conditional execution:** nested `if / elif / else / fi` blocks with built-in `[` test supporting file checks (`-f`, `-d`, `-e`), string comparisons (`=`, `!=`, `-z`, `-n`), and integer comparisons (`-eq`, `-ne`, `-lt`, `-le`, `-gt`, `-ge`).
- **Non-fatal script exit:** `exit n` inside a script terminates only the script execution with status `n` without closing the tab or shell session.

