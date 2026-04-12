
# 🏝️ sandbox — Minimal Linux Sandbox in C

**sandbox** is a minimalist, auditable, and hackable C program that builds a chrooted Linux environment around a target binary or a minimal shell environment, isolating execution in dedicated namespaces with tight controls on filesystem, user privileges, and process capabilities.

---

## Features

- 📦 **Builds minimal chroot environments** for a binary or a shell session
- 🔒 **Isolates with Linux namespaces**: mount, PID, UTS (hostname)
- 🚫 **Drops all Linux capabilities** using `libcap`
- 👤 **Optionally drops to the unprivileged `nobody` user** (`--user`)
- 🔍 **Supports tracing with `strace`** (`--trace`)
- 🏗️ **Auto-copies required dynamic libraries** with `ldd`
- 🧩 **Extensible**: add extra files with `--extras <file>`
- 🗄️ **Auto-populates `/etc/passwd` and `/etc/group`** as needed
- 🧹 **Wipes environment variables** for safety
- 🪶 **Less than 1000 lines, easy to audit and extend**

---

## Usage
usage:

```bash
sudo ./sandbox <rootfs> [<target-binary>] [--user] [--extras <file>]
sudo ./sandbox <rootfs> <target-binary> [--extras <file>] --trace <args...>
```

### Modes

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
    - `--trace` requires a target binary and cannot be combined with `--user`.
- **Sandbox as unprivileged user (`nobody`):**
    ```bash
    sudo ./sandbox /tmp/mychroot --user
    ```
    - *Not compatible with `--trace`.*
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
- Drops all Linux capabilities and wipes environment variables
- Executes `/bin/sh` (or the target) inside the chroot

---

## Security Model

- **Namespaces** isolate filesystem, process IDs, and hostname from the host
- **Capabilities** are dropped, so even root inside the sandbox is powerless
- **No environment variables** (except a safe `PATH`)
- **User `nobody`**: further restricts privilege for untrusted code (unless tracing)
- **No seccomp**: intentionally left out for simplicity (easy to add)
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

- Only works on Linux with root (needs namespaces, chroot, mounts)
- No seccomp syscall filtering
- No cgroup or resource limiting
- No user namespace yet (for rootless operation)

---

## Contributions

Pull requests and feature requests are welcome!  
File issues or send PRs on GitHub.

---

## Disclaimer

This tool is for research purposes.  
Do **not** rely on it for strong security isolation of malicious code in production environments.

---
