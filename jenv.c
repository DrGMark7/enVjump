#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CONFIG_REL ".config/jenv/config"
#define CONFIG_DIR_REL ".config/jenv"
#define BASHRC_REL ".bashrc"
#define HOOK_START "# >>> jenv hook >>>\n"
#define HOOK_END "# <<< jenv hook <<<\n"

static const char *HOOK_BLOCK =
    "# >>> jenv hook >>>\n"
    "if [[ $- == *i* ]]; then\n"
    "    if [[ -f \"$HOME/.config/jenv/config\" ]]; then\n"
    "        source \"$HOME/.config/jenv/config\"\n"
    "        if [[ -n \"$JENV_DIR\" && -d \"$JENV_DIR\" ]]; then\n"
    "            cd \"$JENV_DIR\" >/dev/null 2>&1 || true\n"
    "        fi\n"
    "        if [[ -n \"$JENV_CONDA_ENV\" ]]; then\n"
    "            if command -v conda >/dev/null 2>&1; then\n"
    "                eval \"$(conda shell.bash hook)\" >/dev/null 2>&1\n"
    "            elif [[ -f \"$HOME/miniconda3/etc/profile.d/conda.sh\" ]]; then\n"
    "                source \"$HOME/miniconda3/etc/profile.d/conda.sh\" >/dev/null 2>&1\n"
    "            elif [[ -f \"$HOME/anaconda3/etc/profile.d/conda.sh\" ]]; then\n"
    "                source \"$HOME/anaconda3/etc/profile.d/conda.sh\" >/dev/null 2>&1\n"
    "            fi\n"
    "            conda activate \"$JENV_CONDA_ENV\" >/dev/null 2>&1 || true\n"
    "        fi\n"
    "    fi\n"
    "fi\n"
    "# <<< jenv hook <<<\n";

static void die(const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static const char *must_get_home(void) {
    const char *home = getenv("HOME");

    if (home == NULL || home[0] == '\0') {
        die("HOME is not set");
    }
    return home;
}

static char *path_join_home(const char *rel) {
    const char *home = must_get_home();
    size_t len = strlen(home) + 1 + strlen(rel) + 1;
    char *path = malloc(len);

    if (path == NULL) {
        die("out of memory");
    }
    snprintf(path, len, "%s/%s", home, rel);
    return path;
}

static void ensure_dir(const char *path) {
    if (mkdir(path, 0700) == 0) {
        return;
    }
    if (errno == EEXIST) {
        struct stat st;

        if (stat(path, &st) != 0) {
            die("stat failed for %s: %s", path, strerror(errno));
        }
        if (!S_ISDIR(st.st_mode)) {
            die("%s exists but is not a directory", path);
        }
        return;
    }
    die("mkdir failed for %s: %s", path, strerror(errno));
}

static void mkdir_p_jenv(void) {
    char *config_root = path_join_home(".config");
    char *jenv_dir = path_join_home(CONFIG_DIR_REL);

    ensure_dir(config_root);
    ensure_dir(jenv_dir);
    free(config_root);
    free(jenv_dir);
}

static char *read_file(const char *path, size_t *len_out) {
    FILE *fp = fopen(path, "r");
    char *buf;
    long len;

    if (fp == NULL) {
        if (errno == ENOENT) {
            return NULL;
        }
        die("failed to open %s: %s", path, strerror(errno));
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        die("failed to seek %s: %s", path, strerror(errno));
    }
    len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        die("failed to tell %s: %s", path, strerror(errno));
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        die("failed to rewind %s: %s", path, strerror(errno));
    }

    buf = malloc((size_t)len + 1);
    if (buf == NULL) {
        fclose(fp);
        die("out of memory");
    }
    if (len > 0 && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        die("failed to read %s", path);
    }
    buf[len] = '\0';
    fclose(fp);
    if (len_out != NULL) {
        *len_out = (size_t)len;
    }
    return buf;
}

static bool contains_block(const char *haystack, const char *needle) {
    return haystack != NULL && strstr(haystack, needle) != NULL;
}

static void write_file_atomic(const char *path, const char *content) {
    size_t len = strlen(path) + 5;
    char *tmp = malloc(len);
    FILE *fp;

    if (tmp == NULL) {
        die("out of memory");
    }
    snprintf(tmp, len, "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (fp == NULL) {
        free(tmp);
        die("failed to open %s: %s", tmp, strerror(errno));
    }
    if (fputs(content, fp) == EOF) {
        fclose(fp);
        unlink(tmp);
        free(tmp);
        die("failed to write %s", tmp);
    }
    if (fclose(fp) != 0) {
        unlink(tmp);
        free(tmp);
        die("failed to close %s: %s", tmp, strerror(errno));
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        die("failed to rename %s to %s: %s", tmp, path, strerror(errno));
    }
    free(tmp);
}

static char *shell_escape_single_quote(const char *value) {
    size_t extra = 0;
    size_t i;
    size_t pos = 0;
    char *out;

    for (i = 0; value[i] != '\0'; ++i) {
        if (value[i] == '\'') {
            extra += 3;
        }
    }
    out = malloc(strlen(value) + extra + 3);
    if (out == NULL) {
        die("out of memory");
    }
    out[pos++] = '\'';
    for (i = 0; value[i] != '\0'; ++i) {
        if (value[i] == '\'') {
            memcpy(out + pos, "'\\''", 4);
            pos += 4;
        } else {
            out[pos++] = value[i];
        }
    }
    out[pos++] = '\'';
    out[pos] = '\0';
    return out;
}

static void install_hook(void) {
    char *bashrc = path_join_home(BASHRC_REL);
    char *content = read_file(bashrc, NULL);
    FILE *fp;

    if (content != NULL && contains_block(content, HOOK_START)) {
        free(content);
        free(bashrc);
        return;
    }

    fp = fopen(bashrc, content == NULL ? "w" : "a");
    if (fp == NULL) {
        free(content);
        free(bashrc);
        die("failed to open %s: %s", bashrc, strerror(errno));
    }
    if (content != NULL && strlen(content) > 0 && content[strlen(content) - 1] != '\n') {
        fputc('\n', fp);
    }
    if (fputs(HOOK_BLOCK, fp) == EOF) {
        fclose(fp);
        free(content);
        free(bashrc);
        die("failed to write hook to %s", bashrc);
    }
    if (fclose(fp) != 0) {
        free(content);
        free(bashrc);
        die("failed to close %s: %s", bashrc, strerror(errno));
    }
    free(content);
    free(bashrc);
}

static void uninstall_hook(void) {
    char *bashrc = path_join_home(BASHRC_REL);
    size_t len = 0;
    char *content = read_file(bashrc, &len);
    char *start;
    char *end;
    size_t prefix_len;
    size_t suffix_len;
    char *updated;

    if (content == NULL) {
        free(bashrc);
        return;
    }

    start = strstr(content, HOOK_START);
    if (start == NULL) {
        free(content);
        free(bashrc);
        return;
    }
    end = strstr(start, HOOK_END);
    if (end == NULL) {
        free(content);
        free(bashrc);
        die("found malformed jenv hook block in %s", bashrc);
    }
    end += strlen(HOOK_END);
    prefix_len = (size_t)(start - content);
    suffix_len = len - (size_t)(end - content);
    updated = malloc(prefix_len + suffix_len + 1);
    if (updated == NULL) {
        free(content);
        free(bashrc);
        die("out of memory");
    }
    memcpy(updated, content, prefix_len);
    memcpy(updated + prefix_len, end, suffix_len);
    updated[prefix_len + suffix_len] = '\0';
    write_file_atomic(bashrc, updated);
    free(updated);
    free(content);
    free(bashrc);
}

static void write_config(const char *dir, const char *env_name) {
    char *config = path_join_home(CONFIG_REL);
    char *dir_escaped = shell_escape_single_quote(dir);
    char *env_escaped = shell_escape_single_quote(env_name);
    size_t len = strlen("export JENV_DIR=\nexport JENV_CONDA_ENV=\n") +
                 strlen(dir_escaped) + strlen(env_escaped) + 1;
    char *content = malloc(len);

    if (content == NULL) {
        die("out of memory");
    }
    snprintf(content, len,
             "export JENV_DIR=%s\n"
             "export JENV_CONDA_ENV=%s\n",
             dir_escaped, env_escaped);
    write_file_atomic(config, content);
    free(content);
    free(dir_escaped);
    free(env_escaped);
    free(config);
}

static void set_current(const char *env_override) {
    const char *pwd = getenv("PWD");
    char cwd[PATH_MAX];
    const char *env_name = env_override;

    mkdir_p_jenv();
    install_hook();

    if (pwd == NULL || pwd[0] == '\0') {
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            die("failed to determine current directory: %s", strerror(errno));
        }
        pwd = cwd;
    }
    if (env_name == NULL || env_name[0] == '\0') {
        env_name = getenv("CONDA_DEFAULT_ENV");
    }
    if (env_name == NULL || env_name[0] == '\0') {
        env_name = "base";
    }

    write_config(pwd, env_name);
    printf("Pinned startup directory: %s\n", pwd);
    printf("Pinned Conda env: %s\n", env_name);
}

static void purge(void) {
    char *config = path_join_home(CONFIG_REL);

    if (unlink(config) != 0 && errno != ENOENT) {
        die("failed to remove %s: %s", config, strerror(errno));
    }
    printf("Cleared jenv startup config.\n");
    free(config);
}

static char *parse_single_quoted_value(const char *line, const char *prefix) {
    const char *p;
    const char *end;
    size_t out_len = 0;
    char *out;

    if (strncmp(line, prefix, strlen(prefix)) != 0) {
        return NULL;
    }
    p = line + strlen(prefix);
    if (*p != '\'') {
        return NULL;
    }
    ++p;
    out = malloc(strlen(p) + 1);
    if (out == NULL) {
        die("out of memory");
    }
    while (*p != '\0') {
        if (*p == '\'') {
            if (strncmp(p, "'\\''", 4) == 0) {
                out[out_len++] = '\'';
                p += 4;
                continue;
            }
            end = p + 1;
            if (*end == '\n' || *end == '\0') {
                out[out_len] = '\0';
                return out;
            }
            free(out);
            return NULL;
        }
        out[out_len++] = *p++;
    }
    free(out);
    return NULL;
}

static void status(void) {
    char *config = path_join_home(CONFIG_REL);
    char *content = read_file(config, NULL);
    char *dir = NULL;
    char *env_name = NULL;
    char *line;
    char *saveptr = NULL;

    if (content == NULL) {
        printf("No startup directory or Conda env is pinned.\n");
        free(config);
        return;
    }

    for (line = strtok_r(content, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
        if (dir == NULL) {
            dir = parse_single_quoted_value(line, "export JENV_DIR=");
        }
        if (env_name == NULL) {
            env_name = parse_single_quoted_value(line, "export JENV_CONDA_ENV=");
        }
    }

    if (dir == NULL || env_name == NULL) {
        free(dir);
        free(env_name);
        die("config file %s is malformed", config);
    }

    printf("Pinned startup directory: %s\n", dir);
    printf("Pinned Conda env: %s\n", env_name);
    free(dir);
    free(env_name);
    free(content);
    free(config);
}

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  jenv set [ENV_NAME]\n"
            "  jenv purge\n"
            "  jenv status\n"
            "  jenv install\n"
            "  jenv uninstall\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 1;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc > 3) {
            usage(stderr);
            return 1;
        }
        set_current(argc == 3 ? argv[2] : NULL);
        return 0;
    }
    if (strcmp(argv[1], "purge") == 0) {
        if (argc != 2) {
            usage(stderr);
            return 1;
        }
        purge();
        return 0;
    }
    if (strcmp(argv[1], "status") == 0) {
        if (argc != 2) {
            usage(stderr);
            return 1;
        }
        status();
        return 0;
    }
    if (strcmp(argv[1], "install") == 0) {
        if (argc != 2) {
            usage(stderr);
            return 1;
        }
        mkdir_p_jenv();
        install_hook();
        printf("Installed jenv hook in ~/.bashrc.\n");
        return 0;
    }
    if (strcmp(argv[1], "uninstall") == 0) {
        if (argc != 2) {
            usage(stderr);
            return 1;
        }
        uninstall_hook();
        printf("Removed jenv hook from ~/.bashrc.\n");
        return 0;
    }

    usage(stderr);
    return 1;
}
