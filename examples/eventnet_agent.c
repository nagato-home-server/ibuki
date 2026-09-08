#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "eventnet/types.h"
#include "eventnet/yaml_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#if defined(_WIN32)
#define EN_POPEN _popen
#define EN_PCLOSE _pclose
#else
#define EN_POPEN popen
#define EN_PCLOSE pclose
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct {
    const char *path_id;
    const char *source;
    const char *target;
} agent_probe_t;

typedef struct {
    const char *path_id;
    const char *yaml_file;
    const char *intent_id;
    const char *source;
    const char *target;
    const char *output;
    bool append_output;
    int interval_ms;
    int count;
    bool simulate;
    double simulated_rtt_ms;
    double simulated_loss;
    agent_probe_t probes[EN_MAX_PATHS];
    size_t probe_count;
} agent_options_t;

static void usage(const char *program)
{
    printf("usage: %s --path PATH_ID --target IP [options]\n", program);
    printf("       %s --probe PATH_ID IP [--probe PATH_ID IP ...] [options]\n", program);
    printf("       %s --yaml FILE [--intent INTENT_ID] [options]\n", program);
    printf("  --source NODE              source node label\n");
    printf("  --count N                  number of probes, default 1\n");
    printf("  --interval-ms N            interval between probes, default 1000\n");
    printf("  --output FILE              JSONL output, default stdout\n");
    printf("  --append                   append JSONL to --output instead of replacing it\n");
    printf("  --simulate RTT LOSS        deterministic test output instead of ping\n");
    printf("  --probe PATH_ID IP         add a path/endpoint probe; may be repeated\n");
    printf("  --yaml FILE                derive path endpoints from YAML\n");
    printf("  --intent INTENT_ID         limit YAML probes to an Intent's candidates\n");
}

static bool valid_target(const char *target)
{
    if (target == NULL || target[0] == '\0') {
        return false;
    }
    for (const char *cursor = target; *cursor != '\0'; cursor++) {
        if (!( (*cursor >= '0' && *cursor <= '9') || *cursor == '.' || *cursor == ':' )) {
            return false;
        }
    }
    return true;
}

static bool valid_label(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == ':' || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-')) {
            return false;
        }
    }
    return true;
}

static bool parse_integer_argument(const char *text, long minimum, long maximum, int *value)
{
    char *end = NULL;
    long parsed;
    if (text == NULL || text[0] == '\0') return false;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed < minimum || parsed > maximum) return false;
    *value = (int)parsed;
    return true;
}

static bool parse_decimal_argument(const char *text, double minimum, double maximum, double *value)
{
    char *end = NULL;
    double parsed;
    if (text == NULL || text[0] == '\0') return false;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed) || parsed < minimum || parsed > maximum) return false;
    *value = parsed;
    return true;
}

static bool parse_ping_rtt(const char *text, double *value)
{
    char *end = NULL;
    double parsed;
    if (text == NULL || text[0] == '\0') return false;
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || !isfinite(parsed) || parsed < 0.0 || parsed > 86400000.0) return false;
    while (isspace((unsigned char)*end)) end++;
    if (end[0] != 'm' || end[1] != 's') return false;
    end += 2;
    while (isspace((unsigned char)*end)) end++;
    if (*end != '\0') return false;
    *value = parsed;
    return true;
}

static long long now_ms(void)
{
    struct timespec timestamp;
    if (timespec_get(&timestamp, TIME_UTC) != TIME_UTC) {
        return (long long)time(NULL) * 1000;
    }
    return (long long)timestamp.tv_sec * 1000 + timestamp.tv_nsec / 1000000;
}

static bool measure_ping(const char *target, double *rtt_ms, double *loss_percent)
{
#if defined(_WIN32)
    char command[256];
    if (snprintf(command, sizeof(command), "ping -n 1 -w 1000 %s 2>&1", target) >= (int)sizeof(command)) {
        *rtt_ms = 0.0;
        *loss_percent = 100.0;
        return false;
    }
    FILE *pipe = EN_POPEN(command, "r");
    if (pipe == NULL) {
        *rtt_ms = 0.0;
        *loss_percent = 100.0;
        return false;
    }

    char line[512];
    bool success = false;
    double measured_rtt = 0.0;
    while (fgets(line, sizeof(line), pipe) != NULL) {
        char *time_value = strstr(line, "time=");
        bool less_than_one_ms = strstr(line, "time<1ms") != NULL;
        if (time_value != NULL) {
            double parsed_rtt = 0.0;
            if (parse_ping_rtt(time_value + 5, &parsed_rtt)) {
                measured_rtt = parsed_rtt;
                success = true;
            }
        } else if (less_than_one_ms) {
            measured_rtt = 0.5;
            success = true;
        }
    }
    int status = EN_PCLOSE(pipe);
    if (status != 0) {
        success = false;
    }
#else
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        *rtt_ms = 0.0;
        *loss_percent = 100.0;
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        *rtt_ms = 0.0;
        *loss_percent = 100.0;
        return false;
    }
    if (child == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 || dup2(pipe_fds[1], STDERR_FILENO) < 0) _exit(127);
        close(pipe_fds[1]);
        execlp("ping", "ping", "-c", "1", "-W", "1", target, (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);
    FILE *pipe = fdopen(pipe_fds[0], "r");
    if (pipe == NULL) {
        close(pipe_fds[0]);
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        *rtt_ms = 0.0;
        *loss_percent = 100.0;
        return false;
    }
    char line[512];
    bool success = false;
    double measured_rtt = 0.0;
    while (fgets(line, sizeof(line), pipe) != NULL) {
        char *time_value = strstr(line, "time=");
        if (time_value != NULL) {
            double parsed_rtt = 0.0;
            if (parse_ping_rtt(time_value + 5, &parsed_rtt)) {
                measured_rtt = parsed_rtt;
                success = true;
            }
        }
    }
    fclose(pipe);
    int status = 0;
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) success = false;
#endif
    *rtt_ms = success ? measured_rtt : 0.0;
    *loss_percent = success ? 0.0 : 100.0;
    return success;
}

static void write_measurement(FILE *output, const agent_options_t *options, const char *path_id, const char *target,
    int sequence, double rtt_ms, double loss_percent, double jitter_ms, int consecutive_successes, int consecutive_failures)
{
    const char *state = loss_percent >= 100.0 ? "failed" : "healthy";
    fprintf(output, "{\"schema\":\"ibuki.telemetry.path_health.v1\",\"path_id\":\"%s\"", path_id);
    if (options->source != NULL) fprintf(output, ",\"source\":\"%s\"", options->source);
    fprintf(output,
        ",\"target\":\"%s\",\"sequence\":%d,\"rtt_ms\":%.3f,\"packet_loss_percent\":%.3f,\"jitter_ms\":%.3f,\"state\":\"%s\",\"consecutive_successes\":%d,\"consecutive_failures\":%d,\"timestamp_ms\":%lld}\n",
        target, sequence, rtt_ms, loss_percent, jitter_ms, state, consecutive_successes, consecutive_failures, now_ms());
    fflush(output);
}

static void wait_ms(int milliseconds)
{
#if defined(_WIN32)
    (void)milliseconds;
#else
    if (milliseconds > 0) {
        struct timespec delay = {
            .tv_sec = milliseconds / 1000,
            .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
        };
        nanosleep(&delay, NULL);
    }
#endif
}

static FILE *open_agent_output(const char *filename, bool append)
{
#if defined(_WIN32)
    return fopen(filename, append ? "a" : "w");
#else
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (append ? O_APPEND : 0);
    int output_fd = open(filename, flags, 0600);
    if (output_fd < 0) return NULL;
    struct stat output_stat;
    if (fstat(output_fd, &output_stat) != 0 || !S_ISREG(output_stat.st_mode) ||
        (output_stat.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(output_fd);
        errno = EPERM;
        return NULL;
    }
    if (!append && ftruncate(output_fd, 0) != 0) {
        close(output_fd);
        return NULL;
    }
    FILE *output = fdopen(output_fd, append ? "a" : "w");
    if (output == NULL) close(output_fd);
    return output;
#endif
}

int main(int argc, char **argv)
{
    agent_options_t options = {
        .interval_ms = 1000,
        .count = 1,
        .simulated_rtt_ms = 10.0,
        .simulated_loss = 0.0,
    };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) options.path_id = argv[++i];
        else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) options.source = argv[++i];
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) options.target = argv[++i];
        else if (strcmp(argv[i], "--yaml") == 0 && i + 1 < argc) options.yaml_file = argv[++i];
        else if (strcmp(argv[i], "--intent") == 0 && i + 1 < argc) options.intent_id = argv[++i];
        else if (strcmp(argv[i], "--probe") == 0 && i + 2 < argc) {
            if (options.probe_count >= EN_MAX_PATHS) {
                fprintf(stderr, "too many probes\n");
                return 2;
            }
            options.probes[options.probe_count].path_id = argv[++i];
            options.probes[options.probe_count].source = options.source;
            options.probes[options.probe_count].target = argv[++i];
            options.probe_count++;
        }
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) options.output = argv[++i];
        else if (strcmp(argv[i], "--append") == 0) options.append_output = true;
        else if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
            if (!parse_integer_argument(argv[++i], 1, INT_MAX, &options.count)) {
                fprintf(stderr, "invalid --count argument\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            if (!parse_integer_argument(argv[++i], 0, INT_MAX, &options.interval_ms)) {
                fprintf(stderr, "invalid --interval-ms argument\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "--simulate") == 0 && i + 2 < argc) {
            options.simulate = true;
            if (!parse_decimal_argument(argv[++i], 0.0, 86400000.0, &options.simulated_rtt_ms) ||
                !parse_decimal_argument(argv[++i], 0.0, 100.0, &options.simulated_loss)) {
                fprintf(stderr, "invalid --simulate arguments\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if ((options.yaml_file != NULL && options.probe_count > 0) ||
        (options.yaml_file != NULL && (options.path_id != NULL || options.target != NULL)) ||
        (options.yaml_file != NULL && options.source != NULL) ||
        (options.intent_id != NULL && options.yaml_file == NULL) ||
        (options.probe_count == 0 && options.yaml_file == NULL &&
            (options.path_id == NULL || !valid_label(options.path_id) || !valid_target(options.target))) ||
        (options.probe_count > 0 && (options.path_id != NULL || options.target != NULL)) ||
        (options.source != NULL && !valid_label(options.source)) || options.count <= 0 || options.interval_ms < 0) {
        usage(argv[0]);
        return 2;
    }
    if (options.append_output && options.output == NULL) {
        fprintf(stderr, "--append requires --output\n");
        return 2;
    }
    en_yaml_config_t yaml_config = {0};
    if (options.yaml_file != NULL) {
        char error[256] = {0};
        if (en_yaml_config_load_file(options.yaml_file, &yaml_config, error, sizeof(error)) != EN_ERR_NONE) {
            fprintf(stderr, "failed to load YAML: %s\n", error);
            return 2;
        }
        const en_intent_t *selected_intent = NULL;
        if (options.intent_id != NULL) {
            for (size_t intent_index = 0; intent_index < yaml_config.intent_count; intent_index++) {
                if (strcmp(yaml_config.intents[intent_index].intent_id, options.intent_id) == 0) {
                    selected_intent = &yaml_config.intents[intent_index];
                    break;
                }
            }
            if (selected_intent == NULL) {
                fprintf(stderr, "unknown YAML intent: %s\n", options.intent_id);
                return 2;
            }
        }
        for (size_t path_index = 0; path_index < yaml_config.path_count; path_index++) {
            const en_path_t *path = &yaml_config.paths[path_index];
            bool selected = selected_intent == NULL;
            if (selected_intent != NULL) {
                if (selected_intent->path_selection.mode == EN_SELECT_EXPLICIT) {
                    selected = strcmp(selected_intent->path_selection.path_id, path->path_id) == 0;
                } else {
                    for (size_t candidate_index = 0; candidate_index < selected_intent->path_selection.candidate_count; candidate_index++) {
                        if (strcmp(selected_intent->path_selection.candidates[candidate_index], path->path_id) == 0) selected = true;
                    }
                }
            }
            if (!selected) continue;
            if (options.probe_count >= EN_MAX_PATHS) {
                fprintf(stderr, "too many YAML probe paths\n");
                return 2;
            }
            const en_tunnel_t *tunnel = NULL;
            if (path->segment_count > 0) {
                const en_segment_t *terminal_segment = &path->segments[path->segment_count - 1];
                for (size_t tunnel_index = 0; tunnel_index < yaml_config.tunnel_count; tunnel_index++) {
                    if (strcmp(yaml_config.tunnels[tunnel_index].tunnel_id, terminal_segment->tunnel_id) == 0) {
                        tunnel = &yaml_config.tunnels[tunnel_index];
                        break;
                    }
                }
            }
            const char *target = tunnel == NULL ? path->route_next_hop : tunnel->remote_endpoint;
            if (!valid_target(target)) {
                fprintf(stderr, "YAML path has invalid probe endpoint: %s\n", path->path_id);
                return 2;
            }
            options.probes[options.probe_count].path_id = path->path_id;
            options.probes[options.probe_count].source = path->source;
            options.probes[options.probe_count].target = target;
            options.probe_count++;
        }
        if (options.probe_count == 0) {
            fprintf(stderr, "YAML produced no probe paths\n");
            return 2;
        }
    }
    for (size_t probe_index = 0; probe_index < options.probe_count; probe_index++) {
        if (options.probes[probe_index].path_id == NULL || !valid_label(options.probes[probe_index].path_id) ||
            !valid_target(options.probes[probe_index].target)) {
            fprintf(stderr, "invalid probe target\n");
            return 2;
        }
        for (size_t previous_index = 0; previous_index < probe_index; previous_index++) {
            if (strcmp(options.probes[previous_index].path_id, options.probes[probe_index].path_id) == 0) {
                fprintf(stderr, "duplicate probe path\n");
                return 2;
            }
        }
    }

    FILE *output = stdout;
    if (options.output != NULL) {
        output = open_agent_output(options.output, options.append_output);
        if (output == NULL) {
            perror("failed to open output");
            return 1;
        }
    }
    for (int sequence = 1; sequence <= options.count; sequence++) {
        static double previous_rtt[EN_MAX_PATHS];
        static bool has_previous_rtt[EN_MAX_PATHS];
        static int consecutive_successes[EN_MAX_PATHS];
        static int consecutive_failures[EN_MAX_PATHS];
        size_t probe_count = options.probe_count == 0 ? 1 : options.probe_count;
        for (size_t probe_index = 0; probe_index < probe_count; probe_index++) {
            const char *path_id = options.probe_count == 0 ? options.path_id : options.probes[probe_index].path_id;
            const char *target = options.probe_count == 0 ? options.target : options.probes[probe_index].target;
            const char *source = options.probe_count == 0 ? options.source : options.probes[probe_index].source;
            double rtt_ms = 0.0;
            double loss_percent = 100.0;
            if (options.simulate) {
                rtt_ms = options.simulated_rtt_ms;
                loss_percent = options.simulated_loss;
            } else {
                measure_ping(target, &rtt_ms, &loss_percent);
            }
            size_t jitter_index = options.probe_count == 0 ? 0 : probe_index;
            double jitter_ms = has_previous_rtt[jitter_index] && loss_percent < 100.0 && rtt_ms > 0.0 ?
                rtt_ms > previous_rtt[jitter_index] ? rtt_ms - previous_rtt[jitter_index] : previous_rtt[jitter_index] - rtt_ms : 0.0;
            if (loss_percent < 100.0 && rtt_ms > 0.0) {
                previous_rtt[jitter_index] = rtt_ms;
                has_previous_rtt[jitter_index] = true;
            }
            if (loss_percent >= 100.0) {
                consecutive_failures[jitter_index]++;
                consecutive_successes[jitter_index] = 0;
            } else {
                consecutive_successes[jitter_index]++;
                consecutive_failures[jitter_index] = 0;
            }
            agent_options_t output_options = options;
            output_options.source = source;
            write_measurement(output, &output_options, path_id, target, sequence, rtt_ms, loss_percent, jitter_ms,
                consecutive_successes[jitter_index], consecutive_failures[jitter_index]);
        }
        if (sequence < options.count) wait_ms(options.interval_ms);
    }
    if (options.output != NULL) fclose(output);
    return 0;
}
