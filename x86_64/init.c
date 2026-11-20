
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <errno.h>

static void mount_system(void)
{
    mkdir("/proc", 0755);
    mkdir("/sys", 0755);
    mkdir("/dev", 0755);
    mkdir("/tmp", 01777);
    mkdir("/run", 0755);

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
    mount("tmpfs", "/run", "tmpfs", 0, NULL);
}

static void setup_hostname(void)
{
    if (sethostname("winkc", 5) < 0)
        perror("sethostname");
}

static void setup_console(void)
{
    int fd = open("/dev/console", O_RDWR);

    if (fd < 0) {
        perror("open /dev/console");
        return;
    }

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);

    if (fd > STDERR_FILENO)
        close(fd);
}

static void start_shell(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        sleep(1);
        return;
    }

    if (pid == 0) {
        setsid();
        setup_console();

        execl("/bin/winksh", "winksh", (char *)NULL);

        perror("exec /bin/winksh");
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
}

int main(void)
{
    mount_system();
    setup_hostname();
    setup_console();

    puts("WinkLinuxC is starting...");

    while (1)
        start_shell();

    return 0;
}
