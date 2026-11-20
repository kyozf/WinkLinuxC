#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <errno.h>

static void spawn_shell(void) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        sleep(1);
        return;
    }

    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", (char *)NULL);
        perror("exec /bin/sh");
        _exit(1);
    }

    while (waitpid(pid, NULL, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }
}

int main(void) {
    sethostname("winkc", 5);

    mount("proc", "/proc", "proc", 0, "");
    mount("sysfs", "/sys", "sysfs", 0, "");
    mount("devtmpfs", "/dev", "devtmpfs", 0, "");

    printf("\n    WinkLinuxC init v1.0\n\n");
    fflush(stdout);

    for (;;) {
        spawn_shell();
    }

    return 0;
}