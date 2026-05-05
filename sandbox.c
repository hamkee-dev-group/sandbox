#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <elf.h>
#include <stddef.h>
#include <libgen.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <sys/syscall.h>



#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif

#ifndef __X32_SYSCALL_BIT
#define __X32_SYSCALL_BIT 0x40000000
#endif

#define SECCOMP_ALLOW_SYSCALL(nr) \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

#define PATH_MAX 1024
#define TARGET_NAME_MAX (PATH_MAX - sizeof("/usr/bin/"))
#define STACK_SIZE (1024 * 1024)
static char child_stack[STACK_SIZE];
char *rootfs;
char target_name[TARGET_NAME_MAX + 1];
int target_argc = 0;
char *target_args[64];
int drop_to_nobody = 0;
int trace_mode = 0;
int userns_mode = 0;
int prepare_only = 0;
int userns_pipe[2] = {-1, -1};

int sandbox_exec(char *const argv[]);

const char *dirs[] = {"/bin", "/usr/bin", "/etc", "/proc", "/dev", "/tmp", NULL};
const char *essential_bins[] = {
    "/bin/sh",
    "/bin/ls",
    "/bin/cat",
    "/bin/echo",
    "/bin/mkdir",
    "/bin/rm",
    "/usr/bin/grep",
    "/usr/bin/head",
    "/usr/bin/tail",
    "/usr/bin/wc",
    "/usr/bin/stat",
    "/usr/bin/ldd",
    "/usr/bin/strace",
    "/usr/bin/du",
    NULL
};

static int checked_path_copy(char *out, size_t out_sz, const char *path)
{
    int n = snprintf(out, out_sz, "%s", path);

    if (n < 0 || n >= (int)out_sz) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "path too long: %s\n", path);
        return -1;
    }
    return 0;
}

static int checked_path_join(char *out, size_t out_sz, const char *a, const char *b)
{
    int n = snprintf(out, out_sz, "%s%s", a, b);

    if (n < 0 || n >= (int)out_sz) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "path too long: %s%s\n", a, b);
        return -1;
    }
    return 0;
}

static int checked_path_join3(char *out, size_t out_sz, const char *a, const char *b, const char *c)
{
    int n = snprintf(out, out_sz, "%s%s%s", a, b, c);

    if (n < 0 || n >= (int)out_sz) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "path too long: %s%s%s\n", a, b, c);
        return -1;
    }
    return 0;
}

int build_usr_bin_target_path(char *dst, size_t dst_size, const char *name) {
    int n;

    if (strlen(name) > TARGET_NAME_MAX) {
        fprintf(stderr, "Target basename too long\n");
        return -1;
    }

    n = snprintf(dst, dst_size, "/usr/bin/%s", name);
    if (n < 0 || (size_t)n >= dst_size) {
        fprintf(stderr, "Target path too long\n");
        return -1;
    }

    return 0;
}

int drop_bounding_caps(void) {
    int cap = 0;
    for (;; cap++) {
        int r = prctl(PR_CAPBSET_READ, cap, 0, 0, 0);
        if (r < 0) {
            if (errno == EINVAL)
                break;
            perror("prctl(PR_CAPBSET_READ)");
            return -1;
        }
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) < 0) {
            fprintf(stderr, "PR_CAPBSET_DROP(%d): %s\n", cap, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int drop_all_caps(void) {
    cap_t caps = cap_init();
    if (!caps) {
        perror("cap_init");
        return -1;
    }
    if (cap_set_proc(caps) == -1) {
        perror("cap_set_proc");
        cap_free(caps);
        return -1;
    }
    cap_free(caps);
    return 0;
}
int write_uid_gid_map(pid_t child_pid)
{
    char path[64];
    FILE *fp;
    uid_t uid = getuid();
    gid_t gid = getgid();
    int n;

    n = snprintf(path, sizeof(path), "/proc/%d/setgroups", child_pid);
    if (n < 0 || n >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "path too long: /proc/%d/setgroups\n", child_pid);
        return -1;
    }
    fp = fopen(path, "w");
    if (!fp) {
        perror("open setgroups");
        return -1;
    }
    fprintf(fp, "deny");
    fclose(fp);

    n = snprintf(path, sizeof(path), "/proc/%d/uid_map", child_pid);
    if (n < 0 || n >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "path too long: /proc/%d/uid_map\n", child_pid);
        return -1;
    }
    fp = fopen(path, "w");
    if (!fp) {
        perror("open uid_map");
        return -1;
    }
    fprintf(fp, "0 %d 1\n", uid);
    fclose(fp);

    n = snprintf(path, sizeof(path), "/proc/%d/gid_map", child_pid);
    if (n < 0 || n >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        fprintf(stderr, "path too long: /proc/%d/gid_map\n", child_pid);
        return -1;
    }
    fp = fopen(path, "w");
    if (!fp) {
        perror("open gid_map");
        return -1;
    }
    fprintf(fp, "0 %d 1\n", gid);
    fclose(fp);

    return 0;
}

int is_binary(const char *path, int *invalid_elf) {
    struct stat st;
    int fd;
    ssize_t n;
    unsigned char hdr[20];
    unsigned int e_type;
    unsigned int e_machine;

    *invalid_elf = 0;

    if (access(path, X_OK) != 0) {
        return 0;
    }
    if (stat(path, &st) != 0) {
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        return 0;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    n = read(fd, hdr, sizeof(hdr));
    close(fd);
    if (n < SELFMAG || hdr[EI_MAG0] != ELFMAG0 || hdr[EI_MAG1] != ELFMAG1 || hdr[EI_MAG2] != ELFMAG2 || hdr[EI_MAG3] != ELFMAG3) {
        return 0;
    }
    if (n < (ssize_t)sizeof(hdr) ||
        (hdr[EI_CLASS] != ELFCLASS32 && hdr[EI_CLASS] != ELFCLASS64) ||
        (hdr[EI_DATA] != ELFDATA2LSB && hdr[EI_DATA] != ELFDATA2MSB) ||
        hdr[EI_VERSION] != EV_CURRENT) {
        *invalid_elf = 1;
        return 0;
    }
    if (hdr[EI_DATA] == ELFDATA2LSB) {
        e_type = (unsigned int)hdr[16] | ((unsigned int)hdr[17] << 8);
        e_machine = (unsigned int)hdr[18] | ((unsigned int)hdr[19] << 8);
    } else {
        e_type = ((unsigned int)hdr[16] << 8) | (unsigned int)hdr[17];
        e_machine = ((unsigned int)hdr[18] << 8) | (unsigned int)hdr[19];
    }
    if ((e_type != ET_EXEC && e_type != ET_DYN) || e_machine == EM_NONE) {
        *invalid_elf = 1;
        return 0;
    }
    return 1;
}

int validate_target_binary(const char *path)
{
    int invalid_elf;

    if (is_binary(path, &invalid_elf))
        return 0;
    if (invalid_elf)
        fprintf(stderr, "%s is not a valid ELF executable\n", path);
    else
        fprintf(stderr, "%s is not a binary file\n", path);
    return -1;
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    if (checked_path_copy(tmp, sizeof(tmp), path) < 0)
        return -1;

    for (char *p = tmp + 1; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;

    return 0;
}

int copy_file(const char *src, const char *dst)
{
    int in, out;
    char *tmp;
    char buf[8192];
    ssize_t n;

    in = open(src, O_RDONLY);

    if (in < 0)
    {
        int saved_errno = errno;
        fprintf(stderr, "copy_file: failed to open %s -> %s: %s\n", src, dst, strerror(saved_errno));
        errno = saved_errno;
        return -1;
    }
    tmp = strdup(dst);
    if (!tmp)
    {
        int saved_errno = errno;
        fprintf(stderr, "copy_file: failed to copy %s -> %s: %s\n", src, dst, strerror(saved_errno));
        close(in);
        errno = saved_errno;
        return -1;
    }
    if (mkdir_p(dirname(tmp), 0755) < 0)
    {
        int saved_errno = errno;
        fprintf(stderr, "copy_file: failed to create parent for %s -> %s: %s\n", src, dst, strerror(saved_errno));
        free(tmp);
        close(in);
        errno = saved_errno;
        return -1;
    }
    free(tmp);

    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0)
    {
        int saved_errno = errno;
        fprintf(stderr, "copy_file: failed to create %s -> %s: %s\n", src, dst, strerror(saved_errno));
        close(in);
        errno = saved_errno;
        return -1;
    }

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(out, buf + written, n - written);
            if (w <= 0) {
                int saved_errno = errno;
                if (w == 0)
                    saved_errno = EIO;
                fprintf(stderr, "copy_file: failed to write %s -> %s: %s\n", src, dst, strerror(saved_errno));
                close(in); 
                close(out); 
                errno = saved_errno;
                return -1; 
            }
            written += w;
        }
    }
    if (n < 0) {
        int saved_errno = errno;
        fprintf(stderr, "copy_file: failed to read %s -> %s: %s\n", src, dst, strerror(saved_errno));
        close(in);
        close(out);
        errno = saved_errno;
        return -1;
    }
    close(in);
    close(out);
    if (prepare_only)
        printf("copied %s -> %s\n", src, dst);
    return 0;
}

int copy_ldd_deps(const char *bin, const char *root)
{
    char line[PATH_MAX];
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) == -1)
    {
        perror("pipe");
        return -1;
    }

    pid = fork();
    if (pid == -1)
    {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("ldd", "ldd", bin, (char *)NULL);
        fprintf(stderr, "ldd exec failed for %s: %s\n", bin, strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);
    FILE *fp = fdopen(pipefd[0], "r");
    if (!fp)
    {
        perror("fdopen");
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }

    int ret = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "not found")) {
            fprintf(stderr, "ldd: unresolved dependency for %s: %s", bin, line);
            ret = -1;
            continue;
        }
        char *start = strchr(line, '/');
        if (!start)
            continue;
        char *end = strpbrk(start, " \n");
        if (end)
            *end = '\0';

        char dst[PATH_MAX];
        if (checked_path_join(dst, sizeof(dst), root, start) < 0) {
            ret = -1;
            continue;
        }
        if (copy_file(start, dst) < 0) {
            fprintf(stderr, "Failed to copy dependency for %s: %s -> %s: %s\n", bin, start, dst, strerror(errno));
            ret = -1;
        }
    }

    fclose(fp);
    int status;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) {
        fprintf(stderr, "ldd discovery failed for %s: ldd did not exit normally\n", bin);
        ret = -1;
    } else if (WEXITSTATUS(status) == 127) {
        fprintf(stderr, "ldd discovery failed for %s: ldd exec failed\n", bin);
        ret = -1;
    } else if (WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ldd discovery failed for %s: ldd exited with status %d\n", bin, WEXITSTATUS(status));
        ret = -1;
    }
    return ret;
}

int validate_rootfs_path(const char *path)
{
    struct stat st;
    char copy[PATH_MAX];
    char *parent;
    int n;
    int saved_errno;

    if (stat(path, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode))
        {
            fprintf(stderr, "rootfs '%s': not a directory\n", path);
            return -1;
        }
        if (access(path, W_OK | X_OK) != 0)
        {
            fprintf(stderr, "rootfs '%s': not writable/searchable: %s\n", path, strerror(errno));
            return -1;
        }
        return 0;
    }
    saved_errno = errno;
    if (saved_errno != ENOENT && saved_errno != ENOTDIR)
    {
        fprintf(stderr, "rootfs '%s': %s\n", path, strerror(saved_errno));
        return -1;
    }

    n = snprintf(copy, sizeof(copy), "%s", path);
    if (n < 0 || n >= (int)sizeof(copy))
    {
        fprintf(stderr, "Rootfs path too long\n");
        return -1;
    }
    parent = dirname(copy);
    if (stat(parent, &st) != 0)
    {
        fprintf(stderr, "rootfs '%s': parent '%s': %s\n", path, parent, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "rootfs '%s': parent '%s' is not a directory\n", path, parent);
        return -1;
    }
    if (access(parent, W_OK | X_OK) != 0)
    {
        fprintf(stderr, "rootfs '%s': parent '%s' is not writable/searchable: %s\n", path, parent, strerror(errno));
        return -1;
    }
    return 0;
}

int has_parent_ref_component(const char *path)
{
    const char *p = path;

    while (*p)
    {
        while (*p == '/')
            p++;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        if (p - start == 2 && start[0] == '.' && start[1] == '.')
            return 1;
    }
    return 0;
}

int copy_extras(const char *listfile)
{
    char line[PATH_MAX];
    char listcopy[PATH_MAX];
    char listdir[PATH_MAX];
    FILE *f;
    int ret = 0;
    int n;

    n = snprintf(listcopy, sizeof(listcopy), "%s", listfile);
    if (n < 0 || n >= (int)sizeof(listcopy))
    {
        fprintf(stderr, "extras file path too long\n");
        return -1;
    }
    n = snprintf(listdir, sizeof(listdir), "%s", dirname(listcopy));
    if (n < 0 || n >= (int)sizeof(listdir))
    {
        fprintf(stderr, "extras file path too long\n");
        return -1;
    }
    
    f = fopen(listfile, "r");
    if (!f)
    {
        perror("extras file");
        return -1;
    }
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\n")] = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strlen(line) == 0)
            continue;
        if (*p == '#')
            continue;
        if (has_parent_ref_component(line)) {
            fprintf(stderr, "extras: rejected path traversal entry %s\n", line);
            ret = -1;
            continue;
        }

        char src[PATH_MAX];
        char dst[PATH_MAX];
        int is_dir = line[strlen(line) - 1] == '/';
        if (line[0] == '/')
        {
            n = snprintf(src, sizeof(src), "%s", line);
            if (n < 0 || n >= (int)sizeof(src))
            {
                fprintf(stderr, "extras: failed to copy %s -> %s: %s\n", line, line, strerror(ENAMETOOLONG));
                ret = -1;
                continue;
            }
            n = snprintf(dst, sizeof(dst), "%s%s", rootfs, line);
        }
        else
        {
            n = snprintf(src, sizeof(src), "%s/%s", listdir, line);
            if (n < 0 || n >= (int)sizeof(src))
            {
                fprintf(stderr, "extras: failed to copy %s -> %s: %s\n", line, line, strerror(ENAMETOOLONG));
                ret = -1;
                continue;
            }
            n = snprintf(dst, sizeof(dst), "%s/%s", rootfs, line);
        }
        if (n < 0 || n >= (int)sizeof(dst))
        {
            fprintf(stderr, "extras: failed to copy %s -> %s: %s\n", src, line, strerror(ENAMETOOLONG));
            ret = -1;
            continue;
        }
        if (is_dir)
        {
            if (mkdir_p(dst, 0755) < 0)
            {
                fprintf(stderr, "extras: failed to copy %s -> %s: %s\n", src, dst, strerror(errno));
                ret = -1;
                continue;
            }
            printf("extras: created %s\n", dst);
            continue;
        }

        char parent[PATH_MAX];
        n = snprintf(parent, sizeof(parent), "%s", dst);
        if (n < 0 || n >= (int)sizeof(parent))
        {
            fprintf(stderr, "extras: failed to copy %s -> %s: %s\n", src, dst, strerror(ENAMETOOLONG));
            ret = -1;
            continue;
        }
        if (mkdir_p(dirname(parent), 0755) < 0)
        {
            fprintf(stderr, "extras: failed to copy %s -> %s: %s\n", src, dst, strerror(errno));
            ret = -1;
            continue;
        }
        if (copy_file(src, dst) < 0)
        {
            fprintf(stderr, "extras: failed to copy %s -> %s: %s\n", src, dst, strerror(errno));
            ret = -1;
            continue;
        }
        if (!prepare_only)
            printf("extras: copied %s -> %s\n", src, dst);
    }
    fclose(f);
    return ret;
}

int create_dev_nodes(const char *root)
{
    char path[PATH_MAX];

    struct
    {
        const char *name;
        int major;
        int minor;
    } devs[] = {
        {"null", 1, 3},
        {"zero", 1, 5},
        {"tty", 5, 0},
        {NULL, 0, 0}};

    if (checked_path_join(path, sizeof(path), root, "/dev") < 0)
        return -1;
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
    {
        perror("mkdir /dev");
        return -1;
    }

    for (int i = 0; devs[i].name; ++i)
    {
        if (checked_path_join3(path, sizeof(path), root, "/dev/", devs[i].name) < 0)
            return -1;
        if (userns_mode) {
            int fd = open(path, O_WRONLY | O_CREAT, 0666);
            if (fd < 0 && errno != EEXIST) {
                perror(devs[i].name);
                return -1;
            }
            if (fd >= 0) close(fd);
        } else if (mknod(path, S_IFCHR | 0666, makedev(devs[i].major, devs[i].minor)) < 0 && errno != EEXIST)
        {
            perror(devs[i].name);
            return -1;
        }
    }

    return 0;
}

int create_etc_files(const char *root)
{
    char path[PATH_MAX];
    FILE *fp;
    if (checked_path_join(path, sizeof(path), root, "/etc") < 0)
        return -1;
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
    {
        perror("mkdir /etc");
        return -1;
    }
    if(drop_to_nobody) {
        memset(path, 0, PATH_MAX);
        if (checked_path_join(path, sizeof(path), root, "/etc/passwd") < 0)
            return -1;
        fp = fopen(path, "w+");
        if(!fp) {
            perror("fopen passwd");
            return -1;
        }
        fprintf(fp, "nobody:x:65534:65534:nobody:/home:/bin/sh\n");
        fclose(fp);
        
        memset(path, 0, PATH_MAX);
        if (checked_path_join(path, sizeof(path), root, "/etc/group") < 0)
            return -1;
        fp = fopen(path, "w+");
        if(!fp) {
            perror("fopen group");
            return -1;
        }
        fprintf(fp, "nobody:x:65534:\n");
        fclose(fp);
    }
    return 0;
}

int setup_sandbox_environment(void)
{
    char proc_path[PATH_MAX];
    int uid = 0;
    int gid = 0;
    if(drop_to_nobody) {
        uid = 65534;
        gid = 65534;
    } 
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    {
        perror("prctl(PR_SET_NO_NEW_PRIVS)");
        return -1;
    }

    if (sethostname("sandbox", 7) < 0)
    {
        perror("sethostname");
        return -1;
    }
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    {
        perror("mount MS_PRIVATE");
        return -1;
    }
    if (checked_path_join(proc_path, sizeof(proc_path), rootfs, "/proc") < 0)
        return -1;
    mkdir(proc_path, 0755);
    if (userns_mode) {
        const char *dev_names[] = {"null", "zero", "tty", NULL};
        for (int i = 0; dev_names[i]; i++) {
            char src[PATH_MAX], mnt[PATH_MAX];
            if (checked_path_join(src, sizeof(src), "/dev/", dev_names[i]) < 0)
                return -1;
            if (checked_path_join3(mnt, sizeof(mnt), rootfs, "/dev/", dev_names[i]) < 0)
                return -1;
            if (mount(src, mnt, NULL, MS_BIND, NULL) < 0) {
                fprintf(stderr, "bind mount %s: %s\n", src, strerror(errno));
                return -1;
            }
        }
    }
    if (chroot(rootfs) < 0)
    {
        perror("chroot");
        return -1;
    }
    if(chdir("/") < 0) {
        perror("chdir");
        return -1;
    }
    if(mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, "") <0)
    {
        perror("mount /proc");
        return -1;
    }

    if (drop_bounding_caps() < 0)
        return -1;

    if (setgid(gid) < 0)
    {
        perror("setgid");
        return -1;
    }
    if(setuid(uid) < 0) {
        perror("setuid");
        return -1;
    }

    if (clearenv() != 0)
    {
        perror("clearenv");
        return -1;
    }
    if (setenv("PATH", "/bin:/usr/bin", 1) < 0)
    {
        perror("setenv PATH");
        return -1;
    }
    if (setenv("HOME", "/", 1) < 0)
    {
        perror("setenv HOME");
        return -1;
    }
    if (drop_all_caps() < 0)
        return -1;
    return 0;
}

int install_seccomp_filter(void)
{
#if defined(__x86_64__)
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, __X32_SYSCALL_BIT, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
         
        SECCOMP_ALLOW_SYSCALL(__NR_execve),
        SECCOMP_ALLOW_SYSCALL(__NR_exit),
        SECCOMP_ALLOW_SYSCALL(__NR_exit_group),
        SECCOMP_ALLOW_SYSCALL(__NR_read),
        SECCOMP_ALLOW_SYSCALL(__NR_write),
        SECCOMP_ALLOW_SYSCALL(__NR_close),
        SECCOMP_ALLOW_SYSCALL(__NR_openat),
        SECCOMP_ALLOW_SYSCALL(__NR_access),
        SECCOMP_ALLOW_SYSCALL(__NR_fstat),
        SECCOMP_ALLOW_SYSCALL(__NR_pread64),
        SECCOMP_ALLOW_SYSCALL(__NR_lseek),
        SECCOMP_ALLOW_SYSCALL(__NR_getdents64),
        SECCOMP_ALLOW_SYSCALL(__NR_statfs),
        SECCOMP_ALLOW_SYSCALL(__NR_fcntl),
        SECCOMP_ALLOW_SYSCALL(__NR_ioctl),
        SECCOMP_ALLOW_SYSCALL(__NR_futex),
        SECCOMP_ALLOW_SYSCALL(__NR_brk),
        SECCOMP_ALLOW_SYSCALL(__NR_mmap),
        SECCOMP_ALLOW_SYSCALL(__NR_mprotect),
        SECCOMP_ALLOW_SYSCALL(__NR_munmap),
        SECCOMP_ALLOW_SYSCALL(__NR_arch_prctl),
        SECCOMP_ALLOW_SYSCALL(__NR_prctl),
        SECCOMP_ALLOW_SYSCALL(__NR_prlimit64),
        SECCOMP_ALLOW_SYSCALL(__NR_rt_sigaction),
        SECCOMP_ALLOW_SYSCALL(__NR_rt_sigprocmask),
        SECCOMP_ALLOW_SYSCALL(__NR_rt_sigreturn),
        SECCOMP_ALLOW_SYSCALL(__NR_set_tid_address),
        SECCOMP_ALLOW_SYSCALL(__NR_set_robust_list),
        SECCOMP_ALLOW_SYSCALL(__NR_getuid),
        SECCOMP_ALLOW_SYSCALL(__NR_geteuid),
        SECCOMP_ALLOW_SYSCALL(__NR_getgid),
        SECCOMP_ALLOW_SYSCALL(__NR_getegid),
        SECCOMP_ALLOW_SYSCALL(__NR_getpid),
        SECCOMP_ALLOW_SYSCALL(__NR_getppid),
        SECCOMP_ALLOW_SYSCALL(__NR_getcwd),
        SECCOMP_ALLOW_SYSCALL(__NR_wait4),
        SECCOMP_ALLOW_SYSCALL(__NR_clone),
        SECCOMP_ALLOW_SYSCALL(__NR_dup2),
#ifdef __NR_pipe2
        SECCOMP_ALLOW_SYSCALL(__NR_pipe2),
#endif
#ifdef __NR_fork
        SECCOMP_ALLOW_SYSCALL(__NR_fork),
#endif
#ifdef __NR_vfork
        SECCOMP_ALLOW_SYSCALL(__NR_vfork),
#endif
#ifdef __NR_newfstatat
        SECCOMP_ALLOW_SYSCALL(__NR_newfstatat),
#endif
#ifdef __NR_fadvise64
        SECCOMP_ALLOW_SYSCALL(__NR_fadvise64),
#endif
#ifdef __NR_getrandom
        SECCOMP_ALLOW_SYSCALL(__NR_getrandom),
#endif
#ifdef __NR_rseq
        SECCOMP_ALLOW_SYSCALL(__NR_rseq),
#endif
#ifdef __NR_statx
        SECCOMP_ALLOW_SYSCALL(__NR_statx),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
    };
    struct sock_fprog prog = {
        .len = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) < 0)
    {
        perror("prctl(PR_SET_SECCOMP)");
        return -1;
    }
    return 0;
#else
    fprintf(stderr, "seccomp filter is only implemented for x86_64\n");
    return -1;
#endif
}

static char *trace_argv[64];

int trace_main(void *arg)
{
    (void)arg;
    sandbox_exec(trace_argv);
    _exit(1);
}

int sandbox_main(void *arg)
{
    (void)arg;

    if (userns_mode) {
        close(userns_pipe[1]);
        char c;
        if (read(userns_pipe[0], &c, 1) != 1) {
            fprintf(stderr, "userns sync failed\n");
            return 1;
        }
        close(userns_pipe[0]);
    }

    if (setup_sandbox_environment() < 0)
        return 1;
    if (install_seccomp_filter() < 0)
        return 1;

    if (target_name[0] != '\0') {
        char target_path[PATH_MAX];
        if (build_usr_bin_target_path(target_path, sizeof(target_path), target_name) < 0)
            return 1;
        char *args[66];
        int j = 0;
        args[j++] = target_path;
        for (int i = 0; i < target_argc && j < 65; i++)
            args[j++] = target_args[i];
        args[j] = NULL;
        execv(target_path, args);
        perror("execv");
    } else {
        char *const args[] = {"/bin/sh", NULL};
        execv("/bin/sh", args);
        perror("execv");
    }
    return 1;
}

int sandbox_exec(char *const argv[])
{
    if (setup_sandbox_environment() < 0)
        return 1;
     
    execv(argv[0], argv);
    perror("execv sandbox_exec");
    return 1;
}

int build_rootfs(const char *bin)
{
    char dst[PATH_MAX];
    char *base;
    char target_path[PATH_MAX];
    size_t rootfs_len;
    size_t target_path_len;

    for (int i = 0; dirs[i]; ++i)
    {
        char path[PATH_MAX];
        if (checked_path_join(path, sizeof(path), rootfs, dirs[i]) < 0)
            return -1;
        if (mkdir_p(path, 0755) < 0)
        {
            fprintf(stderr, "mkdir_p failed for rootfs path %s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    base = basename((char *)bin);
    if (strlen(base) > TARGET_NAME_MAX) {
        fprintf(stderr, "Target basename too long\n");
        return -1;
    }
    if (checked_path_copy(target_name, sizeof(target_name), base) < 0)
        return -1;

    if (build_usr_bin_target_path(target_path, sizeof(target_path), target_name) < 0)
        return -1;

    rootfs_len = strlen(rootfs);
    target_path_len = strlen(target_path);
    if (rootfs_len + target_path_len >= sizeof(dst)) {
        fprintf(stderr, "Target path too long\n");
        return -1;
    }
    memcpy(dst, rootfs, rootfs_len);
    memcpy(dst + rootfs_len, target_path, target_path_len + 1);
    if (copy_file(bin, dst) < 0)
    {
        fprintf(stderr, "Failed to copy target binary\n");
        return -1;
    }
    if (bin[0] == '/' && strcmp(dst + strlen(rootfs), bin) != 0) {
        char abs_dst[PATH_MAX];
        if (checked_path_join(abs_dst, sizeof(abs_dst), rootfs, bin) < 0)
            return -1;
        char *slash = strrchr(abs_dst, '/');
        if (slash) {
            *slash = '\0';
            if (mkdir_p(abs_dst, 0755) < 0) {
                fprintf(stderr, "mkdir_p failed for rootfs path %s: %s\n", abs_dst, strerror(errno));
                return -1;
            }
            *slash = '/';
        }
        if (copy_file(bin, abs_dst) < 0) {
            fprintf(stderr, "Failed to copy target to %s\n", abs_dst);
            return -1;
        }
    }
    if (checked_path_join(dst, sizeof(dst), rootfs, "/bin/sh") < 0)
        return -1;
    if (copy_file("/bin/sh", dst) < 0)
    {
        fprintf(stderr, "Failed to copy /bin/sh\n");
        return -1;
    }
    if (copy_ldd_deps(bin, rootfs) < 0)
        return -1;
    if (copy_ldd_deps("/bin/sh", rootfs) < 0)
        return -1;
    if (checked_path_join(dst, sizeof(dst), rootfs, "/usr/bin/strace") < 0)
        return -1;
    if (copy_file("/usr/bin/strace", dst) < 0)
    {
        if (trace_mode) {
            fprintf(stderr, "Failed to copy strace (required for --trace)\n");
            return -1;
        }
    } else {
        if (copy_ldd_deps("/usr/bin/strace", rootfs) < 0)
            return -1;
    }
    if (create_dev_nodes(rootfs) < 0 || create_etc_files(rootfs) < 0)
        return -1;
    return 0;
}

int setup_essential_environment(const char *root) {
    char dst[PATH_MAX];

    for (int i = 0; dirs[i]; ++i) {
        char path[PATH_MAX];
        if (checked_path_join(path, sizeof(path), root, dirs[i]) < 0)
            return -1;
        if (mkdir_p(path, 0755) < 0) {
            fprintf(stderr, "mkdir_p failed for rootfs path %s: %s\n", path, strerror(errno));
            return -1;
        }
    }

    for (int i = 0; essential_bins[i]; ++i) {
        if (checked_path_join(dst, sizeof(dst), root, essential_bins[i]) < 0)
            return -1;
        if (copy_file(essential_bins[i], dst) < 0) {
            fprintf(stderr, "Failed to copy essential bin: %s\n", essential_bins[i]);
            return -1;
        }
        if (copy_ldd_deps(essential_bins[i], root) < 0)
            return -1;
    }

    if (create_dev_nodes(root) < 0)
        return -1;
    if(create_etc_files(root) < 0)
        return -1;
    return 0;
}
int main(int argc, char **argv)
{
    int extras_idx = -1;
    int trace_idx = -1;
    int arg_start = 2;
    const char *target = NULL;
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rootfs> [<target-binary>] [--user] [--userns] [--prepare-only] [--extras <file>] [--trace <args...>]\n", argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "--prepare-only") == 0) {
        prepare_only = 1;
        if (argc < 3) {
            fprintf(stderr, "Usage: %s <rootfs> [<target-binary>] [--user] [--userns] [--prepare-only] [--extras <file>] [--trace <args...>]\n", argv[0]);
            return 1;
        }
        rootfs = argv[2];
        arg_start = 3;
    } else {
        rootfs = argv[1];
    }

    if (strlen(rootfs) >= PATH_MAX - 64) {
        fprintf(stderr, "Rootfs path too long\n");
        return 1;
    }

    for (int i = arg_start; i < argc; ++i) {
        if (strcmp(argv[i], "--user") == 0) {
            drop_to_nobody = 1;
        } else if (strcmp(argv[i], "--userns") == 0) {
            userns_mode = 1;
        } else if (strcmp(argv[i], "--prepare-only") == 0) {
            prepare_only = 1;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace_mode = 1;
            trace_idx = i;
            break;
        } else if (strcmp(argv[i], "--extras") == 0 && i + 1 < argc) {
            extras_idx = i + 1;
            ++i;
        } else if (!target) {
            target = argv[i];
        } else {
            if (target_argc >= 64) {
                fprintf(stderr, "Too many target arguments\n");
                return 1;
            }
            target_args[target_argc++] = argv[i];
        }
    }
    if (prepare_only && !target) {
        fprintf(stderr, "--prepare-only requires a target binary.\n");
        return 1;
    }
    if (prepare_only && trace_mode) {
        fprintf(stderr, "--prepare-only is not compatible with --trace.\n");
        return 1;
    }
    if (prepare_only && drop_to_nobody) {
        fprintf(stderr, "--prepare-only is not compatible with --user.\n");
        return 1;
    }
    if (prepare_only && userns_mode) {
        fprintf(stderr, "--prepare-only is not compatible with --userns.\n");
        return 1;
    }
    if (prepare_only && validate_target_binary(target) < 0) {
        return 1;
    }
    if (prepare_only && validate_rootfs_path(rootfs) < 0) {
        return 1;
    }
    if (geteuid() != 0 && !userns_mode) {
        fprintf(stderr, "This program must be run as root (or use --userns).\n");
        return 1;
    }
    if (drop_to_nobody && trace_mode) {
        fprintf(stderr, "--user is not compatible with --trace.\n");
        return 1;
    }
    if (userns_mode && trace_mode) {
        fprintf(stderr, "--userns is not compatible with --trace.\n");
        return 1;
    }
    if (userns_mode && drop_to_nobody) {
        fprintf(stderr, "--userns is not compatible with --user.\n");
        return 1;
    }
    if (trace_mode && !target) {
        fprintf(stderr, "--trace requires a target binary.\n");
        return 1;
    }

    if (!target) {
        if (!prepare_only && validate_rootfs_path(rootfs) < 0) {
            return 1;
        }
        if (setup_essential_environment(rootfs) < 0) {
            fprintf(stderr, "Failed to set up minimal environment\n");
            return 1;
        }
        if (extras_idx != -1 && copy_extras(argv[extras_idx]) < 0) {
            fprintf(stderr, "Failed to copy extras\n");
            return 1;
        }
        int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
        if (userns_mode) {
            flags |= CLONE_NEWUSER;
            if (pipe(userns_pipe) < 0) { perror("pipe"); return 1; }
        }
        pid_t pid = clone(sandbox_main, child_stack + STACK_SIZE, flags, NULL);
        if (pid < 0) {
            perror("clone");
            return 1;
        }
        if (userns_mode) {
            close(userns_pipe[0]);
            if (write_uid_gid_map(pid) < 0) {
                close(userns_pipe[1]);
                waitpid(pid, NULL, 0);
                return 1;
            }
            char c = 0;
            (void)!write(userns_pipe[1], &c, 1);
            close(userns_pipe[1]);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            fprintf(stderr, "[sandbox exited with %d]\n", WEXITSTATUS(status));
            return WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[sandbox killed by signal %d]\n", WTERMSIG(status));
            return 128 + WTERMSIG(status);
        }
        return 1;
    }

    if (validate_target_binary(target) < 0) {
        return 1;
    }
    if (!prepare_only && validate_rootfs_path(rootfs) < 0) {
        return 1;
    }
    if (build_rootfs(target) < 0) {
        fprintf(stderr, "Rootfs setup failed\n");
        return 1;
    }
    if (extras_idx != -1 && copy_extras(argv[extras_idx]) < 0) {
        fprintf(stderr, "Failed to copy extras\n");
        return 1;
    }
    if (prepare_only) {
        printf("TARGET /usr/bin/%s\n", target_name);
        return 0;
    }

    if (trace_mode) {
        char tmpfile[] = "/tmp/straceXXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd < 0) {
            perror("mkstemp");
            return 1;
        }
        close(fd);

        int num_trace_args = argc - (trace_idx + 1);
        if (num_trace_args + 8 > 64) {
            fprintf(stderr, "Too many arguments for --trace (max %d)\n", 64 - 8);
            return -1;
        }
        static char trace_target[PATH_MAX];
        if (target[0] == '/') {
            if (checked_path_copy(trace_target, sizeof(trace_target), target) < 0)
                return 1;
        } else if (build_usr_bin_target_path(trace_target, sizeof(trace_target), target_name) < 0) {
            return 1;
        }
        trace_argv[0] = "/usr/bin/strace";
        trace_argv[1] = "-f";
        trace_argv[2] = "-e";
        trace_argv[3] = "trace=file";
        trace_argv[4] = "-o";
        trace_argv[5] = tmpfile;
        int j = 6;
        trace_argv[j++] = trace_target;
        for (int k = trace_idx + 1; k < argc; ++k)
            trace_argv[j++] = argv[k];
        trace_argv[j] = NULL;
        int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
        pid_t pid = clone(trace_main, child_stack + STACK_SIZE, flags, NULL);
        if (pid < 0) {
            perror("clone");
            return 1;
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            fprintf(stderr, "[sandbox --trace exited with %d]\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "[sandbox --trace killed by signal %d]\n", WTERMSIG(status));
        char strace_path[PATH_MAX];
        if (checked_path_join3(strace_path, sizeof(strace_path), rootfs, "/", tmpfile) < 0)
            return 1;

        FILE *fp = fopen(strace_path, "r");
        if (!fp) {
            perror("fopen trace");
            return 1;
        }
        char line[PATH_MAX];
        while (fgets(line, sizeof(line), fp)) {
            char *quote1 = strchr(line, '"');
            if (!quote1)
                continue;
            char *quote2 = strchr(quote1 + 1, '"');
            if (!quote2)
                continue;
            *quote2 = '\0';
            const char *path = quote1 + 1;
            if (path[0] != '/' || strstr(path, "/.."))
                continue;
            char dst[PATH_MAX];
            if (checked_path_join(dst, sizeof(dst), rootfs, path) < 0)
                continue;
            copy_file(path, dst);
        }
        fclose(fp);
        unlink(strace_path);
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        return 1;
    }

    
    int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
    if (userns_mode) {
        flags |= CLONE_NEWUSER;
        if (pipe(userns_pipe) < 0) { perror("pipe"); return 1; }
    }
    pid_t pid = clone(sandbox_main, child_stack + STACK_SIZE, flags, NULL);
    if (pid < 0) {
        perror("clone");
        return 1;
    }
    if (userns_mode) {
        close(userns_pipe[0]);
        if (write_uid_gid_map(pid) < 0) {
            close(userns_pipe[1]);
            waitpid(pid, NULL, 0);
            return 1;
        }
        char c = 0;
        (void)!write(userns_pipe[1], &c, 1);
        close(userns_pipe[1]);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        fprintf(stderr, "[sandbox exited with %d]\n", WEXITSTATUS(status));
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[sandbox killed by signal %d]\n", WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return 1;
}
