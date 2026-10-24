#define _GNU_SOURCE

#include "load_balancer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LB_INITIAL_THREAD_CAPACITY 64
#define LB_INITIAL_SAMPLE_CAPACITY 256
#define LB_LOAD_SCALE 256ULL
#define ANDROID_UID_OFFSET 100000UL
#define ANDROID_APP_ID_START 10000UL

typedef struct {
    pid_t pid;
    pid_t tid;
    unsigned long long ticks;
    unsigned long long start_time;
    unsigned long long smoothed_load;
    unsigned long long generation;
} LoadSample;

typedef struct {
    pid_t tid;
    char name[64];
    unsigned long long load;
} ThreadLoad;

struct LoadBalancer {
    LoadSample *samples;
    size_t sample_count;
    size_t sample_capacity;
    ThreadLoad *thread_buffer;
    size_t thread_capacity;
    char *game_list_file;
    char **game_patterns;
    size_t game_pattern_count;
    struct timespec game_list_mtime;
    bool game_list_loaded;
    unsigned long long generation;
};

static bool read_path(const char *path, char *buffer, size_t size) {
    int fd;
    ssize_t length;

    if (!path || !buffer || size < 2) return false;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    length = read(fd, buffer, size - 1);
    close(fd);
    if (length <= 0) return false;
    buffer[length] = '\0';
    return true;
}

static bool is_decimal_name(const char *name) {
    if (!name || !*name) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!isdigit(*p)) return false;
    }
    return true;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return text;
    char *end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return text;
}

static void free_game_patterns(char **patterns, size_t count) {
    if (!patterns) return;
    for (size_t i = 0; i < count; i++) free(patterns[i]);
    free(patterns);
}

static bool append_game_pattern(char ***patterns, size_t *count,
                                size_t *capacity, const char *pattern) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*patterns)[i], pattern) == 0) return true;
    }
    if (*count >= *capacity) {
        if (*capacity > SIZE_MAX / 2) return false;
        size_t new_capacity = *capacity ? *capacity * 2 : 16;
        if (new_capacity > SIZE_MAX / sizeof(**patterns)) return false;
        char **resized = realloc(*patterns, new_capacity * sizeof(*resized));
        if (!resized) return false;
        *patterns = resized;
        *capacity = new_capacity;
    }
    char *copy = strdup(pattern);
    if (!copy) return false;
    (*patterns)[(*count)++] = copy;
    return true;
}

static void reload_game_patterns(LoadBalancer *balancer) {
    if (!balancer->game_list_file) return;
    struct stat st;
    if (stat(balancer->game_list_file, &st) != 0) {
        if (errno == ENOENT && balancer->game_list_loaded) {
            free_game_patterns(balancer->game_patterns,
                               balancer->game_pattern_count);
            balancer->game_patterns = NULL;
            balancer->game_pattern_count = 0;
            balancer->game_list_loaded = false;
        }
        return;
    }
    if (balancer->game_list_loaded &&
        balancer->game_list_mtime.tv_sec == st.st_mtim.tv_sec &&
        balancer->game_list_mtime.tv_nsec == st.st_mtim.tv_nsec) return;

    FILE *file = fopen(balancer->game_list_file, "r");
    if (!file) return;
    char **patterns = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char line[512];
    bool valid = true;
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            valid = false;
            break;
        }
        char *pattern = trim(line);
        if (!*pattern || *pattern == '#' ||
            (pattern[0] == '/' && pattern[1] == '/')) continue;
        if (!append_game_pattern(&patterns, &count, &capacity, pattern)) {
            valid = false;
            break;
        }
    }
    if (ferror(file)) valid = false;
    fclose(file);
    if (!valid) {
        free_game_patterns(patterns, count);
        return;
    }

    free_game_patterns(balancer->game_patterns, balancer->game_pattern_count);
    balancer->game_patterns = patterns;
    balancer->game_pattern_count = count;
    balancer->game_list_mtime = st.st_mtim;
    balancer->game_list_loaded = true;
}

static bool parse_thread_stat(const char *stat_line, unsigned long long *ticks,
                              unsigned long long *start_time,
                              char *name, size_t name_size) {
    const char *open_paren;
    const char *close_paren;
    const char *cursor;
    unsigned long long user_ticks = 0;
    unsigned long long system_ticks = 0;
    unsigned long long thread_start_time = 0;

    if (name && name_size > 0) name[0] = '\0';
    /* comm may contain spaces and ')'; fields start after the final ')'. */
    open_paren = strchr(stat_line, '(');
    close_paren = strrchr(stat_line, ')');
    if (!open_paren || !close_paren || open_paren >= close_paren ||
        close_paren[1] != ' ') return false;
    cursor = close_paren + 2;

    /* cursor starts at field 3; utime/stime/starttime are fields 14/15/22. */
    for (int field = 3; field <= 22; field++) {
        while (*cursor == ' ') cursor++;
        if (!*cursor) return false;
        const char *end = cursor;
        while (*end && *end != ' ') end++;
        if (field == 14 || field == 15 || field == 22) {
            char *number_end;
            errno = 0;
            unsigned long long value = strtoull(cursor, &number_end, 10);
            if (errno == ERANGE || number_end != end) return false;
            if (field == 14) user_ticks = value;
            if (field == 15) system_ticks = value;
            if (field == 22) thread_start_time = value;
        }
        cursor = end;
    }

    if (name && name_size > 0) {
        size_t name_length = (size_t)(close_paren - open_paren - 1);
        if (name_length >= name_size) name_length = name_size - 1;
        memcpy(name, open_paren + 1, name_length);
        name[name_length] = '\0';
        name[strcspn(name, "\r\n")] = '\0';
    }
    *ticks = user_ticks > ULLONG_MAX - system_ticks
                 ? ULLONG_MAX
                 : user_ticks + system_ticks;
    *start_time = thread_start_time;
    return true;
}

static bool read_thread_stat(pid_t pid, pid_t tid, unsigned long long *ticks,
                             unsigned long long *start_time,
                             char *name, size_t name_size) {
    char path[96];
    char stat_line[512];
    if (snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid) < 0 ||
        !read_path(path, stat_line, sizeof(stat_line))) return false;
    return parse_thread_stat(stat_line, ticks, start_time, name, name_size);
}

static bool read_package_name(pid_t pid, char *name, size_t size) {
    char path[64];
    char cmdline[256];
    char *last_slash;

    if (snprintf(path, sizeof(path), "/proc/%d/cmdline", pid) < 0 ||
        !read_path(path, cmdline, sizeof(cmdline))) return false;
    last_slash = strrchr(cmdline, '/');
    if (last_slash) last_slash++;
    else last_slash = cmdline;
    if (!*last_slash) return false;
    if (snprintf(name, size, "%s", last_slash) < 0 || strlen(last_slash) >= size) return false;
    return true;
}

static bool is_android_app_process(pid_t pid) {
    char path[64];
    char status[2048];
    char *uid_line;
    char *end;
    unsigned long uid;
    if (snprintf(path, sizeof(path), "/proc/%d/status", pid) < 0 ||
        !read_path(path, status, sizeof(status))) return false;
    uid_line = strstr(status, "Uid:");
    if (!uid_line) return false;
    uid_line += 4;
    while (isspace((unsigned char)*uid_line)) uid_line++;
    errno = 0;
    uid = strtoul(uid_line, &end, 10);
    if (errno == ERANGE || end == uid_line) return false;
    return uid % ANDROID_UID_OFFSET >= ANDROID_APP_ID_START;
}

static bool contains_ci(const char *text, const char *needle) {
    size_t needle_len;
    if (!text || !needle || !*needle) return false;
    needle_len = strlen(needle);
    for (; *text; text++) {
        size_t i;
        for (i = 0; i < needle_len && text[i]; i++) {
            if (tolower((unsigned char)text[i]) != tolower((unsigned char)needle[i])) break;
        }
        if (i == needle_len) return true;
    }
    return false;
}

static bool game_pattern_matches(const LoadBalancer *balancer,
                                 const char *package_name) {
    const char *separator = strchr(package_name, ':');
    size_t base_length = separator ? (size_t)(separator - package_name) :
                                     strlen(package_name);
    char base_package[256];
    if (base_length >= sizeof(base_package)) return false;
    memcpy(base_package, package_name, base_length);
    base_package[base_length] = '\0';

    for (size_t i = 0; i < balancer->game_pattern_count; i++) {
        const char *pattern = balancer->game_patterns[i];
        if (fnmatch(pattern, package_name, FNM_NOESCAPE) == 0 ||
            fnmatch(pattern, base_package, FNM_NOESCAPE) == 0) return true;
    }
    return false;
}

static bool looks_like_game(const LoadBalancer *balancer,
                            const char *package_name,
                            const ThreadLoad *threads, size_t count) {
    static const char *const keywords[] = {
        "game", "unity", "unreal", "ue4", "ue5", "cocos", "godot", "libmain"
    };
    if (game_pattern_matches(balancer, package_name)) return true;
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (contains_ci(package_name, keywords[i])) return true;
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t k = 0; k < sizeof(keywords) / sizeof(keywords[0]); k++) {
            if (contains_ci(threads[i].name, keywords[k])) return true;
        }
    }
    return false;
}

static int thread_load_cmp(const void *left, const void *right) {
    const ThreadLoad *a = left;
    const ThreadLoad *b = right;
    if (a->load < b->load) return 1;
    if (a->load > b->load) return -1;
    return (a->tid > b->tid) - (a->tid < b->tid);
}

static size_t select_top_threads(ThreadLoad *threads, size_t count,
                                 ThreadLoad *top[3]) {
    /* Only these ranks affect CPU tiers; keep the full-sort tie order. */
    size_t top_count = 0;
    for (size_t i = 0; i < count; i++) {
        ThreadLoad *candidate = &threads[i];
        size_t insert_at;
        if (top_count == 3 && thread_load_cmp(candidate, top[2]) >= 0) continue;
        insert_at = 0;
        while (insert_at < top_count &&
               thread_load_cmp(candidate, top[insert_at]) >= 0) {
            insert_at++;
        }
        if (top_count < 3) top_count++;
        if (insert_at < top_count - 1) {
            memmove(&top[insert_at + 1], &top[insert_at],
                    (top_count - insert_at - 1) * sizeof(*top));
        }
        top[insert_at] = candidate;
    }
    return top_count;
}

static int load_sample_cmp(const void *left, const void *right) {
    const LoadSample *a = left;
    const LoadSample *b = right;
    if (a->pid != b->pid) return (a->pid > b->pid) - (a->pid < b->pid);
    return (a->tid > b->tid) - (a->tid < b->tid);
}

static LoadSample *find_sample(LoadBalancer *balancer, size_t history_count,
                               pid_t pid, pid_t tid,
                               unsigned long long ticks,
                               unsigned long long start_time,
                               bool *has_history) {
    LoadSample key = { .pid = pid, .tid = tid };
    LoadSample *sample = bsearch(&key, balancer->samples, history_count,
                                 sizeof(*balancer->samples), load_sample_cmp);
    if (sample) {
        if (sample->start_time == start_time) {
            *has_history = true;
            return sample;
        }
        sample->ticks = ticks;
        sample->start_time = start_time;
        sample->smoothed_load = 0;
        return sample;
    }
    if (balancer->sample_count >= balancer->sample_capacity) {
        if (balancer->sample_capacity > SIZE_MAX / 2) return NULL;
        size_t new_capacity = balancer->sample_capacity
                                  ? balancer->sample_capacity * 2
                                  : LB_INITIAL_SAMPLE_CAPACITY;
        if (new_capacity > SIZE_MAX / sizeof(*balancer->samples)) return NULL;
        LoadSample *resized = realloc(balancer->samples, new_capacity * sizeof(*resized));
        if (!resized) return NULL;
        balancer->samples = resized;
        balancer->sample_capacity = new_capacity;
    }
    sample = &balancer->samples[balancer->sample_count++];
    *sample = (LoadSample){
        .pid = pid,
        .tid = tid,
        .ticks = ticks,
        .start_time = start_time
    };
    return sample;
}

static ThreadLoad *get_thread_slot(LoadBalancer *balancer, size_t index) {
    if (index >= balancer->thread_capacity) {
        if (balancer->thread_capacity > SIZE_MAX / 2) return NULL;
        size_t new_capacity = balancer->thread_capacity
                                  ? balancer->thread_capacity * 2
                                  : LB_INITIAL_THREAD_CAPACITY;
        if (new_capacity <= index ||
            new_capacity > SIZE_MAX / sizeof(*balancer->thread_buffer)) return NULL;
        ThreadLoad *resized = realloc(balancer->thread_buffer,
                                      new_capacity * sizeof(*resized));
        if (!resized) return NULL;
        balancer->thread_buffer = resized;
        balancer->thread_capacity = new_capacity;
    }
    ThreadLoad *thread = &balancer->thread_buffer[index];
    *thread = (ThreadLoad){0};
    return thread;
}

static void prune_samples(LoadBalancer *balancer) {
    size_t out = 0;
    for (size_t i = 0; i < balancer->sample_count; i++) {
        if (balancer->samples[i].generation == balancer->generation) {
            if (out != i) balancer->samples[out] = balancer->samples[i];
            out++;
        }
    }
    balancer->sample_count = out;
    if (out > 1) qsort(balancer->samples, out, sizeof(*balancer->samples),
                       load_sample_cmp);
}

static cpu_set_t union_cores(const cpu_set_t *first, const cpu_set_t *second) {
    cpu_set_t result;
    CPU_ZERO(&result);
    if (first) CPU_OR(&result, &result, first);
    if (second) CPU_OR(&result, &result, second);
    return result;
}

static cpu_set_t core_or_fallback(const cpu_set_t *primary, const cpu_set_t *fallback) {
    cpu_set_t result;
    CPU_ZERO(&result);
    if (primary && CPU_COUNT(primary) > 0) CPU_OR(&result, &result, primary);
    else if (fallback) CPU_OR(&result, &result, fallback);
    return result;
}

static void apply_thread_affinity(pid_t tid, const cpu_set_t *target,
                                  const cpu_set_t *fallback) {
    cpu_set_t current;
    if (!target || CPU_COUNT(target) == 0) return;
    if (sched_getaffinity(tid, sizeof(current), &current) == 0 && CPU_EQUAL(&current, target)) return;
    if (sched_setaffinity(tid, sizeof(*target), target) == 0 || errno != EINVAL ||
        !fallback || CPU_COUNT(fallback) == 0 || CPU_EQUAL(target, fallback)) return;
    (void)sched_setaffinity(tid, sizeof(*fallback), fallback);
}

static unsigned long long smooth_load(unsigned long long previous,
                                      unsigned long long current) {
    unsigned long long scaled = current > ULLONG_MAX / LB_LOAD_SCALE
                                    ? ULLONG_MAX
                                    : current * LB_LOAD_SCALE;
    return (previous >> 1) + (scaled >> 1) + (previous & scaled & 1ULL);
}

static size_t app_performance_thread_count(ThreadLoad *const *threads,
                                           size_t count) {
    size_t promoted = 0;
    while (promoted < count && promoted < 2 && threads[promoted]->load > 0) promoted++;
    if (promoted == 2 && count > 2 && threads[2]->load > 0 &&
        threads[2]->load >= (threads[1]->load >> 1) + (threads[1]->load & 1ULL)) {
        promoted = 3;
    }
    return promoted;
}

LoadBalancer *load_balancer_create(const char *game_list_file) {
    LoadBalancer *balancer = calloc(1, sizeof(LoadBalancer));
    if (!balancer) return NULL;
    if (game_list_file && *game_list_file) {
        balancer->game_list_file = strdup(game_list_file);
        if (!balancer->game_list_file) {
            free(balancer);
            return NULL;
        }
    }
    return balancer;
}

void load_balancer_destroy(LoadBalancer *balancer) {
    if (!balancer) return;
    free(balancer->samples);
    free(balancer->thread_buffer);
    free(balancer->game_list_file);
    free_game_patterns(balancer->game_patterns, balancer->game_pattern_count);
    free(balancer);
}

void load_balancer_reset(LoadBalancer *balancer, const cpu_set_t *fallback_cpus) {
    if (!balancer) return;
    if (fallback_cpus && CPU_COUNT(fallback_cpus) > 0) {
        for (size_t i = 0; i < balancer->sample_count; i++) {
            (void)sched_setaffinity(balancer->samples[i].tid,
                                    sizeof(*fallback_cpus), fallback_cpus);
        }
    }
    balancer->sample_count = 0;
}

void load_balancer_tick(LoadBalancer *balancer,
                        const LoadBalancerTopology *topology,
                        LoadBalancerRuleMatch rule_match,
                        void *rule_context) {
    DIR *proc_dir;
    struct dirent *proc_entry;
    if (!balancer || !topology) return;
    reload_game_patterns(balancer);
    balancer->generation++;
    if (balancer->generation == 0) {
        balancer->generation = 1;
        for (size_t i = 0; i < balancer->sample_count; i++) {
            balancer->samples[i].generation = 0;
        }
    }
    size_t history_count = balancer->sample_count;
    proc_dir = opendir("/proc");
    if (!proc_dir) return;

    while ((proc_entry = readdir(proc_dir)) != NULL) {
        char package_name[256];
        DIR *task_dir;
        struct dirent *task_entry;
        size_t thread_count = 0;
        bool have_history = false;
        bool allocation_failed = false;
        char task_path[64];
        char *end;
        long pid_long;

        if (!is_decimal_name(proc_entry->d_name)) continue;
        pid_long = strtol(proc_entry->d_name, &end, 10);
        if (*end || pid_long <= 0 || pid_long > INT32_MAX) continue;
        pid_t pid = (pid_t)pid_long;
        if (!read_package_name(pid, package_name, sizeof(package_name))) continue;
        if (!strchr(package_name, '.') || !is_android_app_process(pid)) continue;
        if (rule_match && rule_match(rule_context, package_name)) continue;
        if (snprintf(task_path, sizeof(task_path), "/proc/%d/task", pid) < 0) continue;
        task_dir = opendir(task_path);
        if (!task_dir) continue;

        while ((task_entry = readdir(task_dir)) != NULL) {
            unsigned long long ticks;
            unsigned long long start_time;
            long tid_long;
            if (!is_decimal_name(task_entry->d_name)) continue;
            tid_long = strtol(task_entry->d_name, &end, 10);
            if (*end || tid_long <= 0 || tid_long > INT32_MAX) continue;
            pid_t tid = (pid_t)tid_long;
            ThreadLoad *thread = get_thread_slot(balancer, thread_count);
            if (!thread) {
                allocation_failed = true;
                break;
            }
            if (!read_thread_stat(pid, tid, &ticks, &start_time,
                                  thread->name, sizeof(thread->name))) continue;
            bool thread_has_history = false;
            LoadSample *sample = find_sample(balancer, history_count, pid, tid,
                                             ticks, start_time,
                                             &thread_has_history);
            if (!sample) continue;
            thread_count++;
            thread->tid = tid;
            sample->generation = balancer->generation;
            if (thread_has_history && sample->ticks <= ticks) {
                sample->smoothed_load = smooth_load(sample->smoothed_load,
                                                    ticks - sample->ticks);
                thread->load = sample->smoothed_load;
                have_history = true;
            } else {
                sample->smoothed_load = 0;
            }
            sample->ticks = ticks;
        }
        closedir(task_dir);
        if (allocation_failed || !have_history || thread_count == 0) continue;

        ThreadLoad *threads = balancer->thread_buffer;
        ThreadLoad *top_threads[3] = {0};
        size_t top_thread_count = select_top_threads(threads, thread_count,
                                                     top_threads);
        bool game = looks_like_game(balancer, package_name, threads,
                                    thread_count);
        cpu_set_t p_core = core_or_fallback(&topology->p_core, &topology->hp_core);
        cpu_set_t hp_core = core_or_fallback(&topology->hp_core, &p_core);
        if (CPU_COUNT(&p_core) == 0) p_core = topology->present_cpus;
        if (CPU_COUNT(&hp_core) == 0) hp_core = topology->present_cpus;
        cpu_set_t e_core = core_or_fallback(&topology->e_core,
                                            &topology->present_cpus);
        cpu_set_t mixed = union_cores(&e_core, &p_core);
        if (CPU_COUNT(&mixed) == 0) mixed = topology->present_cpus;
        size_t app_performance_threads = game ? 0 :
            app_performance_thread_count(top_threads, top_thread_count);
        for (size_t i = 0; i < thread_count; i++) {
            const cpu_set_t *target;
            size_t rank = SIZE_MAX;
            for (size_t j = 0; j < top_thread_count; j++) {
                if (&threads[i] == top_threads[j]) {
                    rank = j;
                    break;
                }
            }
            if (game) {
                target = rank == 0 && threads[i].load > 0 ? &hp_core :
                         rank == 1 && threads[i].load > 0 ? &p_core : &mixed;
            } else {
                target = rank < app_performance_threads ? &p_core : &mixed;
            }
            apply_thread_affinity(threads[i].tid, target,
                                  &topology->present_cpus);
        }
    }
    closedir(proc_dir);
    prune_samples(balancer);
}
