
# 🏝️ sandbox — Minimal Linux Sandbox in C

**sandbox** is a minimalist, auditable, and hackable C program that builds a chrooted Linux environment around a target ELF binary or a minimal shell environment, isolating execution in dedicated namespaces with tight controls on filesystem, user privileges, and process capabilities. The target is validated by `is_binary()` (`sandbox.c:138-165`), which only checks that the path is executable (`access(X_OK)`), is a regular file (`S_ISREG`), and that its first four bytes are the ELF magic `\x7fELF` — it is **not** a full ELF-format check. Shell scripts and other non-ELF executables are rejected at this initial check with `"<path> is not a binary file"`, but a malformed file whose first four bytes happen to be `\x7fELF` passes `is_binary()` and fails later during rootfs setup (typically at `ldd failed for <target>` followed by `Rootfs setup failed`).

---

## Features

- 📦 **Builds minimal chroot environments** for a binary or a shell session
- 🔒 **Isolates with Linux namespaces**: mount, PID, UTS (hostname)
- 🚫 **Clears the capability bounding set** before optional UID/GID drop, then **drops all process capabilities** using `libcap` after wiping the environment
- 👤 **Optionally drops to the unprivileged `nobody` user** (`--user`)
- 🔍 **Supports tracing with `strace`** (`--trace`)
- 🏗️ **Auto-copies required dynamic libraries** with `ldd`
- 🧩 **Extensible**: add extra files with `--extras <file>`
- 🗄️ **Auto-populates `/etc/passwd` and `/etc/group`** as needed
- 🧹 **Wipes environment variables** for safety
- 🪶 **Less than 1000 lines, easy to audit and extend**

---

## Prerequisites

### Build (mandatory)

- A **C compiler** in `CC` — the Makefile defaults to `clang` (`CC ?= clang`), so plain `make` requires `clang`. To build with GCC, run `make CC=gcc`.
- **libcap** development headers and library — provides `<sys/capability.h>` and `-lcap`

  These are the only build prerequisites `make preflight` checks (`Makefile:19-22`): a working `CC`, `<sys/capability.h>`, and successful link with `-lcap`.

  ```bash
  # Debian / Ubuntu
  sudo apt install clang libcap-dev

  # Fedora / RHEL
  sudo dnf install clang libcap-devel

  # Arch
  sudo pacman -S clang libcap
  ```

  Distro meta-packages like `build-essential` (Debian/Ubuntu) or `base-devel` (Arch) are **not** required — install them only as an optional convenience bundle if you want the wider toolchain.

### Runtime

- **Root privileges** — required for all modes except `--userns` (namespaces, chroot, mounts).
- **`ldd` on the host `PATH`** — required in every mode. Resolved via `execlp("ldd", ...)` (`sandbox.c:263`), so any location on `PATH` works for that subprocess call. Used by `copy_ldd_deps()` in both target mode and shell mode to discover and copy shared-library dependencies. Typically provided by `libc-bin` (Debian/Ubuntu) or `glibc-common` (Fedora/RHEL). Note that shell mode has an **additional, path-specific** requirement on `/usr/bin/ldd` because `essential_bins[]` copies from that exact path — see the next bullet.
- **Shell-mode `essential_bins[]` (every source path required on the host)** — shell mode requires every hardcoded source path in `essential_bins[]` (`sandbox.c:52-68`) to exist on the host: `/bin/sh`, `/bin/ls`, `/bin/cat`, `/bin/echo`, `/bin/mkdir`, `/bin/rm`, `/usr/bin/grep`, `/usr/bin/head`, `/usr/bin/tail`, `/usr/bin/wc`, `/usr/bin/stat`, `/usr/bin/ldd`, `/usr/bin/strace`, `/usr/bin/du`. The essential-bins loop in `setup_essential_environment()` (`sandbox.c:727-735`) is strict, not best-effort: any `copy_file()` failure for an entry (including a missing source on the host) is logged as `Failed to copy essential bin: <path>` and aborts `setup_essential_environment()` with `return -1`, and a subsequent `copy_ldd_deps()` failure for the same binary likewise aborts setup with `return -1`. This applies to `/usr/bin/strace` exactly as it applies to every other entry in `essential_bins[]` — there is no skip or best-effort fallback for strace. Both `/usr/bin/ldd` and `/usr/bin/strace` are therefore host-side requirements for shell mode on every run, regardless of `--trace` (`--trace` is a target-mode-only flag and has no bearing on shell mode).
- **`/etc` and `/dev` contents depend on flags, not just mode** — both `setup_essential_environment()` and `build_rootfs()` create the standard directory tree from `dirs[]`, so `/etc` and `/dev` exist in both shell mode and target mode. `create_etc_files()` only writes `/etc/passwd` and `/etc/group` when `drop_to_nobody` is enabled via `--user`; otherwise it only ensures `/etc` exists. `create_dev_nodes()` creates real `/dev/null`, `/dev/zero`, and `/dev/tty` device nodes in normal runs, but in `--userns` mode it creates placeholder files that `setup_sandbox_environment()` later bind-mounts over with the host devices. `--extras` can also copy additional absolute paths such as `/etc/...` in any mode.
- **`/usr/bin/strace` in target mode** — target mode attempts to copy `/usr/bin/strace` into the rootfs on every run (`sandbox.c:699-709`), but a missing host `/usr/bin/strace` is only fatal under `--trace`:
    - **Without `--trace`:** tolerated. If `copy_file("/usr/bin/strace", ...)` fails, `build_rootfs()` silently continues and target mode runs to completion without `strace` in the rootfs (the `else` branch that runs `copy_ldd_deps("/usr/bin/strace", ...)` is only entered when the copy succeeded).
    - **With `--trace`:** strictly required. The `if (trace_mode)` branch at `sandbox.c:702-705` returns `-1` and aborts setup with `Failed to copy strace (required for --trace)`.
- **Observable consequence:** on a host with strace installed, a normal (non-`--trace`) target run produces a rootfs containing `<rootfs>/usr/bin/strace` even though the file is not required for the target run to succeed. Shell mode, by contrast, requires `/usr/bin/strace` on the host on every run, because it is copied via the strict `essential_bins[]` loop above.

  ```bash
  # Debian / Ubuntu
  sudo apt install strace

  # Fedora / RHEL
  sudo dnf install strace

  # Arch
  sudo pacman -S strace
  ```

### Development (optional)

Needed only for `make lint`:

- **`cppcheck`** — static analysis for `sandbox.c`
- **`shellcheck`** — static analysis for `tests/smoke.sh`

  ```bash
  # Debian / Ubuntu
  sudo apt install cppcheck shellcheck

  # Fedora / RHEL
  sudo dnf install cppcheck ShellCheck

  # Arch
  sudo pacman -S cppcheck shellcheck
  ```

---

## Build

From the repo root:

```bash
make
```

This produces `./sandbox`. To build with GCC instead of the default `clang`:

```bash
make CC=gcc
```

To remove the built binary:

```bash
make clean
```

---

## Testing

After building, run the smoke test:

```bash
make test
```

The `test` target runs `tests/smoke.sh`, which performs two checks:

- Invokes `./sandbox` with no arguments and verifies the output is exactly:
  ```
  Usage: ./sandbox <rootfs> [<target-binary>] [--user] [--userns] [--extras <file>] [--trace <args...>]
  ```
- Verifies that `./sandbox` exists and is executable (`-x`).

This is a smoke test only — there are no unit tests, no CI, and no coverage of runtime sandboxing behavior. It just confirms the binary was built and prints the expected usage line.

---

## Linting

Run the static-analysis linters:

```bash
make lint
```

The `lint` target runs two checks:

- **cppcheck** against `sandbox.c`:
  ```
  cppcheck --quiet --enable=warning,performance,portability --check-level=reduced --suppress=normalCheckLevelMaxBranches --inline-suppr --error-exitcode=1 sandbox.c
  ```
- **shellcheck** against `tests/smoke.sh`:
  ```
  shellcheck tests/smoke.sh
  ```

Both `cppcheck` and `shellcheck` must be installed on the host (see [Development (optional)](#development-optional) under Prerequisites).

---

## Validation

Run the combined test and lint checks:

```bash
make validate
```

The `validate` target is defined as `validate: test lint` in `Makefile:19` and runs the existing `test` and `lint` targets in sequence: first `tests/smoke.sh` from `make test`, then the `cppcheck` and `shellcheck` invocations from `make lint`.

---

## Usage

```bash
Usage: ./sandbox <rootfs> [<target-binary>] [--user] [--userns] [--extras <file>] [--trace <args...>]
```

Run as root (e.g., via `sudo`) unless `--userns` is used.

After `<rootfs>`, `sandbox` scans `argv` left-to-right until `--trace` (`sandbox.c:759-779`). At each position, `--user`, `--userns`, and `--extras <file>` are recognized as options and may appear either before or after the target binary; `--extras` also consumes the following token as its list-file path, so that token is **not** passed through to the target. Any remaining token is positional: the first positional token becomes `<target-binary>`, and every later positional token is collected into `target_args[]`. Whether those later positionals actually reach the target depends on the execution path:

- **Normal (non-`--trace`) path:** `sandbox_main()` reads `target_args[]` and forwards each entry as an argument to the executed target (`sandbox.c:621-629`). For example, `./sandbox /tmp/x2 --userns /bin/echo hi` runs `/bin/echo hi`, and `./sandbox /tmp/sb-review /bin/echo one --userns two` runs `/bin/echo one two`.
- **`--trace` path:** `target_args[]` is never read — `trace_argv[]` is rebuilt only from the selected target binary plus tokens **after** `--trace` (`sandbox.c:866-885`), so any positional collected between `<target-binary>` and `--trace` is silently dropped. For example, `sudo ./sandbox /tmp/sb-review /bin/echo one --trace two` traces `/bin/echo two` (the pre-`--trace` `one` is dropped). See the `--trace` paragraph below and the **Trace a binary** mode for the full rules.

`--trace` terminates sandbox option parsing: when the parser encounters it, it records the index and immediately `break`s out of the option loop (`sandbox.c:764-767`), after which the traced command line is built from every token after `--trace` (`sandbox.c:866-885`). All sandbox flags (`--user`, `--userns`, `--extras <file>`) must therefore appear **before** `--trace`; every token after `--trace` is appended to the target binary's argv and is never interpreted as a flag for `sandbox` or `strace`. Intended **target arguments must also appear after `--trace`**: any positional tokens collected between `<target-binary>` and `--trace` are stored in `target_args[]`, which is only read by the normal execution path in `sandbox_main()` (`sandbox.c:621-629`) — the trace path builds `trace_argv[]` exclusively from tokens after `--trace` (`sandbox.c:866-885`) and therefore silently discards those pre-`--trace` positional args. For example, `./sandbox /tmp/x /bin/echo one --trace two` traces `/bin/echo two` (the `one` is dropped), while `./sandbox /tmp/x /bin/echo --trace one two` traces `/bin/echo one two`. The ordering effect shows up in which error fires: `./sandbox /tmp/x /bin/echo --userns --trace` fails with `--userns is not compatible with --trace.` because `--userns` was parsed as a sandbox flag before the boundary and tripped the conflict check, whereas `./sandbox /tmp/x /bin/echo --trace --userns` (run non-root) fails with `This program must be run as root (or use --userns).` — parsing stopped at `--trace`, so the second `--userns` was passed through to `/bin/echo` as an argument, the `--userns`/`--trace` conflict check never ran, and the root check fired first instead.

### Modes

Flag constraints (enforced at startup):

- `--trace` requires a target binary.
- `--user` cannot be combined with `--trace`.
- `--userns` cannot be combined with `--trace`.
- `--userns` cannot be combined with `--user`.

Modes:

- **Minimal shell sandbox:**
    ```bash
    sudo ./sandbox /tmp/mychroot
    ```
    - Drops you into `/bin/sh`. `setup_essential_environment()` copies **exactly** the set of binaries hardcoded in `essential_bins[]` (`sandbox.c:52-68`) into the rootfs — nothing more, nothing less: `/bin/sh`, `/bin/ls`, `/bin/cat`, `/bin/echo`, `/bin/mkdir`, `/bin/rm`, `/usr/bin/grep`, `/usr/bin/head`, `/usr/bin/tail`, `/usr/bin/wc`, `/usr/bin/stat`, `/usr/bin/ldd`, `/usr/bin/strace`, `/usr/bin/du`.
    - The copy loop (`sandbox.c:727-735`) is strict, not best-effort: any `copy_file()` failure for an entry (source absent on the host, destination open/write error, or any other I/O failure) is logged as `Failed to copy essential bin: <path>` and aborts setup with `return -1`, and a subsequent `copy_ldd_deps()` failure for the same binary likewise aborts setup. A successful shell-mode run therefore guarantees the rootfs contains every entry in `essential_bins[]`; verify on a host where those binaries exist with:
      ```bash
      sudo find <rootfs>/bin <rootfs>/usr/bin -maxdepth 1 -type f | sort
      ```
- **Run a specific binary:**
    ```bash
    sudo ./sandbox /tmp/mychroot /usr/bin/ls
    ```
    - `<target-binary>` is validated by `is_binary()` (`sandbox.c:138-165`), which only checks that the path is executable (`access(X_OK)`), is a regular file (`S_ISREG`), and that its first four bytes are the ELF magic `\x7fELF`. This is a magic-bytes check, not full ELF validation: shell scripts and other non-ELF executables are rejected here with `"<path> is not a binary file"` (for example, `./sandbox /tmp/sb-review /tmp/sb-script --userns` on an executable shell script fails immediately with `/tmp/sb-script is not a binary file`), but a malformed file whose first four bytes happen to be `\x7fELF` — including an executable regular file containing only those four bytes — passes `is_binary()` and then fails later during `build_rootfs()`, typically at `ldd failed for <target>` followed by `Rootfs setup failed`.
    - Target-mode rootfs assembly is handled by `build_rootfs()` (`sandbox.c:650-713`), not by the shell-mode `essential_bins[]` path. It **always** creates the standard directory tree from `dirs[]` (`/bin`, `/usr/bin`, `/etc`, `/proc`, `/dev`, `/tmp`), **always** copies the requested target to `<rootfs>/usr/bin/<basename>` (`sandbox.c:666-670`), and **always** copies `/bin/sh` to `<rootfs>/bin/sh` (`sandbox.c:689-694`).
    - `build_rootfs()` also **always** copies shared-library dependencies for the requested target and for `/bin/sh` by calling `copy_ldd_deps(bin, rootfs)` and `copy_ldd_deps("/bin/sh", rootfs)` (`sandbox.c:695-698`).
    - Normal target execution later runs through `sandbox_main()` (`sandbox.c:621-630`), which always `execv()`s `/usr/bin/<basename>` inside the sandbox. That means the executed path, and therefore the target process's `argv[0]`, is always `/usr/bin/<basename>` in the non-`--trace` path.
    - If `<target-binary>` is an absolute path and that path differs from `/usr/bin/<basename>` (for example `/usr/local/bin/foo` or `/sbin/foo`), `build_rootfs()` additionally copies the same host binary to `<rootfs><absolute-target-path>` after creating any missing parent directories (`sandbox.c:672-688`). That extra copy is conditional on the input path and exists for path compatibility only; the normal non-`--trace` `execv()` call still uses `/usr/bin/<basename>`, not `<absolute-target-path>`.
    - `build_rootfs()` also attempts to copy `/usr/bin/strace` to `<rootfs>/usr/bin/strace` on every target-mode run (`sandbox.c:699-709`). If that copy succeeds, it also copies `strace`'s shared-library dependencies; if the copy fails, setup aborts only when `--trace` is active, otherwise target mode continues without `strace` in the rootfs.
    - After those copies, `build_rootfs()` calls `create_dev_nodes(rootfs)` and `create_etc_files(rootfs)` (`sandbox.c:710`). That always ensures `<rootfs>/dev` and `<rootfs>/etc` exist. Outside `--userns`, `create_dev_nodes()` creates `/dev/null`, `/dev/zero`, and `/dev/tty` as character device nodes; in `--userns`, it creates placeholder files that `setup_sandbox_environment()` later bind-mounts over with the host devices. `create_etc_files()` always creates `<rootfs>/etc/`; it additionally writes `/etc/passwd` and `/etc/group` only when `--user` is set (the `if(drop_to_nobody)` gate at `sandbox.c:390-410`), and `--extras` may add other files under `/etc` regardless of mode.
- **Trace a binary (replays a filtered subset of strace-reported paths into the rootfs):**
    ```bash
    sudo ./sandbox /tmp/mychroot /usr/bin/curl --trace "https://example.com"
    ```
    - `--trace` requires a target binary and cannot be combined with `--user` or `--userns`.
    - `--trace` consumes all subsequent argv tokens as arguments to the traced binary, so `--user`, `--userns`, and `--extras <file>` must appear **before** `--trace`; otherwise they are silently passed to the target binary (not to `sandbox` and not to `strace`) and the `--user`/`--userns` conflict checks above are bypassed. For example, `./sandbox /tmp/sbroot /bin/echo --trace --userns` runs `/bin/echo --userns` inside the sandbox and does **not** enable user-namespace mode.
    - Arguments for the traced binary must also appear **after** `--trace`. In trace mode, the **first** positional token before `--trace` selects the target binary (the parser sets `target` on its first positional at `sandbox.c:771-772`) and is the only pre-`--trace` positional that reaches the traced program — it becomes the traced program's `argv[0]` via `trace_argv[j++] = trace_target` (`sandbox.c:883`). Any **additional** positional tokens before `--trace` are discarded and do not reach the traced program: they are collected into `target_args[]` and consumed only by the non-trace execution path in `sandbox_main()` (`sandbox.c:621-629`), whereas the trace path appends only tokens after `--trace` to `trace_argv[]` (`sandbox.c:866-885`), so those tokens become the traced program's `argv[1..]`. For example, `sudo ./sandbox /tmp/sbtrace-arg /bin/echo one --trace two` traces `/bin/echo two` (the pre-`--trace` `one` is dropped), while `sudo ./sandbox /tmp/sbtrace-arg2 /bin/echo --trace one two` traces `/bin/echo one two`.
    - In `--trace` mode, `main()` builds `trace_argv[]` before `clone()` (`sandbox.c:857-886`), and `trace_main()` only calls `sandbox_exec(trace_argv)` (`sandbox.c:595-598`); `trace_main()` does not populate `trace_argv[]` itself.
    - `sandbox_exec()` then `execv()`s `/usr/bin/strace` because `trace_argv[0]` is `/usr/bin/strace` (`sandbox.c:640-645`, `sandbox.c:871-876`). The traced program path is passed separately as strace's first non-option command argument: if the user supplied an absolute target path, that argument is the original absolute path; otherwise it is `/usr/bin/<basename>` (`sandbox.c:867-885`). That traced-program path is what the traced binary sees as its own `argv[0]`; it is not the path `sandbox_exec()` directly `execv()`s.
    - **Exact launch form.** `--trace` performs a single sandboxed `execv()` of `/usr/bin/strace` with the fixed token sequence `/usr/bin/strace -f -e trace=file -o /tmp/straceXXXXXX <trace_target> <args-after---trace>` (`sandbox.c:876-886`). The `-o` path is the exact template passed to `mkstemp("/tmp/straceXXXXXX")` at `sandbox.c:858-864`, which runs on the host before the `clone()` call at `sandbox.c:888` — `mkstemp()` overwrites the six `X`s in place with a unique suffix, so every run gets a fresh `/tmp/strace<6chars>` pathname (the mkstemp call leaves an empty file at that path on the host). Because `strace` is only exec'd after `setup_sandbox_environment()` calls `chroot(rootfs)` (`sandbox.c:453`), the trace log strace writes to `/tmp/strace<6chars>` actually lands inside the rootfs at `<rootfs>/tmp/strace<6chars>`, which is exactly where the parent reopens it after the child exits (`sandbox.c:899-902`). `<trace_target>` is selected at `sandbox.c:871-875`: the original absolute `target` string when `target[0] == '/'`, otherwise `/usr/bin/<basename>` (resolved from `target_name`, which `build_rootfs()` sets to `basename(bin)` at `sandbox.c:664`). `<args-after---trace>` is every argv token after `--trace`, copied verbatim in order (`sandbox.c:884-885`). `trace_main()` does nothing except call `sandbox_exec(trace_argv)` (`sandbox.c:595-600`), which `execv()`s that argv directly with no wrapper shell.
    - **Exit-status propagation.** After the cloned trace child exits, `main()` first `waitpid()`s it, then performs the post-trace replay scan plus the best-effort `unlink()` described below, and only after that returns the traced child's status to the shell: `WEXITSTATUS(status)` on normal exit, or `128 + WTERMSIG(status)` on signal termination (`sandbox.c:893-898`, `sandbox.c:902-928`). The `[sandbox --trace exited with N]` / `[sandbox --trace killed by signal N]` message is printed before the trace-output scrape but reports the same status that is subsequently returned. Concretely, `sudo ./sandbox /tmp/sb /bin/true --trace; echo $?` prints `0`, and `sudo ./sandbox /tmp/sb /bin/false --trace; echo $?` prints `1`.
    - **Post-trace file replay.** After the traced child exits, `sandbox` reopens the trace log at `<rootfs>/tmp/strace<6chars>` and scans it line-by-line (`sandbox.c:902-924`). The replay is narrower than "every file accessed during the run": for each line it locates the **first** pair of double quotes and treats the enclosed string as a candidate path; the candidate is then copied into the rootfs **only** if (1) it is quoted in the trace line, (2) it starts with `/` (absolute), and (3) it does not contain the substring `/..`. Paths that pass those three filters are copied via `copy_file(path, <rootfs><path>)`, whose **return value is ignored** — any failure during this replay pass (missing source, permission error, destination write error) is silently skipped and does not abort the run. After the scan completes, `sandbox` **attempts** to `unlink()` the trace log at `<rootfs>/tmp/strace<6chars>` (`sandbox.c:924`); the `unlink()` return value is also ignored, so on the common success path the file is removed, but a failed `unlink()` does not abort the run and can leave the trace file in place. A post-run check like `test ! -e <rootfs>/tmp/strace*` therefore confirms the expected success path but is not guaranteed by the code.
- **Sandbox as unprivileged user (`nobody`):**
    ```bash
    sudo ./sandbox /tmp/mychroot --user
    ```
    - Cannot be combined with `--trace` or `--userns`.
- **Rootless mode (user namespace):**
    ```bash
    ./sandbox /tmp/mychroot --userns
    ./sandbox /tmp/mychroot /usr/bin/ls --userns
    ```
    - Runs without root by creating a user namespace. Requires `sysctl kernel.unprivileged_userns_clone=1` (or equivalent) on the host kernel.
    - Device nodes (`/dev/null`, `/dev/zero`, `/dev/tty`) are set up in **two phases**, because `mknod(2)` requires `CAP_MKNOD` which is not available inside an unprivileged user namespace:
        1. **Placeholder creation** — during rootfs assembly, `create_dev_nodes()` takes the `userns_mode` branch at `sandbox.c:363-369` and creates empty regular files at `<rootfs>/dev/null`, `<rootfs>/dev/zero`, and `<rootfs>/dev/tty` via `open(path, O_WRONLY | O_CREAT, 0666)` instead of calling `mknod()`. These are not device nodes yet — they are zero-byte regular files that exist only to serve as bind-mount targets.
        2. **Bind-mount over the placeholders** — later, inside the child, `setup_sandbox_environment()` (`sandbox.c:441-452`) iterates `{"null", "zero", "tty"}` and performs `mount("/dev/<name>", "<rootfs>/dev/<name>", NULL, MS_BIND, NULL)` for each, attaching the host's real character devices onto the placeholder files before `chroot`.
    - Cannot be combined with `--trace` or `--user`.
- **Add extra files:**
    ```bash
    sudo ./sandbox /tmp/mychroot --extras extras.txt
    sudo ./sandbox /tmp/mychroot /usr/bin/ls --extras extras.txt
    ./sandbox /tmp/mychroot --userns --extras extras.txt
    sudo ./sandbox /tmp/mychroot /usr/bin/curl --extras extras.txt --trace "https://example.com"
    ```
    - `extras.txt` contains a list of absolute file paths, one per line.
    - `--extras` **must be immediately followed by the list file**; the parser only accepts it when a filename argument is present (`sandbox.c:768-770`). A bare `--extras` with no filename is not recognized as a flag and is treated as a positional argument instead — e.g. `./sandbox /tmp/sbroot --userns --extras` ends up with `--extras` taken as the target binary and is rejected as `"--extras is not a binary file"`.
    - Works with both the shell sandbox and target-binary runs, and can be combined with `--user` or `--userns`.
    - If `--trace` is also used, `--extras <file>` must appear **before** `--trace`: once `--trace` is seen, the parser stops scanning flags and treats every remaining argument as a trace arg.

---

## How It Works

- Creates a new mount, PID, and UTS namespace
- Builds up a new root filesystem (`<rootfs>`) by creating the standard directory tree from `dirs[]`: `/bin`, `/usr/bin`, `/etc`, `/proc`, `/dev`, and `/tmp`. `/etc` itself is always created, but `/etc/passwd` and `/etc/group` are only written when `--user` is passed (the `if(drop_to_nobody)` gate at `sandbox.c:390`)
- In shell mode, `setup_essential_environment()` copies every entry in `essential_bins[]`, then calls `create_dev_nodes()` and `create_etc_files()`
- In target-binary mode, `build_rootfs()` always copies the target to `/usr/bin/<basename>`, conditionally also copies it to its original absolute path inside the rootfs when that path differs, always copies `/bin/sh`, always copies shared-library dependencies for the target and `/bin/sh`, conditionally includes `/usr/bin/strace` plus its shared-library dependencies when that copy succeeds (or fails closed under `--trace`), and then calls `create_dev_nodes()` and `create_etc_files()`
- `create_dev_nodes()` prepares `/dev/null`, `/dev/zero`, and `/dev/tty`: in normal runs they are created as character device nodes, while in `--userns` they start as placeholder files that are later bind-mounted to the host devices
- `create_etc_files()` always creates `<rootfs>/etc/`; it additionally writes `/etc/passwd` and `/etc/group` only when `--user` sets `drop_to_nobody` (the `if(drop_to_nobody)` gate at `sandbox.c:390-410`)
- Optionally adds files specified in `--extras`, which can also copy files under paths such as `/etc/...`
- Optionally traces binary with `strace` to discover runtime file dependencies. After the traced child exits, `sandbox` parses the trace log under `<rootfs>/tmp/strace<6chars>`, copies into the rootfs only quoted absolute paths that do not contain `/..`, ignores any `copy_file()` failure during this replay pass, and then attempts to `unlink()` the trace log (the `unlink()` return value is also ignored, so removal is best-effort) (`sandbox.c:902-924`)
- Optionally switches to UID/GID 65534 (`nobody`)
- Optionally creates a user namespace with `--userns` for rootless operation: writes `deny` to `/proc/<pid>/setgroups` and maps namespace uid/gid `0` to the invoking caller's real uid/gid (`getuid()`/`getgid()`) via `/proc/<pid>/uid_map` and `/proc/<pid>/gid_map`, so the sandboxed process appears as `root` inside the namespace while retaining the caller's identity on the host. `--userns` cannot be combined with `--user` or `--trace`.
- In `--userns` mode, the parent process invokes `write_uid_gid_map()` (sandbox.c:101-136) against the child pid to populate these maps: it writes `deny` to `/proc/<pid>/setgroups`, then writes `0 <caller-uid> 1\n` to `/proc/<pid>/uid_map` and `0 <caller-gid> 1\n` to `/proc/<pid>/gid_map`, mapping the caller's real UID/GID to **UID 0 / GID 0 (root) inside the user namespace**. This is distinct from the `--user` flag, which drops the sandboxed process to UID/GID 65534 (`nobody`) inside the sandbox; `--userns` instead gives it namespace-root while keeping the caller's identity on the host.
- Sets `PR_SET_NO_NEW_PRIVS` via `prctl()` as the first step of `setup_sandbox_environment()` (sandbox.c:423), before `chroot` and before the capability bounding set is cleared or process capabilities are dropped. Once set, the bit is inherited across `execve()` and prevents the kernel from granting new privileges through setuid/setgid binaries (or file capabilities) executed inside the sandbox.
- In `--userns` mode only, `setup_sandbox_environment()` bind-mounts the host's `/dev/null`, `/dev/zero`, and `/dev/tty` onto `<rootfs>/dev/null`, `<rootfs>/dev/zero`, and `<rootfs>/dev/tty` by iterating `{"null", "zero", "tty"}` and calling `mount(src, mnt, NULL, MS_BIND, NULL)` for each (`sandbox.c:441-452`). This step runs only when `userns_mode` is set and happens **before** the `chroot()` call on `sandbox.c:453`, attaching the host's real character devices onto the zero-byte placeholder files that `create_dev_nodes()` created earlier in the userns branch (since `mknod(2)` is not available inside an unprivileged user namespace).
- Clears the capability bounding set, drops to unprivileged UID/GID if requested, wipes environment variables with `clearenv()` and restores only `PATH=/bin:/usr/bin` and `HOME=/`, and finally drops all process capabilities
- Execution splits into two distinct paths, but **both paths call `setup_sandbox_environment()` (`sandbox.c:414-499`) before `execv()`**, so `PR_SET_NO_NEW_PRIVS`, hostname/mount-private setup, `chroot` into `<rootfs>`, `/proc` mount, capability bounding set clear, optional UID/GID drop to `nobody`, `clearenv()` followed by restoring only `PATH=/bin:/usr/bin` and `HOME=/`, and the final `drop_all_caps()` all happen in both modes — the only difference between the two paths is whether seccomp is installed:
    - **Normal execution** (no `--trace`) runs through `sandbox_main()` (`sandbox.c:602-638`), which calls `setup_sandbox_environment()` first and then `install_seccomp_filter()` to install a fail-closed x86_64-only seccomp allowlist before `execv()`. On non-x86_64 hosts `install_seccomp_filter()` returns an error and the sandbox fails closed without executing the target.
    - **Trace execution** (`--trace`) runs through `trace_main()` → `sandbox_exec()` (`sandbox.c:640-648`), which **still** calls `setup_sandbox_environment()` — so the chroot, capability drops, and environment reset above are applied — and then intentionally does **not** call `install_seccomp_filter()`; the target is `execv()`'d under `strace` with no seccomp filter applied, because the allowlist would otherwise block `ptrace` and the syscalls `strace` needs.
- Executes `/bin/sh` (or the target) inside the chroot

---

## Security Model

- **Namespaces**: all three `clone()` sites — shell mode at `sandbox.c:811`, `--trace` at `sandbox.c:887`, and the normal target path at `sandbox.c:933` — use the same base flag set `CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD`, so the sandbox always enters exactly three new namespaces: **UTS** (hostname), **PID** (process IDs), and **mount** (filesystem). A fourth **user** namespace is added only under `--userns`, which OR's in `CLONE_NEWUSER` at `sandbox.c:813` (shell mode) and `sandbox.c:935` (normal target path); the `--trace` site at `sandbox.c:887` never adds `CLONE_NEWUSER` because `--trace` and `--userns` are mutually exclusive. Network, IPC, and cgroup namespaces are **not** created in any of the three paths — see Limitations & Roadmap
- **Capabilities**: the bounding set is cleared before the optional UID/GID drop; all process capability sets are dropped only after the environment reset
- **No environment variables**: `clearenv()` runs after the UID/GID drop, then `PATH=/bin:/usr/bin` and `HOME=/` are restored, and only after that are all process capabilities dropped
- **User `nobody`**: further restricts privilege for untrusted code (unless tracing)
- **User namespace (`--userns`)**: optional rootless mode. Writes `deny` to `/proc/<pid>/setgroups` and maps namespace uid/gid `0` to the invoking caller's real uid/gid (`getuid()`/`getgid()`) via `/proc/<pid>/uid_map` and `/proc/<pid>/gid_map`. The process is `root` inside the namespace but keeps the caller's identity on the host — it is **not** a drop to `nobody`. Mutually exclusive with `--user` and `--trace`.
- **Seccomp**: only the normal execution path (`sandbox_main()` → `install_seccomp_filter()` → `execv()`) installs a filter — a small fail-closed allowlist **compiled only for x86_64** (the entire filter is inside `#if defined(__x86_64__)` in `install_seccomp_filter()`). On other architectures the function returns `-1` and `sandbox_main()` exits with status 1 before `execv()`, so **the normal execution path will not start on non-x86_64 hosts** — it does not fall back to running unfiltered. The `--trace` path (`trace_main()` → `sandbox_exec()` → `execv()` under `strace`, `sandbox.c:640-648`) intentionally does **not** call `install_seccomp_filter()` and runs entirely unfiltered on every architecture (including non-x86_64), because the allowlist would block the `ptrace` and related syscalls `strace` depends on. "Unfiltered" here means only seccomp is skipped: `sandbox_exec()` still calls `setup_sandbox_environment()` (`sandbox.c:414-499`) first, so `PR_SET_NO_NEW_PRIVS`, the chroot/`/proc` setup, the capability bounding-set clear plus `drop_all_caps()`, and the `clearenv()` followed by restoring only `PATH=/bin:/usr/bin` and `HOME=/` still apply under `--trace` exactly as they do in `sandbox_main()` (`sandbox.c:602-638`) — only the seccomp layer is omitted
- **Not a container runtime**, but a tight, auditable educational sandbox

### Security tips

- For maximum isolation, use on a dedicated VM or test system
- If running untrusted code, combine with system-level controls (AppArmor, SELinux, VM isolation)

---

## Example

Build and run a minimal shell sandbox:

```bash
sudo ./sandbox /tmp/sandbox-root
# You are now in a sandboxed /bin/sh
```

Run an ELF binary with minimal rootfs. In normal target mode, `sandbox_main()` always executes `/usr/bin/<basename>` inside the sandbox, even if the host path you supplied was different:

```bash
sudo ./sandbox /tmp/sandbox-root /bin/echo hello
# inside the sandbox, the executed path and argv[0] are /usr/bin/echo
```

Trace mode is different: `sandbox_exec()` still executes `/usr/bin/strace`, but the traced program path passed to `strace` stays `/bin/echo` when the original target was absolute:

```bash
sudo ./sandbox /tmp/sandbox-root /bin/echo --trace hello
# sandbox_exec() execv()s /usr/bin/strace
# strace then runs /bin/echo, so the traced program sees argv[0] == /bin/echo
```

---

## Limitations & Roadmap

- Requires root unless `--userns` is used for rootless operation via user namespaces
- Seccomp hardening is compiled only for x86_64; on non-x86_64 hosts the normal execution path **refuses to run** (`install_seccomp_filter()` returns `-1` and `sandbox_main()` exits with status 1 before `execv()` — the normal path does **not** fall back to running without seccomp). The `--trace` path (`trace_main()` → `sandbox_exec()`, `sandbox.c:640-648`) deliberately skips `install_seccomp_filter()` entirely, so `--trace` runs without any seccomp filter on every architecture (including non-x86_64). Note that seccomp is the **only** sandbox layer `--trace` skips: `sandbox_exec()` still calls `setup_sandbox_environment()` (`sandbox.c:414-499`) before the unfiltered `execv()`, so `PR_SET_NO_NEW_PRIVS`, the chroot/`/proc` setup, capability bounding-set clear plus `drop_all_caps()`, and the `clearenv()` + `PATH=/bin:/usr/bin`/`HOME=/` reset all still run in `--trace` mode exactly as in the normal `sandbox_main()` path (`sandbox.c:602-638`)
- No cgroup or resource limiting
- **Network, IPC, and cgroup namespaces are not created** — the clone-flag sets at `sandbox.c:811` (shell mode), `sandbox.c:887` (`--trace`), and `sandbox.c:933` (normal target path) are all `CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS` (with `CLONE_NEWUSER` additionally OR'd in at `sandbox.c:813`/`sandbox.c:935` under `--userns`, but never at the `--trace` site), with no `CLONE_NEWNET`, `CLONE_NEWIPC`, or `CLONE_NEWCGROUP`. The sandboxed process therefore shares the host's network stack (interfaces, routing, listening sockets, abstract-unix namespace), System V / POSIX IPC objects and POSIX message queues, and cgroup hierarchy with the host
- `--userns` requires unprivileged user namespaces to be enabled on the host and cannot be combined with `--trace` or `--user`. It writes `setgroups=deny` and maps namespace uid/gid `0` to the invoking caller's real uid/gid (`getuid()`/`getgid()`), so the sandboxed process is `root` inside the namespace but keeps the caller's identity on the host

---

## Contributions

Pull requests and feature requests are welcome!  
File issues or send PRs on GitHub.

---

## Disclaimer

This tool is for research purposes.  
Do **not** rely on it for strong security isolation of malicious code in production environments.

---
