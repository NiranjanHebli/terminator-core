# Terminator: Custom X11 Terminal Emulator & POSIX Shell

[![C11](https://img.shields.io/badge/Language-C11-00599C?logo=c&logoColor=white)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![X11](https://img.shields.io/badge/GUI-X11%20%2F%20Xlib-CC292B)](https://www.x.org/)
[![POSIX](https://img.shields.io/badge/Standard-POSIX-333333)](https://pubs.opengroup.org/)
[![Concurrency](https://img.shields.io/badge/Concurrency-pthreads%20%7C%20fork-orange)](https://pubs.opengroup.org/onlinepubs/9699919799/)
[![Platform](https://img.shields.io/badge/Platform-macOS%20%7C%20Linux-lightgrey?logo=apple&logoColor=white)](https://github.com/)
[![Build](https://img.shields.io/badge/Build-Makefile-008080?logo=gnu&logoColor=white)](Makefile)

A custom X11 graphical terminal emulator and command shell built from scratch in C11 using raw Xlib graphics and POSIX system calls.

---

## Table of Contents

1. [Background](#1-background)
2. [Technology Stack](#2-technology-stack)
3. [System Architecture](#3-system-architecture)
4. [Screenshots](#4-screenshots)
5. [Demo Video](#5-demo-video)
6. [Features](#6-features)
7. [Use Cases](#7-use-cases)
8. [Prerequisites & Installation](#8-prerequisites--installation)
9. [Quick Start & Execution Modes](#9-quick-start--execution-modes)
10. [Interactive Demo Commands](#10-interactive-demo-commands)
11. [Keybindings & Configuration](#11-keybindings--configuration)
12. [Known Limitations & Future Scope](#12-known-limitations--future-scope)

---

## 1. Background

In modern Unix systems, a terminal session involves two distinct components: a **graphical terminal emulator** (which manages windows, fonts, scrollback buffers, and keyboard/mouse events) and a **command-line shell** (which parses commands, expands variables, wires pipelines, and manages process groups). Normally, these two layers communicate through a kernel pseudo-terminal (`pty`) driver.

**Terminator** unifies both systems into a single cohesive architecture built from the ground up without high-level GUI toolkits (no GTK, Qt, or ncurses) and without delegating command execution to `/bin/sh` or `/bin/bash`. Because there is no kernel `pty` line discipline between the GUI and the shell, Terminator implements its own non-blocking `poll()` event multiplexer, inter-process pipe plumbing, process group management, and signal delivery subsystem directly in C.

Furthermore, Terminator is designed as a dual-concurrency system: it can run every terminal tab either as a lightweight **POSIX thread (`pthread`)** sharing heap memory or as an isolated **forked child process (`fork`)** synchronizing state across process boundaries via **POSIX shared memory (`shm_open` / `mmap`)** and **named semaphores (`sem_open`)**.

---

## 2. Technology Stack

| Layer | Technology / API | Purpose |
| :--- | :--- | :--- |
| **Core Language** | C11 (`-std=c11 -Wall -Wextra`) | Low-level systems programming, memory management, and atomic operations (`_Atomic`) |
| **Windowing & Graphics** | X11 / Xlib (`<X11/Xlib.h>`, `<X11/Xutil.h>`) | Window creation, custom Catppuccin rendering, font metrics, keyboard and mouse wheel event handling |
| **I/O Multiplexing** | POSIX `poll()` (`<poll.h>`) | Zero-spin sleeping event loop monitoring the X11 socket and all tab output pipes simultaneously |
| **Concurrency (Mode 1)** | POSIX Threads (`<pthread.h>`) | Multi-threaded tab workers (`pthread_create`), thread-local storage (`_Thread_local`), and recursive mutexes |
| **Concurrency (Mode 2)** | Process Management (`<unistd.h>`, `<sys/wait.h>`) | Multi-process tab isolation (`fork`, `execvp`, `waitpid`, `setpgid`) |
| **Shared Memory & IPC** | POSIX IPC (`<sys/mman.h>`, `<semaphore.h>`) | Cross-process state sharing (`shm_open`, `ftruncate`, `mmap`) and synchronization (`sem_open`, `sem_wait`, `sem_post`) |
| **Signal Subsystem** | POSIX Signals (`<signal.h>`) | Custom no-TTY signal routing (`kill(-pgid, sig)`, `sigaction`, `SIGINT`, `SIGTSTP`, `SIGCONT`) |
| **Build System** | GNU Make ([Makefile](Makefile)) | Automated OS detection (macOS XQuartz vs. Linux X11) and compiler flag configuration |

---

## 3. System Architecture

Terminator strictly decouples the **X11 GUI Frontend** from the **Shell Execution Engine**. Each tab communicates with the GUI strictly over unidirectional Unix pipes (`in_fd` and `out_fd`), allowing the exact same shell engine to power Thread Mode, Process Mode, and Headless CLI Mode (`--shell`).

For deep-dive architectural diagrams, state transition graphs, and data structure layouts, see the [System Design Document (docs/DESIGN.md)](docs/DESIGN.md).

---

## 4. Screenshots

### Starship-Style Status Bar & Live Syntax Highlighting
Renders active Git branch, path abbreviation, command duration, and real-time syntax coloring:

```text
+---------------------------------------------------------------------------------------+
| 1: tab  |  +                                                                          |
|---------------------------------------------------------------------------------------|
|  ~/Desktop/terminal_demo | on main --------------------- took 12ms | at 03:45:10 PM   |
|  user@terminator> ls -la                                                              |
+---------------------------------------------------------------------------------------+
```
*(Insert screenshots here, e.g., `![Status Bar](docs/screenshots/status_bar.png)`)*

### Multi-Tab Workspace & Smooth Scrollback
Demonstrates concurrent tabs, background unread badges, mouse wheel scrollback, and floating indicator badge (`[+ N lines]`):

```text
+---------------------------------------------------------------------------------------+
| 1: tab  | 2: tab [•] |  +                                               [+ 10 lines]  |
|---------------------------------------------------------------------------------------|
|  [Historical command output and preserved scrollback status bars]                     |
|                                                                                       |
+---------------------------------------------------------------------------------------+
```
*(Insert screenshots here, e.g., `![Scrollback and Tabs](docs/screenshots/tabs_scroll.png)`)*

### Parallel Multi-Command Monitor (`multiWatch`)
Timestamped concurrent execution of multiple shell pipelines:

```text
+---------------------------------------------------------------------------------------+
|  user@terminator> multiWatch ["date +%T", "ls | wc -l"]                               |
|  "date +%T", 1790860020:                                                              |
|  ----------------------------------------------------                                 |
|  18:37:00                                                                             |
|  ----------------------------------------------------                                 |
+---------------------------------------------------------------------------------------+
```
*(Insert screenshots here, e.g., `![multiWatch](docs/screenshots/multiwatch.png)`)*

---

## 5. Demo Video

Watch the complete feature walkthrough and live presentation of Terminator in action:

- **Live Demonstration Video:** [Watch Video Demonstration](https://youtu.be/placeholder) *(Replace with your video link or local embed)*
- **Demo Script & Talk Track:** Follow along with the verbatim 5-minute presentation script in [assignment/LIVE_DEMO_5MIN.md](assignment/LIVE_DEMO_5MIN.md) and the comprehensive test checklist in [assignment/demo.txt](assignment/demo.txt).

---

## 6. Features

### 6.1 Graphical Terminal Interface & UX
- **Catppuccin Mocha Theme & Starship Status Bar:** Renders a two-pill status bar above every prompt showing the abbreviated working directory (`~/path`), active Git branch (`| on main`), execution duration (`took 12ms`), exit status/interrupt badges (`ERR` / `INT`), and completion timestamp (`at 03:45:10 PM`).
- **Multi-Tab Workspace:** Create up to 16 concurrent tabs (`Ctrl+T` or click `+`), switch tabs via mouse click, and close individual tabs (`exit` or `Ctrl+D`) without affecting other sessions.
- **Native Smooth Scrollback & Mouse Wheel Support:** Browse up to 1,000 lines of historical output (including preserved historical status bars) per tab using the mouse wheel (`Button4`/`Button5`) or `Shift+PageUp`/`Shift+PageDown`, complete with a proportional scrollbar thumb and a `[+ N lines]` indicator badge in the tab header.
- **Live Syntax Highlighting & Fish-Style Ghost Autosuggestions:** Highlights valid commands in green, unrecognized commands in red, and quoted strings in peach as you type. Suggests matching historical commands inline in dimmed text ahead of the cursor (accept with `Right Arrow`).
- **Background Tab Activity Badges & Bell:** Displays an unread notification dot on background tabs and rings the X11 bell (`XBell`) when hidden jobs produce output or finish running.

### 6.2 Shell Execution, Quoting & Plumbing
- **External Commands & Built-ins:** Full `fork()` -> `execvp()` -> `waitpid()` lifecycle alongside built-in commands (`cd`, `exit`, `jobs`, `fg`, `bg`, `history`, `multiWatch`, `source`, `[`).
- **Multiline Input, Quoting & Expansion:** Supports single quotes (`'...'`), double quotes (`"..."`), backslash escaping (`\`), variable expansion (`$VAR`, `${VAR}`, `$?`, `$$`), and recursive nested command substitution (`$(...)`). Incomplete lines automatically prompt with `> `.
- **I/O Redirection (`<`, `>`, `>>`):** Rewires file descriptors in forked child processes using `open()` and `dup2()` without mutating parent shell descriptors.
- **Multi-Stage Pipelines (`cmd1 | cmd2 | ... | cmdN`):** Chains arbitrary-length pipelines across $N-1$ Unix pipe pairs, aligns all stages into a single process group (`setpgid`), closes unused descriptors to guarantee clean `EOF` propagation, and records the final stage's exit code in `$?`.

### 6.3 Dual Concurrency & Parallel Execution
- **Thread Mode vs. Process Mode:** Switch seamlessly at launch between single-process `pthread` tabs (`./terminator --thread`) and fault-isolated `fork` tabs (`./terminator --process`) backed by `mmap` shared memory.
- **Parallel Multi-Command Monitor (`multiWatch`):** Executes multiple commands or pipelines in parallel (`multiWatch ["cmd1", "cmd2", ...]`), multiplexing live output streams via `poll()` with real-time timestamps until interrupted via `Ctrl+C`.

### 6.4 No-TTY Signal Handling & Job Control
- **Process Group Signal Routing:** Because there is no kernel `pty` to translate keystrokes into signals, the GUI intercepts `Ctrl+C` and `Ctrl+Z`, looks up the active tab's atomic foreground process group (`fg_pgid`), and dispatches `SIGINT` or `SIGTSTP` to `-pgid`.
- **Interactive Job Management:** Supports background execution (`cmd &` with `/dev/null` stdin redirection), job listing (`jobs`), foreground resumption (`fg %n`), and background resumption (`bg %n`).

### 6.5 Bash-Style Productivity & Scripting
- **Persistent Shared History & Reverse Search (`Ctrl+R`):** Persists 10,000 commands across restarts in `~/.terminator_history`, synchronized immediately across all tabs. Includes `Ctrl+R` reverse search with exact and longest-common-substring (>2 chars) matching.
- **Interactive Tab Filename Completion:** Completes unique filenames inline on `Tab`, or expands to the Longest Common Prefix (LCP) and presents an interactive numbered selection menu (`1..9`) when multiple files match.
- **Native Shell Scripting Engine:** Executes `.sh` scripts natively (`./check.sh`, `bash script.sh`, `source script.sh`) with positional arguments (`$1`..`$9`, `$#`), local variables, nested `if / elif / else / fi` control flow, and `[` test evaluations (`-f`, `-d`, `-e`, `=`, `!=`, `-eq`, `-lt`, etc.).
- **"Did You Mean?" Typo Suggestion Engine:** Uses Damerau-Levenshtein edit distance to suggest corrections for mistyped commands (`sl` -> `ls`) and subcommands (`git cmmit` -> `git commit`).

---

## 7. Use Cases

1. **Systems Programming & OS Education:** Demonstrates practical POSIX kernel interfaces—process groups, signals without a TTY, anonymous/named shared memory, semaphores, thread-local state, and non-blocking I/O multiplexing—in a single readable codebase.
2. **Real-Time Multi-Stream Monitoring:** Using `multiWatch`, developers can monitor multiple log files, system metrics, or build directories simultaneously inside one tab with timestamped output blocks.
3. **Fault-Isolated Command Execution:** Running in Process Mode (`--process`) ensures that experimental workloads or crashes in one tab are completely isolated at the OS process level from the GUI and sibling tabs.
4. **Automated Headless Testing & CI Pipelines:** Using `--shell`, the command engine can execute scripts and complex pipelines in headless environments or automated grading harnesses without requiring an X11 server.

---

## 8. Prerequisites & Installation

### System Requirements
- **macOS:** Install [XQuartz](https://www.xquartz.org/) and `libx11` via Homebrew:
  ```bash
  brew install libx11
  open -a XQuartz
  export DISPLAY=:0
  ```
- **Linux (Ubuntu / Debian):** Install X11 development headers:
  ```bash
  sudo apt-get update && sudo apt-get install -y build-essential libx11-dev
  ```

### Building from Source
Compile the binary using [Makefile](Makefile):
```bash
make
```
To remove compiled artifacts:
```bash
make clean
```

---

## 9. Quick Start & Execution Modes

```bash
# 1. Default Thread Mode (each tab runs as a POSIX thread in a single process):
./terminator
./terminator --thread            # (or -t / useProcess=False)

# 2. Process Mode (each tab runs as an isolated forked child process):
./terminator --process           # (or -p / useProcess=True)

# 3. Headless Shell Mode (runs only the shell engine on stdin/stdout, no X11):
./terminator --shell

# 4. Shared History & CLI Help:
./terminator --history           # Print shared command history across all tabs
./terminator --help              # Display launch options
```

---

## 10. Interactive Demo Commands

### 1. Native Shell Script Execution
```bash
./check.sh terminal.c            # Tests [ -f ] true branch: reports file exists
./check.sh missing_file.c        # Tests [ -f ] false branch: reports missing file
```

### 2. Multi-Stage Pipelines, Redirection & Substitution
```bash
ls -la | grep "terminal" > out.txt
cat < out.txt | wc -l
echo "Files in $(basename $(pwd)): $(ls | wc -l)"
```

### 3. Parallel Live Command Monitor (`multiWatch`)
```bash
multiWatch ["date +%T", "ls | wc -l"]
# Press Ctrl+C to stop all parallel jobs and return cleanly to the prompt
```

### 4. Signal Delivery & Job Control
```bash
sleep 100 &                      # Starts in background: prints [1] <pid>
jobs                             # Lists running and stopped jobs
fg %1                            # Brings job 1 to foreground
# Press Ctrl+Z to suspend foreground job: prints [1]+ Stopped sleep 100
bg %1                            # Resumes job in background: prints [1]+ sleep 100 &
```

### 5. Reverse History Search (`Ctrl+R`) & Auto-Completion (`Tab`)
- Press `Ctrl+R` and type a search query (e.g., `sleep`) to recall matching commands.
- Type `./check.sh ter` and press `Tab` to auto-complete filenames or pick from a numbered candidate list.

---

## 11. Keybindings & Configuration

### Default Keybindings

| Action | Default Key | Description |
| :--- | :--- | :--- |
| **Cursor Movement** | `Left` / `Right` | Move cursor backward / forward (or accept ghost suggestion at EOL) |
| **Line Bounds** | `Ctrl+A` / `Ctrl+E` | Jump to start / end of input line |
| **History Navigation** | `Up` / `Down` | Recall previous / next command from shared history |
| **Reverse Search** | `Ctrl+R` | Search command history interactively |
| **Delete to Bounds** | `Ctrl+U` / `Ctrl+K` | Delete from cursor to start / end of line |
| **Delete Word** | `Ctrl+W` | Delete word before cursor |
| **Clear Screen** | `Ctrl+L` | Clear active tab display buffer |
| **Close Tab / EOF** | `Ctrl+D` | Close current tab (on empty line) or delete character under cursor |
| **Clipboard** | `Ctrl+Shift+C` / `Ctrl+Shift+V` | Copy current input line to / paste from shared clipboard |
| **New Tab** | `Ctrl+T` | Spawn a new terminal tab |
| **Job Signals** | `Ctrl+C` / `Ctrl+Z` | Send `SIGINT` (interrupt) / `SIGTSTP` (suspend) to foreground process group |
| **Scrollback (Keys)** | `Shift+PageUp` / `Shift+PageDown` | Scroll viewport up / down 10 lines through history |
| **Scrollback (Mouse)** | `Wheel Up` / `Wheel Down` | Scroll viewport up / down 3 lines per tick |

### Custom Keybindings (`~/.terminatorrc`)
Keybindings can be customized without recompiling by copying [sample.terminatorrc](sample.terminatorrc) to `~/.terminatorrc` (or setting `$TERMINATOR_RC`):
```bash
cp sample.terminatorrc ~/.terminatorrc
```
Example configuration rules:
```text
bind clear-screen Ctrl+Shift+L
bind delete-word Alt+Backspace
bind scroll-up Shift+PageUp
bind scroll-down Shift+PageDown
```

---

## 12. Known Limitations & Future Scope

### Known Limitations
- **No Pseudo-Terminal (`pty`) Emulation:** Because communication uses standard Unix pipes rather than a kernel `pty`, full-screen curses applications (`vim`, `htop`, `less`) that require terminal capability queries (`ioctl` / `termios`) are not intended to run inside the graphical window.
- **Logical Operators & Globbing:** Chained logical operators (`&&`, `||`, `;`), filename wildcard expansion (`*.c`), and descriptor duplication (`2>&1`) are currently omitted by design.
- **Semaphore Recovery in Process Mode:** If an external signal forcefully terminates a tab process (`SIGKILL`) while it holds the named history semaphore, other tabs waiting on history lock acquisition may block.

### Future Scope
1. **Pseudo-Terminal (`posix_openpt` / `forkpty`) Integration:** Add optional `pty` allocation alongside full ANSI/VT100 escape sequence parsing to support interactive TUI applications like `vim`, `nano`, and `htop`.
2. **Split Panes & Multiplexed Layouts:** Extend the X11 viewport renderer to support horizontal and vertical split panes within a single tab.
3. **Wildcard Globbing & Logical Chaining:** Extend the lexer to expand `*` / `?` globs via `glob(3)` and construct AST nodes for `&&`, `||`, and `;` command lists.
4. **Mouse Selection & System Clipboard Integration:** Add click-and-drag text selection in the X11 viewport wired directly to the `XA_CLIPBOARD` X11 selection atom.
