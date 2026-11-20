
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <limits.h>

#define LOGO_PATH "/usr/share/winklinuxc/logo.txt"

#define RESET   "\033[0m"
#define MAGENTA "\033[1;35m"
#define CYAN    "\033[1;36m"
#define WHITE   "\033[1;37m"
#define GREY    "\033[37m"

typedef struct {
    const char *label;
    char value[512];
} Info;

/* Read the first line of a file. */
static int read_value(const char *path, char *buffer, size_t size) {
    FILE *f = fopen(path, "r");

    if (!f)
        return 0;

    if (!fgets(buffer, size, f)) {
        fclose(f);
        return 0;
    }

    fclose(f);

    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 1;
}

/* Count CPUs using Linux's online CPU list. */
static int get_cpu_count(void) {
    char line[256];

    if (read_value("/sys/devices/system/cpu/online",
                   line, sizeof(line))) {
        int count = 0;
        char *p = line;

        while (*p) {
            char *end;
            long first = strtol(p, &end, 10);

            if (end == p)
                break;

            long last = first;
            p = end;

            if (*p == '-') {
                p++;
                last = strtol(p, &end, 10);
                p = end;
            }

            if (last >= first)
                count += (int)(last - first + 1);

            if (*p == ',')
                p++;
            else
                break;
        }

        if (count > 0)
            return count;
    }

    DIR *dir = opendir("/sys/devices/system/cpu");
    struct dirent *entry;
    int count = 0;

    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "cpu", 3) == 0 &&
                entry->d_name[3] >= '0' &&
                entry->d_name[3] <= '9') {
                count++;
            }
        }

        closedir(dir);
    }

    return count > 0 ? count : 1;
}

/* Identify common ARM CPU parts. */
static const char *arm_cpu_name(const char *implementer,
                                const char *part) {
    if (!implementer || !part)
        return NULL;

    if (strcmp(implementer, "0x41") == 0) {
        if (strcmp(part, "0xd03") == 0) return "ARM Cortex-A53";
        if (strcmp(part, "0xd04") == 0) return "ARM Cortex-A35";
        if (strcmp(part, "0xd05") == 0) return "ARM Cortex-A55";
        if (strcmp(part, "0xd07") == 0) return "ARM Cortex-A57";
        if (strcmp(part, "0xd08") == 0) return "ARM Cortex-A72";
        if (strcmp(part, "0xd09") == 0) return "ARM Cortex-A73";
        if (strcmp(part, "0xd0a") == 0) return "ARM Cortex-A75";
        if (strcmp(part, "0xd0b") == 0) return "ARM Cortex-A76";
        if (strcmp(part, "0xd0d") == 0) return "ARM Cortex-A77";
        if (strcmp(part, "0xd41") == 0) return "ARM Cortex-A78";
        if (strcmp(part, "0xd44") == 0) return "ARM Cortex-X1";
        if (strcmp(part, "0xd46") == 0) return "ARM Cortex-A510";
        if (strcmp(part, "0xd47") == 0) return "ARM Cortex-A710";
        if (strcmp(part, "0xd48") == 0) return "ARM Cortex-X2";
        if (strcmp(part, "0xd4e") == 0) return "ARM Cortex-X3";
    }

    if (strcmp(implementer, "0x61") == 0)
        return "Apple-designed ARM CPU";

    if (strcmp(implementer, "0x42") == 0)
        return "Broadcom ARM CPU";

    if (strcmp(implementer, "0x43") == 0)
        return "Cavium ARM CPU";

    if (strcmp(implementer, "0x51") == 0)
        return "Qualcomm ARM CPU";

    if (strcmp(implementer, "0x53") == 0)
        return "Samsung ARM CPU";

    return NULL;
}

/* Detect CPU model from /proc/cpuinfo. */
static void get_cpu(char *buffer, size_t size) {
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    char implementer[32] = "";
    char part[32] = "";
    int found = 0;

    snprintf(buffer, size, "Not exposed");

    if (!f)
        return;

    while (fgets(line, sizeof(line), f)) {
        char *colon = strchr(line, ':');

        if (!colon)
            continue;

        *colon = '\0';

        char *key = line;
        char *value = colon + 1;

        while (*value == ' ' || *value == '\t')
            value++;

        value[strcspn(value, "\r\n")] = '\0';

        if (!found &&
            (strcasecmp(key, "model name") == 0 ||
             strcasecmp(key, "Processor") == 0 ||
             strcasecmp(key, "Hardware") == 0 ||
             strcasecmp(key, "cpu model") == 0)) {
            snprintf(buffer, size, "%s", value);
            found = 1;
        }

        if (strcasecmp(key, "CPU implementer") == 0)
            snprintf(implementer, sizeof(implementer), "%s", value);

        if (strcasecmp(key, "CPU part") == 0)
            snprintf(part, sizeof(part), "%s", value);
    }

    fclose(f);

    if (!found) {
        const char *name = arm_cpu_name(implementer, part);

        if (name)
            snprintf(buffer, size, "%s", name);
    }
}

/* Memory usage from /proc/meminfo. */
static void get_memory(char *buffer, size_t size) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    long total = 0, available = 0;

    if (!f) {
        snprintf(buffer, size, "Not available");
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %ld kB", &total) == 1)
            continue;

        if (sscanf(line, "MemAvailable: %ld kB", &available) == 1)
            continue;
    }

    fclose(f);

    if (total <= 0) {
        snprintf(buffer, size, "Unknown");
        return;
    }

    long used = total - available;

    snprintf(buffer, size, "%ld / %ld MiB used",
             used / 1024, total / 1024);
}

/* System uptime. */
static void get_uptime(char *buffer, size_t size) {
    FILE *f = fopen("/proc/uptime", "r");
    double seconds = 0;

    if (!f || fscanf(f, "%lf", &seconds) != 1) {
        snprintf(buffer, size, "Not available");
        if (f) fclose(f);
        return;
    }

    fclose(f);

    int days = (int)seconds / 86400;
    int hours = ((int)seconds % 86400) / 3600;
    int minutes = ((int)seconds % 3600) / 60;

    snprintf(buffer, size, "%dd %dh %dm",
             days, hours, minutes);
}

/* GPU vendor identification. */
static const char *gpu_vendor_name(const char *vendor) {
    if (!vendor) return "Unknown";

    if (strcasecmp(vendor, "0x10de") == 0) return "NVIDIA";
    if (strcasecmp(vendor, "0x1002") == 0) return "AMD";
    if (strcasecmp(vendor, "0x8086") == 0) return "Intel";
    if (strcasecmp(vendor, "0x106b") == 0) return "Apple";
    if (strcasecmp(vendor, "0x13b5") == 0) return "ARM";
    if (strcasecmp(vendor, "0x5143") == 0) return "Qualcomm";

    return "Other vendor";
}

/* Detect graphics devices exposed through DRM. */
static void get_gpu(char *buffer, size_t size) {
    DIR *dir = opendir("/sys/class/drm");
    struct dirent *entry;
    int found = 0;

    snprintf(buffer, size, "No GPU detected");

    if (!dir)
        return;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "card", 4) != 0 ||
            entry->d_name[4] < '0' ||
            entry->d_name[4] > '9' ||
            entry->d_name[5] != '\0')
            continue;

        char path[PATH_MAX];
        char vendor[64] = "";
        char device[64] = "";
        char driver[128] = "unknown";

        snprintf(path, sizeof(path),
                 "/sys/class/drm/%s/device/vendor",
                 entry->d_name);
        read_value(path, vendor, sizeof(vendor));

        snprintf(path, sizeof(path),
                 "/sys/class/drm/%s/device/device",
                 entry->d_name);
        read_value(path, device, sizeof(device));

        snprintf(path, sizeof(path),
                 "/sys/class/drm/%s/device/uevent",
                 entry->d_name);

        char uevent[512];

        if (read_value(path, uevent, sizeof(uevent))) {
            char *p = strstr(uevent, "DRIVER=");
            if (p)
                snprintf(driver, sizeof(driver), "%s", p + 7);
        }

        if (found == 0) {
            if (vendor[0]) {
                snprintf(buffer, size, "%s (vendor %s, device %s)",
                         gpu_vendor_name(vendor), vendor,
                         device[0] ? device : "unknown");
            } else {
                snprintf(buffer, size, "DRM device: %s",
                         entry->d_name);
            }
        } else {
            size_t used = strlen(buffer);

            if (used < size)
                snprintf(buffer + used, size - used,
                         "; %s", entry->d_name);
        }

        found++;
    }

    closedir(dir);
}

/* Detect motherboard using DMI. */
static void get_motherboard(char *buffer, size_t size) {
    char vendor[128] = "";
    char model[256] = "";

    read_value("/sys/class/dmi/id/board_vendor",
               vendor, sizeof(vendor));

    read_value("/sys/class/dmi/id/board_name",
               model, sizeof(model));

    if (!model[0])
        read_value("/sys/class/dmi/id/product_name",
                   model, sizeof(model));

    if (vendor[0] && model[0])
        snprintf(buffer, size, "%s %s", vendor, model);
    else if (model[0])
        snprintf(buffer, size, "%s", model);
    else
        snprintf(buffer, size, "Not exposed");
}

/* Detect disks and their capacities. */
static void get_storage(char *buffer, size_t size) {
    DIR *dir = opendir("/sys/block");
    struct dirent *entry;
    int found = 0;

    snprintf(buffer, size, "No disks detected");

    if (!dir)
        return;

    buffer[0] = '\0';

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        if (strncmp(entry->d_name, "loop", 4) == 0 ||
            strncmp(entry->d_name, "ram", 3) == 0 ||
            strncmp(entry->d_name, "zram", 4) == 0)
            continue;

        char path[PATH_MAX];
        char sectors_text[64] = "0";

        snprintf(path, sizeof(path),
                 "/sys/block/%s/size", entry->d_name);

        read_value(path, sectors_text, sizeof(sectors_text));

        unsigned long long sectors =
            strtoull(sectors_text, NULL, 10);

        unsigned long long gb =
            (sectors * 512ULL) / 1000000000ULL;

        size_t used = strlen(buffer);

        if (found > 0 && used < size)
            snprintf(buffer + used, size - used, ", ");

        used = strlen(buffer);

        if (used < size)
            snprintf(buffer + used, size - used,
                     "%s (%llu GB)", entry->d_name, gb);

        found++;

        if (found >= 3)
            break;
    }

    closedir(dir);

    if (!found)
        snprintf(buffer, size, "No disks detected");
}

/* Detect the active terminal. */
static void get_terminal(char *buffer, size_t size) {
    const char *names[] = {
        ttyname(STDOUT_FILENO),
        ttyname(STDIN_FILENO),
        ttyname(STDERR_FILENO)
    };

    for (int i = 0; i < 3; i++) {
        if (names[i]) {
            snprintf(buffer, size, "%s", names[i]);
            return;
        }
    }

    snprintf(buffer, size, "Not attached to a TTY");
}

/* Print logo lines, with the lower section cyan. */
static void print_logo_line(const char *line, int width, int cyan) {
    printf("%s%-*s" RESET,
           cyan ? CYAN : GREY,
           width, line);
}

/* Print 16 terminal background colors. */
static void print_colors(void) {
    int colors[] = {
        40, 41, 42, 43, 44, 45, 46, 47,
        100, 101, 102, 103, 104, 105, 106, 107
    };

    for (int i = 0; i < 16; i++)
        printf("\033[%dm  \033[0m", colors[i]);

    printf(RESET);
}

int main(void) {
    FILE *logo = fopen(LOGO_PATH, "r");

    if (!logo) {
        perror("winkfetch: logo");
        return 1;
    }

    struct utsname system;

    if (uname(&system) != 0) {
        perror("winkfetch: uname");
        fclose(logo);
        return 1;
    }

    char hostname[128] = "Unknown";
    char uptime[64];
    char memory[64];
    char cpu[256];
    char gpu[512];
    char motherboard[384];
    char storage[384];
    char terminal[128];
    char core_text[32];

    gethostname(hostname, sizeof(hostname) - 1);
    hostname[sizeof(hostname) - 1] = '\0';

    get_uptime(uptime, sizeof(uptime));
    get_memory(memory, sizeof(memory));
    get_cpu(cpu, sizeof(cpu));
    get_gpu(gpu, sizeof(gpu));
    get_motherboard(motherboard, sizeof(motherboard));
    get_storage(storage, sizeof(storage));
    get_terminal(terminal, sizeof(terminal));

    snprintf(core_text, sizeof(core_text), "%d",
             get_cpu_count());

    const char *term = getenv("TERM");

    Info info[] = {
        {"OS:",         "WinkLinuxC"},
        {"Host:",       ""},
        {"Kernel:",     ""},
        {"Arch:",       ""},
        {"CPU:",        ""},
        {"Cores:",      ""},
        {"GPU:",        ""},
        {"Motherboard:", ""},
        {"Memory:",     ""},
        {"Storage:",    ""},
        {"Uptime:",     ""},
        {"Terminal:",   ""},
        {"TERM:",       ""},
        {"Shell:",      "WinkShell 0.2"}
    };

    snprintf(info[1].value, sizeof(info[1].value), "%s", hostname);
    snprintf(info[2].value, sizeof(info[2].value), "%s", system.release);
    snprintf(info[3].value, sizeof(info[3].value), "%s", system.machine);
    snprintf(info[4].value, sizeof(info[4].value), "%s", cpu);
    snprintf(info[5].value, sizeof(info[5].value), "%s", core_text);
    snprintf(info[6].value, sizeof(info[6].value), "%s", gpu);
    snprintf(info[7].value, sizeof(info[7].value), "%s", motherboard);
    snprintf(info[8].value, sizeof(info[8].value), "%s", memory);
    snprintf(info[9].value, sizeof(info[9].value), "%s", storage);
    snprintf(info[10].value, sizeof(info[10].value), "%s", uptime);
    snprintf(info[11].value, sizeof(info[11].value), "%s", terminal);
    snprintf(info[12].value, sizeof(info[12].value), "%s",
             term ? term : "Not set");

    int info_count = sizeof(info) / sizeof(info[0]);

    /* Find the longest logo line. */
    char line[2048];
    int logo_width = 0;

    while (fgets(line, sizeof(line), logo)) {
        line[strcspn(line, "\r\n")] = '\0';

        int width = (int)strlen(line);

        if (width > logo_width)
            logo_width = width;
    }

    rewind(logo);

    /* Print logo and information side by side. */
    int index = 0;

    while (fgets(line, sizeof(line), logo)) {
        line[strcspn(line, "\r\n")] = '\0';

        /* Lower logo artwork starts at line 15. */
        print_logo_line(line, logo_width, index >= 14);
        printf("  ");

        if (index == 0) {
            printf(MAGENTA "☆〜（ゝ。∂） WinkLinuxC" RESET);
        } else if (index == 1) {
            printf(MAGENTA "-----------------------" RESET);
        } else if (index >= 2 && index < info_count + 2) {
            int item = index - 2;

            printf(CYAN "%-12s" RESET WHITE "%s" RESET,
                   info[item].label,
                   info[item].value);
        } else if (index == info_count + 2) {
            printf(CYAN "%-12s" RESET, "Colors:");
            print_colors();
        }

        putchar('\n');
        index++;
    }

    /* Continue information if it is taller than the logo. */
    while (index < info_count + 3) {
        printf("%*s  ", logo_width, "");

        if (index == info_count + 2) {
            printf(CYAN "%-12s" RESET, "Colors:");
            print_colors();
        } else {
            int item = index - 2;

            printf(CYAN "%-12s" RESET WHITE "%s" RESET,
                   info[item].label,
                   info[item].value);
        }

        putchar('\n');
        index++;
    }

    fclose(logo);

    /* Never leave terminal colors altered. */
    printf(RESET);

    return 0;
}
