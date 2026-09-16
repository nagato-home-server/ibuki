#if !defined(_WIN32)
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "eventnet/apply_plan.h"
#include "eventnet/render_commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static const en_tunnel_t *find_tunnel(const en_yaml_config_t *config, const char *tunnel_id);
static bool append_conf(char *dst, size_t dst_len, const char *text);
static bool append_command(en_apply_plan_t *plan, const char *command);
static bool append_command_with_rollback(en_apply_plan_t *plan, const char *command, const char *rollback_command);
static bool append_rollback_command(en_apply_plan_t *plan, const char *command);
static en_error_code_t append_tunnel_conf(en_apply_plan_t *plan, const en_tunnel_t *tunnel);
static bool valid_secret(const char *value);
static bool valid_xfrm_selector(const char *value);

#if !defined(_WIN32)
static bool valid_plan_token(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-' ||
              *cursor == ':' || *cursor == '=' || *cursor == '%')) return false;
    }
    return true;
}

static int run_plan_command_without_shell(const char *command)
{
    char copy[EN_MAX_COMMAND_LEN] = {0};
    char *argv[32] = {0};
    size_t argc = 0;
    if (command == NULL || command[0] == '\0') return -1;
    if (snprintf(copy, sizeof(copy), "%s", command) >= (int)sizeof(copy)) return -1;
    for (char *token = strtok(copy, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
        if (argc + 1 >= sizeof(argv) / sizeof(argv[0]) || !valid_plan_token(token)) return -1;
        argv[argc++] = token;
    }
    if (argc == 0) return -1;
    argv[argc] = NULL;
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
#endif

en_error_code_t en_apply_plan_from_config(
    const en_yaml_config_t *config,
    const en_intent_t *intent,
    const en_path_t *selected_path,
    en_apply_plan_t *plan
)
{
    return en_apply_plan_from_config_with_file(config, intent, selected_path, "eventnet-swanctl.conf", plan);
}

en_error_code_t en_apply_plan_from_config_with_file(
    const en_yaml_config_t *config,
    const en_intent_t *intent,
    const en_path_t *selected_path,
    const char *swanctl_conf_filename,
    en_apply_plan_t *plan
)
{
    if (config == NULL || intent == NULL || selected_path == NULL || swanctl_conf_filename == NULL || plan == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }

    memset(plan, 0, sizeof(*plan));
    if (!append_conf(plan->swanctl_conf, sizeof(plan->swanctl_conf), "connections {\n")) return EN_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < selected_path->segment_count; i++) {
        const en_segment_t *segment = &selected_path->segments[i];
        const en_tunnel_t *tunnel = find_tunnel(config, segment->tunnel_id);
        if (tunnel == NULL) {
            return EN_ERR_NOT_FOUND;
        }
        en_error_code_t err = append_tunnel_conf(plan, tunnel);
        if (err != EN_ERR_NONE) {
            return err;
        }
    }

    if (!append_conf(plan->swanctl_conf, sizeof(plan->swanctl_conf), "}\n\nsecrets {\n")) return EN_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < selected_path->segment_count; i++) {
        const en_tunnel_t *tunnel = find_tunnel(config, selected_path->segments[i].tunnel_id);
        if (tunnel == NULL) {
            return EN_ERR_NOT_FOUND;
        }
        if (tunnel->psk[0] != '\0') {
            if (!valid_secret(tunnel->psk)) return EN_ERR_INVALID_ARGUMENT;
            char secret[512] = {0};
            if (snprintf(
                secret,
                sizeof(secret),
                "  ike-%s {\n"
                "    id-1 = %s\n"
                "    id-2 = %s\n"
                "    secret = \"%s\"\n"
                "  }\n",
                tunnel->tunnel_id,
                tunnel->local_id[0] == '\0' ? tunnel->local_endpoint : tunnel->local_id,
                tunnel->remote_id[0] == '\0' ? tunnel->remote_endpoint : tunnel->remote_id,
                tunnel->psk
            ) >= (int)sizeof(secret)) return EN_ERR_INVALID_ARGUMENT;
            if (!append_conf(plan->swanctl_conf, sizeof(plan->swanctl_conf), secret)) return EN_ERR_INVALID_ARGUMENT;
        }
    }
    if (!append_conf(plan->swanctl_conf, sizeof(plan->swanctl_conf), "}\n")) return EN_ERR_INVALID_ARGUMENT;
    char load_command[EN_MAX_COMMAND_LEN] = {0};
    if (snprintf(load_command, sizeof(load_command), "swanctl --load-conns --file %s", swanctl_conf_filename) >= (int)sizeof(load_command)) return EN_ERR_INVALID_ARGUMENT;
    if (!append_command(plan, load_command)) return EN_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < selected_path->segment_count; i++) {
        const en_tunnel_t *tunnel = find_tunnel(config, selected_path->segments[i].tunnel_id);
        char command[EN_MAX_COMMAND_LEN] = {0};
        en_error_code_t err = en_render_swanctl_initiate(tunnel, command, sizeof(command));
        if (err != EN_ERR_NONE) {
            return err;
        }
        char rollback_command[EN_MAX_COMMAND_LEN] = {0};
        err = en_render_swanctl_terminate(tunnel, rollback_command, sizeof(rollback_command));
        if (err != EN_ERR_NONE || !append_command_with_rollback(plan, command, rollback_command)) return EN_ERR_INVALID_ARGUMENT;
    }

    /* The GRE interface is the forwarding endpoint and must precede route installation. */
    for (size_t i = 0; i < selected_path->segment_count; i++) {
        const en_tunnel_t *tunnel = find_tunnel(config, selected_path->segments[i].tunnel_id);
        if (tunnel == NULL || strcmp(tunnel->tunnel_type, "gre_over_ipsec") != 0) continue;
        bool already_added = false;
        for (size_t prior = 0; prior < i; prior++) {
            if (strcmp(selected_path->segments[prior].tunnel_id, tunnel->tunnel_id) == 0) {
                already_added = true;
                break;
            }
        }
        if (already_added) continue;
        char command[EN_MAX_COMMAND_LEN] = {0};
        char rollback_command[EN_MAX_COMMAND_LEN] = {0};
        if (en_render_vpp_gre_create(tunnel, command, sizeof(command)) != EN_ERR_NONE ||
            en_render_vpp_gre_delete(tunnel, rollback_command, sizeof(rollback_command)) != EN_ERR_NONE ||
            !append_command_with_rollback(plan, command, rollback_command) ||
            en_render_vpp_gre_set_address(tunnel, command, sizeof(command)) != EN_ERR_NONE ||
            !append_command(plan, command)) return EN_ERR_INVALID_ARGUMENT;
        if (tunnel->gre_mtu > 0) {
            if (en_render_vpp_gre_set_mtu(tunnel, command, sizeof(command)) != EN_ERR_NONE ||
                !append_command(plan, command)) return EN_ERR_INVALID_ARGUMENT;
        }
        if (en_render_vpp_gre_set_up(tunnel, command, sizeof(command)) != EN_ERR_NONE ||
            !append_command(plan, command)) return EN_ERR_INVALID_ARGUMENT;
    }

    if (intent->block_non_ipsec) {
        for (size_t i = 0; i < selected_path->segment_count; i++) {
            const en_tunnel_t *tunnel = find_tunnel(config, selected_path->segments[i].tunnel_id);
            char command[EN_MAX_COMMAND_LEN] = {0};
            char rollback_command[EN_MAX_COMMAND_LEN] = {0};
            if (tunnel == NULL || !valid_xfrm_selector(tunnel->local_traffic_selector) ||
                !valid_xfrm_selector(tunnel->remote_traffic_selector) ||
                snprintf(command, sizeof(command), "ip xfrm policy add dir out src %s dst %s priority 10000 action block",
                    tunnel->local_traffic_selector, tunnel->remote_traffic_selector) >= (int)sizeof(command) ||
                snprintf(rollback_command, sizeof(rollback_command), "ip xfrm policy delete dir out src %s dst %s priority 10000",
                    tunnel->local_traffic_selector, tunnel->remote_traffic_selector) >= (int)sizeof(rollback_command) ||
                !append_command_with_rollback(plan, command, rollback_command) ||
                snprintf(command, sizeof(command), "ip xfrm policy add dir in src %s dst %s priority 10000 action block",
                    tunnel->remote_traffic_selector, tunnel->local_traffic_selector) >= (int)sizeof(command) ||
                snprintf(rollback_command, sizeof(rollback_command), "ip xfrm policy delete dir in src %s dst %s priority 10000",
                    tunnel->remote_traffic_selector, tunnel->local_traffic_selector) >= (int)sizeof(rollback_command) ||
                !append_command_with_rollback(plan, command, rollback_command)) return EN_ERR_INVALID_ARGUMENT;
        }
    }

    en_error_code_t err = EN_ERR_NONE;
    if (selected_path->route_count > 0) {
        for (size_t i = 0; i < selected_path->route_count; i++) {
            char vpp_command[EN_MAX_COMMAND_LEN] = {0};
            en_route_t route = selected_path->routes[i];
            const en_tunnel_t *gre_tunnel = NULL;
            for (size_t segment_index = 0; segment_index < selected_path->segment_count; segment_index++) {
                const en_tunnel_t *candidate = find_tunnel(config, selected_path->segments[segment_index].tunnel_id);
                if (candidate != NULL && strcmp(candidate->tunnel_type, "gre_over_ipsec") == 0) {
                    gre_tunnel = candidate;
                    break;
                }
            }
            if (gre_tunnel != NULL && route.interface_name[0] == '\0') {
                snprintf(route.next_hop, sizeof(route.next_hop), "%s", gre_tunnel->gre_remote_address);
                snprintf(route.interface_name, sizeof(route.interface_name), "%s", gre_tunnel->gre_interface);
            }
            err = en_render_vpp_route_replace_entry(&route, vpp_command, sizeof(vpp_command));
            if (err != EN_ERR_NONE) {
                return err;
            }
            char delete_command[EN_MAX_COMMAND_LEN] = {0};
            if (en_render_vpp_route_delete_entry(&route, delete_command, sizeof(delete_command)) != EN_ERR_NONE ||
                !append_command_with_rollback(plan, vpp_command, delete_command)) return EN_ERR_INVALID_ARGUMENT;
        }
    } else {
        const en_tunnel_t *egress_tunnel = find_tunnel(config, selected_path->egress_tunnel_id);
        char vpp_command[EN_MAX_COMMAND_LEN] = {0};
        if (egress_tunnel != NULL && strcmp(egress_tunnel->tunnel_type, "gre_over_ipsec") == 0) {
            err = en_render_vpp_gre_route_replace(selected_path, egress_tunnel, vpp_command, sizeof(vpp_command));
        } else {
            err = en_render_vpp_route_replace(selected_path, egress_tunnel, vpp_command, sizeof(vpp_command));
        }
        if (err != EN_ERR_NONE) {
            return err;
        }
        char delete_command[EN_MAX_COMMAND_LEN] = {0};
        if ((egress_tunnel != NULL && strcmp(egress_tunnel->tunnel_type, "gre_over_ipsec") == 0 ?
                en_render_vpp_gre_route_delete(selected_path, egress_tunnel, delete_command, sizeof(delete_command)) :
                en_render_vpp_route_delete(selected_path, delete_command, sizeof(delete_command))) != EN_ERR_NONE ||
            !append_command_with_rollback(plan, vpp_command, delete_command)) return EN_ERR_INVALID_ARGUMENT;
    }

    if (selected_path->route_count > 0) {
        for (size_t i = selected_path->route_count; i > 0; i--) {
            char delete_route_command[EN_MAX_COMMAND_LEN] = {0};
            en_route_t route = selected_path->routes[i - 1];
            const en_tunnel_t *gre_tunnel = NULL;
            for (size_t segment_index = 0; segment_index < selected_path->segment_count; segment_index++) {
                const en_tunnel_t *candidate = find_tunnel(config, selected_path->segments[segment_index].tunnel_id);
                if (candidate != NULL && strcmp(candidate->tunnel_type, "gre_over_ipsec") == 0) {
                    gre_tunnel = candidate;
                    break;
                }
            }
            if (gre_tunnel != NULL && route.interface_name[0] == '\0') {
                snprintf(route.next_hop, sizeof(route.next_hop), "%s", gre_tunnel->gre_remote_address);
                snprintf(route.interface_name, sizeof(route.interface_name), "%s", gre_tunnel->gre_interface);
            }
            err = en_render_vpp_route_delete_entry(&route, delete_route_command, sizeof(delete_route_command));
            if (err == EN_ERR_NONE) {
                if (!append_rollback_command(plan, delete_route_command)) return EN_ERR_INVALID_ARGUMENT;
            }
        }
    } else {
        char delete_route_command[EN_MAX_COMMAND_LEN] = {0};
        const en_tunnel_t *egress_tunnel = find_tunnel(config, selected_path->egress_tunnel_id);
        if (egress_tunnel != NULL && strcmp(egress_tunnel->tunnel_type, "gre_over_ipsec") == 0) {
            err = en_render_vpp_gre_route_delete(selected_path, egress_tunnel, delete_route_command, sizeof(delete_route_command));
        } else {
            err = en_render_vpp_route_delete(selected_path, delete_route_command, sizeof(delete_route_command));
        }
        if (err == EN_ERR_NONE) {
            if (!append_rollback_command(plan, delete_route_command)) return EN_ERR_INVALID_ARGUMENT;
        }
    }
    for (size_t i = selected_path->segment_count; i > 0; i--) {
        const en_tunnel_t *tunnel = find_tunnel(config, selected_path->segments[i - 1].tunnel_id);
        if (tunnel == NULL || strcmp(tunnel->tunnel_type, "gre_over_ipsec") != 0) continue;
        bool already_added = false;
        for (size_t later = selected_path->segment_count; later > i; later--) {
            if (strcmp(selected_path->segments[later - 1].tunnel_id, tunnel->tunnel_id) == 0) {
                already_added = true;
                break;
            }
        }
        if (already_added) continue;
        char delete_command[EN_MAX_COMMAND_LEN] = {0};
        if (en_render_vpp_gre_delete(tunnel, delete_command, sizeof(delete_command)) != EN_ERR_NONE ||
            !append_rollback_command(plan, delete_command)) return EN_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = selected_path->segment_count; i > 0; i--) {
        const en_tunnel_t *tunnel = find_tunnel(config, selected_path->segments[i - 1].tunnel_id);
        char terminate_command[EN_MAX_COMMAND_LEN] = {0};
        err = en_render_swanctl_terminate(tunnel, terminate_command, sizeof(terminate_command));
        if (err != EN_ERR_NONE) {
            return err;
        }
        if (!append_rollback_command(plan, terminate_command)) return EN_ERR_INVALID_ARGUMENT;
    }

    if (intent->block_non_ipsec) {
        for (size_t i = selected_path->segment_count; i > 0; i--) {
            const en_tunnel_t *tunnel = find_tunnel(config, selected_path->segments[i - 1].tunnel_id);
            char command[EN_MAX_COMMAND_LEN] = {0};
            if (tunnel == NULL || !valid_xfrm_selector(tunnel->local_traffic_selector) ||
                !valid_xfrm_selector(tunnel->remote_traffic_selector) ||
                snprintf(command, sizeof(command), "ip xfrm policy delete dir out src %s dst %s priority 10000",
                    tunnel->local_traffic_selector, tunnel->remote_traffic_selector) >= (int)sizeof(command) ||
                !append_rollback_command(plan, command) ||
                snprintf(command, sizeof(command), "ip xfrm policy delete dir in src %s dst %s priority 10000",
                    tunnel->remote_traffic_selector, tunnel->local_traffic_selector) >= (int)sizeof(command) ||
                !append_rollback_command(plan, command)) return EN_ERR_INVALID_ARGUMENT;
        }
    }

    return EN_ERR_NONE;
}

en_error_code_t en_apply_plan_write_swanctl_conf(
    const en_apply_plan_t *plan,
    const char *filename
)
{
    if (plan == NULL || filename == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
#if defined(_WIN32)
    FILE *file = fopen(filename, "w");
#else
    struct stat existing_stat;
    if (lstat(filename, &existing_stat) == 0 && S_ISLNK(existing_stat.st_mode)) {
        return EN_ERR_STATE_CONFLICT;
    }
    char temporary_filename[512] = {0};
    if (snprintf(temporary_filename, sizeof(temporary_filename), "%s.tmp-plan-%ld", filename, (long)getpid()) >= (int)sizeof(temporary_filename)) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    int file_descriptor = open(temporary_filename, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    FILE *file = file_descriptor < 0 ? NULL : fdopen(file_descriptor, "w");
    if (file == NULL && file_descriptor >= 0) {
        close(file_descriptor);
        unlink(temporary_filename);
    }
    if (file != NULL && fchmod(file_descriptor, 0600) != 0) {
        fclose(file);
        unlink(temporary_filename);
        return EN_ERR_STATE_CONFLICT;
    }
#endif
    if (file == NULL) {
        return EN_ERR_STATE_CONFLICT;
    }
    int write_status = fputs(plan->swanctl_conf, file);
    int close_status = fclose(file);
    if (write_status == EOF || close_status != 0) {
#if !defined(_WIN32)
        unlink(temporary_filename);
#endif
        return EN_ERR_STATE_CONFLICT;
    }
#if !defined(_WIN32)
    if (rename(temporary_filename, filename) != 0) {
        unlink(temporary_filename);
        return EN_ERR_STATE_CONFLICT;
    }
#endif
    return EN_ERR_NONE;
}

en_error_code_t en_apply_plan_write_shell_script(
    const en_apply_plan_t *plan,
    const char *filename
)
{
    if (plan == NULL || filename == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        return EN_ERR_STATE_CONFLICT;
    }
    fputs("#!/usr/bin/env sh\n", file);
    fputs("set -eu\n\n", file);
    fputs("if [ \"$(id -u)\" != \"0\" ]; then\n", file);
    fputs("  echo \"warning: applying IPsec/VPP routes usually requires root\" >&2\n", file);
    fputs("fi\n", file);
    fputs("command -v swanctl >/dev/null\n", file);
    fputs("command -v vppctl >/dev/null\n\n", file);
    for (size_t i = 0; i < plan->command_count; i++) {
        fprintf(file, "%s\n", plan->commands[i]);
    }
    if (plan->rollback_command_count > 0) {
        fputs("\n# Rollback commands, run manually if validation fails:\n", file);
        for (size_t i = 0; i < plan->rollback_command_count; i++) {
            fprintf(file, "# %s\n", plan->rollback_commands[i]);
        }
    }
    fclose(file);
    return EN_ERR_NONE;
}

en_error_code_t en_apply_plan_run(
    const en_apply_plan_t *plan,
    bool dry_run
)
{
    if (plan == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < plan->command_count; i++) {
        if (dry_run) {
            printf("[dry-run] %s\n", plan->commands[i]);
            continue;
        }
        int rc = 0;
#if defined(_WIN32)
        rc = system(plan->commands[i]);
#else
        rc = run_plan_command_without_shell(plan->commands[i]);
#endif
        if (rc != 0) {
            bool rollback_failed = false;
            if (plan->has_command_rollbacks) {
                for (size_t command_index = i; command_index > 0; command_index--) {
                    if (!plan->command_has_rollback[command_index - 1]) continue;
                    const char *rollback_command = plan->command_rollbacks[command_index - 1];
                    int rollback_rc = 0;
#if defined(_WIN32)
                    rollback_rc = system(rollback_command);
#else
                    rollback_rc = run_plan_command_without_shell(rollback_command);
#endif
                    if (rollback_rc != 0) rollback_failed = true;
                }
            } else for (size_t rollback_index = plan->rollback_command_count; rollback_index > 0; rollback_index--) {
                const char *rollback_command = plan->rollback_commands[rollback_index - 1];
                int rollback_rc = 0;
#if defined(_WIN32)
                rollback_rc = system(rollback_command);
#else
                rollback_rc = run_plan_command_without_shell(rollback_command);
#endif
                if (rollback_rc != 0) rollback_failed = true;
            }
            return rollback_failed ? EN_ERR_ROLLBACK_FAILED : EN_ERR_STATE_CONFLICT;
        }
    }
    return EN_ERR_NONE;
}

static const en_tunnel_t *find_tunnel(const en_yaml_config_t *config, const char *tunnel_id)
{
    if (config == NULL || tunnel_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < config->tunnel_count; i++) {
        if (strcmp(config->tunnels[i].tunnel_id, tunnel_id) == 0) {
            return &config->tunnels[i];
        }
    }
    return NULL;
}

static en_error_code_t append_tunnel_conf(en_apply_plan_t *plan, const en_tunnel_t *tunnel)
{
    if (tunnel == NULL) {
        return EN_ERR_NOT_FOUND;
    }
    char block[1536] = {0};
    char local_auth[256] = {0};
    char remote_auth[256] = {0};
    if (strcmp(tunnel->auth_method, "pubkey") == 0) {
        if (snprintf(local_auth, sizeof(local_auth), "auth = pubkey\n      certs = %s", tunnel->local_cert) >= (int)sizeof(local_auth)) return EN_ERR_INVALID_ARGUMENT;
        if (tunnel->remote_cacerts[0] != '\0') {
            if (snprintf(remote_auth, sizeof(remote_auth), "auth = pubkey\n      cacerts = %s", tunnel->remote_cacerts) >= (int)sizeof(remote_auth)) return EN_ERR_INVALID_ARGUMENT;
        } else {
            snprintf(remote_auth, sizeof(remote_auth), "auth = pubkey");
        }
    } else {
        snprintf(local_auth, sizeof(local_auth), "auth = psk");
        snprintf(remote_auth, sizeof(remote_auth), "auth = psk");
    }
    const bool gre_over_ipsec = strcmp(tunnel->tunnel_type, "gre_over_ipsec") == 0;
    char local_ts_buffer[EN_MAX_ID_LEN + 16] = {0};
    char remote_ts_buffer[EN_MAX_ID_LEN + 16] = {0};
    const char *local_ts = tunnel->local_traffic_selector;
    const char *remote_ts = tunnel->remote_traffic_selector;
    if (gre_over_ipsec) {
        const char *local_endpoint = tunnel->gre_outer_local_endpoint[0] == '\0' ? tunnel->local_endpoint : tunnel->gre_outer_local_endpoint;
        const char *remote_endpoint = tunnel->gre_outer_remote_endpoint[0] == '\0' ? tunnel->remote_endpoint : tunnel->gre_outer_remote_endpoint;
        if (snprintf(local_ts_buffer, sizeof(local_ts_buffer), "%s/32[gre]", local_endpoint) >= (int)sizeof(local_ts_buffer) ||
            snprintf(remote_ts_buffer, sizeof(remote_ts_buffer), "%s/32[gre]", remote_endpoint) >= (int)sizeof(remote_ts_buffer)) return EN_ERR_INVALID_ARGUMENT;
        local_ts = local_ts_buffer;
        remote_ts = remote_ts_buffer;
    }
    const char *mode = gre_over_ipsec ? "        mode = transport\n" : "";
    if (snprintf(
        block,
        sizeof(block),
        "  %s {\n"
        "    version = 2\n"
        "    local_addrs = %s\n"
        "    remote_addrs = %s\n"
        "    local {\n"
        "      %s\n"
        "      id = %s\n"
        "    }\n"
        "    remote {\n"
        "      %s\n"
        "      id = %s\n"
        "    }\n"
        "    children {\n"
        "      %s {\n"
        "%s"
        "        local_ts = %s\n"
        "        remote_ts = %s\n"
        "        start_action = trap\n"
        "      }\n"
        "    }\n"
        "  }\n",
        tunnel->tunnel_id,
        tunnel->local_endpoint,
        tunnel->remote_endpoint,
        local_auth,
        tunnel->local_id[0] == '\0' ? tunnel->local_endpoint : tunnel->local_id,
        remote_auth,
        tunnel->remote_id[0] == '\0' ? tunnel->remote_endpoint : tunnel->remote_id,
        tunnel->tunnel_id,
        mode,
        local_ts,
        remote_ts
    ) >= (int)sizeof(block)) return EN_ERR_INVALID_ARGUMENT;
    return append_conf(plan->swanctl_conf, sizeof(plan->swanctl_conf), block) ? EN_ERR_NONE : EN_ERR_INVALID_ARGUMENT;
}

static bool append_conf(char *dst, size_t dst_len, const char *text)
{
    if (dst == NULL || text == NULL || dst_len == 0) return false;
    size_t used = strlen(dst);
    if (used >= dst_len) return false;
    size_t text_len = strlen(text);
    if (text_len >= dst_len - used) return false;
    memcpy(dst + used, text, text_len + 1);
    return true;
}

static bool valid_secret(const char *value)
{
    if (value == NULL || value[0] == '\0') return true;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (*cursor < 33 || *cursor > 126 || *cursor == '"' || *cursor == '\\') return false;
    }
    return true;
}

static bool valid_xfrm_selector(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (isalnum(*cursor) || *cursor == '.' || *cursor == '/' || *cursor == ':' || *cursor == '_' || *cursor == '-') continue;
        return false;
    }
    return true;
}

static bool append_command(en_apply_plan_t *plan, const char *command)
{
    if (plan == NULL || command == NULL || plan->command_count >= EN_MAX_PLAN_COMMANDS || strlen(command) >= EN_MAX_COMMAND_LEN) return false;
    memcpy(plan->commands[plan->command_count++], command, strlen(command) + 1);
    return true;
}

static bool append_command_with_rollback(en_apply_plan_t *plan, const char *command, const char *rollback_command)
{
    if (plan == NULL || rollback_command == NULL || strlen(rollback_command) >= EN_MAX_COMMAND_LEN) return false;
    if (!append_command(plan, command)) return false;
    size_t command_index = plan->command_count - 1;
    memcpy(plan->command_rollbacks[command_index], rollback_command, strlen(rollback_command) + 1);
    plan->command_has_rollback[command_index] = true;
    plan->has_command_rollbacks = true;
    return true;
}

static bool append_rollback_command(en_apply_plan_t *plan, const char *command)
{
    if (plan == NULL || command == NULL || plan->rollback_command_count >= EN_MAX_PLAN_COMMANDS || strlen(command) >= EN_MAX_COMMAND_LEN) return false;
    memcpy(plan->rollback_commands[plan->rollback_command_count++], command, strlen(command) + 1);
    return true;
}
