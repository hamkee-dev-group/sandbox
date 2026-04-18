
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

  ```bash
  # Debian / Ubuntu
  sudo apt install build-essential clang libcap-dev

  # Fedora / RHEL
  sudo dnf install clang libcap-devel

  # Arch
  sudo pacman -S base-devel clang libcap
  ```

### Runtime

- **Root privileges** — required for all modes except `--userns` (namespaces, chroot, mounts).
- **`ldd` on the host `PATH`** — the only universal host-side runtime dependency. Resolved via `execlp("ldd", ...)`, so any location on `PATH` works. Used in both setup paths (target-binary mode and shell mode) to discover and copy shared-library dependencies. Typically provided by `libc-bin` (Debian/Ubuntu) or `glibc-common` (Fedora/RHEL).
- **`/usr/bin/strace`** — required **only** for `--trace`. Not needed for the shell sandbox or for non-trace target runs; `--trace` hard-fails if strace is missing on the host.

  ```bash
  # Debian / Ubuntu
  sudo apt install strace

  # Fedora / RHEL
  sudo dnf install strace

  # Arch
  sudo pacman -S strace
  ```

---

## Usage

```bash
Usage: ./sandbox <rootfs> [<target-binary>] [--user] [--userns] [--extras <file>] [--trace <args...>]
```

Run as root (e.g., via `sudo`) unless `--userns` is used.

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
    - Drops you into `/bin/sh` with essential tools (`ls`, `cat`, ...).
- **Run a specific binary:**
    ```bash
    sudo ./sandbox /tmp/mychroot /usr/bin/ls
    ```
    - `<target-binary>` must be an executable regular ELF binary (checked via `access(X_OK)`, `S_ISREG`, and the `\x7fELF` magic bytes). Shell scripts and other non-ELF executables are rejected with `"<path> is not a binary file"`.
- **Trace a binary (copies all files accessed during run):**
    ```bash
    sudo ./sandbox /tmp/mychroot /usr/bin/curl --trace "https://example.com"
    ```
    - `--trace` requires a target binary and cannot be combined with `--user` or `--userns`.
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
    ```
    - `extras.txt` contains a list of absolute file paths, one per line.

---

## How It Works

- Creates a new mount, PID, and UTS namespace
- Builds up a new root filesystem (`<rootfs>`) with essential binaries/libraries
- Optionally copies a target binary and its dependencies
- Optionally adds files specified in `--extras`
- Optionally traces binary with `strace` to discover runtime file dependencies
- Optionally switches to UID/GID 65534 (`nobody`)
- Optionally creates a user namespace with `--userns` for rootless operation: writes `deny` to `/proc/<pid>/setgroups` and maps namespace uid/gid `0` to the invoking caller's real uid/gid (`getuid()`/`getgid()`) via `/proc/<pid>/uid_map` and `/proc/<pid>/gid_map`, so the sandboxed process appears as `root` inside the namespace while retaining the caller's identity on the host. `--userns` cannot be combined with `--user` or `--trace`.
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
