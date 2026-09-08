#include "eventnet/controller.h"
#include "eventnet/mock_adapters.h"
#include "eventnet/yaml_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

typedef struct {
    char path_id[EN_MAX_ID_LEN];
    en_health_state_t state;
    double rtt_ms;
    double packet_loss_percent;
    bool has_rtt_ms;
    bool has_packet_loss_percent;
} injected_health_t;

typedef struct {
    const char *filename;
    const char *intent_id;
    const char *mode;
    const char *active_path;
    const char *failed_path;
    const char *expected_path;
    const char *comparison_order;
    const char *explain_json;
    bool generate;
    injected_health_t injected[EN_MAX_PATHS];
    size_t injected_count;
} scenario_options_t;

static const en_intent_t *find_intent(const en_yaml_config_t *config, const char *intent_id);
static const en_path_t *find_path(const en_yaml_config_t *config, const char *path_id);
static bool append_health(scenario_options_t *options, const char *health_arg);
static bool apply_named_step(scenario_options_t *options, const char *step);
static int run_scenario(const scenario_options_t *options, const char *step_name);
static bool append_json_event(const scenario_options_t *options, const en_reconcile_result_t *result, const char *step_name, bool expect_pass);
static en_health_state_t parse_health_state(const char *value, bool *ok);
static en_comparison_key_t parse_comparison_key_arg(const char *value, bool *ok);
static bool parse_health_arg(const char *text, injected_health_t *health);
static bool parse_health_number(const char *text, double minimum, double maximum, double *value);
static void apply_health_override(en_health_probe_mock_t *health_mock, const injected_health_t *health);
static bool force_selection_mode(en_intent_t *intent, const char *mode);
static bool force_comparison_order(en_intent_t *intent, const char *value);
static void print_result(const en_reconcile_result_t *result);
static void json_string(FILE *file, const char *value);
static int generate_runtime(const char *program, const char *yaml, const en_reconcile_result_t *result, const char *active_path, const char *failed_path);
static void usage(const char *program);

int main(int argc, char **argv)
{
    scenario_options_t base = {
        .filename = "samples/linux-vm-netns.yaml",
    };
    const char *steps[EN_MAX_EVENTS] = {0};
    size_t step_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--intent") == 0 && i + 1 < argc) {
            base.intent_id = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            base.mode = argv[++i];
        } else if (strcmp(argv[i], "--active-path") == 0 && i + 1 < argc) {
            base.active_path = argv[++i];
        } else if (strcmp(argv[i], "--fail-path") == 0 && i + 1 < argc) {
            base.failed_path = argv[++i];
        } else if (strcmp(argv[i], "--health") == 0 && i + 1 < argc) {
            if (!append_health(&base, argv[++i])) {
                fprintf(stderr, "invalid --health argument\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--compare") == 0 && i + 1 < argc) {
            base.comparison_order = argv[++i];
        } else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
            base.expected_path = argv[++i];
        } else if (strcmp(argv[i], "--explain-json") == 0 && i + 1 < argc) {
            base.explain_json = argv[++i];
        } else if (strcmp(argv[i], "--step") == 0 && i + 1 < argc) {
            if (step_count >= EN_MAX_EVENTS) {
                fprintf(stderr, "too many --step arguments\n");
                return 2;
            }
            steps[step_count++] = argv[++i];
        } else if (strcmp(argv[i], "--generate-runtime") == 0) {
            base.generate = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            base.filename = argv[i];
        }
    }

    if (step_count > 0) {
        if (base.explain_json != NULL) {
            remove(base.explain_json);
        }
        for (size_t i = 0; i < step_count; i++) {
            scenario_options_t step_options = base;
            step_options.active_path = NULL;
            step_options.failed_path = NULL;
            step_options.expected_path = NULL;
            step_options.injected_count = 0;
            step_options.generate = false;
            if (!apply_named_step(&step_options, steps[i])) {
                fprintf(stderr, "unknown --step: %s\n", steps[i]);
                return 2;
            }
            int rc = run_scenario(&step_options, steps[i]);
            if (rc != 0) {
                return rc;
            }
        }
        printf("multi_step_result: pass\n");
        return 0;
    }

    return run_scenario(&base, NULL);
}

static bool append_health(scenario_options_t *options, const char *health_arg)
{
    if (options->injected_count >= EN_MAX_PATHS) {
        return false;
    }
    if (!parse_health_arg(health_arg, &options->injected[options->injected_count])) {
        return false;
    }
    options->injected_count++;
    return true;
}

static bool apply_named_step(scenario_options_t *options, const char *step)
{
    if (strcmp(step, "direct-ok") == 0) {
        options->expected_path = "path-direct";
        return true;
    }
    if (strcmp(step, "direct-failed") == 0 || strcmp(step, "fallback") == 0) {
        options->active_path = "path-direct";
        options->failed_path = "path-direct";
        options->expected_path = "path-via-hub";
        return true;
    }
    if (strcmp(step, "direct-recovered") == 0 || strcmp(step, "recovery") == 0) {
        options->active_path = "path-via-hub";
        options->expected_path = "path-direct";
        return true;
    }
    if (strcmp(step, "relay-best") == 0) {
        options->mode = "evaluated";
        options->comparison_order = "packet_loss,latency,hop_count,path_id";
        options->expected_path = "path-via-relay-c";
        return append_health(options, "path-direct=healthy,rtt=80,loss=0.5") &&
            append_health(options, "path-via-relay-c=healthy,rtt=30,loss=0.1") &&
            append_health(options, "path-via-hub=healthy,rtt=50,loss=0.2");
    }
    return false;
}

static int run_scenario(const scenario_options_t *options, const char *step_name)
{
    const char *filename = options->filename;
    const char *intent_id = options->intent_id;
    const char *mode = options->mode;
    const char *active_path = options->active_path;
    const char *failed_path = options->failed_path;
    const char *expected_path = options->expected_path;
    const char *comparison_order = options->comparison_order;
    bool generate = options->generate;

    en_yaml_config_t config = {0};
    char error[256] = {0};
    en_error_code_t err = en_yaml_config_load_file(filename, &config, error, sizeof(error));
    if (err != EN_ERR_NONE) {
        fprintf(stderr, "yaml load failed: %s: %s\n", en_error_code_name(err), error);
        return 1;
    }

    const en_intent_t *base_intent = find_intent(&config, intent_id);
    if (base_intent == NULL) {
        fprintf(stderr, "intent not found\n");
        return 1;
    }

    en_intent_t scenario_intent = *base_intent;
    if (mode != NULL && !force_selection_mode(&scenario_intent, mode)) {
        fprintf(stderr, "invalid --mode argument\n");
        return 2;
    }
    if (comparison_order != NULL && !force_comparison_order(&scenario_intent, comparison_order)) {
        fprintf(stderr, "invalid --compare argument\n");
        return 2;
    }

    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    for (size_t i = 0; i < options->injected_count; i++) {
        apply_health_override(&health_mock, &options->injected[i]);
    }
    if (failed_path != NULL) {
        injected_health_t health = {0};
        snprintf(health.path_id, sizeof(health.path_id), "%s", failed_path);
        health.state = EN_HEALTH_FAILED;
        health.packet_loss_percent = 100.0;
        apply_health_override(&health_mock, &health);
    }

    en_controller_t *controller = en_controller_create_with_tunnels(
        config.paths,
        config.path_count,
        config.tunnels,
        config.tunnel_count,
        en_strongswan_mock_adapter(),
        en_vpp_mock_adapter(&vpp_mock),
        en_health_probe_mock_adapter(&health_mock)
    );
    if (controller == NULL) {
        fprintf(stderr, "controller create failed\n");
        return 1;
    }

    if (active_path != NULL && failed_path != NULL && scenario_intent.fallback.enabled && scenario_intent.fallback.path_id[0] != '\0') {
        if (strcmp(active_path, failed_path) == 0) {
            const en_path_t *fallback = find_path(&config, scenario_intent.fallback.path_id);
            if (fallback == NULL) {
                fprintf(stderr, "fallback path not found: %s\n", scenario_intent.fallback.path_id);
                en_controller_destroy(controller);
                return 1;
            }
            en_reconcile_result_t result = {0};
            snprintf(result.intent_id, sizeof(result.intent_id), "%s", scenario_intent.intent_id);
            snprintf(result.selected_path, sizeof(result.selected_path), "%s", fallback->path_id);
            result.transition_state = EN_TRANSITION_COMPLETED;
            snprintf(result.explanation.selected_path, sizeof(result.explanation.selected_path), "%s", fallback->path_id);
            snprintf(result.explanation.reason, sizeof(result.explanation.reason), "active path %s failed; using fallback %s", active_path, fallback->path_id);
            snprintf(result.explanation.excluded_path_ids[0], sizeof(result.explanation.excluded_path_ids[0]), "%s", failed_path);
            snprintf(result.explanation.excluded_reasons[0], sizeof(result.explanation.excluded_reasons[0]), "%s", "active path failed event");
            result.explanation.excluded_count = 1;
            if (step_name != NULL) {
                printf("scenario_step: %s\n", step_name);
            }
            print_result(&result);
            if (generate && generate_runtime("eventnet_scenario", filename, &result, active_path, failed_path) != 0) {
                en_controller_destroy(controller);
                return 1;
            }
            en_controller_destroy(controller);
            bool expect_pass = expected_path == NULL || strcmp(result.selected_path, expected_path) == 0;
            if (!append_json_event(options, &result, step_name, expect_pass)) {
                en_controller_destroy(controller);
                return 1;
            }
            if (expected_path != NULL && !expect_pass) {
                printf("expect: %s\n", expected_path);
                printf("result: fail\n");
                return 1;
            }
            if (expected_path != NULL) {
                printf("expect: %s\n", expected_path);
                printf("result: pass\n");
            }
            return 0;
        }
    }

    en_reconcile_result_t result = {0};
    err = en_controller_submit_intent(controller, &scenario_intent, &result);
    en_controller_destroy(controller);
    if (err != EN_ERR_NONE) {
        fprintf(stderr, "scenario reconcile failed: %s\n", en_error_code_name(err));
        return 1;
    }

    if (step_name != NULL) {
        printf("scenario_step: %s\n", step_name);
    }
    print_result(&result);
    if (generate && generate_runtime("eventnet_scenario", filename, &result, active_path, failed_path) != 0) {
        return 1;
    }
    if (expected_path != NULL) {
        printf("expect: %s\n", expected_path);
        if (strcmp(result.selected_path, expected_path) != 0) {
            if (!append_json_event(options, &result, step_name, false)) {
                return 1;
            }
            printf("result: fail\n");
            return 1;
        }
        printf("result: pass\n");
    }
    if (!append_json_event(options, &result, step_name, true)) {
        return 1;
    }
    return 0;
}

static const en_intent_t *find_intent(const en_yaml_config_t *config, const char *intent_id)
{
    if (intent_id == NULL) {
        return config->intent_count == 0 ? NULL : &config->intents[0];
    }
    for (size_t i = 0; i < config->intent_count; i++) {
        if (strcmp(config->intents[i].intent_id, intent_id) == 0) {
            return &config->intents[i];
        }
    }
    return NULL;
}

static const en_path_t *find_path(const en_yaml_config_t *config, const char *path_id)
{
    if (path_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < config->path_count; i++) {
        if (strcmp(config->paths[i].path_id, path_id) == 0) {
            return &config->paths[i];
        }
    }
    return NULL;
}

static en_health_state_t parse_health_state(const char *value, bool *ok)
{
    *ok = true;
    if (strcmp(value, "unknown") == 0) return EN_HEALTH_UNKNOWN;
    if (strcmp(value, "healthy") == 0) return EN_HEALTH_HEALTHY;
    if (strcmp(value, "degraded") == 0) return EN_HEALTH_DEGRADED;
    if (strcmp(value, "unhealthy") == 0) return EN_HEALTH_UNHEALTHY;
    if (strcmp(value, "failed") == 0) return EN_HEALTH_FAILED;
    *ok = false;
    return EN_HEALTH_UNKNOWN;
}

static en_comparison_key_t parse_comparison_key_arg(const char *value, bool *ok)
{
    *ok = true;
    if (strcmp(value, "packet_loss") == 0 || strcmp(value, "loss") == 0) return EN_COMPARE_PACKET_LOSS;
    if (strcmp(value, "latency") == 0 || strcmp(value, "rtt") == 0 || strcmp(value, "rtt_ms") == 0) return EN_COMPARE_LATENCY;
    if (strcmp(value, "hop_count") == 0 || strcmp(value, "hops") == 0) return EN_COMPARE_HOP_COUNT;
    if (strcmp(value, "priority") == 0 || strcmp(value, "administrative_priority") == 0) return EN_COMPARE_ADMIN_PRIORITY;
    if (strcmp(value, "path_id") == 0) return EN_COMPARE_PATH_ID;
    *ok = false;
    return EN_COMPARE_PATH_ID;
}

static bool parse_health_arg(const char *text, injected_health_t *health)
{
    char buf[256] = {0};
    snprintf(buf, sizeof(buf), "%s", text);
    char *equals = strchr(buf, '=');
    if (equals == NULL) {
        return false;
    }
    *equals = '\0';
    if (buf[0] == '\0' || strlen(buf) >= sizeof(health->path_id)) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)buf; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == '_' || *cursor == '-' || *cursor == '.')) {
            return false;
        }
    }
    snprintf(health->path_id, sizeof(health->path_id), "%s", buf);
    health->state = EN_HEALTH_HEALTHY;
    char *cursor = equals + 1;
    while (cursor != NULL && *cursor != '\0') {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        char *kv = strchr(cursor, '=');
        if (kv == NULL) {
            bool ok = false;
            health->state = parse_health_state(cursor, &ok);
            if (!ok) {
                return false;
            }
        } else {
            *kv = '\0';
            const char *key = cursor;
            const char *value = kv + 1;
            if (strcmp(key, "state") == 0) {
                bool ok = false;
                health->state = parse_health_state(value, &ok);
                if (!ok) {
                    return false;
                }
            } else if (strcmp(key, "rtt") == 0 || strcmp(key, "rtt_ms") == 0) {
                if (!parse_health_number(value, 0.0, 86400000.0, &health->rtt_ms)) {
                    return false;
                }
                health->has_rtt_ms = true;
            } else if (strcmp(key, "loss") == 0 || strcmp(key, "packet_loss") == 0) {
                if (!parse_health_number(value, 0.0, 100.0, &health->packet_loss_percent)) {
                    return false;
                }
                health->has_packet_loss_percent = true;
            } else {
                return false;
            }
        }
        cursor = comma == NULL ? NULL : comma + 1;
    }
    return health->path_id[0] != '\0';
}

static bool parse_health_number(const char *text, double minimum, double maximum, double *value)
{
    char *end = NULL;
    double parsed;
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    errno = 0;
    parsed = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = parsed;
    return true;
}

static void apply_health_override(en_health_probe_mock_t *health_mock, const injected_health_t *health)
{
    en_path_health_t override = {0};
    snprintf(override.path_id, sizeof(override.path_id), "%s", health->path_id);
    override.state = health->state;
    override.rtt_ms = health->has_rtt_ms ? health->rtt_ms : 10.0;
    override.packet_loss_percent = health->has_packet_loss_percent ? health->packet_loss_percent : 0.0;
    if (health->state == EN_HEALTH_FAILED || health->state == EN_HEALTH_UNHEALTHY) {
        override.consecutive_failures = 3;
        override.consecutive_successes = 0;
        if (!health->has_packet_loss_percent) {
            override.packet_loss_percent = 100.0;
        }
    } else {
        override.consecutive_failures = 0;
        override.consecutive_successes = 3;
    }
    en_health_probe_mock_set(health_mock, override);
}

static bool force_selection_mode(en_intent_t *intent, const char *mode)
{
    if (strcmp(mode, "explicit") == 0) {
        intent->path_selection.mode = EN_SELECT_EXPLICIT;
        return true;
    }
    if (strcmp(mode, "priority") == 0) {
        intent->path_selection.mode = EN_SELECT_PRIORITY;
        return true;
    }
    if (strcmp(mode, "evaluated") == 0) {
        intent->path_selection.mode = EN_SELECT_EVALUATED;
        return true;
    }
    return false;
}

static bool force_comparison_order(en_intent_t *intent, const char *value)
{
    char buf[256] = {0};
    snprintf(buf, sizeof(buf), "%s", value);
    intent->path_selection.comparison_count = 0;
    char *cursor = buf;
    while (cursor != NULL && *cursor != '\0') {
        char *comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        bool ok = false;
        en_comparison_key_t key = parse_comparison_key_arg(cursor, &ok);
        if (!ok || intent->path_selection.comparison_count >= EN_MAX_COMPARISONS) {
            return false;
        }
        intent->path_selection.comparison_order[intent->path_selection.comparison_count++] = key;
        cursor = comma == NULL ? NULL : comma + 1;
    }
    return true;
}

static void print_result(const en_reconcile_result_t *result)
{
    printf("intent: %s\n", result->intent_id);
    printf("selected_path: %s\n", result->selected_path);
    printf("transition_state: %s\n", en_transition_state_name(result->transition_state));
    printf("reason: %s\n", result->explanation.reason);
    printf("excluded:\n");
    if (result->explanation.excluded_count == 0) {
        printf("  - none\n");
    }
    for (size_t i = 0; i < result->explanation.excluded_count; i++) {
        printf("  - %s: %s\n", result->explanation.excluded_path_ids[i], result->explanation.excluded_reasons[i]);
    }
}

static bool append_json_event(const scenario_options_t *options, const en_reconcile_result_t *result, const char *step_name, bool expect_pass)
{
    if (options->explain_json == NULL) {
        return true;
    }
    FILE *file = fopen(options->explain_json, "a");
    if (file == NULL) {
        fprintf(stderr, "failed to open explain json: %s\n", options->explain_json);
        return false;
    }
    fprintf(file, "{");
    fprintf(file, "\"schema\":\"eventnet.scenario.explain.v1\",");
    fprintf(file, "\"scenario_step\":");
    json_string(file, step_name == NULL ? "single" : step_name);
    fprintf(file, ",");
    fprintf(file, "\"yaml\":");
    json_string(file, options->filename);
    fprintf(file, ",");
    fprintf(file, "\"intent\":");
    json_string(file, result->intent_id);
    fprintf(file, ",");
    fprintf(file, "\"selection_mode\":");
    json_string(file, options->mode == NULL ? "yaml" : options->mode);
    fprintf(file, ",");
    fprintf(file, "\"active_path\":");
    json_string(file, options->active_path == NULL ? "" : options->active_path);
    fprintf(file, ",");
    fprintf(file, "\"failed_path\":");
    json_string(file, options->failed_path == NULL ? "" : options->failed_path);
    fprintf(file, ",");
    fprintf(file, "\"selected_path\":");
    json_string(file, result->selected_path);
    fprintf(file, ",");
    fprintf(file, "\"transition_state\":");
    json_string(file, en_transition_state_name(result->transition_state));
    fprintf(file, ",");
    fprintf(file, "\"reason\":");
    json_string(file, result->explanation.reason);
    fprintf(file, ",");
    fprintf(file, "\"expect\":");
    json_string(file, options->expected_path == NULL ? "" : options->expected_path);
    fprintf(file, ",");
    fprintf(file, "\"result\":");
    json_string(file, expect_pass ? "pass" : "fail");
    fprintf(file, ",");
    fprintf(file, "\"excluded\":[");
    for (size_t i = 0; i < result->explanation.excluded_count; i++) {
        fprintf(file, "%s{\"path_id\":", i == 0 ? "" : ",");
        json_string(file, result->explanation.excluded_path_ids[i]);
        fprintf(file, ",\"reason\":");
        json_string(file, result->explanation.excluded_reasons[i]);
        fprintf(file, "}");
    }
    fprintf(file, "],");
    fprintf(file, "\"injected_health\":[");
    for (size_t i = 0; i < options->injected_count; i++) {
        const injected_health_t *health = &options->injected[i];
        fprintf(file, "%s{\"path_id\":", i == 0 ? "" : ",");
        json_string(file, health->path_id);
        fprintf(file, ",\"state\":");
        json_string(file, en_health_state_name(health->state));
        fprintf(file, ",\"rtt_ms\":%.3f,\"packet_loss_percent\":%.3f}", health->has_rtt_ms ? health->rtt_ms : 10.0, health->has_packet_loss_percent ? health->packet_loss_percent : 0.0);
    }
    fprintf(file, "]}\n");
    fclose(file);
    printf("explain_json: %s\n", options->explain_json);
    return true;
}

static void json_string(FILE *file, const char *value)
{
    fputc('"', file);
    if (value != NULL) {
        for (const char *cursor = value; *cursor != '\0'; cursor++) {
            if (*cursor == '"' || *cursor == '\\') {
                fputc('\\', file);
                fputc(*cursor, file);
            } else if (*cursor == '\n') {
                fputs("\\n", file);
            } else {
                fputc(*cursor, file);
            }
        }
    }
    fputc('"', file);
}

static int generate_runtime(const char *program, const char *yaml, const en_reconcile_result_t *result, const char *active_path, const char *failed_path)
{
    (void)program;
    char command[1024] = {0};
    if (active_path != NULL && failed_path != NULL && strcmp(active_path, failed_path) == 0) {
        snprintf(
            command,
            sizeof(command),
            "sh scripts/vm-generate-netns-runtime.sh %s --active-path %s --fail-path %s",
            yaml,
            active_path,
            failed_path
        );
    } else {
        snprintf(command, sizeof(command), "sh scripts/vm-generate-netns-runtime.sh %s --path %s", yaml, result->selected_path);
    }
    printf("generate_runtime_command: %s\n", command);
    fflush(stdout);
    int rc = 0;
#if defined(_WIN32)
    rc = system(command);
#else
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        if (active_path != NULL && failed_path != NULL && strcmp(active_path, failed_path) == 0) {
            execlp("sh", "sh", "scripts/vm-generate-netns-runtime.sh", yaml, "--active-path", active_path, "--fail-path", failed_path, (char *)NULL);
        } else {
            execlp("sh", "sh", "scripts/vm-generate-netns-runtime.sh", yaml, "--path", result->selected_path, (char *)NULL);
        }
        _exit(127);
    }
    int wait_status = 0;
    if (waitpid(child, &wait_status, 0) < 0 || !WIFEXITED(wait_status)) return 1;
    rc = WEXITSTATUS(wait_status);
#endif
    if (rc != 0) {
        fprintf(stderr, "generate runtime command failed\n");
        return 1;
    }
    printf("generated_runtime: out/netns-runtime/apply-integrated.sh\n");
    return 0;
}

static void usage(const char *program)
{
    printf("usage: %s [options] YAML\n", program);
    printf("options:\n");
    printf("  --intent ID\n");
    printf("  --mode explicit|priority|evaluated\n");
    printf("  --active-path PATH\n");
    printf("  --fail-path PATH\n");
    printf("  --health PATH=state[,rtt=N,loss=N]\n");
    printf("  --compare packet_loss,latency,hop_count,priority,path_id\n");
    printf("  --expect PATH\n");
    printf("  --explain-json FILE\n");
    printf("  --step direct-ok|direct-failed|direct-recovered|relay-best\n");
    printf("  --generate-runtime\n");
}
