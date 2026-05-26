#include <ctype.h>
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
#define ENV_REL ".config/jenv/env"
#define CONFIG_DIR_REL ".config/jenv"
#define BASHRC_REL ".bashrc"
#define HOOK_START "# >>> jenv hook >>>\n"
#define HOOK_END "# <<< jenv hook <<<\n"

extern char **environ;

enum env_type {
    ENV_TYPE_CONDA,
    ENV_TYPE_UV
};

enum capture_mode {
    CAPTURE_NONE,
    CAPTURE_ALLOWLIST,
    CAPTURE_ALL
};

struct jenv_config {
    char *dir;
    enum env_type env_type;
    char *conda_env;
    char *uv_venv;
};

struct env_entry {
    char *name;
    char *value;
};

struct env_file {
    struct env_entry *entries;
    size_t count;
};

static const char *HOOK_BLOCK =
    "# >>> jenv hook >>>\n"
    "if [[ $- == *i* ]]; then\n"
    "    if [[ -f \"$HOME/.config/jenv/config\" ]]; then\n"
    "        source \"$HOME/.config/jenv/config\"\n"
    "        if [[ -f \"$HOME/.config/jenv/env\" ]]; then\n"
    "            source \"$HOME/.config/jenv/env\"\n"
    "        fi\n"
    "        if [[ -n \"$JENV_DIR\" && -d \"$JENV_DIR\" ]]; then\n"
    "            cd \"$JENV_DIR\" >/dev/null 2>&1 || true\n"
    "        fi\n"
    "        if [[ \"$JENV_ENV_TYPE\" == \"conda\" && -n \"$JENV_CONDA_ENV\" ]]; then\n"
    "            if command -v conda >/dev/null 2>&1; then\n"
    "                eval \"$(conda shell.bash hook)\" >/dev/null 2>&1\n"
    "            elif [[ -f \"$HOME/miniconda3/etc/profile.d/conda.sh\" ]]; then\n"
    "                source \"$HOME/miniconda3/etc/profile.d/conda.sh\" >/dev/null 2>&1\n"
    "            elif [[ -f \"$HOME/anaconda3/etc/profile.d/conda.sh\" ]]; then\n"
    "                source \"$HOME/anaconda3/etc/profile.d/conda.sh\" >/dev/null 2>&1\n"
    "            fi\n"
    "            conda activate \"$JENV_CONDA_ENV\" >/dev/null 2>&1 || true\n"
    "        elif [[ \"$JENV_ENV_TYPE\" == \"uv\" && -n \"$JENV_UV_VENV\" && -f \"$JENV_UV_VENV/bin/activate\" ]]; then\n"
    "            source \"$JENV_UV_VENV/bin/activate\" >/dev/null 2>&1 || true\n"
    "        fi\n"
    "    fi\n"
    "fi\n"
    "# <<< jenv hook <<<\n";

static const char *ALLOWLIST[] = {
    "LD_LIBRARY_PATH", "LIBRARY_PATH",   "CPATH",              "C_INCLUDE_PATH",
    "CPLUS_INCLUDE_PATH", "CUDA_HOME",   "CUDA_PATH",          "CUDNN_HOME",
    "NCCL_HOME",          "PYTHONPATH",  "PKG_CONFIG_PATH",    "WANDB_PROJECT",
    "WANDB_ENTITY",       "HF_HOME",     "HF_DATASETS_CACHE",  "TRANSFORMERS_CACHE",
    "TORCH_HOME",         "OMP_NUM_THREADS", "MKL_NUM_THREADS", "OPENBLAS_NUM_THREADS"
};

static const char *BLACKLIST[] = {
    "_",                  "PWD",                  "OLDPWD",              "SHLVL",
    "SHELL",              "HOME",                 "USER",                "LOGNAME",
    "HOSTNAME",           "TERM",                 "TERMCAP",             "COLUMNS",
    "LINES",              "RANDOM",               "SECONDS",             "UID",
    "EUID",               "PPID",                 "BASHPID",             "BASHOPTS",
    "BASH_VERSINFO",      "BASH_VERSION",         "HISTFILE",            "HISTSIZE",
    "HISTCONTROL",        "HISTFILESIZE",         "PROMPT_COMMAND",      "PS1",
    "PS2",                "PS4",                  "SSH_AUTH_SOCK",       "SSH_AGENT_PID",
    "SSH_CLIENT",         "SSH_CONNECTION",       "SSH_TTY",             "DISPLAY",
    "WAYLAND_DISPLAY",    "XAUTHORITY",           "XDG_SESSION_ID",      "XDG_RUNTIME_DIR",
    "DBUS_SESSION_BUS_ADDRESS", "CONDA_PREFIX",   "CONDA_PREFIX_1",      "CONDA_PREFIX_2",
    "CONDA_SHLVL",        "CONDA_PROMPT_MODIFIER","CONDA_EXE",           "_CE_CONDA",
    "_CE_M",              "VIRTUAL_ENV",          "VIRTUAL_ENV_PROMPT"
};

static const char *SECRET_WORDS[] = {
    "TOKEN", "SECRET", "PASSWORD", "PASS", "KEY", "CREDENTIAL", "COOKIE"
};

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

static char *xstrdup(const char *s) {
    char *copy;

    if (s == NULL) {
        return NULL;
    }
    copy = strdup(s);
    if (copy == NULL) {
        die("out of memory");
    }
    return copy;
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

static char *path_join_two(const char *left, const char *right) {
    size_t len = strlen(left) + 1 + strlen(right) + 1;
    char *path = malloc(len);

    if (path == NULL) {
        die("out of memory");
    }
    snprintf(path, len, "%s/%s", left, right);
    return path;
}

static bool path_exists(const char *path) {
    return access(path, F_OK) == 0;
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

static bool valid_var_name(const char *name) {
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        return false;
    }
    for (i = 1; name[i] != '\0'; ++i) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool string_in_list(const char *value, const char *const *list, size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (strcmp(value, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool looks_secret_name(const char *name) {
    size_t len = strlen(name);
    char *upper = malloc(len + 1);
    size_t i;
    bool secret = false;

    if (upper == NULL) {
        die("out of memory");
    }
    for (i = 0; i < len; ++i) {
        upper[i] = (char)toupper((unsigned char)name[i]);
    }
    upper[len] = '\0';
    for (i = 0; i < sizeof(SECRET_WORDS) / sizeof(SECRET_WORDS[0]); ++i) {
        if (strstr(upper, SECRET_WORDS[i]) != NULL) {
            secret = true;
            break;
        }
    }
    free(upper);
    return secret;
}

static bool is_blacklisted_name(const char *name) {
    return string_in_list(name, BLACKLIST, sizeof(BLACKLIST) / sizeof(BLACKLIST[0]));
}

static bool is_allowlisted_name(const char *name) {
    return string_in_list(name, ALLOWLIST, sizeof(ALLOWLIST) / sizeof(ALLOWLIST[0]));
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

static void free_config(struct jenv_config *config) {
    free(config->dir);
    free(config->conda_env);
    free(config->uv_venv);
    memset(config, 0, sizeof(*config));
}

static void free_env_file(struct env_file *env_file) {
    size_t i;

    for (i = 0; i < env_file->count; ++i) {
        free(env_file->entries[i].name);
        free(env_file->entries[i].value);
    }
    free(env_file->entries);
    env_file->entries = NULL;
    env_file->count = 0;
}

static void add_env_entry(struct env_file *env_file, const char *name, const char *value) {
    struct env_entry *resized;

    resized = realloc(env_file->entries, (env_file->count + 1) * sizeof(env_file->entries[0]));
    if (resized == NULL) {
        die("out of memory");
    }
    env_file->entries = resized;
    env_file->entries[env_file->count].name = xstrdup(name);
    env_file->entries[env_file->count].value = xstrdup(value);
    env_file->count += 1;
}

static ssize_t find_env_entry_index(const struct env_file *env_file, const char *name) {
    size_t i;

    for (i = 0; i < env_file->count; ++i) {
        if (strcmp(env_file->entries[i].name, name) == 0) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static void set_env_entry(struct env_file *env_file, const char *name, const char *value) {
    ssize_t index = find_env_entry_index(env_file, name);

    if (index >= 0) {
        char *copy = xstrdup(value);
        free(env_file->entries[index].value);
        env_file->entries[index].value = copy;
        return;
    }
    add_env_entry(env_file, name, value);
}

static void remove_env_entry(struct env_file *env_file, size_t index) {
    free(env_file->entries[index].name);
    free(env_file->entries[index].value);
    for (; index + 1 < env_file->count; ++index) {
        env_file->entries[index] = env_file->entries[index + 1];
    }
    env_file->count -= 1;
    if (env_file->count == 0) {
        free(env_file->entries);
        env_file->entries = NULL;
    }
}

static struct jenv_config read_config_file(bool required) {
    struct jenv_config config;
    char *path = path_join_home(CONFIG_REL);
    char *content = read_file(path, NULL);
    char *line;
    char *saveptr = NULL;

    memset(&config, 0, sizeof(config));
    config.env_type = ENV_TYPE_CONDA;
    if (content == NULL) {
        free(path);
        if (required) {
            die("No startup directory or environment is pinned.");
        }
        return config;
    }

    for (line = strtok_r(content, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
        char *value;

        if (config.dir == NULL) {
            value = parse_single_quoted_value(line, "export JENV_DIR=");
            if (value != NULL) {
                config.dir = value;
                continue;
            }
        }
        value = parse_single_quoted_value(line, "export JENV_ENV_TYPE=");
        if (value != NULL) {
            if (strcmp(value, "uv") == 0) {
                config.env_type = ENV_TYPE_UV;
            } else if (strcmp(value, "conda") != 0) {
                free(value);
                free(content);
                free(path);
                free_config(&config);
                die("config file %s has invalid JENV_ENV_TYPE", path);
            }
            free(value);
            continue;
        }
        if (config.conda_env == NULL) {
            value = parse_single_quoted_value(line, "export JENV_CONDA_ENV=");
            if (value != NULL) {
                config.conda_env = value;
                continue;
            }
        }
        if (config.uv_venv == NULL) {
            value = parse_single_quoted_value(line, "export JENV_UV_VENV=");
            if (value != NULL) {
                config.uv_venv = value;
                continue;
            }
        }
    }

    free(content);
    free(path);
    if (config.dir == NULL) {
        free_config(&config);
        die("jenv config is malformed");
    }
    return config;
}

static struct env_file read_env_file(void) {
    struct env_file env_file;
    char *path = path_join_home(ENV_REL);
    char *content = read_file(path, NULL);
    char *line;
    char *saveptr = NULL;

    env_file.entries = NULL;
    env_file.count = 0;
    free(path);
    if (content == NULL) {
        return env_file;
    }

    for (line = strtok_r(content, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
        const char *prefix = "export ";
        char *equal;
        char *name;
        char *value;

        if (strncmp(line, prefix, strlen(prefix)) != 0) {
            free(content);
            free_env_file(&env_file);
            die("jenv env file is malformed");
        }
        equal = strchr(line + strlen(prefix), '=');
        if (equal == NULL) {
            free(content);
            free_env_file(&env_file);
            die("jenv env file is malformed");
        }
        *equal = '\0';
        name = line + strlen(prefix);
        if (!valid_var_name(name)) {
            free(content);
            free_env_file(&env_file);
            die("jenv env file contains invalid variable name");
        }
        name = xstrdup(name);
        *equal = '=';
        value = parse_single_quoted_value(equal + 1, "");
        if (value == NULL) {
            free(name);
            free(content);
            free_env_file(&env_file);
            die("jenv env file is malformed");
        }
        add_env_entry(&env_file, name, value);
        free(name);
        free(value);
    }

    free(content);
    return env_file;
}

static void write_config_file(const struct jenv_config *config) {
    char *path = path_join_home(CONFIG_REL);
    char *dir_escaped = shell_escape_single_quote(config->dir);
    char *type_escaped = shell_escape_single_quote(config->env_type == ENV_TYPE_UV ? "uv" : "conda");
    char *conda_escaped = shell_escape_single_quote(config->conda_env != NULL ? config->conda_env : "");
    char *uv_escaped = shell_escape_single_quote(config->uv_venv != NULL ? config->uv_venv : "");
    size_t len = strlen(dir_escaped) + strlen(type_escaped) + strlen(conda_escaped) + strlen(uv_escaped) +
                 strlen("export JENV_DIR=\nexport JENV_ENV_TYPE=\nexport JENV_CONDA_ENV=\nexport JENV_UV_VENV=\n") + 1;
    char *content = malloc(len);

    if (content == NULL) {
        die("out of memory");
    }
    snprintf(content, len,
             "export JENV_DIR=%s\n"
             "export JENV_ENV_TYPE=%s\n"
             "export JENV_CONDA_ENV=%s\n"
             "export JENV_UV_VENV=%s\n",
             dir_escaped, type_escaped, conda_escaped, uv_escaped);
    write_file_atomic(path, content);
    free(content);
    free(path);
    free(dir_escaped);
    free(type_escaped);
    free(conda_escaped);
    free(uv_escaped);
}

static void write_env_file(const struct env_file *env_file) {
    char *path = path_join_home(ENV_REL);
    size_t total = 1;
    size_t i;
    char *content;
    char *cursor;

    if (env_file->count == 0) {
        if (unlink(path) != 0 && errno != ENOENT) {
            free(path);
            die("failed to remove %s: %s", path, strerror(errno));
        }
        free(path);
        return;
    }

    for (i = 0; i < env_file->count; ++i) {
        char *escaped = shell_escape_single_quote(env_file->entries[i].value);
        total += strlen("export =\n") + strlen(env_file->entries[i].name) + strlen(escaped);
        free(escaped);
    }
    content = malloc(total);
    if (content == NULL) {
        free(path);
        die("out of memory");
    }
    cursor = content;
    cursor[0] = '\0';
    for (i = 0; i < env_file->count; ++i) {
        char *escaped = shell_escape_single_quote(env_file->entries[i].value);
        int written = snprintf(cursor, total - (size_t)(cursor - content), "export %s=%s\n",
                               env_file->entries[i].name, escaped);
        free(escaped);
        if (written < 0) {
            free(content);
            free(path);
            die("failed to render env file");
        }
        cursor += written;
    }
    write_file_atomic(path, content);
    free(content);
    free(path);
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

static const char *current_directory(void) {
    const char *pwd = getenv("PWD");
    static char cwd[PATH_MAX];

    if (pwd != NULL && pwd[0] != '\0') {
        return pwd;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        die("failed to determine current directory: %s", strerror(errno));
    }
    return cwd;
}

static char *detect_uv_venv(const char *dir) {
    const char *virtual_env = getenv("VIRTUAL_ENV");
    char *activate_path;
    char *venv_path;

    if (virtual_env != NULL && virtual_env[0] != '\0') {
        return xstrdup(virtual_env);
    }
    venv_path = path_join_two(dir, ".venv");
    activate_path = path_join_two(venv_path, "bin/activate");
    if (path_exists(activate_path)) {
        free(activate_path);
        return venv_path;
    }
    free(activate_path);
    free(venv_path);
    die("Could not detect a uv virtual environment. Run `uv venv` first or activate the venv.");
    return NULL;
}

static struct env_file capture_env_file(enum capture_mode mode) {
    struct env_file env_file;
    char **entry;

    env_file.entries = NULL;
    env_file.count = 0;
    if (mode == CAPTURE_NONE) {
        return env_file;
    }

    for (entry = environ; *entry != NULL; ++entry) {
        char *equal = strchr(*entry, '=');
        size_t name_len;
        char *name;
        const char *value;

        if (equal == NULL) {
            continue;
        }
        name_len = (size_t)(equal - *entry);
        name = malloc(name_len + 1);
        if (name == NULL) {
            free_env_file(&env_file);
            die("out of memory");
        }
        memcpy(name, *entry, name_len);
        name[name_len] = '\0';
        value = equal + 1;
        if (!valid_var_name(name)) {
            free(name);
            continue;
        }
        if (mode == CAPTURE_ALLOWLIST && !is_allowlisted_name(name)) {
            free(name);
            continue;
        }
        if (mode == CAPTURE_ALL && (is_blacklisted_name(name) || looks_secret_name(name))) {
            free(name);
            continue;
        }
        set_env_entry(&env_file, name, value);
        free(name);
    }

    return env_file;
}

static void print_env_entries_names(const struct env_file *env_file) {
    size_t i;

    if (env_file->count == 0) {
        printf("Captured env vars: none\n");
        return;
    }
    printf("Captured env vars (%zu):\n", env_file->count);
    for (i = 0; i < env_file->count; ++i) {
        printf("  %s\n", env_file->entries[i].name);
    }
}

static void save_set(enum env_type env_type, const char *conda_env_override, enum capture_mode capture_mode) {
    struct jenv_config config;
    struct env_file env_file;
    const char *dir = current_directory();

    memset(&config, 0, sizeof(config));
    config.dir = xstrdup(dir);
    config.env_type = env_type;
    if (env_type == ENV_TYPE_CONDA) {
        const char *env_name = conda_env_override;
        if (env_name == NULL || env_name[0] == '\0') {
            env_name = getenv("CONDA_DEFAULT_ENV");
        }
        if (env_name == NULL || env_name[0] == '\0') {
            env_name = "base";
        }
        config.conda_env = xstrdup(env_name);
        config.uv_venv = xstrdup("");
    } else {
        config.conda_env = xstrdup("");
        config.uv_venv = detect_uv_venv(dir);
    }

    mkdir_p_jenv();
    install_hook();
    write_config_file(&config);

    if (capture_mode == CAPTURE_NONE) {
        char *env_path = path_join_home(ENV_REL);
        if (unlink(env_path) != 0 && errno != ENOENT) {
            free(env_path);
            free_config(&config);
            die("failed to remove %s: %s", env_path, strerror(errno));
        }
        free(env_path);
    } else {
        env_file = capture_env_file(capture_mode);
        write_env_file(&env_file);
        free_env_file(&env_file);
    }

    printf("Pinned directory: %s\n", config.dir);
    printf("Env type: %s\n", config.env_type == ENV_TYPE_UV ? "uv" : "conda");
    if (config.env_type == ENV_TYPE_UV) {
        printf("uv venv: %s\n", config.uv_venv);
    } else {
        printf("Conda env: %s\n", config.conda_env);
    }
    free_config(&config);
}

static void purge(void) {
    char *config_path = path_join_home(CONFIG_REL);
    char *env_path = path_join_home(ENV_REL);

    if (unlink(config_path) != 0 && errno != ENOENT) {
        free(config_path);
        free(env_path);
        die("failed to remove %s: %s", config_path, strerror(errno));
    }
    if (unlink(env_path) != 0 && errno != ENOENT) {
        free(config_path);
        free(env_path);
        die("failed to remove %s: %s", env_path, strerror(errno));
    }
    printf("Cleared jenv startup config and captured env vars.\n");
    free(config_path);
    free(env_path);
}

static void status(void) {
    struct jenv_config config;
    struct env_file env_file;

    config = read_config_file(false);
    env_file = read_env_file();
    if (config.dir == NULL) {
        printf("No startup directory or environment is pinned.\n");
        if (env_file.count > 0) {
            print_env_entries_names(&env_file);
        }
        free_env_file(&env_file);
        return;
    }

    printf("Pinned directory: %s\n", config.dir);
    printf("Env type: %s\n", config.env_type == ENV_TYPE_UV ? "uv" : "conda");
    if (config.env_type == ENV_TYPE_UV) {
        printf("uv venv: %s\n", config.uv_venv != NULL ? config.uv_venv : "");
    } else {
        printf("Conda env: %s\n", config.conda_env != NULL ? config.conda_env : "");
    }
    print_env_entries_names(&env_file);
    free_env_file(&env_file);
    free_config(&config);
}

static void env_add(const char *name) {
    const char *value;
    struct env_file env_file;

    if (!valid_var_name(name)) {
        die("Invalid environment variable name: %s", name);
    }
    if (is_blacklisted_name(name)) {
        die("Refusing to capture blacklisted environment variable: %s", name);
    }
    if (looks_secret_name(name)) {
        die("Refusing to capture secret-looking environment variable: %s", name);
    }
    value = getenv(name);
    if (value == NULL) {
        die("Environment variable %s is not set", name);
    }

    mkdir_p_jenv();
    env_file = read_env_file();
    set_env_entry(&env_file, name, value);
    write_env_file(&env_file);
    free_env_file(&env_file);
    printf("Captured env var: %s\n", name);
}

static void env_remove_cmd(const char *name) {
    struct env_file env_file;
    ssize_t index;

    if (!valid_var_name(name)) {
        die("Invalid environment variable name: %s", name);
    }
    env_file = read_env_file();
    index = find_env_entry_index(&env_file, name);
    if (index < 0) {
        free_env_file(&env_file);
        printf("Env var not captured: %s\n", name);
        return;
    }
    remove_env_entry(&env_file, (size_t)index);
    write_env_file(&env_file);
    free_env_file(&env_file);
    printf("Removed env var: %s\n", name);
}

static void env_list_cmd(void) {
    struct env_file env_file = read_env_file();
    size_t i;

    if (env_file.count == 0) {
        printf("No env vars captured.\n");
        free_env_file(&env_file);
        return;
    }
    for (i = 0; i < env_file.count; ++i) {
        printf("%s=%s\n", env_file.entries[i].name, env_file.entries[i].value);
    }
    free_env_file(&env_file);
}

static void env_clear_cmd(void) {
    char *env_path = path_join_home(ENV_REL);

    if (unlink(env_path) != 0 && errno != ENOENT) {
        free(env_path);
        die("failed to remove %s: %s", env_path, strerror(errno));
    }
    free(env_path);
    printf("Cleared captured env vars.\n");
}

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  jenv set [ENV_NAME]\n"
            "  jenv set --conda [ENV_NAME] [--env|--env-all]\n"
            "  jenv set --uv [--env|--env-all]\n"
            "  jenv env add NAME\n"
            "  jenv env remove NAME\n"
            "  jenv env list\n"
            "  jenv env clear\n"
            "  jenv purge\n"
            "  jenv status\n"
            "  jenv install\n"
            "  jenv uninstall\n");
}

static void command_set(int argc, char **argv) {
    enum env_type env_type = ENV_TYPE_CONDA;
    enum capture_mode capture_mode = CAPTURE_NONE;
    const char *conda_env_name = NULL;
    bool explicit_manager = false;
    int i;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--uv") == 0) {
            if (explicit_manager && env_type != ENV_TYPE_UV) {
                die("Choose either --uv or --conda, not both");
            }
            env_type = ENV_TYPE_UV;
            explicit_manager = true;
            continue;
        }
        if (strcmp(argv[i], "--conda") == 0) {
            if (explicit_manager && env_type != ENV_TYPE_CONDA) {
                die("Choose either --uv or --conda, not both");
            }
            env_type = ENV_TYPE_CONDA;
            explicit_manager = true;
            continue;
        }
        if (strcmp(argv[i], "--env") == 0) {
            if (capture_mode == CAPTURE_ALL) {
                die("Choose either --env or --env-all, not both");
            }
            capture_mode = CAPTURE_ALLOWLIST;
            continue;
        }
        if (strcmp(argv[i], "--env-all") == 0) {
            if (capture_mode == CAPTURE_ALLOWLIST) {
                die("Choose either --env or --env-all, not both");
            }
            capture_mode = CAPTURE_ALL;
            continue;
        }
        if (argv[i][0] == '-') {
            die("Unknown option: %s", argv[i]);
        }
        if (env_type == ENV_TYPE_UV) {
            die("jenv set --uv does not accept an environment name");
        }
        if (conda_env_name != NULL) {
            die("Too many arguments for jenv set");
        }
        conda_env_name = argv[i];
    }

    save_set(env_type, conda_env_name, capture_mode);
}

static void command_env(int argc, char **argv) {
    if (argc < 3) {
        usage(stderr);
        exit(1);
    }
    if (strcmp(argv[2], "add") == 0) {
        if (argc != 4) {
            usage(stderr);
            exit(1);
        }
        env_add(argv[3]);
        return;
    }
    if (strcmp(argv[2], "remove") == 0) {
        if (argc != 4) {
            usage(stderr);
            exit(1);
        }
        env_remove_cmd(argv[3]);
        return;
    }
    if (strcmp(argv[2], "list") == 0) {
        if (argc != 3) {
            usage(stderr);
            exit(1);
        }
        env_list_cmd();
        return;
    }
    if (strcmp(argv[2], "clear") == 0) {
        if (argc != 3) {
            usage(stderr);
            exit(1);
        }
        env_clear_cmd();
        return;
    }

    usage(stderr);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(stderr);
        return 1;
    }
    if (strcmp(argv[1], "set") == 0) {
        command_set(argc, argv);
        return 0;
    }
    if (strcmp(argv[1], "env") == 0) {
        command_env(argc, argv);
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
