# Xinu (RAZ Extended Edition) User's Manual

This manual describes how to use an extended Embedded Xinu that adds:

- a hierarchical filesystem **XFS** (on a RAM disk),
- a tiny built-in C compiler **cc** and bytecode VM **a.out**,
- an actor-oriented small language **ABCL/c+** and its C translator **abclc**,
- a window manager **wm** (a framebuffer GUI on ARM/QEMU),
- Unix-like shell extensions (`ls` / `cd` / `cat` / `make` / `edit`, …).

All of the stock Embedded Xinu facilities (threads, semaphores, networking,
etc.) remain available.

---

## Contents

1. Introduction
2. Booting
3. Using the shell
4. XFS — the filesystem
5. cc / run — the C compiler and launcher
6. abclc — the ABCL/c+ actor language
7. make — the minimal build system
8. edit — the built-in editor
9. wm — the window manager
10. Sample programs in detail
11. Development workflow
12. Troubleshooting

---

## 1. Introduction

Xinu is a small UNIX-like OS originally written by Douglas Comer for teaching.
This extended edition adds a *self-contained workbench that can edit, compile,
and run C / ABCL source on the OS itself* — so you can close the whole
develop-build-run loop inside QEMU (or on real hardware) without relying on a
host cross-compiler.

Supported platforms:

| Platform name | Use | Notes |
| --- | --- | --- |
| `arm-qemu` | QEMU `-M versatilepb` | UART shell + framebuffer + WM auto-start |
| `arm-rpi`  | Raspberry Pi hardware | UART / HDMI / USB keyboard |

XFS / cc / abclc / make / edit behave identically on every platform. The
WM-related pieces (`rotlines`, `wm_line`, …) are only active on `arm-qemu`.

---

## 2. Booting

### 2.1 Build

```sh
cd compile
make PLATFORM=arm-qemu        # default; arm-rpi also works
```

On success this produces `compile/xinu.boot`.

### 2.2 Run under QEMU

If your host is macOS/Linux with `qemu-system-arm` installed, the bundled
wrapper scripts are the easiest way in.

#### Console-only mode (no framebuffer)

```sh
./compile/run-console.sh
```

- The UART shell (`xsh$`) is multiplexed onto the terminal.
- Quit with **Ctrl-A → x**.
- Env vars: `XINU_MEM` (default `128M`), `XINU_PLATFORM` (default `arm-qemu`).

#### Window mode (with the WM demo)

```sh
./compile/run-window.sh
```

- The LCD framebuffer appears in a macOS Cocoa window.
- The UART shell runs in the launching terminal. `-serial mon:stdio` keeps the
  QEMU monitor alongside it.
- Quit by closing the window or **Ctrl-A → x** in the terminal.
- The window scales smoothly up to 4× by dragging a corner (zoom-to-fit).

### 2.3 Boot sequence

The `main()` thread in `system/main.c` does, in order:

1. Print the OS version and memory layout.
2. `xfsBootstrap()` — format `RAMDISK0` as XFS and mount it at `/`.
3. Seed `/home/hello.c`, `/home/sum.c`, `/home/abclcp/abclc/PingPong.abcl`,
   `/home/abclcp/abclc/RotLines.abcl`.
4. Open the network device (when `NETHER` is enabled).
5. Open CONSOLE / TTY1.
6. On `arm-qemu` only, `create()` the `wm_main` thread.
7. Start a shell thread per CONSOLE / TTY1.

When boot finishes, the prompt

```
xsh$
```

means you can type commands.

---

## 3. Using the shell

### 3.1 Basics

```
command [args...]    [< input-redirect] [> output-redirect] [&]
```

- `&` runs in the background.
- `<` `>` redirect to/from I/O devices.
- There is no completion or history.

### 3.2 Command list (added/extended by this edition)

| Command | Usage | Description |
| --- | --- | --- |
| `pwd` | `pwd` | Print the current directory |
| `cd` | `cd [PATH]` | Change directory (default `/`) |
| `ls` | `ls [-l] [PATH]` | Directory listing; `-l` shows type/size |
| `cat` | `cat FILE...` | Print file contents |
| `mkdir` | `mkdir DIR...` | Create directories |
| `rmdir` | `rmdir DIR...` | Remove empty directories |
| `touch` | `touch FILE...` | Create empty file / update mtime |
| `rm` | `rm FILE...` | Remove files |
| `cp` | `cp SRC DST` | Copy (DST is truncated) |
| `mv` | `mv SRC DST` | Rename / move |
| `write` | `write FILE TEXT...` | Join args with spaces + newline, write to FILE |
| `edit` | `edit FILE` | Built-in Emacs-like editor |
| `mkfs` | `mkfs DEVICE [VOL]` | Format a block device as XFS |
| `mount` | `mount DEV MNT` | Mount an XFS |
| `umount` | `umount MNT` | Unmount an XFS |
| `cc` | `cc SRC [-o OUT]` | Compile C source to a.out |
| `abclc` | `abclc SRC.abcl [-o OUT]` | ABCL → C → a.out in one step |
| `make` | `make [TARGET...]` | Run the Makefile in the current directory |
| `run` | `run PATH` | Execute an a.out |
| `rotlines` | `rotlines [FRAMES] [DEG/F]` | Draw rotating lines on the WM |
| `clear` | `clear` | Clear the screen |
| `sleep` | `sleep N` | Sleep N ms |

#### Implicit `run`

If you type a command the shell does not recognise, it tries
`aoutRun(<that name>)` as a last resort (around `shell/shell.c:332`). So after
`cc hello.c -o hello` you can launch a program directly by its path-like name:

```
xsh$ hello
hello...
```

---

## 4. XFS — the filesystem

### 4.1 Overview

A 4 KB-block hierarchical filesystem defined in `include/xfs.h`. It is created
on a 16 MB RAM disk (`RAMDISK0`) at boot and mounted at `/`.

| Item | Value |
| --- | --- |
| Block size | 4096 B |
| Max filename length | 56 B |
| inode size | 128 B |
| Direct blocks | 12 (= 48 KB) |
| Single indirect | 1024 blocks (≈ 4 MB) |
| Double indirect | yes |
| Magic | `XFS!` (0x58465321) |

Directories store `struct xdirent[]` (64 B per entry) inside the inode and are
read sequentially with `xfsReaddir()`.

### 4.2 Thread-local CWD

`xfsChdir()` / `xfsGetcwd()` keep a per-thread absolute-path CWD. `cd /home` in
shell A does not affect shell B.

### 4.3 Bootstrap

`xfsBootstrap()` does:

1. Check the first 64 KB of `RAMDISK0` for the XFS magic; if absent, initialise
   with `xfsMkfs("RAMDISK0", "xfs")`.
2. Mount `RAMDISK0` at `/`.
3. Prepare the root inode.

### 4.4 Main API (from C programs)

```c
int fd = xfsOpen("/home/foo", XFS_O_RDWR | XFS_O_CREAT | XFS_O_TRUNC);
xfsWrite(fd, buf, n);
xfsRead (fd, buf, n);
xfsSeek (fd, 0, XFS_SEEK_SET);
xfsClose(fd);

xfsMkdir("/home/sub");
xfsRename("/home/old", "/home/new");
xfsUnlink("/home/foo");
```

### 4.5 RAM-disk persistence

RAMDISK0 is, as the name says, RAM. **Files disappear when QEMU exits.** Back up
anything you want to keep on the host (copy/paste, `tftp`, keep pre-build
material on the host side, etc.).

---

## 5. cc / run — the C compiler and launcher

### 5.1 Supported C subset

Documented at the top of `system/cc.c`:

- one source file, one `int main(void)` plus argument-less helper functions;
- declarations: only `int x;` and `int x = expr;` (`char` etc. are recognised as
  type keywords only);
- statements: `if/else`, `while`, `for`, `return`, blocks, expression statements;
- expressions: integers, strings, identifiers, `+ - * / %`,
  `== != < <= > >=`, `! && ||`, unary `-`, parentheses;
- only built-in functions may be called;
- `#include <...>` / `"..."` are accepted and ignored.

Unsupported: structs, pointers, arrays, floating point, multiple files.

### 5.2 Built-in functions

Numbered in `include/aout.h`:

| BI | Function | Use |
| --- | --- | --- |
| 0 | `printf(fmt, ...)` | string / integer formatting |
| 1 | `puts(s)` | string + newline |
| 2 | `putchar(c)` | one character |
| 3 | `getchar()` | one character of input |
| 4 | `exit(code)` | terminate |
| 5 | `rgb(r,g,b)` | BGR565 16-bit colour value |
| 6 | `wm_line(idx,x1,y1,x2,y2,color)` | set WM user-line slot (0..7) |
| 7 | `wm_render(on)` | enable/disable user-line rendering |
| 8 | `wm_clear()` | clear user lines |
| 9 | `sleep_ms(ms)` | sleep |
| 10/11 | `isin(deg)` / `icos(deg)` | Q12 fixed point (4096 = 1.0) |
| 12/13 | `screen_w()` / `screen_h()` | screen size |

### 5.3 a.out format

Described by `struct aout_header` in `include/aout.h`:

```
+--------------------------+
| magic = "XAOU"           |
| version = 1              |
| code_size                |
| const_size               |
| entry                    |
| nlocals (main's locals)  |
+--------------------------+
| bytecode body            |
+--------------------------+
| string constant pool     |
| (NUL-terminated)         |
+--------------------------+
```

The VM is stack-based (256 entries) with one-byte opcodes such as `OP_PUSH_I32`,
`OP_LOAD_LOC`, `OP_ADD`, `OP_JZ`, `OP_CALL_BI`. See `aoutRun()` in
`system/aout.c`.

### 5.4 Shell usage

```
xsh$ cc /home/hello.c -o /home/hello
xsh$ run /home/hello
hello...
xsh$ /home/hello              # implicit run also works
hello...
```

---

## 6. abclc — the ABCL/c+ actor language

### 6.1 Language scope (this implementation)

ABCL/c+ is a small actor-oriented language built from "classes + message
sends". Here it is a two-stage pipeline: **abclc translates to C, and cc
compiles that C to bytecode**.

```
class ClassName {
    var field1;
    var field2;

    method methodName(p1, p2) {
        // statements: assignment, if/else, while, printf, send
        field1 = p1 + 1;
        send other.methodName(arg1, arg2);
    }
}

main {
    new ClassName a;
    new ClassName b;
    send a.methodName(b, 10);
}
```

- Implicit identifiers: `self` (current actor id), `sender` (sender id).
- At most one `send` per method (tail-call style), so each actor processes one
  message at a time, sequentially.
- All values are `int`-equivalent.
- Limits: 4 classes, 16 methods per class, 8 fields, 8 arguments (caps of the
  generated C runtime).

### 6.2 Shell usage

```
xsh$ cd /home/abclcp/abclc
xsh$ abclc PingPong.abcl
abclc: translated PingPong.abcl -> PingPong.c
abclc: compiled PingPong.c -> PingPong
xsh$ run PingPong
```

With `-o NAME`, both `NAME.c` (intermediate C) and `NAME` (a.out) are written at
that path.

### 6.3 Runtime

The C that abclc emits uses only cc's built-ins (`printf`, etc.) and a few
scheduling helpers. Execution is one message per time-slice; after a `send` the
message is queued. The WM-linked ABCL built-ins (`wm_line`, `isin`, `icos`,
`sleep_ms`, `rgb`, …) share names with cc's built-ins and can be called directly
from ABCL (see RotLines.abcl).

---

## 7. make — the minimal build system

Behaviour of `shell/xsh_make.c`:

1. If the CWD has no `Makefile`, scan `*.c` and `*.abcl` and auto-generate one.
2. Read the `Makefile` and build each target named in `TARGETS = ...` (or in
   rule-declaration order).
3. `.c` → `cc SRC -o TARGET`; `.abcl` → `abclc SRC -o TARGET`.

### 7.1 Makefile format

```
# comment
TARGETS = hello sum PingPong RotLines

hello:    hello.c
sum:      sum.c
PingPong: PingPong.abcl
RotLines: RotLines.abcl
```

Targets with no explicit rule are inferred by checking for `TARGET.c` then
`TARGET.abcl`.

### 7.2 Example

```
xsh$ cd /home
xsh$ ls
hello.c
sum.c
xsh$ make
make: generated Makefile (2 .c, 0 .abcl)
    cc hello.c -o hello
    cc sum.c -o sum
make: built 2 target(s)
xsh$ ./hello
hello...
```

---

## 8. edit — the built-in editor

`shell/xsh_edit.c` is an Emacs-like modeless editor, up to 400 lines × 256
characters.

### 8.1 Launch

```
xsh$ edit /home/hello.c
```

A non-existent path opens a new buffer.

### 8.2 Key bindings

| Key | Action |
| --- | --- |
| `C-f` / `C-b` | forward / back one character |
| `C-n` / `C-p` | down / up one line |
| `C-a` / `C-e` | start / end of line |
| `←↑→↓` | arrows (ESC[ sequences) |
| `C-d` | delete at cursor |
| `Backspace` | delete previous character |
| `Enter` | insert newline |
| `Tab` | two spaces |
| `C-k` | kill to end of line |
| `C-l` | redraw |
| `C-x C-s` | save |
| `C-x C-c` | save and exit |
| `C-g` | exit without saving |

The status line shows `Lrow:Ccol  status`.

---

## 9. wm — the window manager

### 9.1 Overview

`apps/wm.c` drives, on QEMU's VersatilePB:

- the PL110 LCD controller (`0x10120000`, 16bpp BGR565),
- the PL050 PS/2 mouse (`0x10007000`),

directly, to show a desktop-like GUI on a 1024×1024 virtual screen (640×480
physical). With `run-window.sh`, `wm_main` starts automatically at boot.

It provides:

- a status bar,
- three draggable demo windows,
- a mouse cursor,
- a user draw hook `wm_set_user_render(void(*)(void))`,
- direct drawing APIs: `put_pixel_pub` / `fill_rect_pub` / `rect_outline_pub` /
  `draw_string_pub` / `wm_draw_line`.

### 9.2 Use from a user program

From C / ABCL, via the a.out built-ins:

- `rgb(r, g, b)` to make a colour,
- `wm_render(1)` to turn the line layer on,
- `wm_line(idx, x1, y1, x2, y2, color)` to update slot 0..7,
- `wm_clear()` to clear,
- `screen_w()` / `screen_h()`.

is the basic pattern (see RotLines.abcl). The shell command `rotlines` is a
little lower-level: it uses `wm_set_user_render(rot_render)` to register a
per-frame draw function directly with the WM thread.

---

## 10. Sample programs in detail

At boot, `system/main.c` seeds these four into XFS:

| Path | Kind | Focus |
| --- | --- | --- |
| `/home/hello.c` | C | `printf` |
| `/home/sum.c` | C | `for` loop |
| `/home/abclcp/abclc/PingPong.abcl` | ABCL | inter-actor messaging |
| `/home/abclcp/abclc/RotLines.abcl` | ABCL + WM | animation |

In addition the repo contains these sources, useful as reading material:

| File | Contents |
| --- | --- |
| `apps/abcl_program.c` | a sample of the C that abclc *generates* (bounded-buffer) |
| `apps/abcl_xinu_gui.c` | ABCL ↔ WM runtime (bounded-buffer / dining-philosophers drawing) |
| `apps/wm.c` | the WM itself |
| `shell/xsh_rotlines.c` | a WM demo written in C |

### 10.1 hello.c — first step

Seeded content (`system/main.c:40-45`):

```c
#include <stdio.h>
int main(void) {
    printf("hello...\n");
    return 0;
}
```

Points:

- `#include` is "accepted and ignored" by cc; it does not actually open
  `stdio.h`.
- `printf` is called as built-in number 0, forwarded internally to a
  `kprintf`-equivalent.
- The return value 0 becomes `aoutRun()`'s result via `OP_RET` and is reflected
  in the shell exit code.

Run:

```
xsh$ cd /home
xsh$ cc hello.c -o hello
xsh$ run hello
hello...
xsh$ ./hello              # implicit run
hello...
```

**a.out bytecode (simplified):**

```
ENTER 0                         ; main has no locals
PUSH_STR offset_of("hello...\n")
CALL_BI 0, 1                    ; printf, 1 arg
PUSH_I32 0
RET
```

### 10.2 sum.c — control structures

```c
#include <stdio.h>
int main(void) {
    int i;
    int s;
    s = 0;
    for (i = 1; i <= 10; i = i + 1) {
        s = s + i;
    }
    printf("sum 1..10 = %d\n", s);
    return 0;
}
```

What it teaches:

- locals `i`, `s` are accessed via `OP_LOAD_LOC` / `OP_STORE_LOC` numbered slots;
- `for (init; cond; step)` is expanded inside cc to `init; while(cond){ body; step; }`;
- `+= ++ --` are unsupported, so write `i = i + 1`;
- the `%d` format is interpreted by the `printf` built-in; `%s` also works.

Run:

```
xsh$ cc /home/sum.c -o /home/sum
xsh$ run /home/sum
sum 1..10 = 55
```

### 10.3 PingPong.abcl — inter-actor messaging

Source (seeded at `system/main.c:76-94`):

```abcl
class Player {
    var hits;
    method bounce(other, n) {
        if (n > 0) {
            printf("Player %d: tick (n=%d) hits=%d\n", self, n, hits);
            hits = hits + 1;
            send other.bounce(self, n - 1);
        }
    }
}

main {
    new Player p1;
    new Player p2;
    send p1.bounce(p2, 6);
}
```

Meaning:

- `Player` has field `hits` (how many times it has hit) and one method
  `bounce(other, n)`.
- `bounce`: if `n` is positive, advance own state by one and send the *other*
  actor "bounce with myself as argument and `n-1`".
- `main`: create `p1`, `p2` and send `p1` the first `bounce(p2, 6)`, so
  p1 → p2 → p1 → … bounces six times.

Run:

```
xsh$ cd /home/abclcp/abclc
xsh$ abclc PingPong.abcl
abclc: translated PingPong.abcl -> PingPong.c
abclc: compiled PingPong.c -> PingPong
xsh$ run PingPong
Player 0: tick (n=6) hits=0
Player 1: tick (n=5) hits=0
Player 0: tick (n=4) hits=1
Player 1: tick (n=3) hits=1
Player 0: tick (n=2) hits=2
Player 1: tick (n=1) hits=2
```

Note:

- `self` is the current actor id inside the method (creation order 0, 1, …).
- `send` is **asynchronous**: the calling method returns immediately and the
  message runs on the next scheduling.
- when `n - 1` reaches 0 the `if` is skipped, no `send` is emitted, and the
  chain stops naturally.

Reading the intermediate `PingPong.c` shows how abclc lowers the
"class × method × actor" dispatch into a `switch`.

### 10.4 RotLines.abcl — ABCL + WM animation

Seeded at `system/main.c:106-143`:

```abcl
class Rotator {
    var angle;
    method spin(n) {
        if (n > 0) {
            wm_line(0, 512, 384,
                    512 + icos(angle) * 320 / 4096,
                    384 + isin(angle) * 320 / 4096,
                    rgb(255, 80, 80));
            wm_line(1, 512, 384,
                    512 + icos(angle + 90) * 320 / 4096,
                    384 + isin(angle + 90) * 320 / 4096,
                    rgb(80, 255, 80));
            // (180 deg, 270 deg likewise)
            sleep_ms(16);
            angle = angle + 3;
            send self.spin(n - 1);
        } else {
            wm_render(0);
        }
    }
    method start(n) {
        wm_render(1);
        send self.spin(n);
    }
}
main {
    new Rotator r;
    send r.start(360);
}
```

Points:

- turn the user-line layer on with `wm_render(1)`, then update four line slots
  (0..3) each frame;
- `icos` / `isin` return **Q12 fixed point**, so after multiplying by a 320 px
  radius, divide by `4096` for real coordinates;
- `sleep_ms(16)` ≈ 60 fps;
- finish with `send self.spin(n-1)` — sending to the actor itself realises the
  animation loop; at the stop condition (`n == 0`) call `wm_render(0)` to remove
  the line layer.

Run (window mode):

```
xsh$ cd /home/abclcp/abclc
xsh$ abclc RotLines.abcl
xsh$ run RotLines       # four lines rotate in the QEMU window
```

### 10.5 rotlines (shell command) — the C-direct equivalent

`shell/xsh_rotlines.c`. An example of a shell command registering a draw hook
with the WM directly, without going through ABCL.

```
xsh$ rotlines            # 240 frames (~4 s)
xsh$ rotlines 600 5      # 600 frames, 5 deg per frame
```

Mechanism (`shell/xsh_rotlines.c:59-81`):

- hand `rot_render()` to the WM thread via `wm_set_user_render()`;
- the WM calls the draw function each frame; `rot_render` computes four endpoints
  from the latest `rot_angle` and draws with `wm_draw_line`;
- the main thread just loops advancing the angle and `sleep(16)`.

vs. the ABCL version:

| Aspect | rotlines (C) | RotLines.abcl |
| --- | --- | --- |
| Drawing | `wm_draw_line` (per-frame function) | `wm_line` (slot-based state) |
| Angle update | C loop | actor recursive send |
| Purpose | minimal WM-linked example | learning ABCL animation |

### 10.6 apps/abcl_program.c — bounded buffer (sample generated C)

This file is "a sample of the C that abclc generates from ABCL". Its structure
(`apps/abcl_program.c`):

- **Classes**: `Buffer`, `Producer`, `Consumer`, `Controller`.
- Six actors (`P0..P2`, `C0..C2`), one `Buffer`, and a `Controller` that binds
  them.
- Each actor owns a mailbox + a Xinu thread; `abcl_actor_main` pulls one message
  at a time and calls `dispatch_*`.
- `Controller_init` (`apps/abcl_program.c:411`):
  - builds the GUI slots/sliders/Start/Stop buttons via `xinu_gui_*`,
  - registers each actor as a ticker → the WM sends `tick` every 16 ms.
- `Producer_tick` / `Consumer_tick` are small three-state machines:
  1. `idle` (countdown), 2. `working`, 3. `waiting` (issue `put`/`take` to the
  Buffer and wait for a reply).
- `Buffer_put` / `Buffer_take` do full/empty checks and reply with
  `put_ok`/`put_full`/`take_ok`/`take_empty`.

The GUI integration (`apps/abcl_xinu_gui.c`) is registered in the WM's
per-frame hook; colours change with producer/consumer state, the slider changes
production rate, and Start/Stop buttons work.

**How to run**: the default `main.c` does *not* auto-start this demo. To run it
you build the corresponding `BoundedBuffer.abcl` via abclc (or link this C
directly and call `abcl_main`). As reading material, note:

- `mailbox_t` (`apps/abcl_program.c:78-83`) — a classic bounded queue using two
  semaphores;
- `abcl_enqueue` (`apps/abcl_program.c:119-139`) — the comment "the name
  `enqueue` clashes with queue.h, so it is overridden by a macro" is a real
  implementation gotcha;
- `dispatch` (`apps/abcl_program.c:461-469`) — a mega-dispatch that branches on
  class id;
- `abcl_actor_main` (`apps/abcl_program.c:510-536`) — the common loop each actor
  runs, with a safety valve `abcl_shutdown()` once the global message cap
  `_abcl_cap` is exceeded.

This source is very useful as a reference for what ABCL → C translation
produces: an ABCL `class` becomes a C `dispatch_<Class>` function, a `field`
becomes an `enum` index, and `send` becomes `enqueue`.

---

## 11. Development workflow

A typical session:

```
# 1) boot
$ ./compile/run-window.sh

# 2) run existing samples
xsh$ cd /home
xsh$ make
xsh$ ./hello
xsh$ ./sum

# 3) write your own C
xsh$ edit /home/myprog.c
        (C-x C-s to save, C-x C-c to exit)
xsh$ cc myprog.c -o myprog
xsh$ ./myprog

# 4) write ABCL
xsh$ cd /home/abclcp/abclc
xsh$ edit MyActor.abcl
xsh$ abclc MyActor.abcl
xsh$ ./MyActor

# 5) try the WM
xsh$ rotlines
xsh$ run /home/abclcp/abclc/RotLines
```

Tips:

- to build a single target: `make MyActor`;
- to rebuild everything: `rm Makefile && make`;
- to clean temporaries: `rm *.c.bak` etc. XFS has up to 32768 inodes, so be
  careful creating many files.

---

## 12. Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| `qemu-system-arm not found` | `brew install qemu` (macOS) / `apt install qemu-system-arm` (Linux) |
| stale build boots | `make clean && make PLATFORM=arm-qemu` in `compile/` |
| no shell prompt | `Ctrl-A → c` for the QEMU monitor, then `info registers` for the stop point |
| `cc: line N: ...` error | check for unsupported syntax (structs, pointers, arrays, float, …) |
| `abclc: translation failed` | two `send`s in one method, too many `var`s, too many methods, … |
| `command not found` but the file exists | implicit `run` only works on a **single absolute/relative path token**; to pass args use `run PATH ARGS...` |
| no WM window | is it an `arm-qemu` build? did you use `run-window.sh`? |
| files vanished | the RAM disk is volatile — back up on the host |
| path beyond `xfs.h:171` | `XFS_PATH_MAX = 256`; overly long paths are truncated |
| garbled screen | in the editor press `C-l`; in the shell run `clear` |

---

## Appendix A. File map

| Area | Key files |
| --- | --- |
| Boot | `system/main.c`, `system/initialize.c`, `compile/run-*.sh` |
| XFS  | `include/xfs.h`, `system/xfs.c`, `device/ramdisk/`, `include/ramdisk.h` |
| cc   | `include/aout.h`, `system/cc.c`, `system/aout.c`, `shell/xsh_cc.c`, `shell/xsh_run.c` |
| abclc| `include/abclc.h`, `system/abclc.c`, `shell/xsh_abclc.c` |
| make/edit | `shell/xsh_make.c`, `shell/xsh_edit.c` |
| WM   | `apps/wm.c`, `apps/abcl_xinu_gui.c`, `shell/xsh_rotlines.c` |
| shell | `shell/shell.c`, `shell/xsh_*.c`, `include/shell.h` |
| platform | `compile/platforms/arm-qemu/`, `compile/platforms/arm-rpi/` |

## Appendix B. Existing samples (after FS seeding)

```
/home/
├── hello.c
├── sum.c
└── abclcp/
    └── abclc/
        ├── PingPong.abcl
        └── RotLines.abcl
```

Running `cd /home && make` produces `hello`, `sum`; `cd /home/abclcp/abclc &&
make` produces `PingPong`, `RotLines`, each as an a.out next to its source.

---

That concludes the overview. To go deeper, read `system/cc.c` and
`system/aout.c` (the bytecode VM) and `system/abclc.c` (the ABCL translator).
