#include <ctype.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_LINE_LEN 4096
#define MAX_IF_DEPTH 128

static void print_usage(void) {
    printf("SLYZ Tools for Windows\n");
    printf("Usage:\n");
    printf("  slyztool check <file.slyz>\n");
    printf("  slyztool run <file.slyz> [Y|N]\n");
    printf("  slyztool translate <file.slyz> [out.bat]\n");
    printf("  slyztool doc [topic]\n");
    printf("  slyztool help\n\n");
    printf("Supported SLYZ commands:\n");
    printf("  echo <text>\n");
    printf("  ask <prompt>\n");
    printf("  if yes|no\n");
    printf("  endif\n");
    printf("  dir | ls\n");
    printf("  run <file.slyz>\n");
    printf("  exit | quit\n");
    printf("  help\n");
}

static char *trim_in_place(char *s) {
    char *start = s;
    while (*start != '\0' && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
        start++;
    }
    if (*start == '\0') {
        return start;
    }
    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return start;
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int is_known_command(const char *cmd) {
    static const char *known[] = {
        "echo", "ask", "if", "endif", "dir", "ls", "run", "help", "exit", "quit", "cls", "clear", "call", "mkdir", "type", "rem", "set", "goto", "pause", "copy", "del", "rd", "rmdir", "cd", "ver", "date", "time", NULL
    };
    for (int i = 0; known[i] != NULL; i++) {
        if (_stricmp(cmd, known[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int count_lines(const char *text, char ***out_lines) {
    char **lines = NULL;
    int count = 0;
    const char *p = text;
    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != '\n' && *p != '\r') {
            p++;
        }
        if (p > start) {
            size_t n = (size_t)(p - start);
            char *line = (char *)malloc(n + 1);
            if (!line) {
                fprintf(stderr, "Out of memory.\n");
                exit(1);
            }
            memcpy(line, start, n);
            line[n] = '\0';
            lines = (char **)realloc(lines, (count + 1) * sizeof(char *));
            lines[count++] = line;
        }
        while (*p == '\r' || *p == '\n') {
            p++;
        }
    }
    *out_lines = lines;
    return count;
}

static void free_lines(char **lines, int count) {
    if (!lines) return;
    for (int i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);
}

static int validate_script(const char *path, const char *text, int *error_count) {
    char **lines = NULL;
    int count = count_lines(text, &lines);
    int if_stack[MAX_IF_DEPTH];
    int depth = 0;
    int errors = 0;

    for (int i = 0; i < count; i++) {
        char *line = trim_in_place(lines[i]);
        if (*line == '\0' || *line == '#') {
            continue;
        }

        if (depth > 0 && line[0] == ' ' && !(line[0] == '\t')) {
            /* informational style hint; not fatal */
        }

        if (starts_with(line, "if ")) {
            char *cond = line + 3;
            cond = trim_in_place(cond);
            if (_stricmp(cond, "yes") != 0 && _stricmp(cond, "no") != 0) {
                printf("ERROR %s:%d: invalid if condition '%s' (expected yes or no)\n", path, i + 1, cond);
                errors++;
            }
            if (depth >= MAX_IF_DEPTH) {
                printf("ERROR %s:%d: too deep nesting\n", path, i + 1);
                errors++;
            } else {
                if_stack[depth++] = i + 1;
            }
            continue;
        }

        if (_stricmp(line, "endif") == 0) {
            if (depth <= 0) {
                printf("ERROR %s:%d: unexpected endif\n", path, i + 1);
                errors++;
            } else {
                depth--;
            }
            continue;
        }

        if (starts_with(line, "ask")) {
            char *rest = line + 3;
            rest = trim_in_place(rest);
            if (*rest == '\0' || *rest == '"' || *rest == '\'') {
                printf("ERROR %s:%d: ask requires a prompt string\n", path, i + 1);
                errors++;
            }
            continue;
        }

        if (starts_with(line, "echo")) {
            char *rest = line + 4;
            rest = trim_in_place(rest);
            if (*rest == '\0') {
                printf("ERROR %s:%d: echo requires a message\n", path, i + 1);
                errors++;
            }
            continue;
        }

        if (starts_with(line, "run")) {
            char *rest = line + 3;
            rest = trim_in_place(rest);
            if (*rest == '\0') {
                printf("ERROR %s:%d: run requires a file name\n", path, i + 1);
                errors++;
            }
            continue;
        }

        if (_stricmp(line, "dir") == 0 || _stricmp(line, "ls") == 0 || _stricmp(line, "help") == 0 || _stricmp(line, "exit") == 0 || _stricmp(line, "quit") == 0 || _stricmp(line, "clear") == 0 || _stricmp(line, "cls") == 0) {
            continue;
        }

        if (strncmp(line, "#", 1) == 0) {
            continue;
        }

        char *cmd = line;
        while (*cmd != '\0' && !isspace((unsigned char)*cmd)) {
            cmd++;
        }
        if (*cmd != '\0') {
            *cmd = '\0';
            cmd = trim_in_place(cmd + 1);
        }
        if (!strcmp(line, "") || is_known_command(line)) {
            continue;
        }

        printf("ERROR %s:%d: unknown command '%s'\n", path, i + 1, line);
        errors++;
    }

    if (depth > 0) {
        printf("ERROR %s:%d: missing endif for if started at line %d\n", path, count, if_stack[depth - 1]);
        errors++;
    }

    free_lines(lines, count);
    *error_count = errors;
    return errors == 0;
}

static void replace_ext(char *out, size_t outsz, const char *in, const char *newext) {
    strncpy(out, in, outsz - 1);
    out[outsz - 1] = '\0';
    char *dot = strrchr(out, '.');
    if (dot == NULL) {
        strncat(out, newext, outsz - strlen(out) - 1);
    } else {
        *dot = '\0';
        strncat(out, newext, outsz - strlen(out) - 1);
    }
}

static void write_bat_translate(const char *in_path, const char *out_path, const char *forced_answer) {
    FILE *fp = fopen(in_path, "rb");
    if (!fp) {
        fprintf(stderr, "Could not open '%s'\n", in_path);
        exit(1);
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        fprintf(stderr, "Out of memory while reading '%s'\n", in_path);
        exit(1);
    }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';

    char **lines = NULL;
    int count = count_lines(buf, &lines);
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "Could not write '%s'\n", out_path);
        exit(1);
    }

    fprintf(out, "@echo off\r\n");
    fprintf(out, "setlocal EnableExtensions\r\n");
    fprintf(out, "set \"SLYZ_LAST=0\"\r\n");
    fprintf(out, "set \"SLYZ_ASK=\"\r\n");
    if (forced_answer && *forced_answer) {
        fprintf(out, "set \"SLYZ_FORCE_ANSWER=%s\"\r\n", forced_answer);
    }
    for (int i = 0; i < count; i++) {
        char *line = trim_in_place(lines[i]);
        if (*line == '\0' || *line == '#') {
            fprintf(out, "rem %s\r\n", line);
            continue;
        }

        if (_strnicmp(line, "echo ", 5) == 0) {
            fprintf(out, "echo %s\r\n", line + 5);
        } else if (_strnicmp(line, "ask ", 4) == 0) {
            char *prompt = line + 4;
            prompt = trim_in_place(prompt);
            if (*prompt == '"') {
                char *p = prompt + 1;
                while (*p && *p != '"') p++;
                if (*p == '"') *p = '\0';
                prompt++;
            }
            fprintf(out, "if not \"%%SLYZ_FORCE_ANSWER%%\"==\"\" (\r\n");
            fprintf(out, "  if /I \"%%SLYZ_FORCE_ANSWER%%\"==\"Y\" ( set \"SLYZ_LAST=1\" ) else ( set \"SLYZ_LAST=0\" )\r\n");
            fprintf(out, ") else (\r\n");
            fprintf(out, "  set /p \"SLYZ_ASK=%s [Y/N]? \"\r\n", prompt);
            fprintf(out, "  if /I \"%%SLYZ_ASK%%\"==\"Y\" ( set \"SLYZ_LAST=1\" ) else ( set \"SLYZ_LAST=0\" )\r\n");
            fprintf(out, ")\r\n");
        } else if (_strnicmp(line, "if ", 3) == 0) {
            char *cond = line + 3;
            cond = trim_in_place(cond);
            if (_stricmp(cond, "yes") == 0) {
                fprintf(out, "if /I \"%%SLYZ_LAST%%\"==\"1\" (\r\n");
            } else if (_stricmp(cond, "no") == 0) {
                fprintf(out, "if /I \"%%SLYZ_LAST%%\"==\"0\" (\r\n");
            } else {
                fprintf(out, "rem invalid condition: %s\r\n", cond);
            }
        } else if (_stricmp(line, "endif") == 0) {
            fprintf(out, ")\r\n");
        } else if (_stricmp(line, "dir") == 0 || _stricmp(line, "ls") == 0) {
            fprintf(out, "dir\r\n");
        } else if (_stricmp(line, "help") == 0) {
            fprintf(out, "echo SLYZ help: echo, ask, if yes/no, endif, dir, run, exit\r\n");
        } else if (_stricmp(line, "exit") == 0 || _stricmp(line, "quit") == 0) {
            fprintf(out, "exit /b 0\r\n");
        } else if (_strnicmp(line, "run ", 4) == 0) {
            char *target = line + 4;
            target = trim_in_place(target);
            char bat_target[260];
            replace_ext(bat_target, sizeof(bat_target), target, ".bat");
            fprintf(out, "call \"%%~dp0%s\"\r\n", bat_target);
        } else {
            fprintf(out, "%s\r\n", line);
        }
    }

    fprintf(out, "endlocal\r\n");
    fclose(out);
    free_lines(lines, count);
    free(buf);
}

static void run_script_file(const char *path, const char *forced_answer) {
    char out_path[MAX_PATH];
    replace_ext(out_path, sizeof(out_path), path, ".bat");
    write_bat_translate(path, out_path, forced_answer);

    char cmdline[MAX_PATH * 2];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /C \"%s\"", out_path);
    int rc = system(cmdline);
    if (rc != 0) {
        printf("Script exited with code %d\n", rc);
    }
}

static void show_doc(const char *topic) {
    char doc_path[MAX_PATH];
    GetModuleFileNameA(NULL, doc_path, MAX_PATH);
    char *slash = strrchr(doc_path, '\\');
    if (slash) {
        *(slash + 1) = '\0';
    }
    strcat(doc_path, "docs\\slyz-reference.md");

    if (topic && *topic) {
        printf("Documentation topic: %s\n", topic);
    }

    printf("Documentation file: %s\n", doc_path);

    char *text = read_entire_file(doc_path);
    if (!text) {
        fprintf(stderr, "Unable to read documentation file: %s\n", doc_path);
        return;
    }

    printf("\n--- SLYZ reference ---\n");
    printf("%s\n", text);
    printf("--- end of document ---\n");
    free(text);
}

static char *read_entire_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open '%s'\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        fprintf(stderr, "Out of memory\n");
        return NULL;
    }
    size_t read_count = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read_count] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (_stricmp(argv[1], "help") == 0 || _stricmp(argv[1], "-h") == 0 || _stricmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }

    if (_stricmp(argv[1], "check") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: slyztool check <file.slyz>\n");
            return 1;
        }
        char *text = read_entire_file(argv[2]);
        if (!text) return 1;
        int errors = 0;
        int ok = validate_script(argv[2], text, &errors);
        free(text);
        if (!ok) {
            printf("Syntax check failed with %d error(s).\n", errors);
            return 1;
        }
        printf("Syntax OK.\n");
        return 0;
    }

    if (_stricmp(argv[1], "translate") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: slyztool translate <file.slyz> [out.bat]\n");
            return 1;
        }
        char out_path[MAX_PATH];
        if (argc >= 4) {
            strncpy(out_path, argv[3], sizeof(out_path) - 1);
            out_path[sizeof(out_path) - 1] = '\0';
        } else {
            replace_ext(out_path, sizeof(out_path), argv[2], ".bat");
        }
        char *text = read_entire_file(argv[2]);
        if (!text) return 1;
        int errors = 0;
        if (!validate_script(argv[2], text, &errors)) {
            free(text);
            fprintf(stderr, "Translation aborted because the script has syntax errors.\n");
            return 1;
        }
        free(text);
        write_bat_translate(argv[2], out_path, NULL);
        printf("Translated to: %s\n", out_path);
        return 0;
    }

    if (_stricmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: slyztool run <file.slyz> [Y|N]\n");
            return 1;
        }
        char *text = read_entire_file(argv[2]);
        if (!text) return 1;
        int errors = 0;
        int ok = validate_script(argv[2], text, &errors);
        free(text);
        if (!ok) {
            fprintf(stderr, "Cannot run because the script has syntax errors.\n");
            return 1;
        }

        const char *forced_answer = (argc >= 4) ? argv[3] : NULL;
        if (!forced_answer && !isatty(fileno(stdin))) {
            char std_in_buf[32];
            if (fgets(std_in_buf, sizeof(std_in_buf), stdin)) {
                char *trimmed = trim_in_place(std_in_buf);
                if (*trimmed == 'Y' || *trimmed == 'y' || *trimmed == 'N' || *trimmed == 'n') {
                    char *tmp = _strdup(trimmed);
                    if (tmp) {
                        forced_answer = tmp;
                    }
                }
            }
        }

        if (forced_answer && (*forced_answer != 'Y' && *forced_answer != 'y' && *forced_answer != 'N' && *forced_answer != 'n')) {
            fprintf(stderr, "Answer must be Y or N when provided.\n");
            return 1;
        }
        run_script_file(argv[2], forced_answer);
        if (forced_answer && forced_answer != argv[3]) {
            free((void *)forced_answer);
        }
        return 0;
    }

    if (_stricmp(argv[1], "doc") == 0) {
        const char *topic = argc >= 3 ? argv[2] : "all";
        show_doc(topic);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", argv[1]);
    print_usage();
    return 1;
}
