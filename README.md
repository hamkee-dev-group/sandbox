
# 🏝️ sandbox — Minimal Linux Sandbox in C

**sandbox** is a minimalist, auditable, and hackable C program that builds a chrooted Linux environment around a target binary or a minimal shell environment, isolating execution in dedicated namespaces with tight controls on filesystem, user privileges, and process capabilities.

---

## Features

- 📦 **Builds minimal chroot environments** for a binary or a shell session
- 🔒 **Isolates with Linux namespaces**: mount, PID, UTS (hostname)
- 🚫 **Clears the capability bounding set** before optional UID/GID drop, then **drops all process capabilities** using `libcap`
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

- A **C compiler** (`gcc` or `clang`)
- **libcap** development headers and library — provides `<sys/capability.h>` and `-lcap`

  ```bash
  # Debian / Ubuntu
  sudo apt install build-essential libcap-dev

  # Fedora / RHEL
  sudo dnf install gcc libcap-devel

  # Arch
  sudo pacman -S base-devel libcap
  ```

### Runtime

- **Root privileges** — required for all modes except `--userns` (namespaces, chroot, mounts).
- **`/usr/bin/ldd`** — used by every mode to discover and copy shared-library dependencies. Typically provided by `libc-bin` (Debian/Ubuntu) or `glibc-common` (Fedora/RHEL).
- **`/usr/bin/strace`** (trace mode only) — `--trace` hard-fails if strace is not present on the host.

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
usage:

```bash
sudo ./sandbox <rootfs> [<target-binary>] [--user] [--userns] [--extras <file>]
sudo ./sandbox <rootfs> <target-binary> [--extras <file>] --trace <args...>
```

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
- Clears the capability bounding set, drops to unprivileged UID/GID if requested, then drops all process capabilities and wipes environment variables
- Executes `/bin/sh` (or the target) inside the chroot

---

## Security Model

- **Namespaces** isolate filesystem, process IDs, and hostname from the host
- **Capabilities**: the bounding set is cleared before the optional UID/GID drop, and all process capability sets are dropped afterward
- **No environment variables** (except `PATH=/bin:/usr/bin` and `HOME=/`)
- **User `nobody`**: further restricts privilege for untrusted code (unless tracing)
- **Seccomp** hardens the normal sandbox execution path on x86_64 with a small fail-closed allowlist; `--trace` is intentionally left unfiltered so `strace` can still run
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

Run a binary with minimal rootfs:

```bash
sudo ./sandbox /tmp/sandbox-root /usr/bin/wc
```

---

## Limitations & Roadmap

- Requires root unless `--userns` is used for rootless operation via user namespaces
- Seccomp hardening applies only to the normal sandbox execution path on x86_64, not `--trace`
- No cgroup or resource limiting
- `--userns` requires unprivileged user namespaces to be enabled on the host and cannot be combined with `--trace` or `--user`

---

## Contributions

Pull requests and feature requests are welcome!  
File issues or send PRs on GitHub.

---

## Disclaimer

This tool is for research purposes.  
Do **not** rely on it for strong security isolation of malicious code in production environments.

---
