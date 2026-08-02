#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#define VERSION            "aki-1.0.0"
#define CPUSET_ROOT        "/dev/cpuset"
#define DEFAULT_CPUSET     "AkiAppOpt"
#define BASE_CPUSET_MAX    256
#define MAX_PKG_LEN        128
#define MAX_THREAD_LEN     32
#define INITIAL_RULE_CAPACITY 64
#define DELAY_UNIT_MS      100ULL
#define DELAY_POLL_US      100000

typedef struct {
    char pkg[MAX_PKG_LEN];
    char thread[MAX_THREAD_LEN];
    char cpuset_dir[256];
    cpu_set_t cpus;
    uint64_t delay_ms;
    uint32_t package_priority;
    uint32_t thread_priority;
    bool package_is_pattern;
} AffinityRule;

typedef struct {
    pid_t tid;
    char name[MAX_THREAD_LEN];
    char cpuset_dir[256];
    cpu_set_t cpus;
    uint64_t bind_after_ms;
} ThreadInfo;

typedef struct {
    pid_t pid;
    char pkg[MAX_PKG_LEN];
    char base_cpuset[128];
    cpu_set_t base_cpus;
    uint64_t detected_at_ms;
    uint64_t base_delay_ms;
    ThreadInfo* threads;
    size_t num_threads;
    size_t threads_cap;
    AffinityRule** thread_rules;
    size_t num_thread_rules;
    size_t thread_rules_cap;
} ProcessInfo;

typedef struct {
    pid_t pid;
    char pkg[MAX_PKG_LEN];
    uint64_t detected_at_ms;
} DetectedProcess;

typedef struct {
    cpu_set_t present_cpus;
    cpu_set_t e_core;
    cpu_set_t p_core;
    cpu_set_t hp_core;
    char present_str[128];
    char mems_str[32];
    bool cpuset_enabled;
    int base_cpuset_fd;
} CpuTopology;

typedef struct {
    atomic_int ref_count;
    AffinityRule* rules;
    size_t num_rules;
    struct timespec mtime;
    CpuTopology topo;
    const char** exact_pkg_slots;
    size_t exact_pkg_capacity;
    const char** wildcard_pkgs;
    size_t num_wildcard_pkgs;
    char config_file[4096];
} AppConfig;

typedef struct {
    ProcessInfo* procs;
    size_t num_procs;
    size_t procs_cap;
    int last_proc_count;
    bool scan_all_proc;
    pid_t* tracked_pids;
    size_t num_tracked_pids;
    size_t tracked_pids_cap;
    int last_proc_total;
} ProcCache;

static atomic_int config_updated = ATOMIC_VAR_INIT(0);
static int inotify_fd = -1;
static int inotify_wd = -1;
static int inotify_supported = 0;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static AppConfig* current_config = NULL;
static char base_cpuset[BASE_CPUSET_MAX] = CPUSET_ROOT "/" DEFAULT_CPUSET;

static char* strtrim(char* s) {
    char* end;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
    return s;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
    }
    return (uint64_t)time(NULL) * 1000ULL;
}

static uint64_t delay_deadline(uint64_t detected_at_ms, uint64_t delay_ms) {
    if (delay_ms > UINT64_MAX - detected_at_ms) return UINT64_MAX;
    return detected_at_ms + delay_ms;
}

static bool read_file(int dir_fd, const char* filename, char* buf, size_t buf_size) {
    int fd = openat(dir_fd, filename, O_RDONLY | O_CLOEXEC);
    if (fd == -1) return false;
    ssize_t n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}

static bool write_file(int dir_fd, const char* filename, const char* content, int flags) {
    int fd = openat(dir_fd, filename, flags | O_CLOEXEC, 0644);
    if (fd == -1) return false;
    ssize_t n = write(fd, content, strlen(content));
    close(fd);
    return (n == (ssize_t)strlen(content));
}

static int build_str(char *dest, size_t dest_size, ...) {
    va_list args;
    const char *segment;
    char *p = dest;
    size_t remaining = dest_size - 1;
    va_start(args, dest_size);
    while ((segment = va_arg(args, const char *)) != NULL) {
        size_t len = strlen(segment);
        if (len > remaining) {
            *p = '\0';
            va_end(args);
            return 0;
        }
        memcpy(p, segment, len);
        p += len;
        remaining -= len;
    }
    *p = '\0';
    va_end(args);
    return 1;
}

static bool set_base_cpuset(const char* name) {
    if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return false;
    }
    int len = snprintf(base_cpuset, sizeof(base_cpuset), "%s/%s", CPUSET_ROOT, name);
    return len > 0 && (size_t)len < sizeof(base_cpuset);
}

static bool parse_cpu_ranges(const char* spec, cpu_set_t* set, const cpu_set_t* present) {
    if (!spec) return false;
    const char* s = spec;
    bool parsed = false;

    while (true) {
        while (isspace((unsigned char)*s)) s++;
        if (!isdigit((unsigned char)*s)) return false;

        errno = 0;
        char* end;
        unsigned long a = strtoul(s, &end, 10);
        if (errno == ERANGE || end == s) return false;
        s = end;
        while (isspace((unsigned char)*s)) s++;

        unsigned long b = a;
        if (*s == '-') {
            s++;
            while (isspace((unsigned char)*s)) s++;
            if (!isdigit((unsigned char)*s)) return false;

            errno = 0;
            b = strtoul(s, &end, 10);
            if (errno == ERANGE || end == s) return false;
            s = end;
            while (isspace((unsigned char)*s)) s++;
        }

        if (a > b) { unsigned long t = a; a = b; b = t; }
        for (unsigned long i = a; i <= b && i < CPU_SETSIZE; i++) {
            if (present && !CPU_ISSET(i, present)) continue;
            CPU_SET(i, set);
        }
        parsed = true;

        if (!*s) break;
        if (*s != ',') return false;
        s++;
    }

    return parsed;
}

static bool parse_cpu_list(const char* spec, cpu_set_t* set, const cpu_set_t* present) {
    const char* s = spec;
    bool parsed = false;

    while (s && *s) {
        while (isspace((unsigned char)*s) || *s == ',') s++;
        if (!*s) break;
        if (!isdigit((unsigned char)*s)) return false;

        errno = 0;
        char* end = NULL;
        unsigned long first = strtoul(s, &end, 10);
        if (errno == ERANGE || end == s) return false;
        unsigned long last = first;
        s = end;

        if (*s == '-') {
            s++;
            if (!isdigit((unsigned char)*s)) return false;
            errno = 0;
            last = strtoul(s, &end, 10);
            if (errno == ERANGE || end == s) return false;
            s = end;
        }
        if (*s && !isspace((unsigned char)*s) && *s != ',') return false;
        if (first > last) {
            unsigned long tmp = first;
            first = last;
            last = tmp;
        }
        for (unsigned long cpu = first; cpu <= last && cpu < CPU_SETSIZE; cpu++) {
            if (!present || CPU_ISSET(cpu, present)) CPU_SET(cpu, set);
        }
        parsed = true;
    }
    return parsed;
}

static bool parse_cpu_spec(const char* spec, cpu_set_t* set, const CpuTopology* topo) {
    if (!spec || !*spec) return false;
    char* copy = strdup(spec);
    if (!copy) return false;

    bool parsed = false;
    char* cursor = copy;
    char* part;
    while ((part = strsep(&cursor, ",")) != NULL) {
        part = strtrim(part);
        if (!*part) {
            free(copy);
            return false;
        }

        const cpu_set_t* semantic = NULL;
        if (strcmp(part, "e-core") == 0) semantic = &topo->e_core;
        else if (strcmp(part, "p-core") == 0) semantic = &topo->p_core;
        else if (strcmp(part, "hp-core") == 0) semantic = &topo->hp_core;
        else if (strcmp(part, "all-core") == 0) semantic = &topo->present_cpus;

        if (semantic) {
            CPU_OR(set, set, semantic);
        } else {
            cpu_set_t numeric;
            CPU_ZERO(&numeric);
            if (!parse_cpu_ranges(part, &numeric, &topo->present_cpus)) {
                free(copy);
                return false;
            }
            CPU_OR(set, set, &numeric);
        }
        parsed = true;
    }
    free(copy);
    return parsed;
}

static char* cpu_set_to_str(const cpu_set_t *set) {
    size_t buf_size = 8 * CPU_SETSIZE;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;
    int start = -1, end = -1;
    char *p = buf;
    size_t remain = buf_size - 1;
    bool first = true;

    for (int i = 0; i < CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, set)) {
            if (start == -1) {
                start = end = i;
            } else if (i == end + 1) {
                end = i;
            } else {
                int needed;
                if (start == end) {
                    needed = snprintf(p, remain + 1, "%s%d", first ? "" : ",", start);
                } else {
                    needed = snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
                }
                if (needed < 0 || (size_t)needed > remain) {
                    free(buf);
                    return NULL;
                }
                p += needed;
                remain -= needed;
                start = end = i;
                first = false;
            }
        }
    }
    if (start != -1) {
        int needed;
        if (start == end) {
            needed = snprintf(p, remain + 1, "%s%d", first ? "" : ",", start);
        } else {
            needed = snprintf(p, remain + 1, "%s%d-%d", first ? "" : ",", start, end);
        }
        if (needed < 0 || (size_t)needed > remain) {
            free(buf);
            return NULL;
        }
        p += needed;
    }
    *p = '\0';
    return buf;
}

static bool create_cpuset_dir(const char *path, const char *cpus, const char *mems) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) return false;
    if (chmod(path, 0755) != 0) return false;
    if (chown(path, 0, 0) != 0) return false;

    char cpus_path[256];
    build_str(cpus_path, sizeof(cpus_path), path, "/cpus", NULL);
    if (!write_file(AT_FDCWD, cpus_path, cpus, O_WRONLY | O_CREAT | O_TRUNC)) return false;

    char mems_path[256];
    build_str(mems_path, sizeof(mems_path), path, "/mems", NULL);
    return write_file(AT_FDCWD, mems_path, mems, O_WRONLY | O_CREAT | O_TRUNC);
}

static bool ensure_cpuset_dir(const cpu_set_t* cpus, const CpuTopology* topo,
                              char* dir_name, size_t dir_name_size) {
    if (!topo->cpuset_enabled) {
        dir_name[0] = '\0';
        return true;
    }

    char* name = cpu_set_to_str(cpus);
    if (!name) return false;
    char path[BASE_CPUSET_MAX + 32];
    bool ok = build_str(path, sizeof(path), base_cpuset, "/", name, NULL) &&
              build_str(dir_name, dir_name_size, name, NULL) &&
              create_cpuset_dir(path, name, topo->mems_str);
    if (!ok) dir_name[0] = '\0';
    free(name);
    return ok;
}

typedef struct {
    unsigned long long max_freq;
    cpu_set_t cpus;
} CpuFreqGroup;

static int cpu_freq_group_cmp(const void* lhs, const void* rhs) {
    const CpuFreqGroup* a = lhs;
    const CpuFreqGroup* b = rhs;
    return (a->max_freq > b->max_freq) - (a->max_freq < b->max_freq);
}

static void detect_core_types(CpuTopology* topo) {
    DIR* dir = opendir("/sys/devices/system/cpu/cpufreq");
    if (!dir) return;

    CpuFreqGroup* groups = NULL;
    size_t groups_cnt = 0;
    int root_fd = dirfd(dir);
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "policy", 6) != 0) continue;
        int policy_fd = openat(root_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (policy_fd == -1) continue;

        char freq_buf[32] = {0};
        char cpus_buf[256] = {0};
        bool ok = read_file(policy_fd, "cpuinfo_max_freq", freq_buf, sizeof(freq_buf)) &&
                  read_file(policy_fd, "related_cpus", cpus_buf, sizeof(cpus_buf));
        close(policy_fd);
        if (!ok) continue;

        char* freq_end = NULL;
        errno = 0;
        unsigned long long freq = strtoull(strtrim(freq_buf), &freq_end, 10);
        if (errno == ERANGE || freq == 0 || !freq_end || *strtrim(freq_end)) continue;

        cpu_set_t policy_cpus;
        CPU_ZERO(&policy_cpus);
        if (!parse_cpu_list(cpus_buf, &policy_cpus, &topo->present_cpus) ||
            CPU_COUNT(&policy_cpus) == 0) continue;

        size_t group_idx = groups_cnt;
        for (size_t i = 0; i < groups_cnt; i++) {
            if (groups[i].max_freq == freq) {
                group_idx = i;
                break;
            }
        }
        if (group_idx == groups_cnt) {
            CpuFreqGroup* resized = realloc(groups, (groups_cnt + 1) * sizeof(*groups));
            if (!resized) break;
            groups = resized;
            groups[group_idx].max_freq = freq;
            CPU_ZERO(&groups[group_idx].cpus);
            groups_cnt++;
        }
        CPU_OR(&groups[group_idx].cpus, &groups[group_idx].cpus, &policy_cpus);
    }
    closedir(dir);

    if (groups_cnt > 1) qsort(groups, groups_cnt, sizeof(*groups), cpu_freq_group_cmp);
    for (size_t i = 0; i < groups_cnt; i++) {
        cpu_set_t* target = i == 0 ? &topo->e_core :
                            i == groups_cnt - 1 ? &topo->hp_core : &topo->p_core;
        CPU_OR(target, target, &groups[i].cpus);
    }
    free(groups);
}

static CpuTopology init_cpu_topo(void) {
    CpuTopology topo = { .cpuset_enabled = false, .base_cpuset_fd = -1 };
    CPU_ZERO(&topo.present_cpus);
    CPU_ZERO(&topo.e_core);
    CPU_ZERO(&topo.p_core);
    CPU_ZERO(&topo.hp_core);

    if (read_file(AT_FDCWD, "/sys/devices/system/cpu/present", topo.present_str, sizeof(topo.present_str))) {
        strtrim(topo.present_str);
    }
    parse_cpu_ranges(topo.present_str, &topo.present_cpus, NULL);
    detect_core_types(&topo);

    if (access("/dev/cpuset", F_OK) != 0) return topo;

    if (create_cpuset_dir(base_cpuset, topo.present_str, "0")) {
        topo.base_cpuset_fd = open(base_cpuset, O_RDONLY | O_DIRECTORY);
        if (topo.base_cpuset_fd != -1) topo.cpuset_enabled = true;
    }

    char mems_path[256];
    build_str(mems_path, sizeof(mems_path), base_cpuset, "/mems", NULL);
    if (!read_file(AT_FDCWD, mems_path, topo.mems_str, sizeof(topo.mems_str))) {
        build_str(topo.mems_str, sizeof(topo.mems_str), "0", NULL);
    } else {
        strtrim(topo.mems_str);
    }

    return topo;
}

typedef enum {
    RULE_ADD_ADDED,
    RULE_ADD_INVALID,
    RULE_ADD_NO_MEMORY
} RuleAddResult;

static uint32_t calculate_pattern_priority(const char* pattern) {
    if (!pattern || !*pattern) return 0;

    uint32_t literal_count = 0;
    bool has_range = false;
    bool has_question = false;
    bool has_star = false;
    for (const unsigned char* cursor = (const unsigned char*)pattern; *cursor; cursor++) {
        if (*cursor == '[') has_range = true;
        else if (*cursor == '?') has_question = true;
        else if (*cursor == '*') has_star = true;
        else literal_count++;
    }

    uint32_t pattern_class = !has_range && !has_question && !has_star ? 4U :
                             has_range ? 3U : has_question ? 2U : 1U;
    return (pattern_class << 16) | literal_count;
}

static bool pattern_has_wildcards(const char* pattern) {
    return pattern && strpbrk(pattern, "*?[") != NULL;
}

static bool rule_is_more_specific(const AffinityRule* candidate,
                                  const AffinityRule* selected,
                                  bool compare_thread) {
    if (!selected) return true;
    if (compare_thread && candidate->thread_priority != selected->thread_priority) {
        return candidate->thread_priority > selected->thread_priority;
    }
    return candidate->package_priority > selected->package_priority;
}

static RuleAddResult add_rule(AffinityRule** rules, size_t* rules_cnt,
                              size_t* rules_capacity,
                              const CpuTopology* topo, const char* pkg,
                              const char* thread, const char* cpus_spec,
                              uint64_t delay_ms) {
    if (!*pkg || strlen(pkg) >= MAX_PKG_LEN || strlen(thread) >= MAX_THREAD_LEN) {
        return RULE_ADD_INVALID;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    if (!parse_cpu_spec(cpus_spec, &set, topo) || CPU_COUNT(&set) == 0) {
        return RULE_ADD_INVALID;
    }

    AffinityRule rule = {0};
    if (!build_str(rule.pkg, sizeof(rule.pkg), pkg, NULL) ||
        !build_str(rule.thread, sizeof(rule.thread), thread, NULL)) {
        return RULE_ADD_INVALID;
    }

    if (!*thread)
        ensure_cpuset_dir(&set, topo, rule.cpuset_dir, sizeof(rule.cpuset_dir));

    rule.cpus = set;
    rule.delay_ms = delay_ms;
    rule.package_priority = calculate_pattern_priority(rule.pkg);
    rule.thread_priority = calculate_pattern_priority(rule.thread);
    rule.package_is_pattern = pattern_has_wildcards(rule.pkg);

    if (*rules_cnt >= *rules_capacity) {
        size_t new_capacity = *rules_capacity ? *rules_capacity : INITIAL_RULE_CAPACITY;
        if (*rules_capacity) {
            if (*rules_capacity > SIZE_MAX / 2) return RULE_ADD_NO_MEMORY;
            new_capacity = *rules_capacity * 2;
        }
        if (new_capacity > SIZE_MAX / sizeof(**rules)) return RULE_ADD_NO_MEMORY;
        AffinityRule* resized = realloc(*rules, new_capacity * sizeof(**rules));
        if (!resized) return RULE_ADD_NO_MEMORY;
        *rules = resized;
        *rules_capacity = new_capacity;
    }
    memcpy(&(*rules)[*rules_cnt], &rule, sizeof(AffinityRule));
    (*rules_cnt)++;
    return RULE_ADD_ADDED;
}

static uint64_t hash_package_name(const char* package_name) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char* cursor = (const unsigned char*)package_name;
         *cursor; cursor++) {
        hash ^= *cursor;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool build_package_index(const AffinityRule* rules, size_t rules_cnt,
                                const char*** out_exact_slots, size_t* out_exact_capacity,
                                const char*** out_wildcard_pkgs,
                                size_t* out_wildcard_count) {
    size_t exact_rule_count = 0;
    size_t wildcard_rule_count = 0;
    for (size_t i = 0; i < rules_cnt; i++) {
        if (rules[i].package_is_pattern) wildcard_rule_count++;
        else exact_rule_count++;
    }

    size_t exact_capacity = 0;
    const char** exact_slots = NULL;
    if (exact_rule_count > 0) {
        if (exact_rule_count > SIZE_MAX / 2) return false;
        size_t required_capacity = exact_rule_count * 2;
        exact_capacity = 16;
        while (exact_capacity < required_capacity) {
            if (exact_capacity > SIZE_MAX / 2) return false;
            exact_capacity *= 2;
        }
        exact_slots = calloc(exact_capacity, sizeof(*exact_slots));
        if (!exact_slots) return false;
    }

    const char** wildcard_pkgs = NULL;
    if (wildcard_rule_count > 0) {
        if (wildcard_rule_count > SIZE_MAX / sizeof(*wildcard_pkgs)) {
            free(exact_slots);
            return false;
        }
        wildcard_pkgs = malloc(wildcard_rule_count * sizeof(*wildcard_pkgs));
        if (!wildcard_pkgs) {
            free(exact_slots);
            return false;
        }
    }

    size_t wildcard_count = 0;
    for (size_t i = 0; i < rules_cnt; i++) {
        const AffinityRule* rule = &rules[i];
        if (rule->package_is_pattern) {
            bool duplicate = false;
            for (size_t j = 0; j < wildcard_count; j++) {
                if (strcmp(wildcard_pkgs[j], rule->pkg) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) wildcard_pkgs[wildcard_count++] = rule->pkg;
            continue;
        }

        size_t slot = (size_t)hash_package_name(rule->pkg) & (exact_capacity - 1);
        while (exact_slots[slot] && strcmp(exact_slots[slot], rule->pkg) != 0) {
            slot = (slot + 1) & (exact_capacity - 1);
        }
        if (!exact_slots[slot]) exact_slots[slot] = rule->pkg;
    }

    *out_exact_slots = exact_slots;
    *out_exact_capacity = exact_capacity;
    *out_wildcard_pkgs = wildcard_pkgs;
    *out_wildcard_count = wildcard_count;
    return true;
}

static bool package_index_matches(const AppConfig* cfg, const char* package_name) {
    if (cfg->exact_pkg_capacity > 0) {
        size_t slot = (size_t)hash_package_name(package_name) &
                      (cfg->exact_pkg_capacity - 1);
        while (cfg->exact_pkg_slots[slot]) {
            if (strcmp(cfg->exact_pkg_slots[slot], package_name) == 0) return true;
            slot = (slot + 1) & (cfg->exact_pkg_capacity - 1);
        }
    }

    for (size_t i = 0; i < cfg->num_wildcard_pkgs; i++) {
        if (fnmatch(cfg->wildcard_pkgs[i], package_name, FNM_NOESCAPE) == 0) return true;
    }
    return false;
}

static bool rule_matches_package(const AffinityRule* rule, const char* package_name) {
    if (rule->package_is_pattern) {
        return fnmatch(rule->pkg, package_name, FNM_NOESCAPE) == 0;
    }
    return strcmp(rule->pkg, package_name) == 0;
}

static bool comment_or_empty(char* text) {
    text = strtrim(text);
    return !*text || *text == '#' || (text[0] == '/' && text[1] == '/');
}

static void strip_cpu_comment(char* cpus) {
    char* comment = strchr(cpus, '#');
    char* slash_comment = strstr(cpus, "//");
    if (!comment || (slash_comment && slash_comment < comment)) comment = slash_comment;
    if (comment) *comment = '\0';
}

static bool parse_delay_suffix(char* suffix, uint64_t* delay_ms) {
    suffix = strtrim(suffix);
    *delay_ms = 0;
    if (!*suffix) return true;
    if (*suffix != ':') return false;
    char* delay = strtrim(suffix + 1);
    if (!isdigit((unsigned char)*delay)) return false;
    errno = 0;
    char* end = NULL;
    unsigned long long units = strtoull(delay, &end, 10);
    end = strtrim(end);
    if (errno == ERANGE || *end || units > UINT64_MAX / DELAY_UNIT_MS) return false;
    *delay_ms = (uint64_t)units * DELAY_UNIT_MS;
    return true;
}

static bool append_rule(AffinityRule** rules, size_t* rules_cnt, size_t* rules_capacity,
                        const CpuTopology* topo,
                        char* pkg, char* thread, char* cpus, char* delay_suffix,
                        size_t* fail_cnt) {
    strip_cpu_comment(cpus);
    uint64_t delay_ms = 0;
    if (!parse_delay_suffix(delay_suffix, &delay_ms)) {
        (*fail_cnt)++;
        return true;
    }
    RuleAddResult result = add_rule(rules, rules_cnt, rules_capacity, topo, strtrim(pkg),
                                    strtrim(thread), strtrim(cpus), delay_ms);
    if (result == RULE_ADD_NO_MEMORY) return false;
    if (result == RULE_ADD_INVALID) (*fail_cnt)++;
    return true;
}

static AppConfig* load_config(const char* config_file, const CpuTopology* topo,
                              struct timespec* last_mtime) {
    struct stat st;
    if (stat(config_file, &st)) return NULL;
    AppConfig* cfg = calloc(1, sizeof(AppConfig));
    if (!cfg) return NULL;
    cfg->ref_count = 1;
    cfg->topo = *topo;
    build_str(cfg->config_file, sizeof(cfg->config_file), config_file, NULL);

    if (last_mtime && last_mtime->tv_sec == st.st_mtim.tv_sec &&
        last_mtime->tv_nsec == st.st_mtim.tv_nsec) {
        free(cfg);
        return NULL;
    }

    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        free(cfg);
        return NULL;
    }

    size_t rules_capacity = INITIAL_RULE_CAPACITY;
    AffinityRule* new_rules = malloc(rules_capacity * sizeof(*new_rules));
    if (!new_rules) {
        fclose(fp);
        free(cfg);
        return NULL;
    }
    const char** exact_pkg_slots = NULL;
    size_t exact_pkg_capacity = 0;
    const char** wildcard_pkgs = NULL;
    size_t wildcard_pkg_count = 0;
    size_t rules_cnt = 0, fail_cnt = 0;
    char line[256];
    char block_pkg[MAX_PKG_LEN] = {0};
    bool in_block = false;

    while (fgets(line, sizeof(line), fp)) {
        char* p = strtrim(line);
        if (!*p || *p == '#' || (p[0] == '/' && p[1] == '/')) continue;

        if (in_block) {
            char* close = strchr(p, '}');
            if (close) {
                *close = '\0';
                if (!comment_or_empty(close + 1)) fail_cnt++;
            }

            char* content = strtrim(p);
            if (*content) {
                char* eq = strchr(content, '=');
                if (!eq) {
                    fail_cnt++;
                } else {
                    *eq++ = '\0';
                    char* thread = strtrim(content);
                    char* delay_suffix = thread + strlen(thread);
                    char delay_text[32] = {0};
                    char* colon = strrchr(thread, ':');
                    if (colon && isdigit((unsigned char)colon[1])) {
                        if (!build_str(delay_text, sizeof(delay_text), ":", colon + 1, NULL))
                            goto error;
                        *colon = '\0';
                        delay_suffix = delay_text;
                    }
                    if (!append_rule(&new_rules, &rules_cnt, &rules_capacity, &cfg->topo,
                                     block_pkg,
                                     thread, eq, delay_suffix, &fail_cnt)) goto error;
                }
            }
            if (close) {
                in_block = false;
                block_pkg[0] = '\0';
            }
            continue;
        }

        char* br = strchr(p, '{');
        char* eb = br ? strchr(br + 1, '}') : NULL;
        if (br && !eb) {
            *br = '\0';
            if (!comment_or_empty(br + 1)) {
                fail_cnt++;
                continue;
            }

            char* prefix = strtrim(p);
            char* eq = strchr(prefix, '=');
            if (eq) {
                *eq++ = '\0';
                if (!build_str(block_pkg, sizeof(block_pkg), strtrim(prefix), NULL)) {
                    fail_cnt++;
                    continue;
                }
                if (!append_rule(&new_rules, &rules_cnt, &rules_capacity, &cfg->topo,
                                 block_pkg,
                                 "", eq, "", &fail_cnt)) goto error;
            } else if (!build_str(block_pkg, sizeof(block_pkg), prefix, NULL)) {
                fail_cnt++;
                continue;
            }
            if (!*block_pkg) {
                fail_cnt++;
                continue;
            }
            in_block = true;
            continue;
        }

        char* eq = strchr(p, '=');
        if (!eq) {
            fail_cnt++;
            continue;
        }
        *eq++ = '\0';
        char* pkg = strtrim(p);
        char* thread = "";
        char* delay_suffix = "";

        if (br && eb) {
            *br++ = '\0';
            *eb = '\0';
            pkg = strtrim(p);
            thread = strtrim(br);
            delay_suffix = strtrim(eb + 1);
            if (*delay_suffix && *delay_suffix != ':') {
                fail_cnt++;
                continue;
            }
        } else if (br || eb) {
            fail_cnt++;
            continue;
        }

        if (!append_rule(&new_rules, &rules_cnt, &rules_capacity, &cfg->topo,
                         pkg, thread, eq,
                         delay_suffix, &fail_cnt)) goto error;
    }
    if (in_block) fail_cnt++;

    if (!build_package_index(new_rules, rules_cnt, &exact_pkg_slots,
                             &exact_pkg_capacity, &wildcard_pkgs,
                             &wildcard_pkg_count)) goto error;

    if (last_mtime) *last_mtime = st.st_mtim;
    cfg->rules = new_rules;
    cfg->num_rules = rules_cnt;
    cfg->exact_pkg_slots = exact_pkg_slots;
    cfg->exact_pkg_capacity = exact_pkg_capacity;
    cfg->wildcard_pkgs = wildcard_pkgs;
    cfg->num_wildcard_pkgs = wildcard_pkg_count;
    cfg->mtime = st.st_mtim;

    fclose(fp);
    printf("配置文件解析完成，共加载 %zu 条规则\n", rules_cnt);
    if (fail_cnt > 0) {
        fprintf(stderr, "警告: %zu 条规则因格式、CPU 范围或长度无效被跳过\n", fail_cnt);
    }
    return cfg;

error:
    if (new_rules) free(new_rules);
    free(exact_pkg_slots);
    free(wildcard_pkgs);
    fclose(fp);
    free(cfg);
    return NULL;
}

static int pid_cmp(const void *a, const void *b) {
    return (*(pid_t*)a - *(pid_t*)b);
}

static void proc_collect(const AppConfig* cfg, ProcCache* cache, size_t* count) {
    DIR* proc_dir = opendir("/proc");
    if (!proc_dir) return;
    int proc_fd = dirfd(proc_dir);
    *count = 0;

    if (cache->procs == NULL) {
        cache->procs_cap = 2048;
        cache->procs = calloc(cache->procs_cap, sizeof(ProcessInfo));
        if (!cache->procs) {
            closedir(proc_dir);
            return;
        }
    }

    size_t detected_count = cache->num_procs;
    DetectedProcess* detected = NULL;
    if (detected_count > 0) {
        detected = malloc(detected_count * sizeof(DetectedProcess));
        if (detected) {
            for (size_t i = 0; i < detected_count; i++) {
                detected[i].pid = cache->procs[i].pid;
                build_str(detected[i].pkg, sizeof(detected[i].pkg), cache->procs[i].pkg, NULL);
                detected[i].detected_at_ms = cache->procs[i].detected_at_ms;
            }
        } else {
            detected_count = 0;
        }
    }

    struct dirent* ent;
    time_t current_time = time(NULL);
    uint64_t current_monotonic_ms = monotonic_ms();
    int current_proc_total = 0;
    while ((ent = readdir(proc_dir))) {
        char *end;
        long pid = strtol(ent->d_name, &end, 10);
        if (*end != '\0')  continue;
        current_proc_total++;

        if (!cache->scan_all_proc) {
            bool is_tracked = (cache->num_tracked_pids > 0 &&
                bsearch(&pid, cache->tracked_pids, cache->num_tracked_pids,
                        sizeof(pid_t), pid_cmp) != NULL);
            if (!is_tracked) {
                struct stat statbuf;
                if (fstatat(proc_fd, ent->d_name, &statbuf, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (current_time - statbuf.st_mtime > 60) continue;
            }
        }

        int pid_fd = openat(proc_fd, ent->d_name, O_RDONLY | O_DIRECTORY);
        if (pid_fd == -1) continue;

        char cmd[MAX_PKG_LEN] = {0};
        if (!read_file(pid_fd, "cmdline", cmd, sizeof(cmd))) {
            close(pid_fd);
            continue;
        }
        char* name = strrchr(cmd, '/');
        name = name ? name + 1 : cmd;

        if (!package_index_matches(cfg, name)) {
            close(pid_fd);
            continue;
        }

        if (*count >= cache->procs_cap) {
            size_t new_cap = cache->procs_cap * 2;
            ProcessInfo* new_procs = realloc(cache->procs, new_cap * sizeof(ProcessInfo));
            if (!new_procs) {
                close(pid_fd);
                continue;
            }
            memset(new_procs + cache->procs_cap, 0, (new_cap - cache->procs_cap) * sizeof(ProcessInfo));
            cache->procs = new_procs;
            cache->procs_cap = new_cap;
        }

        ProcessInfo* proc = &cache->procs[*count];

        proc->pid = pid;
        build_str(proc->pkg, sizeof(proc->pkg), name, NULL);
        proc->detected_at_ms = current_monotonic_ms;
        for (size_t i = 0; i < detected_count; i++) {
            if (detected[i].pid == pid && strcmp(detected[i].pkg, name) == 0) {
                proc->detected_at_ms = detected[i].detected_at_ms;
                break;
            }
        }
        CPU_ZERO(&proc->base_cpus);
        proc->base_cpuset[0] = '\0';
        proc->base_delay_ms = 0;
        proc->num_threads = 0;
        proc->num_thread_rules = 0;

        if (!proc->thread_rules || proc->thread_rules_cap < 8) {
            size_t new_cap = proc->thread_rules_cap ? proc->thread_rules_cap * 2 : 8;
            AffinityRule** tmp = realloc(proc->thread_rules, new_cap * sizeof(AffinityRule*));
            if (!tmp) {
                close(pid_fd);
                continue;
            }
            proc->thread_rules = tmp;
            proc->thread_rules_cap = new_cap;
        }

        const AffinityRule* base_rule = NULL;
        for (size_t i = 0; i < cfg->num_rules; i++) {
            const AffinityRule* rule = &cfg->rules[i];
            if (!rule_matches_package(rule, proc->pkg)) continue;

            if (rule->thread[0]) {
                if (proc->num_thread_rules >= proc->thread_rules_cap) {
                    size_t new_cap = proc->thread_rules_cap * 2;
                    AffinityRule** tmp = realloc(proc->thread_rules, new_cap * sizeof(AffinityRule*));
                    if (!tmp) break;
                    proc->thread_rules = tmp;
                    proc->thread_rules_cap = new_cap;
                }
                proc->thread_rules[proc->num_thread_rules++] = (AffinityRule*)rule;
            } else if (rule_is_more_specific(rule, base_rule, false)) {
                base_rule = rule;
            }
        }

        if (base_rule) {
            proc->base_cpus = base_rule->cpus;
            proc->base_delay_ms = base_rule->delay_ms;
            build_str(proc->base_cpuset, sizeof(proc->base_cpuset),
                      base_rule->cpuset_dir, NULL);
        }

        if (CPU_COUNT(&proc->base_cpus) == 0 && proc->num_thread_rules == 0) {
            close(pid_fd);
            continue;
        }

        int task_fd = openat(pid_fd, "task", O_RDONLY | O_DIRECTORY);
        close(pid_fd);
        if (task_fd == -1) {
            continue;
        }

        DIR* task_dir = fdopendir(task_fd);
        if (!task_dir) {
            close(task_fd);
            continue;
        }

        if (!proc->threads || proc->threads_cap < 512) {
            size_t new_cap = proc->threads_cap ? proc->threads_cap * 2 : 64;
            ThreadInfo* tmp = realloc(proc->threads, new_cap * sizeof(ThreadInfo));
            if (!tmp) {
                closedir(task_dir);
                continue;
            }
            proc->threads = tmp;
            proc->threads_cap = new_cap;
        }

        struct dirent* tent;
        while ((tent = readdir(task_dir))) {
            char *end2;
            long tid = strtol(tent->d_name, &end2, 10);
            if (*end2 != '\0')  continue;
            char tname[MAX_THREAD_LEN] = {0};
            char comm_path[64];
            snprintf(comm_path, sizeof(comm_path), "%s/comm", tent->d_name);
            if (!read_file(task_fd, comm_path, tname, sizeof(tname))) continue;

            strtrim(tname);

            if (proc->num_threads >= proc->threads_cap) {
                size_t new_cap = proc->threads_cap * 2;
                ThreadInfo* tmp = realloc(proc->threads, new_cap * sizeof(ThreadInfo));
                if (!tmp) continue;
                proc->threads = tmp;
                proc->threads_cap = new_cap;
            }

            ThreadInfo* ti = &proc->threads[proc->num_threads];
            ti->tid = tid;
            build_str(ti->name, sizeof(ti->name), tname, NULL);
            CPU_ZERO(&ti->cpus);
            const AffinityRule* matched_rule = NULL;

            for (size_t i = 0; i < proc->num_thread_rules; i++) {
                const AffinityRule* rule = proc->thread_rules[i];
                if (fnmatch(rule->thread, ti->name, FNM_NOESCAPE) == 0 &&
                    rule_is_more_specific(rule, matched_rule, true)) {
                    matched_rule = rule;
                }
            }

            if (matched_rule) {
                ti->cpus = matched_rule->cpus;
                ensure_cpuset_dir(&ti->cpus, &cfg->topo, ti->cpuset_dir,
                                  sizeof(ti->cpuset_dir));
                ti->bind_after_ms = delay_deadline(proc->detected_at_ms,
                                                   matched_rule->delay_ms);
            } else {
                ti->cpus = proc->base_cpus;
                build_str(ti->cpuset_dir, sizeof(ti->cpuset_dir), proc->base_cpuset, NULL);
                ti->bind_after_ms = delay_deadline(proc->detected_at_ms, proc->base_delay_ms);
            }

            proc->num_threads++;
        }

        closedir(task_dir);

        if (proc->num_threads > 0 && proc->threads_cap > proc->num_threads * 2) {
            size_t new_cap = proc->num_threads + proc->num_threads / 2;
            if (new_cap < 64) new_cap = 64;
            ThreadInfo* tmp = realloc(proc->threads, new_cap * sizeof(ThreadInfo));
            if (tmp) {
                proc->threads = tmp;
                proc->threads_cap = new_cap;
            }
        }

        (*count)++;
    }
    closedir(proc_dir);
    free(detected);
    if (current_proc_total > cache->last_proc_total) {
        cache->scan_all_proc = true;
    } else {
        cache->scan_all_proc = false;
    }
    cache->last_proc_total = current_proc_total;
}

static void update_cache(ProcCache* cache, const AppConfig* cfg, int* affinity_counter,
                         bool force_reload) {
    bool need_reload = force_reload;
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        need_reload = true;
    } else {
        int current_proc_count = info.procs;
        if (!need_reload) {
            if (current_proc_count > cache->last_proc_count + 11) {
                need_reload = true;
            } else if (current_proc_count > cache->last_proc_count) {
                *affinity_counter = 0;
            }
        }
        cache->last_proc_count = current_proc_count;
    }
    static int kill_check_skip = 0;
    if (!need_reload && cache->procs != NULL) {
        if (++kill_check_skip >= 5) {
            kill_check_skip = 0;
            for (size_t i = 0; i < cache->num_procs; i++) {
                if (kill(cache->procs[i].pid, 0) != 0) {
                    need_reload = true;
                    break;
                }
            }
        }
    }
    if (need_reload) {
        kill_check_skip = 0;
        size_t new_count = 0;
        proc_collect(cfg, cache, &new_count);

        if (new_count > cache->tracked_pids_cap) {
            size_t new_cap = cache->tracked_pids_cap ? cache->tracked_pids_cap * 2 : new_count;
            pid_t* new_pids = realloc(cache->tracked_pids, new_cap * sizeof(pid_t));
            if (new_pids) {
                cache->tracked_pids = new_pids;
                cache->tracked_pids_cap = new_cap;
            }
        }

        if (cache->tracked_pids) {
            cache->num_tracked_pids = 0;
            for (size_t i = 0; i < new_count; i++) {
                if (cache->num_tracked_pids < cache->tracked_pids_cap) {
                    cache->tracked_pids[cache->num_tracked_pids++] = cache->procs[i].pid;
                }
            }
            if (cache->num_tracked_pids > 0) {
                qsort(cache->tracked_pids, cache->num_tracked_pids, sizeof(pid_t), pid_cmp);
            }
        }
        
        cache->num_procs = new_count;
        *affinity_counter = 0;
    }
}

static bool restore_thread_affinity(pid_t tid, const CpuTopology* topo) {
    bool cpuset_restored = true;
    if (topo->cpuset_enabled && topo->base_cpuset_fd != -1) {
        char tid_str[32];
        snprintf(tid_str, sizeof(tid_str), "%d\n", tid);
        cpuset_restored = write_file(topo->base_cpuset_fd, "tasks", tid_str,
                                     O_WRONLY | O_APPEND);
    }

    if (sched_setaffinity(tid, sizeof(topo->present_cpus), &topo->present_cpus) == 0) {
        return cpuset_restored;
    }
    return errno == ESRCH;
}

static bool restore_cached_affinity(const ProcCache* cache, const CpuTopology* topo) {
    bool restored = true;
    for (size_t i = 0; i < cache->num_procs; i++) {
        const ProcessInfo* proc = &cache->procs[i];
        char task_path[64];
        int path_len = snprintf(task_path, sizeof(task_path), "/proc/%d/task", proc->pid);
        if (path_len < 0 || (size_t)path_len >= sizeof(task_path)) {
            restored = false;
            continue;
        }

        DIR* task_dir = opendir(task_path);
        if (!task_dir) {
            if (errno != ENOENT && errno != ESRCH) restored = false;
            continue;
        }

        while (true) {
            errno = 0;
            struct dirent* entry = readdir(task_dir);
            if (!entry) {
                if (errno != 0) restored = false;
                break;
            }
            char* end;
            long tid = strtol(entry->d_name, &end, 10);
            if (*end != '\0') continue;
            if (!restore_thread_affinity((pid_t)tid, topo)) restored = false;
        }
        closedir(task_dir);
    }
    return restored;
}

static uint64_t apply_affinity(ProcCache* cache, const CpuTopology* topo, uint64_t now_ms) {
    uint64_t next_delayed_bind_ms = UINT64_MAX;
    for (size_t i = 0; i < cache->num_procs; i++) {
        const ProcessInfo* proc = &cache->procs[i];
        for (size_t j = 0; j < proc->num_threads; j++) {
            const ThreadInfo* ti = &proc->threads[j];
            if (ti->bind_after_ms > now_ms) {
                if (ti->bind_after_ms < next_delayed_bind_ms) {
                    next_delayed_bind_ms = ti->bind_after_ms;
                }
                continue;
            }
            bool restore_default = CPU_COUNT(&ti->cpus) == 0;
            const cpu_set_t* target_cpus = restore_default ? &topo->present_cpus : &ti->cpus;
            cpu_set_t current_cpus;
            bool affinity_matches = false;
            if (sched_getaffinity(ti->tid, sizeof(current_cpus), &current_cpus) == 0) {
                affinity_matches = CPU_EQUAL(target_cpus, &current_cpus);
            } else if (errno == ESRCH) {
                cache->last_proc_count = 0;
                continue;
            }

            if (affinity_matches) continue;

            if (topo->cpuset_enabled && topo->base_cpuset_fd != -1) {
                char tid_str[32];
                snprintf(tid_str, sizeof(tid_str), "%d\n", ti->tid);
                if (restore_default) {
                    write_file(topo->base_cpuset_fd, "tasks", tid_str, O_WRONLY | O_APPEND);
                } else {
                    if (ti->cpuset_dir[0]) {
                        int fd = openat(topo->base_cpuset_fd, ti->cpuset_dir, O_RDONLY | O_DIRECTORY);
                        if (fd != -1) {
                            write_file(fd, "tasks", tid_str, O_WRONLY | O_APPEND);
                            close(fd);
                        }
                    }
                }
            }
            if (sched_setaffinity(ti->tid, sizeof(*target_cpus), target_cpus) == -1 && errno == ESRCH) {
                cache->last_proc_count = 0;
            }
        }
    }
    return next_delayed_bind_ms;
}

static void config_release(AppConfig* cfg) {
    if (!cfg) return;
    if (atomic_fetch_sub(&cfg->ref_count, 1) == 1) {
        if (cfg->rules) free(cfg->rules);
        free(cfg->exact_pkg_slots);
        free(cfg->wildcard_pkgs);
        free(cfg);
    }
}

static AppConfig* get_config() {
    pthread_mutex_lock(&config_mutex);
    AppConfig* cfg = current_config;
    if (cfg) atomic_fetch_add_explicit(&cfg->ref_count, 1, memory_order_relaxed);
    pthread_mutex_unlock(&config_mutex);
    return cfg;
}

static AppConfig* replace_config(AppConfig* new_config) {
    pthread_mutex_lock(&config_mutex);
    AppConfig* old_config = current_config;
    current_config = new_config;
    pthread_mutex_unlock(&config_mutex);
    return old_config;
}

static void* config_loader_thread(void* arg) {
    int interval = *(int*)arg;
    free(arg);
    pthread_setname_np(pthread_self(), "ConfigLoader");

    struct timespec last_mtime = { .tv_sec = -1, .tv_nsec = -1 };
    while (1) {
        if (inotify_supported) {
            fd_set rfds;
            struct timeval tv;
            FD_ZERO(&rfds);
            FD_SET(inotify_fd, &rfds);
            tv.tv_sec = interval;
            tv.tv_usec = 0;

            int ret = select(inotify_fd + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                inotify_supported = 0;
                close(inotify_fd);
                inotify_fd = -1;
                continue;
            } else if (ret == 0) {
                continue;
            }

            char buf[4096] __attribute__((aligned(8)));
            ssize_t len = read(inotify_fd, buf, sizeof(buf));
            if (len <= 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    inotify_supported = 0;
                    close(inotify_fd);
                    inotify_fd = -1;
                }
                continue;
            }

            bool reload_needed = false;
            for (char* p = buf; p < buf + len;) {
                struct inotify_event* event = (struct inotify_event*)p;
                if (event->mask & (IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF)) {
                    reload_needed = true;

                    if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
                        sleep(interval);
                        AppConfig* cfg = get_config();
                        if (cfg) {
                            inotify_rm_watch(inotify_fd, inotify_wd);
                            inotify_wd = inotify_add_watch(inotify_fd, cfg->config_file, IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
                            last_mtime = (struct timespec){ .tv_sec = -1, .tv_nsec = -1 };
                            config_release(cfg);
                        }
                        if (inotify_wd < 0) {
                            inotify_supported = 0;
                            close(inotify_fd);
                            inotify_fd = -1;
                            break;
                        }
                    }
                }
                p += sizeof(struct inotify_event) + event->len;
            }

            if (reload_needed) {
                AppConfig* cfg = get_config();
                if (cfg) {
                    AppConfig* new_config = load_config(cfg->config_file, &cfg->topo, &last_mtime);
                    if (new_config) {
                        AppConfig* old_config = replace_config(new_config);
                        atomic_store(&config_updated, 1);
                        if (old_config) config_release(old_config);
                    }
                    config_release(cfg);
                }
            }
        } else {
            AppConfig* cfg = get_config();
            if (cfg) {
                AppConfig* new_config = load_config(cfg->config_file, &cfg->topo, &last_mtime);
                if (new_config) {
                    AppConfig* old_config = replace_config(new_config);
                    atomic_store(&config_updated, 1);
                    if (old_config) config_release(old_config);
                }
                config_release(cfg);
            }
            sleep(interval);
        }
    }
    return NULL;
}

static void print_help(const char* prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("Options:\n");
    printf("  -c <config_file>   指定配置文件 (默认: ./applist.conf)\n");
    printf("  -s <interval>      设置检查间隔(秒) (必须>=1, 默认: 2)\n");
    printf("  -b <cpuset_name>   指定 cpuset 目录名 (默认: AkiAppOpt)\n");
    printf("  -v                 显示程序版本\n");
    printf("  -h                 显示帮助信息\n");
    printf("\n示例:\n");
    printf("  %s -c /data/applist.conf -s 3\n", prog_name);
    printf("  %s -b MyAppOpt\n", prog_name);
    printf("\nCPU 规格支持数字范围及 e-core、p-core、hp-core、all-core。\n");
    printf("块规则示例:\n");
    printf("  com.example {\n");
    printf("    RenderThread=hp-core\n");
    printf("  }\n");
}

int main(int argc, char **argv) {
    char config_file[4096] = "./applist.conf";
    int sleep_interval = 2;
    int opt;
    while ((opt = getopt(argc, argv, "c:s:b:hv")) != -1) {
        switch (opt) {
            case 'c':
                build_str(config_file, sizeof(config_file), optarg, NULL);
                printf("配置文件: %s\n", config_file);
                break;
            case 's':
            {
                char *endptr;
                long val = strtol(optarg, &endptr, 10);
                if (endptr == optarg || *endptr != '\0' || val < 1) {
                    fprintf(stderr, "无效的时间间隔: %s\n", optarg);
                    fprintf(stderr, "间隔必须是 >=1 的整数\n");
                    exit(EXIT_FAILURE);
                }
                sleep_interval = (int)val;
                printf("检查间隔: %d 秒\n", sleep_interval);
                break;
            }
            case 'b':
                if (!set_base_cpuset(optarg)) {
                    fprintf(stderr, "无效的 cpuset 目录名: %s\n", optarg);
                    fprintf(stderr, "仅允许字母、数字、点、下划线和连字符，且不能为 . 或 ..\n");
                    exit(EXIT_FAILURE);
                }
                printf("cpuset 目录: %s\n", base_cpuset);
                break;
            case 'v':
                printf("AppOpt 版本 %s\n", VERSION);
                exit(EXIT_SUCCESS);
            case 'h':
                print_help(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                print_help(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    CpuTopology topo = init_cpu_topo();

    struct stat st;
    if (stat(config_file, &st) != 0) {
        const char* initial_content = "# 规则编写与使用说明请参考 http://AppOpt.suto.top\n\n";
        if (write_file(AT_FDCWD, config_file, initial_content, O_WRONLY | O_CREAT | O_TRUNC)) {
            printf("配置文件不存在，重建一个空的配置文件: %s\n", config_file);
        }
    }

    AppConfig* initial_config = load_config(config_file, &topo, NULL);
    if (!initial_config) {
        fprintf(stderr, "初始配置加载失败\n");
        exit(EXIT_FAILURE);
    }
    replace_config(initial_config);
    atomic_store(&config_updated, 1);

    inotify_fd = inotify_init1(IN_CLOEXEC);
    if (inotify_fd >= 0) {
        int flags = fcntl(inotify_fd, F_GETFL);
        if (flags >= 0) fcntl(inotify_fd, F_SETFL, flags | O_NONBLOCK);
        inotify_wd = inotify_add_watch(inotify_fd, config_file, IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
        if (inotify_wd >= 0) {
            inotify_supported = 1;
            printf("启用inotify监控配置文件变更\n");
        } else {
            close(inotify_fd);
            inotify_fd = -1;
            printf("inotify初始化失败，使用轮询模式\n");
        }
    }

    pthread_t loader_thread;
    int* interval_ptr = malloc(sizeof(int));
    if (!interval_ptr) {
        config_release(replace_config(NULL));
        if (inotify_supported) close(inotify_fd);
        exit(EXIT_FAILURE);
    }
    *interval_ptr = sleep_interval;

    if (pthread_create(&loader_thread, NULL, config_loader_thread, interval_ptr) != 0) {
        perror("配置加载器线程创建失败");
        free(interval_ptr);
        config_release(replace_config(NULL));
        if (inotify_supported) close(inotify_fd);
        exit(EXIT_FAILURE);
    }
    pthread_detach(loader_thread);

    ProcCache cache = {0};
    int affinity_counter = 0;
    uint64_t next_scan_ms = 0;
    uint64_t next_delayed_bind_ms = UINT64_MAX;
    bool affinity_restore_pending = false;
    bool affinity_restore_warning_shown = false;
    printf("启动AppOpt服务 v%s\n", VERSION);

    for (;;) {
        uint64_t now_ms = monotonic_ms();
        if (atomic_exchange(&config_updated, 0)) {
            affinity_restore_pending = true;
            affinity_restore_warning_shown = false;
        }

        bool force_reload = false;
        if (affinity_restore_pending) {
            if (!restore_cached_affinity(&cache, &topo)) {
                if (!affinity_restore_warning_shown) {
                    fprintf(stderr, "警告: 旧线程亲和性恢复失败，将继续重试后再应用新配置\n");
                    affinity_restore_warning_shown = true;
                }
                sleep(1);
                continue;
            }
            affinity_restore_pending = false;
            affinity_restore_warning_shown = false;
            force_reload = true;
            cache.scan_all_proc = true;
            cache.last_proc_count = 0;
            next_scan_ms = 0;
            next_delayed_bind_ms = UINT64_MAX;
        }

        if (now_ms >= next_scan_ms) {
            AppConfig* cfg = get_config();
            if (cfg) {
                update_cache(&cache, cfg, &affinity_counter, force_reload);
                affinity_counter--;
                if (affinity_counter < 1) {
                    now_ms = monotonic_ms();
                    next_delayed_bind_ms = apply_affinity(&cache, &cfg->topo, now_ms);
                    affinity_counter = 5;
                }
                config_release(cfg);
            }
            next_scan_ms = now_ms + (uint64_t)sleep_interval * 1000ULL;
        }

        now_ms = monotonic_ms();
        if (next_delayed_bind_ms != UINT64_MAX && now_ms >= next_delayed_bind_ms) {
            next_delayed_bind_ms = apply_affinity(&cache, &topo, now_ms);
        }
        usleep(DELAY_POLL_US);
    }
}
