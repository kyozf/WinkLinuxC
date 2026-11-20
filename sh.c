#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_TOKENS 256
#define MAX_HISTORY 64
#define MAX_PIPE 16

#define SCROLLBACK_LINES 512
#define SCROLLBACK_LINE_SIZE 2048

typedef struct {
    char *argv[MAX_ARGS];
    int argc;
    char *input;
    char *output;
    int append;
} Command;

static char history[MAX_HISTORY][MAX_LINE];
static int history_count = 0;
static int shell_exit = 0;
static int shell_status = 0;
static volatile sig_atomic_t interrupted = 0;

static char scrollback_lines[SCROLLBACK_LINES][SCROLLBACK_LINE_SIZE];
static size_t scrollback_lengths[SCROLLBACK_LINES];

static int scrollback_start = 0;
static int scrollback_count = 0;

static char scrollback_current[SCROLLBACK_LINE_SIZE];
static size_t scrollback_current_length = 0;

static int scrollback_active = 0;
static size_t scrollback_offset = 0;
static int scrollback_pending_cr = 0;

static const char *current_layout = "us";

/* Forward declarations */

static void redraw(const char *line, size_t len, size_t cursor);
static void scrollback_draw(int show_status);

/* Scrollback buffer */

static void scrollback_store_line(void)
{
    int index;

    if (scrollback_count < SCROLLBACK_LINES) {
        index = (scrollback_start + scrollback_count) % SCROLLBACK_LINES;
        scrollback_count++;
    } else {
        index = scrollback_start;
        scrollback_start = (scrollback_start + 1) % SCROLLBACK_LINES;
    }

    memcpy(scrollback_lines[index], scrollback_current,
           scrollback_current_length);

    scrollback_lines[index][scrollback_current_length] = '\0';
    scrollback_lengths[index] = scrollback_current_length;

    scrollback_current_length = 0;
    scrollback_current[0] = '\0';
}

static void scrollback_reset(void)
{
    scrollback_start = 0;
    scrollback_count = 0;
    scrollback_current_length = 0;
    scrollback_current[0] = '\0';
    scrollback_offset = 0;
    scrollback_active = 0;
    scrollback_pending_cr = 0;
}

static void scrollback_record(const char *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];

        if (scrollback_pending_cr) {
            if (c == '\n') {
                scrollback_store_line();
                scrollback_pending_cr = 0;
                continue;
            }

            scrollback_current_length = 0;
            scrollback_current[0] = '\0';
            scrollback_pending_cr = 0;
        }

        if (c == '\r') {
            scrollback_pending_cr = 1;
        } else if (c == '\n') {
            scrollback_store_line();
        } else if (c == '\b') {
            if (scrollback_current_length > 0)
                scrollback_current_length--;
        } else if (c != '\0' &&
                   scrollback_current_length < SCROLLBACK_LINE_SIZE - 1) {
            scrollback_current[scrollback_current_length++] = (char)c;
            scrollback_current[scrollback_current_length] = '\0';
        }
    }
}

static void scrollback_output(const char *data, size_t length)
{
    scrollback_record(data, length);

    size_t written = 0;

    while (written < length) {
        ssize_t n = write(STDOUT_FILENO, data + written, length - written);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            break;

        written += (size_t)n;
    }
}

static size_t scrollback_total_lines(void)
{
    return (size_t)scrollback_count +
           (scrollback_current_length > 0 ? 1 : 0);
}

static size_t scrollback_page_size(void)
{
    struct winsize ws;
    size_t rows = 24;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2)
        rows = ws.ws_row;

    return rows - 2;
}

static size_t scrollback_max_offset(void)
{
    struct winsize ws;
    size_t rows = 24;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1)
        rows = ws.ws_row;

    size_t visible = rows - 1;
    size_t total = scrollback_total_lines();

    return total > visible ? total - visible : 0;
}

static void scrollback_draw(int show_status)
{
    struct winsize ws;
    int rows = 24;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1)
        rows = ws.ws_row;

    size_t visible = show_status ? (size_t)rows - 1 : (size_t)rows;
    size_t total = scrollback_total_lines();
    size_t max_offset = scrollback_max_offset();

    if (scrollback_offset > max_offset)
        scrollback_offset = max_offset;

    size_t end = total - scrollback_offset;
    size_t first = end > visible ? end - visible : 0;

    printf("\033[2J\033[H");

    for (size_t i = first; i < end; i++) {
        if (i < (size_t)scrollback_count) {
            int index = (scrollback_start + (int)i) % SCROLLBACK_LINES;

            fwrite(scrollback_lines[index], 1,
                   scrollback_lengths[index], stdout);
        } else {
            fwrite(scrollback_current, 1,
                   scrollback_current_length, stdout);
        }

        putchar('\n');
    }

    if (show_status) {
        printf("\033[%d;1H\033[2K", rows);
        printf("\033[7m WinkShell scrollback | Fn+Up/Down: scroll | Q: return \033[0m");
    }

    fflush(stdout);
}

static void scrollback_leave(const char *line, size_t len, size_t cursor)
{
    scrollback_offset = 0;
    scrollback_active = 0;

    scrollback_draw(0);
    redraw(line, len, cursor);
}

static void scrollback_page_up(void)
{
    size_t maximum = scrollback_max_offset();
    size_t page = scrollback_page_size();

    if (maximum == 0)
        return;

    if (scrollback_offset + page > maximum)
        scrollback_offset = maximum;
    else
        scrollback_offset += page;

    scrollback_active = 1;
    scrollback_draw(1);
}

static void scrollback_page_down(const char *line, size_t len, size_t cursor)
{
    size_t page = scrollback_page_size();

    if (scrollback_offset > page)
        scrollback_offset -= page;
    else
        scrollback_offset = 0;

    if (scrollback_offset == 0) {
        scrollback_leave(line, len, cursor);
        return;
    }

    scrollback_draw(1);
}

static void scrollback_to_oldest(void)
{
    scrollback_offset = scrollback_max_offset();

    if (scrollback_offset == 0)
        return;

    scrollback_active = 1;
    scrollback_draw(1);
}

/* UTF-8 helpers */

static size_t utf8_next(const char *s, size_t pos, size_t len)
{
    unsigned char c = (unsigned char)s[pos];
    size_t n = 1;

    if ((c & 0xE0) == 0xC0)
        n = 2;
    else if ((c & 0xF0) == 0xE0)
        n = 3;
    else if ((c & 0xF8) == 0xF0)
        n = 4;

    if (pos + n > len)
        return pos + 1;

    for (size_t i = 1; i < n; i++) {
        if (((unsigned char)s[pos + i] & 0xC0) != 0x80)
            return pos + 1;
    }

    return pos + n;
}

static size_t utf8_prev(const char *s, size_t pos)
{
    if (pos == 0)
        return 0;

    pos--;

    while (pos > 0 &&
           (((unsigned char)s[pos] & 0xC0) == 0x80))
        pos--;

    return pos;
}

static int utf8_char_width(const char *s, size_t pos)
{
    unsigned char c = (unsigned char)s[pos];

    if (c < 0x80)
        return 1;

    if ((c & 0xE0) == 0xC0)
        return 1;

    if ((c & 0xF0) == 0xE0) {
        unsigned char b = (unsigned char)s[pos + 1];

        if (c == 0xE3 ||
            (c == 0xE2 && b >= 0x80 && b <= 0xA0))
            return 2;
    }

    return 1;
}

static size_t utf8_columns(const char *s, size_t len)
{
    size_t pos = 0;
    size_t columns = 0;

    while (pos < len) {
        size_t next = utf8_next(s, pos, len);
        columns += utf8_char_width(s, pos);
        pos = next;
    }

    return columns;
}

static size_t utf8_cursor_columns(const char *s, size_t cursor)
{
    return utf8_columns(s, cursor);
}

/* Signal handling */

static void handle_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
    write(STDOUT_FILENO, "\n", 1);
}

/* Prompt and line editing */

static void prompt(void)
{
    char cwd[512];

    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");

    printf("\033[95m☆〜（ゝ。∂）\033[0m "
           "\033[92mroot@winkc\033[0m:"
           "\033[94m%s\033[0m$ ", cwd);

    fflush(stdout);
}

static void redraw(const char *line, size_t len, size_t cursor)
{
    size_t total_columns = utf8_columns(line, len);
    size_t cursor_columns = utf8_cursor_columns(line, cursor);

    printf("\r\033[2K");
    prompt();
    fwrite(line, 1, len, stdout);

    if (total_columns > cursor_columns)
        printf("\033[%zuD", total_columns - cursor_columns);

    fflush(stdout);
}

static void add_history(const char *line)
{
    if (!line[0])
        return;

    if (history_count > 0 &&
        strcmp(history[history_count - 1], line) == 0)
        return;

    if (history_count == MAX_HISTORY) {
        memmove(history, history + 1,
                sizeof(history[0]) * (MAX_HISTORY - 1));
        history_count--;
    }

    snprintf(history[history_count++], MAX_LINE, "%s", line);
}

static void load_history(void)
{
    FILE *file = fopen("/root/.wink_history", "r");

    if (!file)
        return;

    char line[MAX_LINE];

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = '\0';
        add_history(line);
    }

    fclose(file);
}

static void save_history(void)
{
    FILE *file = fopen("/root/.wink_history", "w");

    if (!file)
        return;

    for (int i = 0; i < history_count; i++)
        fprintf(file, "%s\n", history[i]);

    fclose(file);
}

/* Read one complete UTF-8 character from the terminal. */

static int read_utf8_character(char *buffer, size_t *length)
{
    unsigned char first;
    ssize_t n = read(STDIN_FILENO, &first, 1);

    if (n != 1)
        return 0;

    size_t needed = 1;

    if ((first & 0xE0) == 0xC0)
        needed = 2;
    else if ((first & 0xF0) == 0xE0)
        needed = 3;
    else if ((first & 0xF8) == 0xF0)
        needed = 4;

    buffer[0] = (char)first;

    for (size_t i = 1; i < needed; i++) {
        unsigned char next;

        if (read(STDIN_FILENO, &next, 1) != 1)
            return 0;

        if ((next & 0xC0) != 0x80)
            return 0;

        buffer[i] = (char)next;
    }

    *length = needed;
    return 1;
}

static int read_line(char *line)
{
    struct termios old_term, raw_term;

    if (!isatty(STDIN_FILENO) ||
        tcgetattr(STDIN_FILENO, &old_term) != 0) {
        return fgets(line, MAX_LINE, stdin) != NULL;
    }

    raw_term = old_term;
    raw_term.c_lflag &= ~(ICANON | ECHO);
    raw_term.c_cc[VMIN] = 1;
    raw_term.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);

    size_t len = 0;
    size_t cursor = 0;
    int hist_pos = history_count;
    char saved[MAX_LINE] = "";

    line[0] = '\0';
    interrupted = 0;

    while (1) {
        char input[4];
        size_t input_len = 0;

        if (!read_utf8_character(input, &input_len)) {
            if (interrupted) {
                line[0] = '\0';
                len = cursor = 0;
                break;
            }

            continue;
        }

        unsigned char c = (unsigned char)input[0];

        /*
         * While viewing scrollback, ordinary input is ignored.
         * Q returns to the live command line.
         */
        if (scrollback_active && input_len == 1 && c != 27) {
            if (c == 'q' || c == 'Q')
                scrollback_leave(line, len, cursor);

            continue;
        }

        if (input_len == 1 && (c == '\r' || c == '\n')) {
            putchar('\n');
            break;
        }

        if (input_len == 1 && c == 4) {
            if (len == 0) {
                tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
                return 0;
            }

            if (cursor < len) {
                size_t next = utf8_next(line, cursor, len);
                memmove(line + cursor, line + next, len - next + 1);
                len -= next - cursor;
                redraw(line, len, cursor);
            }

            continue;
        }

        if (input_len == 1 && c == 3) {
            len = cursor = 0;
            line[0] = '\0';
            putchar('\n');
            prompt();
            continue;
        }

        if (input_len == 1 && c == 1) {
            cursor = 0;
            redraw(line, len, cursor);
            continue;
        }

        if (input_len == 1 && c == 5) {
            cursor = len;
            redraw(line, len, cursor);
            continue;
        }

        if (input_len == 1 && c == 12) {
            printf("\033[2J\033[H");
            redraw(line, len, cursor);
            continue;
        }

        if (input_len == 1 && c == 21) {
            len = cursor = 0;
            line[0] = '\0';
            redraw(line, len, cursor);
            continue;
        }

        if (input_len == 1 && c == 11) {
            len = cursor;
            line[len] = '\0';
            redraw(line, len, cursor);
            continue;
        }

        if (input_len == 1 && c == 23) {
            size_t old_cursor = cursor;

            while (cursor > 0 &&
                   isspace((unsigned char)line[cursor - 1]))
                cursor--;

            while (cursor > 0 &&
                   !isspace((unsigned char)line[cursor - 1]))
                cursor--;

            memmove(line + cursor, line + old_cursor,
                    len - old_cursor + 1);

            len -= old_cursor - cursor;
            redraw(line, len, cursor);
            continue;
        }

        if (input_len == 1 && (c == 127 || c == 8)) {
            if (cursor > 0) {
                size_t previous = utf8_prev(line, cursor);

                memmove(line + previous, line + cursor,
                        len - cursor + 1);

                len -= cursor - previous;
                cursor = previous;
                redraw(line, len, cursor);
            }

            continue;
        }

        if (input_len == 1 && c == '\t') {
            putchar('\a');
            continue;
        }

        if (input_len == 1 && c == 27) {
            char a, b;

            if (read(STDIN_FILENO, &a, 1) != 1)
                continue;

            if (a == '[') {
                if (read(STDIN_FILENO, &b, 1) != 1)
                    continue;

                /*
                 * In scrollback mode, only Page Up, Page Down,
                 * modified Home, and modified End are relevant.
                 */
                if (scrollback_active &&
                    b != '5' && b != '6' && b != '1' && b != '4')
                    continue;

                if (b == 'A') {
                    if (hist_pos > 0) {
                        if (hist_pos == history_count)
                            strcpy(saved, line);

                        strcpy(line, history[--hist_pos]);
                        len = cursor = strlen(line);
                        redraw(line, len, cursor);
                    }
                } else if (b == 'B') {
                    if (hist_pos < history_count) {
                        hist_pos++;

                        if (hist_pos == history_count)
                            strcpy(line, saved);
                        else
                            strcpy(line, history[hist_pos]);

                        len = cursor = strlen(line);
                        redraw(line, len, cursor);
                    }
                } else if (b == 'C' && cursor < len) {
                    cursor = utf8_next(line, cursor, len);
                    redraw(line, len, cursor);
                } else if (b == 'D' && cursor > 0) {
                    cursor = utf8_prev(line, cursor);
                    redraw(line, len, cursor);
                } else if (b == 'H') {
                    cursor = 0;
                    redraw(line, len, cursor);
                } else if (b == 'F') {
                    cursor = len;
                    redraw(line, len, cursor);
                } else if (b == '3') {
                    char tilde;

                    if (read(STDIN_FILENO, &tilde, 1) == 1 &&
                        tilde == '~' && cursor < len) {
                        size_t next = utf8_next(line, cursor, len);

                        memmove(line + cursor, line + next,
                                len - next + 1);

                        len -= next - cursor;
                        redraw(line, len, cursor);
                    }
                } else if (b == '5' || b == '6') {
                    char tail;
                    int valid = 0;

                    if (read(STDIN_FILENO, &tail, 1) == 1) {
                        if (tail == '~') {
                            valid = 1;
                        } else if (tail == ';') {
                            char modifier;
                            char finish;

                            if (read(STDIN_FILENO, &modifier, 1) == 1 &&
                                read(STDIN_FILENO, &finish, 1) == 1 &&
                                modifier == '2' && finish == '~') {
                                valid = 1;
                            }
                        }
                    }

                    if (valid) {
                        if (b == '5')
                            scrollback_page_up();
                        else
                            scrollback_page_down(line, len, cursor);
                    }
                } else if (b == '1' || b == '4') {
                    char tail;

                    if (read(STDIN_FILENO, &tail, 1) == 1) {
                        if (tail == '~') {
                            cursor = b == '1' ? 0 : len;
                            redraw(line, len, cursor);
                        } else if (tail == ';') {
                            char modifier;
                            char finish;

                            if (read(STDIN_FILENO, &modifier, 1) == 1 &&
                                read(STDIN_FILENO, &finish, 1) == 1 &&
                                modifier == '2') {
                                if (b == '1' && finish == 'H')
                                    scrollback_to_oldest();
                                else if (b == '4' && finish == 'F')
                                    scrollback_leave(line, len, cursor);
                            }
                        }
                    }
                }
            } else if (a == 'O') {
                if (read(STDIN_FILENO, &b, 1) == 1) {
                    if (b == 'H')
                        cursor = 0;
                    else if (b == 'F')
                        cursor = len;

                    redraw(line, len, cursor);
                }
            }

            continue;
        }

        if (input_len == 1 && c < 32)
            continue;

        if (len + input_len < MAX_LINE) {
            memmove(line + cursor + input_len,
                    line + cursor,
                    len - cursor + 1);

            memcpy(line + cursor, input, input_len);

            cursor += input_len;
            len += input_len;

            redraw(line, len, cursor);
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    return 1;
}

/* Tokenizer */

static int tokenize(const char *line, char **tokens)
{
    int count = 0;
    char word[MAX_LINE];
    size_t len = 0;
    int started = 0;
    char quote = 0;

    while (*line) {
        char c = *line++;

        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && *line) {
                word[len++] = *line++;
            } else if (len < MAX_LINE - 1) {
                word[len++] = c;
            }

            continue;
        }

        if (c == '#' && !started)
            break;

        if (c == '\'' || c == '"') {
            quote = c;
            started = 1;
            continue;
        }

        if (c == '\\' && *line) {
            word[len++] = *line++;
            started = 1;
            continue;
        }

        if (isspace((unsigned char)c)) {
            if (started) {
                if (count >= MAX_TOKENS)
                    return -1;

                word[len] = '\0';
                tokens[count++] = strdup(word);
                len = 0;
                started = 0;
            }

            continue;
        }

        if (c == '|' || c == '<' || c == '>' ||
            c == ';' || c == '&') {
            if (started) {
                if (count >= MAX_TOKENS)
                    return -1;

                word[len] = '\0';
                tokens[count++] = strdup(word);
                len = 0;
                started = 0;
            }

            if (count >= MAX_TOKENS)
                return -1;

            if (c == '>' && *line == '>') {
                tokens[count++] = strdup(">>");
                line++;
            } else if (c == '&' && *line == '&') {
                tokens[count++] = strdup("&&");
                line++;
            } else if (c == '|' && *line == '|') {
                tokens[count++] = strdup("||");
                line++;
            } else {
                char op[2] = {c, '\0'};
                tokens[count++] = strdup(op);
            }

            continue;
        }

        if (len < MAX_LINE - 1)
            word[len++] = c;

        started = 1;
    }

    if (quote) {
        fprintf(stderr, "winksh: unmatched quote\n");
        return -1;
    }

    if (started) {
        if (count >= MAX_TOKENS)
            return -1;

        word[len] = '\0';
        tokens[count++] = strdup(word);
    }

    return count;
}

static void free_tokens(char **tokens, int count)
{
    for (int i = 0; i < count; i++)
        free(tokens[i]);
}

/* Command lookup */

static int is_builtin(const char *name)
{
    const char *names[] = {
        "cd", "pwd", "echo", "exit", "help", "clear",
        "history", "export", "unset", "env", "whoami",
        "hostname", "uname", "true", "false", "type",
        "which", "umask", "version", "layout", NULL
    };

    for (int i = 0; names[i]; i++) {
        if (strcmp(name, names[i]) == 0)
            return 1;
    }

    return 0;
}

static int find_command(const char *name, char *result, size_t size)
{
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            snprintf(result, size, "%s", name);
            return 1;
        }

        return 0;
    }

    const char *path = getenv("PATH");

    if (!path)
        path = "/bin:/usr/bin";

    char *copy = strdup(path);

    if (!copy)
        return 0;

    char *save = NULL;
    char *dir = strtok_r(copy, ":", &save);

    while (dir) {
        char candidate[1024];

        snprintf(candidate, sizeof(candidate),
                 "%s/%s", *dir ? dir : ".", name);

        if (access(candidate, X_OK) == 0) {
            snprintf(result, size, "%s", candidate);
            free(copy);
            return 1;
        }

        dir = strtok_r(NULL, ":", &save);
    }

    free(copy);
    return 0;
}

/* Help */

static void show_help(void)
{
    puts("");
    puts("  ☆〜（ゝ。∂） WinkShell 0.2");
    puts("");
    puts("  FILES & NAVIGATION");
    puts("  cd [DIR]       Change directory");
    puts("  pwd            Show current directory");
    puts("  ls             List files (external)");
    puts("  clear          Clear screen");
    puts("");
    puts("  COMMANDS");
    puts("  echo [TEXT]    Print text");
    puts("  history        Show command history");
    puts("  type NAME      Identify a command");
    puts("  which NAME     Locate a command");
    puts("  help           Show this menu");
    puts("  exit [STATUS]  Exit WinkShell");
    puts("");
    puts("  SYSTEM");
    puts("  uname [-a]     System information");
    puts("  hostname       Show hostname");
    puts("  whoami         Show username");
    puts("  version        Show WinkShell version");
    puts("  umask [MASK]   File permission mask");
    puts("");
    puts("  ENVIRONMENT");
    puts("  export         Show/set variables");
    puts("  unset NAME     Remove variable");
    puts("  env            List environment");
    puts("");
    puts("  KEYBOARD LAYOUT");
    puts("  layout         Show current layout");
    puts("  layout us      English U.S. QWERTY");
    puts("  layout uk      English U.K. QWERTY");
    puts("  layout de      German QWERTZ");
    puts("  layout ch      Swiss QWERTZ");
    puts("  layout fr      French AZERTY");
    puts("");
    puts("  INPUT AND LINE EDITING");
    puts("  ↑ / ↓          Command history");
    puts("  ← / →          Move cursor");
    puts("  Home / End     Start / end of line");
    puts("  Backspace      Delete previous character");
    puts("  Ctrl+A / E     Start / end of line");
    puts("  Ctrl+U / K     Clear line / delete to end");
    puts("  Ctrl+W         Delete previous word");
    puts("  Ctrl+L         Clear screen");
    puts("  Ctrl+C         Cancel input");
    puts("  Ctrl+D         Exit on empty line");
    puts("  Fn+Up/Down     Scroll through terminal output");
    puts("  Shift+Fn+Left  Jump to oldest output");
    puts("  Shift+Fn+Right Return to latest output");
    puts("  Q              Leave scrollback view");
    puts("");
    puts("  OPERATORS");
    puts("  |  Pipe    ;  Sequence");
    puts("  >  Output  >> Append  < Input");
    puts("  && Success  || Failure");
    puts("");
}

/* Keyboard layouts */

static int set_keyboard_layout(const char *name, int announce)
{
    const char *keymap;
    const char *display_name;

    if (!strcmp(name, "us")) {
        keymap = "/usr/share/keymaps/xkb/us.map.gz";
        display_name = "English U.S. QWERTY";
    } else if (!strcmp(name, "uk")) {
        keymap = "/usr/share/keymaps/xkb/gb.map.gz";
        display_name = "English U.K. QWERTY";
    } else if (!strcmp(name, "de")) {
        keymap = "/usr/share/keymaps/xkb/de.map.gz";
        display_name = "English German QWERTZ";
    } else if (!strcmp(name, "ch")) {
        keymap = "/usr/share/keymaps/xkb/ch.map.gz";
        display_name = "English Swiss QWERTZ";
    } else if (!strcmp(name, "fr")) {
        keymap = "/usr/share/keymaps/xkb/fr.map.gz";
        display_name = "English French AZERTY";
    } else {
        fprintf(stderr, "layout: unknown layout '%s'\n", name);
        fprintf(stderr, "Available layouts: us, uk, de, ch, fr\n");
        return 1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        perror("layout: fork");
        return 1;
    }

    if (pid == 0) {
        execlp("loadkeys", "loadkeys", keymap, (char *)NULL);
        perror("layout: loadkeys");
        _exit(127);
    }

    int status;

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("layout: waitpid");
            return 1;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "layout: failed to apply keyboard layout\n");
        return 1;
    }

    current_layout =
        !strcmp(name, "us") ? "us" :
        !strcmp(name, "uk") ? "uk" :
        !strcmp(name, "de") ? "de" :
        !strcmp(name, "ch") ? "ch" : "fr";

    if (announce)
        printf("Keyboard layout changed to: %s\n", display_name);

    return 0;
}

static void show_keyboard_layout(void)
{
    if (!strcmp(current_layout, "us"))
        puts("Current keyboard layout: English U.S. QWERTY");
    else if (!strcmp(current_layout, "uk"))
        puts("Current keyboard layout: English U.K. QWERTY");
    else if (!strcmp(current_layout, "de"))
        puts("Current keyboard layout: English German QWERTZ");
    else if (!strcmp(current_layout, "ch"))
        puts("Current keyboard layout: English Swiss QWERTZ");
    else if (!strcmp(current_layout, "fr"))
        puts("Current keyboard layout: English French AZERTY");
}

/* Detect keyboard-layout typos before normal command execution. */

static int detect_layout_typo(const char *line)
{
    while (isspace((unsigned char)*line))
        line++;

    size_t word_length = strcspn(line, " \t\r\n");

    if (word_length == 6 &&
        strncmp(line, "lazout", 6) == 0) {
        char choice[32];

        printf("Looks like you misspelled \"layout.\" Which keyboard layout do you have? German (de) or Swiss? (ch) ");
        fflush(stdout);

        if (!fgets(choice, sizeof(choice), stdin))
            return 1;

        choice[strcspn(choice, "\r\n")] = '\0';

        if (!strcmp(choice, "de") || !strcmp(choice, "ch"))
            return set_keyboard_layout(choice, 1);

        fprintf(stderr, "Please enter de or ch.\n");
        return 2;
    }

    if (word_length >= 6 &&
        strncmp(line, "lqyout", 6) == 0 &&
        (word_length == 6 || line[6] == ',' || line[6] == '.')) {

        if (set_keyboard_layout("fr", 0) != 0)
            return 1;

        puts("Looks like you misspelled \"layout.\" Current keyboard layout was set as French (fr).");
        return 0;
    }

    return -1;
}

/* Builtins */

static int builtin(char **a)
{
    int argc = 0;

    while (a[argc])
        argc++;

    if (!strcmp(a[0], "layout")) {
        if (argc == 1) {
            show_keyboard_layout();
            return 0;
        }

        if (argc > 2) {
            fprintf(stderr, "Usage: layout [us|uk|de|ch|fr]\n");
            return 2;
        }

        return set_keyboard_layout(a[1], 1);
    }

    if (!strcmp(a[0], "cd")) {
        char old[512], now[512];
        const char *dest;

        getcwd(old, sizeof(old));

        if (argc < 2)
            dest = getenv("HOME");
        else if (!strcmp(a[1], "-"))
            dest = getenv("OLDPWD");
        else
            dest = a[1];

        if (!dest)
            dest = "/";

        if (chdir(dest) != 0) {
            perror("cd");
            return 1;
        }

        if (getcwd(now, sizeof(now))) {
            setenv("OLDPWD", old, 1);
            setenv("PWD", now, 1);

            if (argc > 1 && !strcmp(a[1], "-"))
                puts(now);
        }

        return 0;
    }

    if (!strcmp(a[0], "pwd")) {
        char cwd[512];

        if (getcwd(cwd, sizeof(cwd)))
            puts(cwd);
        else {
            perror("pwd");
            return 1;
        }

        return 0;
    }

    if (!strcmp(a[0], "echo")) {
        int start = 1;
        int newline = 1;

        if (argc > 1 && !strcmp(a[1], "-n")) {
            newline = 0;
            start = 2;
        }

        for (int i = start; i < argc; i++)
            printf("%s%s", a[i], i == argc - 1 ? "" : " ");

        if (newline)
            putchar('\n');

        return 0;
    }

    if (!strcmp(a[0], "exit")) {
        shell_exit = 1;
        shell_status = argc > 1 ? atoi(a[1]) : shell_status;
        return shell_status;
    }

    if (!strcmp(a[0], "clear")) {
        scrollback_reset();
        printf("\033[2J\033[H");
        return 0;
    }

    if (!strcmp(a[0], "help")) {
        show_help();
        return 0;
    }

    if (!strcmp(a[0], "history")) {
        if (argc > 1 && !strcmp(a[1], "-c")) {
            history_count = 0;
            return 0;
        }

        for (int i = 0; i < history_count; i++)
            printf("%4d  %s\n", i + 1, history[i]);

        return 0;
    }

    if (!strcmp(a[0], "export")) {
        extern char **environ;

        if (argc == 1) {
            for (char **e = environ; *e; e++)
                puts(*e);

            return 0;
        }

        for (int i = 1; i < argc; i++) {
            char *equals = strchr(a[i], '=');

            if (equals) {
                *equals = '\0';

                if (setenv(a[i], equals + 1, 1) != 0) {
                    perror("export");
                    return 1;
                }

                *equals = '=';
            } else if (!getenv(a[i])) {
                setenv(a[i], "", 1);
            }
        }

        return 0;
    }

    if (!strcmp(a[0], "unset")) {
        for (int i = 1; i < argc; i++)
            unsetenv(a[i]);

        return 0;
    }

    if (!strcmp(a[0], "env")) {
        extern char **environ;

        for (char **e = environ; *e; e++)
            puts(*e);

        return 0;
    }

    if (!strcmp(a[0], "whoami")) {
        struct passwd *pw = getpwuid(getuid());
        puts(pw ? pw->pw_name : "root");
        return 0;
    }

    if (!strcmp(a[0], "hostname")) {
        char name[256];

        if (argc > 1) {
            if (sethostname(a[1], strlen(a[1])) != 0) {
                perror("hostname");
                return 1;
            }
        } else {
            if (gethostname(name, sizeof(name)) != 0) {
                perror("hostname");
                return 1;
            }

            puts(name);
        }

        return 0;
    }

    if (!strcmp(a[0], "uname")) {
        struct utsname u;

        if (uname(&u) != 0) {
            perror("uname");
            return 1;
        }

        if (argc > 1 && !strcmp(a[1], "-a")) {
            printf("%s %s %s %s %s\n",
                   u.sysname, u.nodename, u.release,
                   u.version, u.machine);
        } else {
            puts(u.sysname);
        }

        return 0;
    }

    if (!strcmp(a[0], "true"))
        return 0;

    if (!strcmp(a[0], "false"))
        return 1;

    if (!strcmp(a[0], "version")) {
        puts("WinkShell 0.2");
        puts("Part of WinkLinuxC");
        return 0;
    }

    if (!strcmp(a[0], "umask")) {
        if (argc == 1) {
            mode_t old = umask(0);
            umask(old);
            printf("%04o\n", old);
        } else {
            char *end;
            unsigned long value = strtoul(a[1], &end, 8);

            if (*end || value > 0777) {
                fprintf(stderr, "umask: invalid mask\n");
                return 1;
            }

            umask((mode_t)value);
        }

        return 0;
    }

    if (!strcmp(a[0], "type") || !strcmp(a[0], "which")) {
        int status = 0;

        for (int i = 1; i < argc; i++) {
            char path[1024];

            if (is_builtin(a[i])) {
                if (!strcmp(a[0], "type"))
                    printf("%s is a shell builtin\n", a[i]);
                else
                    printf("%s: shell builtin\n", a[i]);
            } else if (find_command(a[i], path, sizeof(path))) {
                if (!strcmp(a[0], "type"))
                    printf("%s is %s\n", a[i], path);
                else
                    puts(path);
            } else {
                fprintf(stderr, "%s: not found\n", a[i]);
                status = 1;
            }
        }

        return status;
    }

    return 127;
}

/* Redirections */

static int apply_redirections(Command *cmd)
{
    int fd;

    if (cmd->input) {
        fd = open(cmd->input, O_RDONLY);

        if (fd < 0) {
            perror(cmd->input);
            return -1;
        }

        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    if (cmd->output) {
        int flags = O_WRONLY | O_CREAT |
                    (cmd->append ? O_APPEND : O_TRUNC);

        fd = open(cmd->output, flags, 0666);

        if (fd < 0) {
            perror(cmd->output);
            return -1;
        }

        dup2(fd, STDOUT_FILENO);
        close(fd);
    }

    return 0;
}

/* Run a builtin in the parent process and capture its output. */

static int run_parent_builtin(Command *cmd)
{
    int saved_in = -1;
    int saved_out = -1;
    int saved_err = -1;

    FILE *capture_file = NULL;

    if (isatty(STDOUT_FILENO) &&
        !cmd->output &&
        strcmp(cmd->argv[0], "clear") != 0) {
        capture_file = tmpfile();
    }

    if (cmd->input || cmd->output || capture_file) {
        fflush(NULL);

        saved_in = dup(STDIN_FILENO);
        saved_out = dup(STDOUT_FILENO);
        saved_err = dup(STDERR_FILENO);

        if (saved_in < 0 || saved_out < 0 || saved_err < 0) {
            perror("dup");

            if (saved_in >= 0)
                close(saved_in);

            if (saved_out >= 0)
                close(saved_out);

            if (saved_err >= 0)
                close(saved_err);

            if (capture_file)
                fclose(capture_file);

            return 1;
        }

        if ((cmd->input || cmd->output) &&
            apply_redirections(cmd) != 0) {
            dup2(saved_in, STDIN_FILENO);
            dup2(saved_out, STDOUT_FILENO);
            dup2(saved_err, STDERR_FILENO);

            close(saved_in);
            close(saved_out);
            close(saved_err);

            if (capture_file)
                fclose(capture_file);

            return 1;
        }

        if (capture_file) {
            if (dup2(fileno(capture_file), STDOUT_FILENO) < 0 ||
                dup2(fileno(capture_file), STDERR_FILENO) < 0) {
                perror("dup2");

                dup2(saved_in, STDIN_FILENO);
                dup2(saved_out, STDOUT_FILENO);
                dup2(saved_err, STDERR_FILENO);

                close(saved_in);
                close(saved_out);
                close(saved_err);
                fclose(capture_file);

                return 1;
            }
        }
    }

    int status = builtin(cmd->argv);

    fflush(NULL);

    if (saved_in >= 0) {
        dup2(saved_in, STDIN_FILENO);
        close(saved_in);
    }

    if (saved_out >= 0) {
        dup2(saved_out, STDOUT_FILENO);
        close(saved_out);
    }

    if (saved_err >= 0) {
        dup2(saved_err, STDERR_FILENO);
        close(saved_err);
    }

    if (capture_file) {
        rewind(capture_file);

        char buffer[4096];
        size_t n;

        while ((n = fread(buffer, 1, sizeof(buffer), capture_file)) > 0)
            scrollback_output(buffer, n);

        fclose(capture_file);
    }

    return status;
}

/* Identify commands that need direct terminal access. */

static int command_requires_terminal(const char *name)
{
    const char *base = strrchr(name, '/');

    if (base)
        name = base + 1;

    const char *names[] = {
        "less", "more", "top", "htop",
        "vi", "vim", "nano", "screen",
        "tmux", "watch", "ssh", "login",
        "su", "sh", "ash", "bash",
        "python", "python3",
        NULL
    };

    for (int i = 0; names[i]; i++) {
        if (strcmp(name, names[i]) == 0)
            return 1;
    }

    return 0;
}

/* Run external commands and capture their output. */

static int run_pipeline(Command *cmds, int count)
{
    if (count == 1 && is_builtin(cmds[0].argv[0]))
        return run_parent_builtin(&cmds[0]);

    int pipes[MAX_PIPE - 1][2];
    int capture_pipe[2] = {-1, -1};

    pid_t pids[MAX_PIPE];
    int launched = 0;

    int capture_output =
        isatty(STDOUT_FILENO) &&
        cmds[count - 1].output == NULL &&
        !command_requires_terminal(cmds[count - 1].argv[0]);

    if (capture_output && pipe(capture_pipe) != 0) {
        perror("pipe");
        return 1;
    }

    for (int i = 0; i < count - 1; i++) {
        if (pipe(pipes[i]) != 0) {
            perror("pipe");

            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (capture_output) {
                close(capture_pipe[0]);
                close(capture_pipe[1]);
            }

            return 1;
        }
    }

    for (int i = 0; i < count; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            break;
        }

        if (pid == 0) {
            signal(SIGINT, SIG_DFL);

            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);

            if (i < count - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            if (capture_output) {
                if (i == count - 1)
                    dup2(capture_pipe[1], STDOUT_FILENO);

                dup2(capture_pipe[1], STDERR_FILENO);
            }

            for (int j = 0; j < count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (capture_output) {
                close(capture_pipe[0]);
                close(capture_pipe[1]);
            }

            if (apply_redirections(&cmds[i]) != 0)
                _exit(1);

            if (is_builtin(cmds[i].argv[0])) {
                int status = builtin(cmds[i].argv);
                fflush(NULL);
                _exit(status);
            }

            execvp(cmds[i].argv[0], cmds[i].argv);
            perror(cmds[i].argv[0]);
            _exit(127);
        }

        pids[launched++] = pid;
    }

    for (int i = 0; i < count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (capture_output) {
        close(capture_pipe[1]);

        char buffer[4096];
        ssize_t n;

        while ((n = read(capture_pipe[0], buffer, sizeof(buffer))) > 0)
            scrollback_output(buffer, (size_t)n);

        close(capture_pipe[0]);
    }

    int status = 1;

    for (int i = 0; i < launched; i++) {
        int result = 0;
        pid_t waited;

        do {
            waited = waitpid(pids[i], &result, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0) {
            if (i == launched - 1)
                status = 1;

            continue;
        }

        if (i == launched - 1) {
            if (WIFEXITED(result))
                status = WEXITSTATUS(result);
            else if (WIFSIGNALED(result))
                status = 128 + WTERMSIG(result);
        }
    }

    return launched == count ? status : 1;
}

/* Parse pipeline tokens. */

static int execute_pipeline_tokens(char **tokens, int count)
{
    Command cmds[MAX_PIPE] = {0};
    int command_count = 0;
    int valid = 1;

    if (count == 0)
        return 0;

    for (int i = 0; i < count; i++) {
        Command *cmd = &cmds[command_count];

        if (!strcmp(tokens[i], "|")) {
            if (cmd->argc == 0 || command_count == MAX_PIPE - 1) {
                valid = 0;
                break;
            }

            command_count++;
            continue;
        }

        if (!strcmp(tokens[i], "<") ||
            !strcmp(tokens[i], ">") ||
            !strcmp(tokens[i], ">>")) {
            if (i + 1 >= count ||
                !strcmp(tokens[i + 1], "|") ||
                !strcmp(tokens[i + 1], ";") ||
                !strcmp(tokens[i + 1], "&&") ||
                !strcmp(tokens[i + 1], "||")) {
                valid = 0;
                break;
            }

            if (!strcmp(tokens[i], "<")) {
                cmd->input = tokens[++i];
            } else {
                cmd->output = tokens[++i];
                cmd->append = !strcmp(tokens[i - 1], ">>");
            }

            continue;
        }

        if (cmd->argc >= MAX_ARGS - 1) {
            valid = 0;
            break;
        }

        cmd->argv[cmd->argc++] = tokens[i];
        cmd->argv[cmd->argc] = NULL;
    }

    if (cmds[command_count].argc == 0)
        valid = 0;

    if (!valid) {
        fprintf(stderr, "winksh: invalid command syntax\n");
        return 2;
    }

    return run_pipeline(cmds, command_count + 1);
}

/* Execute a complete command line. */

static int execute_line(const char *line)
{
    int typo_status = detect_layout_typo(line);

    if (typo_status != -1)
        return typo_status;

    char *tokens[MAX_TOKENS];
    int count = tokenize(line, tokens);

    if (count < 0 || count >= MAX_TOKENS) {
        free_tokens(tokens, count > 0 ? count : 0);
        fprintf(stderr, "winksh: too many tokens or invalid input\n");
        return 2;
    }

    int status = 0;
    int should_run = 1;
    int start = 0;

    while (start < count) {
        int end = start;

        while (end < count &&
               strcmp(tokens[end], ";") &&
               strcmp(tokens[end], "&&") &&
               strcmp(tokens[end], "||"))
            end++;

        if (should_run && end > start)
            status = execute_pipeline_tokens(tokens + start,
                                             end - start);

        if (end == count)
            break;

        if (!strcmp(tokens[end], ";"))
            should_run = 1;
        else if (!strcmp(tokens[end], "&&"))
            should_run = status == 0;
        else
            should_run = status != 0;

        start = end + 1;
    }

    free_tokens(tokens, count);
    return status;
}

/* Main */

int main(int argc, char **argv)
{
    char line[MAX_LINE];

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    setenv("HOME", "/root", 0);
    setenv("USER", "root", 0);
    setenv("HOSTNAME", "winkc", 0);
    setenv("PATH",
           "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
           0);

    if (argc >= 2 && strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            fprintf(stderr, "winksh: -c requires a command\n");
            return 2;
        }

        return execute_line(argv[2]);
    }

    if (argc >= 2) {
        FILE *script = fopen(argv[1], "r");

        if (!script) {
            perror(argv[1]);
            return 1;
        }

        while (fgets(line, sizeof(line), script)) {
            shell_status = execute_line(line);

            if (shell_exit)
                break;
        }

        if (ferror(script)) {
            perror(argv[1]);
            shell_status = 1;
        }

        fclose(script);
        return shell_status;
    }

    load_history();

    while (!shell_exit) {
        prompt();

        if (!read_line(line))
            break;

        add_history(line);

        if (!line[0])
            continue;

        shell_status = execute_line(line);
    }

    save_history();
    return shell_status;
}