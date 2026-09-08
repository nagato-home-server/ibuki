#include "eventnet/apply_plan.h"
#include "eventnet/controller.h"
#include "eventnet/mock_adapters.h"
#include "eventnet/yaml_config.h"

#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define access _access
#define R_OK 4
#else
#include <unistd.h>
#endif

static const en_path_t *find_path(const en_yaml_config_t *config, const char *path_id)
{
    for (size_t i = 0; i < config->path_count; i++) {
        if (strcmp(config->paths[i].path_id, path_id) == 0) {
            return &config->paths[i];
        }
    }
    return NULL;
}

static void usage(const char *program)
{
    printf("usage: %s [--apply] [--validate-only] [--conf FILE] [--emit-script FILE] [--check-cert-files] [--check-cert-validity SECONDS] [--intent ID] [YAML]\n", program);
    printf("  default is dry-run mode\n");
}

static bool parse_nonnegative_seconds(const char *text, long long *value)
{
    char *end = NULL;
    long long parsed;
    if (text == NULL || text[0] == '\0') return false;
    errno = 0;
    parsed = strtoll(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed < 0) return false;
    *value = parsed;
    return true;
}

int main(int argc, char **argv)
{
    const char *filename = "samples/ipsec-routes.yaml";
    const char *conf_filename = "eventnet-swanctl.conf";
    const char *script_filename = NULL;
    const char *intent_filter = NULL;
    bool dry_run = true;
    bool validate_only = false;
    bool check_cert_files = false;
    long long check_cert_validity = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--apply") == 0) {
            dry_run = false;
        } else if (strcmp(argv[i], "--validate-only") == 0) {
            validate_only = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--conf") == 0 && i + 1 < argc) {
            conf_filename = argv[++i];
        } else if (strcmp(argv[i], "--emit-script") == 0 && i + 1 < argc) {
            script_filename = argv[++i];
        } else if (strcmp(argv[i], "--check-cert-files") == 0) {
            check_cert_files = true;
        } else if (strcmp(argv[i], "--check-cert-validity") == 0 && i + 1 < argc) {
            if (!parse_nonnegative_seconds(argv[++i], &check_cert_validity)) {
                fprintf(stderr, "invalid --check-cert-validity argument\n");
                return 2;
            }
        } else if (strcmp(argv[i], "--intent") == 0 && i + 1 < argc) {
            intent_filter = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            filename = argv[i];
        }
    }

    en_yaml_config_t config = {0};
    char error[256] = {0};
    en_error_code_t err = en_yaml_config_load_file(filename, &config, error, sizeof(error));
    if (err != EN_ERR_NONE) {
        fprintf(stderr, "yaml load failed: %s: %s\n", en_error_code_name(err), error);
        return 1;
    }
    if (validate_only) {
        printf("YAML validation passed: nodes=%zu paths=%zu tunnels=%zu intents=%zu\n",
            config.node_count, config.path_count, config.tunnel_count, config.intent_count);
        return 0;
    }
    if (check_cert_files || check_cert_validity >= 0) {
        for (size_t i = 0; i < config.tunnel_count; i++) {
            const en_tunnel_t *tunnel = &config.tunnels[i];
            if (strcmp(tunnel->auth_method, "pubkey") != 0) continue;
            if (check_cert_files && access(tunnel->local_cert, R_OK) != 0) {
                fprintf(stderr, "certificate file is not readable: %s\n", tunnel->local_cert);
                return 1;
            }
            if (check_cert_files && tunnel->remote_cacerts[0] != '\0' && access(tunnel->remote_cacerts, R_OK) != 0) {
                fprintf(stderr, "CA certificate file is not readable: %s\n", tunnel->remote_cacerts);
                return 1;
            }
#if !defined(_WIN32)
            if (check_cert_validity >= 0) {
                char command[EN_MAX_ID_LEN * 2] = {0};
                snprintf(command, sizeof(command), "openssl x509 -in '%s' -noout -checkend %lld 2>/dev/null", tunnel->local_cert, check_cert_validity);
                FILE *probe = popen(command, "r");
                if (probe == NULL) return 1;
                char output[128];
                while (fgets(output, sizeof(output), probe) != NULL) { }
                int status = pclose(probe);
                if (status != 0) {
                    fprintf(stderr, "certificate is expired or expires within %lld seconds: %s\n", check_cert_validity, tunnel->local_cert);
                    return 1;
                }
                if (tunnel->remote_cacerts[0] != '\0') {
                    snprintf(command, sizeof(command), "openssl x509 -in '%s' -noout -checkend %lld 2>/dev/null", tunnel->remote_cacerts, check_cert_validity);
                    probe = popen(command, "r");
                    if (probe == NULL) return 1;
                    while (fgets(output, sizeof(output), probe) != NULL) { }
                    status = pclose(probe);
                    if (status != 0) {
                        fprintf(stderr, "CA certificate is expired or expires within %lld seconds: %s\n", check_cert_validity, tunnel->remote_cacerts);
                        return 1;
                    }
                }
            }
#elif defined(_WIN32)
            if (check_cert_validity >= 0) {
                fprintf(stderr, "--check-cert-validity is supported on Linux only\n");
                return 2;
            }
#endif
        }
        if (check_cert_files) printf("certificate files: readable\n");
        if (check_cert_validity >= 0) printf("certificate validity: at least %lld seconds\n", check_cert_validity);
    }

    en_vpp_mock_t vpp_mock = {0};
    en_health_probe_mock_t health_mock = {0};
    en_controller_t *controller = en_controller_create_with_nodes_and_tunnels(
        config.nodes,
        config.node_count,
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

    printf("loaded nodes: %zu\n", config.node_count);
    for (size_t i = 0; i < config.node_count; i++) {
        const en_node_t *node = &config.nodes[i];
        printf("  node: %s", node->node_id);
        if (node->role[0] != '\0') printf(" role=%s", node->role);
        printf(" capabilities:");
        if (node->capability_count == 0) printf(" none");
        for (size_t j = 0; j < node->capability_count; j++) printf(" %s", node->capabilities[j]);
        printf("\n");
    }
    printf("loaded paths: %zu\n", config.path_count);
    printf("loaded tunnels: %zu\n", config.tunnel_count);
    printf("loaded intents: %zu\n", config.intent_count);
    bool matched_intent = false;
    for (size_t i = 0; i < config.intent_count; i++) {
        if (intent_filter != NULL && strcmp(config.intents[i].intent_id, intent_filter) != 0) {
            continue;
        }
        matched_intent = true;
        en_reconcile_result_t result = {0};
        err = en_controller_submit_intent(controller, &config.intents[i], &result);
        if (err != EN_ERR_NONE) {
            fprintf(stderr, "intent %s failed: %s\n", config.intents[i].intent_id, en_error_code_name(err));
            en_controller_destroy(controller);
            return 1;
        }
        printf("intent: %s\n", result.intent_id);
        printf("  selected_path: %s\n", result.selected_path);
        printf("  reason: %s\n", result.explanation.reason);

        const en_path_t *selected_path = find_path(&config, result.selected_path);
        en_apply_plan_t plan = {0};
        err = en_apply_plan_from_config_with_file(&config, &config.intents[i], selected_path, conf_filename, &plan);
        if (err != EN_ERR_NONE) {
            fprintf(stderr, "apply plan failed: %s\n", en_error_code_name(err));
            en_controller_destroy(controller);
            return 1;
        }
        err = en_apply_plan_write_swanctl_conf(&plan, conf_filename);
        if (err != EN_ERR_NONE) {
            fprintf(stderr, "failed to write swanctl conf: %s\n", en_error_code_name(err));
            en_controller_destroy(controller);
            return 1;
        }
        printf("  wrote: %s\n", conf_filename);
        if (script_filename != NULL) {
            err = en_apply_plan_write_shell_script(&plan, script_filename);
            if (err != EN_ERR_NONE) {
                fprintf(stderr, "failed to write script: %s\n", en_error_code_name(err));
                en_controller_destroy(controller);
                return 1;
            }
            printf("  wrote: %s\n", script_filename);
        }
        printf("  apply_plan:\n");
        for (size_t j = 0; j < plan.command_count; j++) {
            printf("    - %s\n", plan.commands[j]);
        }
        if (plan.rollback_command_count > 0) {
            printf("  rollback_plan:\n");
            for (size_t j = 0; j < plan.rollback_command_count; j++) {
                printf("    - %s\n", plan.rollback_commands[j]);
            }
        }
        if (!dry_run) {
            err = en_apply_plan_run(&plan, false);
            if (err != EN_ERR_NONE) {
                fprintf(stderr, "apply failed: %s\n", en_error_code_name(err));
                en_controller_destroy(controller);
                return 1;
            }
        }
    }
    if (!matched_intent) {
        fprintf(stderr, "intent not found: %s\n", intent_filter == NULL ? "(none)" : intent_filter);
        en_controller_destroy(controller);
        return 1;
    }

    en_controller_destroy(controller);
    return 0;
}
