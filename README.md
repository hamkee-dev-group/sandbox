
# 🏝️ sandbox — Minimal Linux Sandbox in C

**sandbox** is a minimalist, auditable, and hackable C program that builds a chrooted Linux environment around a target ELF binary or a minimal shell environment, isolating execution in dedicated namespaces with tight controls on filesystem, user privileges, and process capabilities. The target must be an executable regular ELF binary; shell scripts and other non-ELF executables are rejected with `"<path> is not a binary file"`.

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
- **`ldd` on the host `PATH`** — the only universal host-side runtime dependency. Resolved via `execlp("ldd", ...)`, so any location on `PATH` works. Used in both setup paths (target-binary mode and shell mode) to discover and copy shared-library dependencies. Typically provided by `libc-bin` (Debian/Ubuntu) or `glibc-common` (Fedora/RHEL).
- **`/usr/bin/strace`** — this bullet separates the **host-side runtime requirement** from the **rootfs copy behavior**, because they are not the same thing:
    - **Host-side runtime requirement (when strace is mandatory):** strictly required **only** for `--trace`. If the host is missing `/usr/bin/strace`, `--trace` hard-fails with `Failed to copy strace (required for --trace)` — this is the fatal branch in `build_rootfs()` guarded by `if (trace_mode)` (`sandbox.c:702-705`). For the shell sandbox and for non-`--trace` target runs, strace is **not** a host-side requirement: those modes still start and run to completion when strace is absent on the host.
    - **Rootfs copy behavior (when strace is attempted regardless of `--trace`):** both shell mode and target mode attempt to place `/usr/bin/strace` into the rootfs on every run, regardless of `--trace`:
        - **Shell mode** lists `/usr/bin/strace` in the hardcoded `essential_bins[]` array (`sandbox.c:52-68`) and the essential-bins loop attempts to copy it like every other entry; if that copy fails it is logged as `Failed to copy essential bin: /usr/bin/strace` and skipped, same best-effort handling as the other essential bins (`sandbox.c:727-735`).
        - **Target mode** (`build_rootfs()`) likewise calls `copy_file("/usr/bin/strace", <rootfs>/usr/bin/strace)` unconditionally (`sandbox.c:699-709`). Whether a `copy_file()` failure here is fatal depends solely on `trace_mode`: under `--trace` the `if (trace_mode)` branch returns `-1` and aborts setup; in all other runs the failure is silently tolerated and `build_rootfs()` continues (the `else` branch that runs `copy_ldd_deps("/usr/bin/strace", ...)` is only entered when the copy succeeded).
    - **Observable consequence:** on a host with strace installed, a normal (non-`--trace`) run in either mode produces a rootfs containing `<rootfs>/usr/bin/strace`, even though that file is not required for the run to succeed.

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

## Usage

```bash
Usage: ./sandbox <rootfs> [<target-binary>] [--user] [--userns] [--extras <file>] [--trace <args...>]
```

Run as root (e.g., via `sudo`) unless `--userns` is used.

After `<rootfs>`, `sandbox` scans `argv` left-to-right until `--trace` (`sandbox.c:759-779`). At each position, `--user`, `--userns`, and `--extras <file>` are recognized as options and may appear either before or after the target binary; `--extras` also consumes the following token as its list-file path, so that token is **not** passed through to the target. Any remaining token is positional: the first positional token becomes `<target-binary>`, and every later positional token is appended to `target_args[]` and passed as an argument to the target when it is executed (`sandbox.c:621-629`). For example, `./sandbox /tmp/x2 --userns /bin/echo hi` runs `/bin/echo hi`, and `./sandbox /tmp/x3 /bin/echo one --userns two` runs `/bin/echo one two`.

`--trace` terminates sandbox option parsing: when the parser encounters it, it records the index and immediately `break`s out of the option loop (`sandbox.c:764-767`), after which the traced command line is built from every token after `--trace` (`sandbox.c:866-885`). All sandbox flags (`--user`, `--userns`, `--extras <file>`) must therefore appear **before** `--trace`; every token after `--trace` is appended to the target binary's argv and is never interpreted as a flag for `sandbox` or `strace`. The ordering effect shows up in which error fires: `./sandbox /tmp/x /bin/echo --userns --trace` fails with `--userns is not compatible with --trace.` because `--userns` was parsed as a sandbox flag before the boundary and tripped the conflict check, whereas `./sandbox /tmp/x /bin/echo --trace --userns` (run non-root) fails with `This program must be run as root (or use --userns).` — parsing stopped at `--trace`, so the second `--userns` was passed through to `/bin/echo` as an argument, the `--userns`/`--trace` conflict check never ran, and the root check fired first instead.

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
    - Drops you into `/bin/sh` with the exact set of binaries hardcoded in `essential_bins[]` (`sandbox.c:52-68`): `/bin/sh`, `/bin/ls`, `/bin/cat`, `/bin/echo`, `/bin/mkdir`, `/bin/rm`, `/usr/bin/grep`, `/usr/bin/head`, `/usr/bin/tail`, `/usr/bin/wc`, `/usr/bin/stat`, `/usr/bin/ldd`, `/usr/bin/strace`, `/usr/bin/du`.
    - These binaries are copied on a **best-effort** basis: any `copy_file()` failure for an entry in `essential_bins[]` (including, but not limited to, the source being absent on the host — `copy_file()` also fails on destination open/write errors and other I/O failures) is logged as `Failed to copy essential bin: <path>` and skipped (`sandbox.c:727-731`), so the resulting shell toolbox is whatever subset of that list was successfully copied — nothing is added beyond it. A `copy_ldd_deps()` failure for a binary that was copied still aborts setup.
- **Run a specific binary:**
    ```bash
    sudo ./sandbox /tmp/mychroot /usr/bin/ls
    ```
    - `<target-binary>` must be an executable regular ELF binary (checked via `access(X_OK)`, `S_ISREG`, and the `\x7fELF` magic bytes). Shell scripts and other non-ELF executables are rejected with `"<path> is not a binary file"`.
    - Rootfs copy behavior (`build_rootfs()`, `sandbox.c:650-709`) — distinct from the host-side runtime prerequisites listed above: the target is **always** copied to `<rootfs>/usr/bin/<basename>` (`sandbox.c:666-670`) and `/bin/sh` is **always** copied to `<rootfs>/bin/sh` (`sandbox.c:689-694`). For absolute-path targets whose original path differs from `/usr/bin/<basename>` (e.g. `/usr/local/bin/foo`, `/sbin/foo`), the target is additionally copied to the same absolute path inside the rootfs (`sandbox.c:672-688`); this second copy is what the `--trace` path uses, because `trace_main` execs the absolute original path when `target[0] == '/'` (`sandbox.c:872-875`), whereas the normal (non-trace) execution path always execs `/usr/bin/<basename>` regardless of the input path (`sandbox.c:621-630`). Shared-library dependencies for both the target and `/bin/sh` are then discovered by invoking the host `ldd` (`copy_ldd_deps()`, `sandbox.c:237-309`) and copied into the rootfs. `/usr/bin/strace` is also copied when present on the host; a missing host strace is only fatal under `--trace` (`sandbox.c:699-704`).
- **Trace a binary (copies all files accessed during run):**
    ```bash
    sudo ./sandbox /tmp/mychroot /usr/bin/curl --trace "https://example.com"
    ```
    - `--trace` requires a target binary and cannot be combined with `--user` or `--userns`.
    - `--trace` consumes all subsequent argv tokens as arguments to the traced binary, so `--user`, `--userns`, and `--extras <file>` must appear **before** `--trace`; otherwise they are silently passed to the target binary (not to `sandbox` and not to `strace`) and the `--user`/`--userns` conflict checks above are bypassed. For example, `./sandbox /tmp/sbroot /bin/echo --trace --userns` runs `/bin/echo --userns` inside the sandbox and does **not** enable user-namespace mode.
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
    - Device nodes (`/dev/null`, `/dev/zero`, `/dev/tty`) are bind-mounted from the host instead of created with `mknod`.
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
- Builds up a new root filesystem (`<rootfs>`) with essential binaries/libraries
- Optionally copies a target binary and its dependencies
- Optionally adds files specified in `--extras`
- Optionally traces binary with `strace` to discover runtime file dependencies
- Optionally switches to UID/GID 65534 (`nobody`)
- Optionally creates a user namespace with `--userns` for rootless operation: writes `deny` to `/proc/<pid>/setgroups` and maps namespace uid/gid `0` to the invoking caller's real uid/gid (`getuid()`/`getgid()`) via `/proc/<pid>/uid_map` and `/proc/<pid>/gid_map`, so the sandboxed process appears as `root` inside the namespace while retaining the caller's identity on the host. `--userns` cannot be combined with `--user` or `--trace`.
- In `--userns` mode, the parent process invokes `write_uid_gid_map()` (sandbox.c:101-136) against the child pid to populate these maps: it writes `deny` to `/proc/<pid>/setgroups`, then writes `0 <caller-uid> 1\n` to `/proc/<pid>/uid_map` and `0 <caller-gid> 1\n` to `/proc/<pid>/gid_map`, mapping the caller's real UID/GID to **UID 0 / GID 0 (root) inside the user namespace**. This is distinct from the `--user` flag, which drops the sandboxed process to UID/GID 65534 (`nobody`) inside the sandbox; `--userns` instead gives it namespace-root while keeping the caller's identity on the host.
- Sets `PR_SET_NO_NEW_PRIVS` via `prctl()` as the first step of `setup_sandbox_environment()` (sandbox.c:423), before `chroot` and before the capability bounding set is cleared or process capabilities are dropped. Once set, the bit is inherited across `execve()` and prevents the kernel from granting new privileges through setuid/setgid binaries (or file capabilities) executed inside the sandbox.
- Clears the capability bounding set, drops to unprivileged UID/GID if requested, wipes environment variables with `clearenv()` and restores only `PATH=/bin:/usr/bin` and `HOME=/`, and finally drops all process capabilities
- Execution splits into two distinct paths:
    - **Normal execution** (no `--trace`) runs through `sandbox_main()`, which calls `install_seccomp_filter()` to install a fail-closed x86_64-only seccomp allowlist before `execv()`. On non-x86_64 hosts `install_seccomp_filter()` returns an error and the sandbox fails closed without executing the target.
    - **Trace execution** (`--trace`) runs through `trace_main()` → `sandbox_exec()`, which intentionally does **not** call `install_seccomp_filter()`; the target is `execv()`'d under `strace` with no seccomp filter applied, because the allowlist would otherwise block `ptrace` and the syscalls `strace` needs.
- Executes `/bin/sh` (or the target) inside the chroot

---

## Security Model

- **Namespaces** isolate filesystem, process IDs, and hostname from the host
- **Capabilities**: the bounding set is cleared before the optional UID/GID drop; all process capability sets are dropped only after the environment reset
- **No environment variables**: `clearenv()` runs after the UID/GID drop, then `PATH=/bin:/usr/bin` and `HOME=/` are restored, and only after that are all process capabilities dropped
- **User `nobody`**: further restricts privilege for untrusted code (unless tracing)
- **User namespace (`--userns`)**: optional rootless mode. Writes `deny` to `/proc/<pid>/setgroups` and maps namespace uid/gid `0` to the invoking caller's real uid/gid (`getuid()`/`getgid()`) via `/proc/<pid>/uid_map` and `/proc/<pid>/gid_map`. The process is `root` inside the namespace but keeps the caller's identity on the host — it is **not** a drop to `nobody`. Mutually exclusive with `--user` and `--trace`.
- **Seccomp**: only the normal execution path (`sandbox_main()` → `install_seccomp_filter()` → `execv()`) installs a filter — a small fail-closed allowlist **compiled only for x86_64** (the entire filter is inside `#if defined(__x86_64__)` in `install_seccomp_filter()`). On other architectures the function returns `-1` and `sandbox_main()` exits with status 1 before `execv()`, so **the normal execution path will not start on non-x86_64 hosts** — it does not fall back to running unfiltered. The `--trace` path (`trace_main()` → `sandbox_exec()` → `execv()` under `strace`) intentionally does **not** call `install_seccomp_filter()` and runs entirely unfiltered on every architecture (including non-x86_64), because the allowlist would block the `ptrace` and related syscalls `strace` depends on
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

Run an ELF binary with minimal rootfs (non-ELF targets such as shell scripts are rejected):

```bash
sudo ./sandbox /tmp/sandbox-root /usr/bin/wc
```

---

## Limitations & Roadmap

- Requires root unless `--userns` is used for rootless operation via user namespaces
- Seccomp hardening is compiled only for x86_64; on non-x86_64 hosts the normal execution path **refuses to run** (`install_seccomp_filter()` returns `-1` and `sandbox_main()` exits with status 1 before `execv()` — the normal path does **not** fall back to running without seccomp). The `--trace` path (`trace_main()` → `sandbox_exec()`) deliberately skips `install_seccomp_filter()` entirely, so `--trace` runs without any seccomp filter on every architecture (including non-x86_64)
- No cgroup or resource limiting
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
