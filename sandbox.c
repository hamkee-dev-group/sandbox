#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <libgen.h>
#include <sched.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/prctl.h>
#include <sys/capability.h>



#define PATH_MAX 1024
#define STACK_SIZE (1024 * 1024)
static char child_stack[STACK_SIZE];
char *rootfs;
char target_name[PATH_MAX];
int target_argc = 0;
char *target_args[64];
int drop_to_nobody = 0;
int trace_mode = 0;

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
int is_binary(const char *path) {
    struct stat st;
    int fd;
    ssize_t n;
    unsigned char magic[4];

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

    n = read(fd, magic, sizeof(magic));
    close(fd);
    if (n != 4 || magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        return 0;
    }
    return 1;
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);

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
        return -1;
    tmp = strdup(dst);
    if (!tmp)
    {
        close(in);
        return -1;
    }
    mkdir_p(dirname(tmp), 0755);
    free(tmp);

    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out < 0)
    {
        close(in);
        return -1;
    }

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(out, buf + written, n - written);
            if (w <= 0) { 
                close(in); 
                close(out); 
                return -1; 
            }
            written += w;
        }
    }
    close(in);
    close(out);
    return 0;
}

void copy_ldd_deps(const char *bin, const char *root)
{
    char line[PATH_MAX];
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) == -1)
    {
        perror("pipe");
        return;
    }

    pid = fork();
    if (pid == -1)
    {
        perror("fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0)
    {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("ldd", "ldd", bin, (char *)NULL);
        perror("execlp ldd");
        _exit(127);
    }

    close(pipefd[1]);
    FILE *fp = fdopen(pipefd[0], "r");
    if (!fp)
    {
        perror("fdopen");
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return;
    }

    while (fgets(line, sizeof(line), fp))
    {
        char *start = strchr(line, '/');
        if (!start)
            continue;
        char *end = strpbrk(start, " \n");
        if (end)
            *end = '\0';

        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s%s", root, start);
        copy_file(start, dst);
    }

    fclose(fp);
    waitpid(pid, NULL, 0);
}

int copy_extras(const char *listfile)
{
    char line[PATH_MAX];
    FILE *f;
    
    f = fopen(listfile, "r");
    if (!f)
    {
        perror("extras file");
        return -1;
    }
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) == 0 || line[0] != '/')
            continue;
        char dst[PATH_MAX];
        snprintf(dst, sizeof(dst), "%s%s", rootfs, line);
        if (copy_file(line, dst) < 0)
        {
            fprintf(stderr, "[WARN] Failed to copy extra: %s\n", line);
        }
    }
    fclose(f);
    return 0;
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

    snprintf(path, sizeof(path), "%s/dev", root);
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
    {
        perror("mkdir /dev");
        return -1;
    }

    for (int i = 0; devs[i].name; ++i)
    {
        snprintf(path, sizeof(path), "%s/dev/%s", root, devs[i].name);
        if (mknod(path, S_IFCHR | 0666, makedev(devs[i].major, devs[i].minor)) < 0 && errno != EEXIST)
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
    snprintf(path, sizeof(path), "%s/etc", root);
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
    {
        perror("mkdir /etc");
        return -1;
    }
    if(drop_to_nobody) {
        memset(path, 0, PATH_MAX);
        snprintf(path, sizeof(path), "%s/etc/passwd", root);
        fp = fopen(path, "w+");
        if(!fp) {
            perror("fopen passwd");
            return -1;
        }
        fprintf(fp, "nobody:x:65534:65534:nobody:/home:/bin/sh\n");
        fclose(fp);
        
        memset(path, 0, PATH_MAX);
        snprintf(path, sizeof(path), "%s/etc/group", root);
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
    snprintf(proc_path, sizeof(proc_path), "%s/proc", rootfs);
    mkdir(proc_path, 0755);
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

    if (setgid(gid) < 0) 
    {
        perror("setgid");
        return -1;
    }
    if(setuid(uid) < 0) {
        perror("setuid");
        return -1;
    }

    unsetenv("LC_ALL");
    unsetenv("LANG");
    if (drop_all_caps() < 0)
        return -1;
    return 0;
}

int sandbox_main(void *arg)
{
    (void)arg;

    if (setup_sandbox_environment() < 0)
        return 1;

    if (target_name[0] != '\0') {
        char target_path[PATH_MAX];
        snprintf(target_path, sizeof(target_path), "/usr/bin/%s", target_name);
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

    for (int i = 0; dirs[i]; ++i)
    {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", rootfs, dirs[i]);
        if (mkdir_p(path, 0755) < 0)
        {
            fprintf(stderr, "mkdir_p failed: %s\n", path);
            return -1;
        }
    }
    snprintf(target_name, sizeof(target_name), "%s", basename((char *)bin));

    snprintf(dst, sizeof(dst), "%s/usr/bin/%s", rootfs, target_name);
    if (copy_file(bin, dst) < 0)
    {
        fprintf(stderr, "Failed to copy target binary\n");
        return -1;
    }
    snprintf(dst, sizeof(dst), "%s/bin/sh", rootfs);
    if (copy_file("/bin/sh", dst) < 0)
    {
        fprintf(stderr, "Failed to copy /bin/sh\n");
        return -1;
    }
    copy_ldd_deps(bin, rootfs);
    copy_ldd_deps("/bin/sh", rootfs);
    snprintf(dst, sizeof(dst), "%s/usr/bin/strace", rootfs);
    if (copy_file("/usr/bin/strace", dst) < 0)
    {
        if (trace_mode) {
            fprintf(stderr, "Failed to copy strace (required for --trace)\n");
            return -1;
        }
    } else {
        copy_ldd_deps("/usr/bin/strace", rootfs);
    }
    if (create_dev_nodes(rootfs) < 0 || create_etc_files(rootfs) < 0)
        return -1;
    return 0;
}

int setup_essential_environment(const char *root) {
    char dst[PATH_MAX];

    for (int i = 0; dirs[i]; ++i) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s%s", root, dirs[i]);
        if (mkdir_p(path, 0755) < 0) {
            fprintf(stderr, "mkdir_p failed: %s\n", path);
            return -1;
        }
    }

    for (int i = 0; essential_bins[i]; ++i) {
        snprintf(dst, sizeof(dst), "%s%s", root, essential_bins[i]);
        if (copy_file(essential_bins[i], dst) < 0) {
            fprintf(stderr, "Failed to copy essential bin: %s\n", essential_bins[i]);
            continue;
        }
        copy_ldd_deps(essential_bins[i], root);
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
    const char *target = NULL;
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rootfs> [<target-binary>] [--user] [--extras <file>] [--trace <args...>]\n", argv[0]);
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "This program must be run as root.\n");
        return 1;
    }

    rootfs = argv[1];

    if (strlen(rootfs) >= PATH_MAX - 64) {
        fprintf(stderr, "Rootfs path too long\n");
        return 1;
    }

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--user") == 0) {
            drop_to_nobody = 1;
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
    if (drop_to_nobody && trace_mode) {
        fprintf(stderr, "--user is not compatible with --trace.\n");
        return 1;
    }
    if (trace_mode && !target) {
        fprintf(stderr, "--trace requires a target binary.\n");
        return 1;
    }

    if (!target) {
        if (setup_essential_environment(rootfs) < 0) {
            fprintf(stderr, "Failed to set up minimal environment\n");
            return 1;
        }
        if (extras_idx != -1 && copy_extras(argv[extras_idx]) < 0) {
            fprintf(stderr, "Failed to copy extras\n");
            return 1;
        }
        int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
        pid_t pid = clone(sandbox_main, child_stack + STACK_SIZE, flags, NULL);
        if (pid < 0) {
            perror("clone");
            return 1;
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            fprintf(stderr, "[sandbox exited with %d]\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "[sandbox killed by signal %d]\n", WTERMSIG(status));
        return 0;
    }

    if (!is_binary(target)) {
        fprintf(stderr, "%s is not a binary file\n", target);
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

    if (trace_mode) {
        char tmpfile[] = "/tmp/straceXXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd < 0) {
            perror("mkstemp");
            return 1;
        }
        close(fd);

        pid_t pid = fork();
        if (pid == 0) {
            char *cmd_argv[64] = {"/usr/bin/strace", "-f", "-e", "trace=file", "-o", tmpfile};
            int num_trace_args = argc - (trace_idx + 1);
            if (num_trace_args + 7 > 64) {
                fprintf(stderr, "Too many arguments for --trace (max %d)\n", 64 - 7);
                return -1;
            }
            int j = 6;
            cmd_argv[j++] = malloc(strlen(target) + 1);
            sprintf(cmd_argv[j - 1], "%s", target);
            for (int k = trace_idx + 1; k < argc; ++k)
                cmd_argv[j++] = argv[k];
            cmd_argv[j] = NULL;
            sandbox_exec(cmd_argv);
            free(cmd_argv[6]);
            _exit(1);
        }
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            fprintf(stderr, "[sandbox --trace exited with %d]\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "[sandbox --trace killed by signal %d]\n", WTERMSIG(status));
        char strace_path[PATH_MAX];
        snprintf(strace_path, sizeof(strace_path), "%s/%s", rootfs, tmpfile);

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
            snprintf(dst, sizeof(dst), "%s%s", rootfs, path);
            copy_file(path, dst);
        }
        fclose(fp);
        unlink(strace_path);
        return 0;
    }

    // Normal sandboxed run (with or without --user)
    int flags = CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(sandbox_main, child_stack + STACK_SIZE, flags, NULL);
    if (pid < 0) {
        perror("clone");
        return 1;
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        fprintf(stderr, "[sandbox exited with %d]\n", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        fprintf(stderr, "[sandbox killed by signal %d]\n", WTERMSIG(status));
    return 0;
}

