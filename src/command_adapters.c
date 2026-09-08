#include "eventnet/command_adapters.h"
#include "eventnet/render_commands.h"
#include "eventnet/strongswan_observer.h"
#include "eventnet/vpp_observer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

static en_error_code_t strongswan_ensure(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed);
static en_error_code_t strongswan_remove(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed);
static en_error_code_t vpp_install(void *ctx, const char *traffic_key, const en_path_t *path);
static en_error_code_t vpp_remove(void *ctx, const char *traffic_key, const en_path_t *path);
static const char *vpp_active_path(void *ctx, const char *traffic_key);
static en_error_code_t health_validate(void *ctx, const en_path_t *path, en_path_health_t *health);
static en_error_code_t run_template(const char *template_text, bool dry_run, const en_tunnel_t *tunnel, const en_path_t *path, const char *traffic_key);
static en_error_code_t run_shell_command(const char *command, bool dry_run);
static en_error_code_t run_exec_command(const char *command, bool dry_run);
static en_error_code_t run_exec_capture(const char *command, char *output, size_t output_len);
static en_error_code_t apply_xfrm_block(en_strongswan_command_ctx_t *ctx, const en_tunnel_t *tunnel);
static en_error_code_t remove_xfrm_block(en_strongswan_command_ctx_t *ctx, const en_tunnel_t *tunnel);
static bool has_xfrm_block(const en_strongswan_command_ctx_t *ctx, const char *tunnel_id);
static bool xfrm_policy_exists(const en_strongswan_command_ctx_t *ctx, const char *direction, const char *source, const char *destination);
static bool range_contains(const char *begin, const char *end, const char *needle);
static en_error_code_t verify_vpp_route(const en_vpp_command_ctx_t *ctx, const char *prefix, const char *expected_next_hop, const char *interface_name, int table_id);
static en_error_code_t capture_vpp_command(const en_vpp_command_ctx_t *ctx, const char *suffix, char *output, size_t output_len);
static en_error_code_t verify_vpp_vlan_interfaces(const en_vpp_command_ctx_t *ctx, const en_path_t *path);
static en_error_code_t ensure_vpp_vlan_interfaces(const en_vpp_command_ctx_t *ctx, const en_path_t *path);
static en_error_code_t ensure_vpp_vlan_interface_tables(const en_vpp_command_ctx_t *ctx, const en_path_t *path);
static en_error_code_t ensure_vpp_unmatched_vlan_acl(const en_vpp_command_ctx_t *ctx, const en_path_t *path);
static en_error_code_t remove_vpp_unmatched_vlan_acl(const en_vpp_command_ctx_t *ctx, const char *traffic_key, const en_path_t *path);
static bool vpp_vlan_edge_is_shared(const en_vpp_command_ctx_t *ctx, const char *traffic_key, const en_path_t *path, const en_vpp_edge_t *edge);
static bool vpp_edge_used_by_path(const en_vpp_edge_t *edge, const en_path_t *path);
static int vpp_table_for_node(const en_path_t *path, const char *node_id);
static en_error_code_t run_vpp_command(const en_vpp_command_ctx_t *ctx, const char *command);
static en_error_code_t ensure_vpp_route_tables(const en_vpp_command_ctx_t *ctx, const en_path_t *path);
static bool valid_command_token(const char *value);
static void expand_template(char *out, size_t out_len, const char *template_text, const en_tunnel_t *tunnel, const en_path_t *path, const char *traffic_key);
static void replace_all(char *text, size_t text_len, const char *needle, const char *replacement);
static void append_text(char *dst, size_t dst_len, const char *src);
static void remember_active_path(en_vpp_command_ctx_t *ctx, const char *traffic_key, const en_path_t *path);
static void forget_active_path(en_vpp_command_ctx_t *ctx, const char *traffic_key, const char *path_id);
static const en_tunnel_t *find_ctx_tunnel(const en_vpp_command_ctx_t *ctx, const char *tunnel_id);
static en_error_code_t format_command(char *output, size_t output_len, const char *format, ...);

en_strongswan_adapter_t en_strongswan_command_adapter(en_strongswan_command_ctx_t *ctx)
{
    en_strongswan_adapter_t adapter = {
        .ensure_tunnel = strongswan_ensure,
        .remove_tunnel = strongswan_remove,
        .ctx = ctx,
    };
    return adapter;
}

en_vpp_adapter_t en_vpp_command_adapter(en_vpp_command_ctx_t *ctx)
{
    en_vpp_adapter_t adapter = {
        .install_path = vpp_install,
        .remove_path = vpp_remove,
        .active_path = vpp_active_path,
        .ctx = ctx,
    };
    return adapter;
}

en_health_probe_t en_health_command_probe(en_health_command_ctx_t *ctx)
{
    en_health_probe_t probe = {
        .validate_path = health_validate,
        .ctx = ctx,
    };
    return probe;
}

static en_error_code_t strongswan_ensure(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    en_strongswan_command_ctx_t *command_ctx = ctx;
    if (command_ctx == NULL || desired == NULL || observed == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    en_error_code_t err = EN_ERR_NONE;
    if (command_ctx->swanctl_config_file[0] != '\0' && !command_ctx->connections_loaded) {
        char load_command[512] = {0};
        err = en_render_swanctl_load_conns_uri(command_ctx->swanctl_uri, command_ctx->swanctl_config_file, load_command, sizeof(load_command));
        if (err == EN_ERR_NONE) err = run_exec_command(load_command, command_ctx->dry_run);
        if (err != EN_ERR_NONE) return err;
        command_ctx->connections_loaded = true;
    }
    if (command_ctx->ensure_tunnel_command[0] != '\0') {
        err = run_template(command_ctx->ensure_tunnel_command, command_ctx->dry_run, desired, NULL, NULL);
    } else {
        char command[512] = {0};
        err = en_render_swanctl_initiate_uri(desired, command_ctx->swanctl_uri, command, sizeof(command));
        if (err == EN_ERR_NONE) {
            err = run_exec_command(command, command_ctx->dry_run);
        }
    }
    if (err != EN_ERR_NONE) {
        return err;
    }
    if (command_ctx->verify_tunnel_command[0] != '\0') {
        err = run_template(command_ctx->verify_tunnel_command, command_ctx->dry_run, desired, NULL, NULL);
        if (err != EN_ERR_NONE) return err;
    } else if (command_ctx->verify_tunnel) {
        char command[512] = {0};
        err = en_render_swanctl_list_sas_uri(desired, command_ctx->swanctl_uri, command, sizeof(command));
        if (err != EN_ERR_NONE) return err;
        if (command_ctx->dry_run) {
            err = run_exec_command(command, true);
            if (err != EN_ERR_NONE) return err;
        } else {
            char output[8192] = {0};
            en_strongswan_sa_observation_t observation = {0};
            err = run_exec_capture(command, output, sizeof(output));
            if (err != EN_ERR_NONE) return err;
            err = en_strongswan_parse_list_sas(output, desired->tunnel_id, &observation, NULL, 0);
            if (err != EN_ERR_NONE || (observation.state != EN_TUNNEL_ESTABLISHED && observation.state != EN_TUNNEL_REKEYING)) {
                return EN_ERR_STATE_CONFLICT;
            }
        }
    }
    if (command_ctx->block_non_ipsec) {
        err = apply_xfrm_block(command_ctx, desired);
        if (err != EN_ERR_NONE) return err;
    }
    *observed = *desired;
    observed->state = EN_TUNNEL_ESTABLISHED;
    observed->health = EN_HEALTH_HEALTHY;
    return EN_ERR_NONE;
}

static en_error_code_t strongswan_remove(void *ctx, const en_tunnel_t *desired, en_tunnel_t *observed)
{
    en_strongswan_command_ctx_t *command_ctx = ctx;
    if (command_ctx == NULL || desired == NULL || observed == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    en_error_code_t err = EN_ERR_NONE;
    if (command_ctx->remove_tunnel_command[0] != '\0') {
        err = run_template(command_ctx->remove_tunnel_command, command_ctx->dry_run, desired, NULL, NULL);
    } else {
        char command[512] = {0};
        err = en_render_swanctl_terminate_uri(desired, command_ctx->swanctl_uri, command, sizeof(command));
        if (err == EN_ERR_NONE) {
            err = run_exec_command(command, command_ctx->dry_run);
        }
    }
    if (err != EN_ERR_NONE) {
        return err;
    }
    if (command_ctx->block_non_ipsec) {
        err = remove_xfrm_block(command_ctx, desired);
        if (err != EN_ERR_NONE) return err;
    }
    *observed = *desired;
    observed->state = EN_TUNNEL_ABSENT;
    observed->health = EN_HEALTH_UNKNOWN;
    return EN_ERR_NONE;
}

static en_error_code_t vpp_install(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_command_ctx_t *command_ctx = ctx;
    if (command_ctx == NULL || traffic_key == NULL || path == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    en_error_code_t err = EN_ERR_NONE;
    if (command_ctx->install_path_command[0] != '\0') {
        err = run_template(command_ctx->install_path_command, command_ctx->dry_run, NULL, path, traffic_key);
    } else {
        err = ensure_vpp_vlan_interfaces(command_ctx, path);
        if (err != EN_ERR_NONE) return err;
        err = ensure_vpp_unmatched_vlan_acl(command_ctx, path);
        if (err != EN_ERR_NONE) return err;
    }
    if (err == EN_ERR_NONE && path->routes_explicit && path->route_count > 0) {
        err = ensure_vpp_route_tables(command_ctx, path);
        if (err != EN_ERR_NONE) return err;
        err = ensure_vpp_vlan_interface_tables(command_ctx, path);
        if (err != EN_ERR_NONE) return err;
        for (size_t index = 0; index < path->route_count; index++) {
            char command[512] = {0};
            err = en_render_vpp_route_replace_entry(&path->routes[index], command, sizeof(command));
            if (err != EN_ERR_NONE) break;
            err = run_vpp_command(command_ctx, command);
            if (err != EN_ERR_NONE) break;
        }
    } else if (err == EN_ERR_NONE && command_ctx->install_path_command[0] == '\0') {
        const en_tunnel_t *egress_tunnel = find_ctx_tunnel(command_ctx, path->egress_tunnel_id);
        char command[512] = {0};
        err = en_render_vpp_route_replace(path, egress_tunnel, command, sizeof(command));
        if (err == EN_ERR_NONE) {
            err = run_vpp_command(command_ctx, command);
        }
    }
    if (err != EN_ERR_NONE) {
        return err;
    }
    if (command_ctx->verify_route && !command_ctx->dry_run) {
        if (path->routes_explicit && path->route_count > 0) {
            for (size_t index = 0; index < path->route_count; index++) {
                if (verify_vpp_route(command_ctx, path->routes[index].destination_prefix, path->routes[index].next_hop, path->routes[index].interface_name, path->routes[index].table_id) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
            }
        } else if (verify_vpp_route(command_ctx, path->route_destination_prefix, path->route_next_hop, NULL, -1) != EN_ERR_NONE) {
            return EN_ERR_STATE_CONFLICT;
        }
        if (verify_vpp_vlan_interfaces(command_ctx, path) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    }
    remember_active_path(command_ctx, traffic_key, path);
    return EN_ERR_NONE;
}

static const char *vpp_active_path(void *ctx, const char *traffic_key)
{
    en_vpp_command_ctx_t *command_ctx = ctx;
    if (command_ctx == NULL || traffic_key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < command_ctx->active_count; i++) {
        if (strcmp(command_ctx->traffic_keys[i], traffic_key) == 0) {
            return command_ctx->active_paths[i];
        }
    }
    return NULL;
}

static en_error_code_t vpp_remove(void *ctx, const char *traffic_key, const en_path_t *path)
{
    en_vpp_command_ctx_t *command_ctx = ctx;
    if (command_ctx == NULL || traffic_key == NULL || path == NULL) return EN_ERR_INVALID_ARGUMENT;
    en_error_code_t err = EN_ERR_NONE;
    if (path->routes_explicit && path->route_count > 0) {
        for (size_t index = 0; index < path->route_count; index++) {
            char command[512] = {0};
            err = en_render_vpp_route_delete_entry(&path->routes[index], command, sizeof(command));
            if (err != EN_ERR_NONE) break;
            err = run_vpp_command(command_ctx, command);
            if (err != EN_ERR_NONE) break;
        }
    } else {
        char command[512] = {0};
        const en_tunnel_t *egress_tunnel = find_ctx_tunnel(command_ctx, path->egress_tunnel_id);
        err = en_render_vpp_route_delete_with_tunnel(path, egress_tunnel, command, sizeof(command));
        if (err == EN_ERR_NONE) err = run_vpp_command(command_ctx, command);
    }
    if (err == EN_ERR_NONE && command_ctx->deny_unmatched_vlan &&
        vpp_active_path(command_ctx, traffic_key) != NULL &&
        strcmp(vpp_active_path(command_ctx, traffic_key), path->path_id) == 0) {
        err = remove_vpp_unmatched_vlan_acl(command_ctx, traffic_key, path);
    }
    if (err == EN_ERR_NONE) forget_active_path(command_ctx, traffic_key, path->path_id);
    return err;
}

static en_error_code_t health_validate(void *ctx, const en_path_t *path, en_path_health_t *health)
{
    en_health_command_ctx_t *command_ctx = ctx;
    if (command_ctx == NULL || path == NULL || health == NULL) {
        return EN_ERR_INVALID_ARGUMENT;
    }
    en_error_code_t err = run_template(command_ctx->validate_path_command, command_ctx->dry_run, NULL, path, NULL);
    snprintf(health->path_id, sizeof(health->path_id), "%s", path->path_id);
    health->state = err == EN_ERR_NONE ? EN_HEALTH_HEALTHY : EN_HEALTH_FAILED;
    health->rtt_ms = 0.0;
    health->packet_loss_percent = err == EN_ERR_NONE ? 0.0 : 100.0;
    health->jitter_ms = 0.0;
    health->consecutive_successes = err == EN_ERR_NONE ? 1 : 0;
    health->consecutive_failures = err == EN_ERR_NONE ? 0 : 1;
    return err;
}

static en_error_code_t run_template(const char *template_text, bool dry_run, const en_tunnel_t *tunnel, const en_path_t *path, const char *traffic_key)
{
    if (template_text == NULL || template_text[0] == '\0') {
        return EN_ERR_NONE;
    }
    char command[512] = {0};
    expand_template(command, sizeof(command), template_text, tunnel, path, traffic_key);
    if (strpbrk(command, ";&|<>`$()'\"\\") == NULL) return run_exec_command(command, dry_run);
    return run_shell_command(command, dry_run);
}

static en_error_code_t run_shell_command(const char *command, bool dry_run)
{
    if (dry_run) {
        printf("[dry-run] %s\n", command);
        return EN_ERR_NONE;
    }
    int rc = system(command);
    return rc == 0 ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
}

static en_error_code_t run_exec_command(const char *command, bool dry_run)
{
    if (command == NULL || command[0] == '\0') return EN_ERR_INVALID_ARGUMENT;
    if (dry_run) {
        printf("[dry-run] %s\n", command);
        return EN_ERR_NONE;
    }
#if defined(_WIN32)
    int rc = system(command);
    return rc == 0 ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
#else
    char copy[512] = {0};
    if (strlen(command) >= sizeof(copy)) return EN_ERR_INVALID_ARGUMENT;
    memcpy(copy, command, strlen(command) + 1);
    char *argv[16] = {0};
    size_t argc = 0;
    for (char *token = strtok(copy, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
        if (argc + 1 >= sizeof(argv) / sizeof(argv[0])) return EN_ERR_INVALID_ARGUMENT;
        if (!valid_command_token(token)) return EN_ERR_INVALID_ARGUMENT;
        argv[argc++] = token;
    }
    if (argc == 0) return EN_ERR_INVALID_ARGUMENT;
    argv[argc] = NULL;
    pid_t child = fork();
    if (child < 0) return EN_ERR_STATE_CONFLICT;
    if (child == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return EN_ERR_STATE_CONFLICT;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
#endif
}

static en_error_code_t run_exec_capture(const char *command, char *output, size_t output_len)
{
    if (command == NULL || output == NULL || output_len < 2) return EN_ERR_INVALID_ARGUMENT;
    output[0] = '\0';
#if defined(_WIN32)
    (void)command;
    return EN_ERR_STATE_CONFLICT;
#else
    char copy[512] = {0};
    if (strlen(command) >= sizeof(copy)) return EN_ERR_INVALID_ARGUMENT;
    memcpy(copy, command, strlen(command) + 1);
    char *argv[16] = {0};
    size_t argc = 0;
    for (char *token = strtok(copy, " \t\r\n"); token != NULL; token = strtok(NULL, " \t\r\n")) {
        if (argc + 1 >= sizeof(argv) / sizeof(argv[0]) || !valid_command_token(token)) return EN_ERR_INVALID_ARGUMENT;
        argv[argc++] = token;
    }
    if (argc == 0) return EN_ERR_INVALID_ARGUMENT;
    argv[argc] = NULL;
    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) return EN_ERR_STATE_CONFLICT;
    pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return EN_ERR_STATE_CONFLICT;
    }
    if (child == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipe_fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    size_t used = 0;
    while (used + 1 < output_len) {
        ssize_t received = read(pipe_fds[0], output + used, output_len - used - 1);
        if (received <= 0) break;
        used += (size_t)received;
    }
    output[used] = '\0';
    close(pipe_fds[0]);
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return EN_ERR_STATE_CONFLICT;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? EN_ERR_NONE : EN_ERR_STATE_CONFLICT;
#endif
}

static bool has_xfrm_block(const en_strongswan_command_ctx_t *ctx, const char *tunnel_id)
{
    if (ctx == NULL || tunnel_id == NULL) return false;
    for (size_t index = 0; index < ctx->xfrm_block_count; index++) {
        if (strcmp(ctx->xfrm_block_tunnels[index], tunnel_id) == 0) return true;
    }
    return false;
}

static en_error_code_t apply_xfrm_block(en_strongswan_command_ctx_t *ctx, const en_tunnel_t *tunnel)
{
    if (ctx == NULL || tunnel == NULL || tunnel->tunnel_id[0] == '\0' ||
        tunnel->local_traffic_selector[0] == '\0' || tunnel->remote_traffic_selector[0] == '\0' ||
        !valid_command_token(tunnel->tunnel_id) || !valid_command_token(tunnel->local_traffic_selector) ||
        !valid_command_token(tunnel->remote_traffic_selector)) return EN_ERR_INVALID_ARGUMENT;
    if (has_xfrm_block(ctx, tunnel->tunnel_id)) return EN_ERR_NONE;
    bool outgoing_exists = xfrm_policy_exists(ctx, "out", tunnel->local_traffic_selector, tunnel->remote_traffic_selector);
    bool incoming_exists = xfrm_policy_exists(ctx, "in", tunnel->remote_traffic_selector, tunnel->local_traffic_selector);
    if (outgoing_exists != incoming_exists) return EN_ERR_STATE_CONFLICT;
    if (outgoing_exists && incoming_exists) return EN_ERR_NONE;
    char command[512] = {0};
    if (format_command(command, sizeof(command), "ip xfrm policy add dir out src %s dst %s priority 10000 action block",
        tunnel->local_traffic_selector, tunnel->remote_traffic_selector) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
    if (run_exec_command(command, ctx->dry_run) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    if (format_command(command, sizeof(command), "ip xfrm policy add dir in src %s dst %s priority 10000 action block",
        tunnel->remote_traffic_selector, tunnel->local_traffic_selector) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
    if (run_exec_command(command, ctx->dry_run) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    if (ctx->xfrm_block_count >= EN_MAX_TUNNELS) return EN_ERR_INVALID_ARGUMENT;
    snprintf(ctx->xfrm_block_tunnels[ctx->xfrm_block_count++], EN_MAX_ID_LEN, "%s", tunnel->tunnel_id);
    return EN_ERR_NONE;
}

static bool xfrm_policy_exists(const en_strongswan_command_ctx_t *ctx, const char *direction, const char *source, const char *destination)
{
    if (ctx == NULL || ctx->dry_run || direction == NULL || source == NULL || destination == NULL) return false;
    char command[256] = {0};
    snprintf(command, sizeof(command), "ip xfrm policy list");
    char output[65536] = {0};
    if (run_exec_capture(command, output, sizeof(output)) != EN_ERR_NONE) return false;
    char selector[256] = {0};
    char direction_marker[64] = {0};
    char priority_marker[64] = {0};
    snprintf(selector, sizeof(selector), "src %s dst %s", source, destination);
    snprintf(direction_marker, sizeof(direction_marker), "dir %s", direction);
    snprintf(priority_marker, sizeof(priority_marker), "priority 10000");
    const char *cursor = output;
    while ((cursor = strstr(cursor, selector)) != NULL) {
        const char *section_end = strstr(cursor, "\n\n");
        if (section_end == NULL) section_end = cursor + strlen(cursor);
        if (range_contains(cursor, section_end, direction_marker) &&
            range_contains(cursor, section_end, priority_marker) &&
            range_contains(cursor, section_end, "action block")) return true;
        cursor += strlen(selector);
    }
    return false;
}

static bool range_contains(const char *begin, const char *end, const char *needle)
{
    if (begin == NULL || end == NULL || needle == NULL || begin > end) return false;
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || (size_t)(end - begin) < needle_len) return false;
    for (const char *cursor = begin; cursor + needle_len <= end; cursor++) {
        if (memcmp(cursor, needle, needle_len) == 0) return true;
    }
    return false;
}

static en_error_code_t remove_xfrm_block(en_strongswan_command_ctx_t *ctx, const en_tunnel_t *tunnel)
{
    if (ctx == NULL || tunnel == NULL || !has_xfrm_block(ctx, tunnel->tunnel_id)) return EN_ERR_NONE;
    char command[512] = {0};
    if (format_command(command, sizeof(command), "ip xfrm policy delete dir out src %s dst %s priority 10000",
        tunnel->local_traffic_selector, tunnel->remote_traffic_selector) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
    if (run_exec_command(command, ctx->dry_run) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    if (format_command(command, sizeof(command), "ip xfrm policy delete dir in src %s dst %s priority 10000",
        tunnel->remote_traffic_selector, tunnel->local_traffic_selector) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
    if (run_exec_command(command, ctx->dry_run) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    for (size_t index = 0; index < ctx->xfrm_block_count; index++) {
        if (strcmp(ctx->xfrm_block_tunnels[index], tunnel->tunnel_id) != 0) continue;
        for (size_t next = index + 1; next < ctx->xfrm_block_count; next++) {
            snprintf(ctx->xfrm_block_tunnels[next - 1], EN_MAX_ID_LEN, "%s", ctx->xfrm_block_tunnels[next]);
        }
        ctx->xfrm_block_count--;
        break;
    }
    return EN_ERR_NONE;
}

static en_error_code_t run_vpp_command(const en_vpp_command_ctx_t *ctx, const char *command)
{
    if (ctx == NULL || command == NULL) return EN_ERR_INVALID_ARGUMENT;
    if (ctx->vppctl_socket[0] == '\0') return run_exec_command(command, ctx->dry_run);
    if (!valid_command_token(ctx->vppctl_socket)) return EN_ERR_INVALID_ARGUMENT;
    const char *prefix = "vppctl ";
    if (strncmp(command, prefix, strlen(prefix)) != 0) return EN_ERR_INVALID_ARGUMENT;
    char socket_command[512] = {0};
    if (format_command(socket_command, sizeof(socket_command), "vppctl -s %s %s", ctx->vppctl_socket, command + strlen(prefix)) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
    return run_exec_command(socket_command, ctx->dry_run);
}

static en_error_code_t ensure_vpp_route_tables(const en_vpp_command_ctx_t *ctx, const en_path_t *path)
{
    if (ctx == NULL || path == NULL) return EN_ERR_INVALID_ARGUMENT;
    int tables[EN_MAX_ROUTES] = {0};
    size_t table_count = 0;
    for (size_t index = 0; index < path->route_count; index++) {
        int table_id = path->routes[index].table_id;
        if (table_id <= 0) continue;
        bool known = false;
        for (size_t table_index = 0; table_index < table_count; table_index++) {
            if (tables[table_index] == table_id) {
                known = true;
                break;
            }
        }
        if (known) continue;
        if (table_count >= EN_MAX_ROUTES) return EN_ERR_INVALID_ARGUMENT;
        tables[table_count++] = table_id;
        char command[128] = {0};
        if (format_command(command, sizeof(command), "vppctl ip table add %d", table_id) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
        if (ctx->dry_run) {
            if (run_vpp_command(ctx, command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
            continue;
        }
        char output[65536] = {0};
        if (capture_vpp_command(ctx, "show ip fib", output, sizeof(output)) == EN_ERR_NONE) {
            char marker[64] = {0};
            if (format_command(marker, sizeof(marker), "ipv4-VRF:%d", table_id) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
            if (strstr(output, marker) != NULL) continue;
        }
        if (run_vpp_command(ctx, command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

static en_error_code_t verify_vpp_route(const en_vpp_command_ctx_t *ctx, const char *prefix, const char *expected_next_hop, const char *interface_name, int table_id)
{
    if (ctx == NULL || prefix == NULL || prefix[0] == '\0' || !valid_command_token(prefix) ||
        (expected_next_hop != NULL && expected_next_hop[0] != '\0' && !valid_command_token(expected_next_hop)) ||
        (interface_name != NULL && interface_name[0] != '\0' && !valid_command_token(interface_name))) return EN_ERR_INVALID_ARGUMENT;
    char output[65536] = {0};
    if (capture_vpp_command(ctx, "show ip fib", output, sizeof(output)) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    en_vpp_route_observation_t observation = {0};
    if (en_vpp_parse_show_ip_fib_in_table(output, prefix, table_id, &observation, NULL, 0) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    if (expected_next_hop != NULL && expected_next_hop[0] != '\0' && strcmp(observation.next_hop, expected_next_hop) != 0) return EN_ERR_STATE_CONFLICT;
    if (interface_name != NULL && interface_name[0] != '\0') {
        memset(output, 0, sizeof(output));
        if (capture_vpp_command(ctx, "show interface", output, sizeof(output)) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
        en_vpp_interface_observation_t interface_observation = {0};
        if (en_vpp_parse_show_interface(output, interface_name, &interface_observation, NULL, 0) != EN_ERR_NONE || !interface_observation.up) return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

static en_error_code_t capture_vpp_command(const en_vpp_command_ctx_t *ctx, const char *suffix, char *output, size_t output_len)
{
    if (ctx == NULL || suffix == NULL || output == NULL || output_len < 2) return EN_ERR_INVALID_ARGUMENT;
    char command[512] = {0};
    if (ctx->vppctl_socket[0] != '\0') {
        if (!valid_command_token(ctx->vppctl_socket)) return EN_ERR_INVALID_ARGUMENT;
        if (format_command(command, sizeof(command), "vppctl -s %s %s", ctx->vppctl_socket, suffix) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
    } else {
        if (format_command(command, sizeof(command), "vppctl %s", suffix) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
    }
    return run_exec_capture(command, output, output_len);
}

static en_error_code_t verify_vpp_vlan_interfaces(const en_vpp_command_ctx_t *ctx, const en_path_t *path)
{
    if (ctx == NULL || path == NULL || !ctx->has_vlan_id) return EN_ERR_NONE;
    for (size_t index = 0; index < ctx->edge_count; index++) {
        if (!vpp_edge_used_by_path(&ctx->edges[index], path)) continue;
        char interface_name[EN_MAX_ID_LEN] = {0};
        if (strlen(ctx->edges[index].vpp_interface) >= sizeof(interface_name) - 12) return EN_ERR_INVALID_ARGUMENT;
        if (snprintf(interface_name, sizeof(interface_name), "%.*s.%d", (int)(sizeof(interface_name) - 1), ctx->edges[index].vpp_interface, ctx->vlan_id) >= (int)sizeof(interface_name)) return EN_ERR_INVALID_ARGUMENT;
        char output[65536] = {0};
        if (capture_vpp_command(ctx, "show interface", output, sizeof(output)) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
        en_vpp_interface_observation_t observation = {0};
        if (en_vpp_parse_show_interface(output, interface_name, &observation, NULL, 0) != EN_ERR_NONE || !observation.up) return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

static en_error_code_t ensure_vpp_vlan_interfaces(const en_vpp_command_ctx_t *ctx, const en_path_t *path)
{
    if (ctx == NULL || path == NULL) return EN_ERR_INVALID_ARGUMENT;
    if (!ctx->has_vlan_id) return EN_ERR_NONE;
    if (ctx->vlan_id < 1 || ctx->vlan_id > 4094) return EN_ERR_INVALID_ARGUMENT;
    for (size_t index = 0; index < ctx->edge_count; index++) {
        const en_vpp_edge_t *edge = &ctx->edges[index];
        if (vpp_edge_used_by_path(edge, path) && vpp_table_for_node(path, edge->node_id) < -1) return EN_ERR_STATE_CONFLICT;
    }
    for (size_t index = 0; index < ctx->edge_count; index++) {
        const en_vpp_edge_t *edge = &ctx->edges[index];
        if (!vpp_edge_used_by_path(edge, path)) continue;
        if (edge->allowed_vlan_count > 0) {
            bool allowed = false;
            for (size_t vlan_index = 0; vlan_index < edge->allowed_vlan_count; vlan_index++) {
                if (edge->allowed_vlans[vlan_index] == ctx->vlan_id) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) return EN_ERR_STATE_CONFLICT;
        }
        if (edge->vpp_interface[0] == '\0' || !valid_command_token(edge->vpp_interface)) return EN_ERR_INVALID_ARGUMENT;
        char interface_name[EN_MAX_ID_LEN] = {0};
        if (strlen(edge->vpp_interface) >= sizeof(interface_name) - 12) return EN_ERR_INVALID_ARGUMENT;
        if (snprintf(interface_name, sizeof(interface_name), "%.*s.%d", (int)(sizeof(interface_name) - 1), edge->vpp_interface, ctx->vlan_id) >= (int)sizeof(interface_name)) return EN_ERR_INVALID_ARGUMENT;
        if (!valid_command_token(interface_name)) return EN_ERR_INVALID_ARGUMENT;
        bool exists = false;
        if (!ctx->dry_run) {
            char query[256] = {0};
            if (format_command(query, sizeof(query), "show interface %s", interface_name) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
            char output[65536] = {0};
            if (capture_vpp_command(ctx, query, output, sizeof(output)) == EN_ERR_NONE && strstr(output, interface_name) != NULL) {
                exists = true;
            }
        }
        if (!exists) {
            char create_command[256] = {0};
            if (format_command(create_command, sizeof(create_command), "vppctl create sub-interfaces %s %d", edge->vpp_interface, ctx->vlan_id) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
            if (run_vpp_command(ctx, create_command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
        }
        char up_command[256] = {0};
        if (format_command(up_command, sizeof(up_command), "vppctl set interface %s up", interface_name) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
        if (run_vpp_command(ctx, up_command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

static en_error_code_t ensure_vpp_vlan_interface_tables(const en_vpp_command_ctx_t *ctx, const en_path_t *path)
{
    if (ctx == NULL || path == NULL || !ctx->has_vlan_id) return EN_ERR_INVALID_ARGUMENT;
    for (size_t index = 0; index < ctx->edge_count; index++) {
        const en_vpp_edge_t *edge = &ctx->edges[index];
        if (!vpp_edge_used_by_path(edge, path)) continue;
        int table_id = vpp_table_for_node(path, edge->node_id);
        if (table_id < -1) return EN_ERR_STATE_CONFLICT;
        if (table_id > 0) {
            char interface_name[EN_MAX_ID_LEN] = {0};
            if (snprintf(interface_name, sizeof(interface_name), "%s.%d", edge->vpp_interface, ctx->vlan_id) >= (int)sizeof(interface_name) ||
                !valid_command_token(interface_name)) return EN_ERR_INVALID_ARGUMENT;
            char table_command[256] = {0};
            if (format_command(table_command, sizeof(table_command),
                    "vppctl set interface ip table %s %d", interface_name, table_id) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
            if (run_vpp_command(ctx, table_command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
        }
    }
    return EN_ERR_NONE;
}

static en_error_code_t ensure_vpp_unmatched_vlan_acl(const en_vpp_command_ctx_t *ctx, const en_path_t *path)
{
    if (ctx == NULL || path == NULL) return EN_ERR_INVALID_ARGUMENT;
    if (!ctx->deny_unmatched_vlan) return EN_ERR_NONE;
    if (!ctx->has_vlan_id || ctx->vlan_id < 1 || ctx->vlan_id > 4094) return EN_ERR_INVALID_ARGUMENT;
    for (size_t index = 0; index < ctx->edge_count; index++) {
        const en_vpp_edge_t *edge = &ctx->edges[index];
        if (!vpp_edge_used_by_path(edge, path)) continue;
        if (edge->vpp_interface[0] == '\0' || !valid_command_token(edge->vpp_interface)) return EN_ERR_INVALID_ARGUMENT;
        int acl_index = 10000 + ctx->vlan_id + (int)index;
        char acl_command[512] = {0};
        if (format_command(acl_command, sizeof(acl_command),
                "vppctl set acl-plugin acl index %d deny src 0.0.0.0/0 dst 0.0.0.0/0 , deny src ::/0 dst ::/0 tag ibuki-vlan-deny-%d",
                acl_index, acl_index) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
        if (run_vpp_command(ctx, acl_command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
        char interface_command[256] = {0};
        if (format_command(interface_command, sizeof(interface_command),
                "vppctl set acl-plugin interface %s input acl %d", edge->vpp_interface, acl_index) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
        if (run_vpp_command(ctx, interface_command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

static en_error_code_t remove_vpp_unmatched_vlan_acl(const en_vpp_command_ctx_t *ctx, const char *traffic_key, const en_path_t *path)
{
    if (ctx == NULL || path == NULL || !ctx->deny_unmatched_vlan) return EN_ERR_NONE;
    if (!ctx->has_vlan_id || ctx->vlan_id < 1 || ctx->vlan_id > 4094) return EN_ERR_INVALID_ARGUMENT;
    for (size_t index = 0; index < ctx->edge_count; index++) {
        const en_vpp_edge_t *edge = &ctx->edges[index];
        if (!vpp_edge_used_by_path(edge, path)) continue;
        if (vpp_vlan_edge_is_shared(ctx, traffic_key, path, edge)) continue;
        if (edge->vpp_interface[0] == '\0' || !valid_command_token(edge->vpp_interface)) return EN_ERR_INVALID_ARGUMENT;
        int acl_index = 10000 + ctx->vlan_id + (int)index;
        char interface_command[256] = {0};
        if (format_command(interface_command, sizeof(interface_command),
                "vppctl set acl-plugin interface %s input acl %d del", edge->vpp_interface, acl_index) != EN_ERR_NONE) {
            return EN_ERR_INVALID_ARGUMENT;
        }
        if (run_vpp_command(ctx, interface_command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
        char delete_command[256] = {0};
        if (format_command(delete_command, sizeof(delete_command),
                "vppctl delete acl-plugin acl index %d", acl_index) != EN_ERR_NONE) return EN_ERR_INVALID_ARGUMENT;
        if (run_vpp_command(ctx, delete_command) != EN_ERR_NONE) return EN_ERR_STATE_CONFLICT;
    }
    return EN_ERR_NONE;
}

static bool vpp_edge_used_by_path(const en_vpp_edge_t *edge, const en_path_t *path)
{
    if (edge == NULL || path == NULL || edge->node_id[0] == '\0') return false;
    if (strcmp(edge->node_id, path->source) == 0 || strcmp(edge->node_id, path->destination) == 0) return true;
    for (size_t index = 0; index < path->waypoint_count; index++) {
        if (strcmp(edge->node_id, path->waypoints[index]) == 0) return true;
    }
    for (size_t index = 0; index < path->segment_count; index++) {
        if (strcmp(edge->node_id, path->segments[index].from_node) == 0 || strcmp(edge->node_id, path->segments[index].to_node) == 0) return true;
    }
    for (size_t index = 0; index < path->route_count; index++) {
        if (strcmp(edge->node_id, path->routes[index].node_id) == 0) return true;
    }
    return false;
}

static int vpp_table_for_node(const en_path_t *path, const char *node_id)
{
    if (path == NULL || node_id == NULL || node_id[0] == '\0') return -2;
    int table_id = -1;
    bool found = false;
    for (size_t index = 0; index < path->route_count; index++) {
        const en_route_t *route = &path->routes[index];
        if (strcmp(route->node_id, node_id) != 0 || route->table_id < 0) continue;
        if (found && table_id != route->table_id) return -2;
        table_id = route->table_id;
        found = true;
    }
    return table_id;
}

static bool valid_command_token(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; cursor++) {
        if (!(isalnum(*cursor) || *cursor == ':' || *cursor == '/' || *cursor == '.' || *cursor == '_' || *cursor == '-')) return false;
    }
    return true;
}

static en_error_code_t format_command(char *output, size_t output_len, const char *format, ...)
{
    if (output == NULL || output_len == 0 || format == NULL) return EN_ERR_INVALID_ARGUMENT;
    va_list arguments;
    va_start(arguments, format);
    int written = vsnprintf(output, output_len, format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= output_len) {
        output[0] = '\0';
        return EN_ERR_INVALID_ARGUMENT;
    }
    return EN_ERR_NONE;
}

static void expand_template(char *out, size_t out_len, const char *template_text, const en_tunnel_t *tunnel, const en_path_t *path, const char *traffic_key)
{
    snprintf(out, out_len, "%s", template_text);
    if (tunnel != NULL) {
        replace_all(out, out_len, "{tunnel_id}", tunnel->tunnel_id);
        replace_all(out, out_len, "{local_node}", tunnel->local_node);
        replace_all(out, out_len, "{remote_node}", tunnel->remote_node);
        replace_all(out, out_len, "{local_endpoint}", tunnel->local_endpoint);
        replace_all(out, out_len, "{remote_endpoint}", tunnel->remote_endpoint);
        replace_all(out, out_len, "{local_ts}", tunnel->local_traffic_selector);
        replace_all(out, out_len, "{remote_ts}", tunnel->remote_traffic_selector);
    }
    if (path != NULL) {
        replace_all(out, out_len, "{path_id}", path->path_id);
        replace_all(out, out_len, "{source}", path->source);
        replace_all(out, out_len, "{destination}", path->destination);
    }
    if (traffic_key != NULL) {
        replace_all(out, out_len, "{traffic_key}", traffic_key);
    }
}

static void replace_all(char *text, size_t text_len, const char *needle, const char *replacement)
{
    char buffer[512] = {0};
    char *cursor = text;
    while (*cursor != '\0') {
        char *found = strstr(cursor, needle);
        if (found == NULL) {
            append_text(buffer, sizeof(buffer), cursor);
            break;
        }
        *found = '\0';
        append_text(buffer, sizeof(buffer), cursor);
        append_text(buffer, sizeof(buffer), replacement == NULL ? "" : replacement);
        cursor = found + strlen(needle);
    }
    snprintf(text, text_len, "%s", buffer);
}

static void append_text(char *dst, size_t dst_len, const char *src)
{
    size_t used = strlen(dst);
    if (used >= dst_len - 1) {
        return;
    }
    snprintf(dst + used, dst_len - used, "%s", src == NULL ? "" : src);
}

static bool vpp_vlan_edge_is_shared(const en_vpp_command_ctx_t *ctx, const char *traffic_key, const en_path_t *path, const en_vpp_edge_t *edge)
{
    if (ctx == NULL || traffic_key == NULL || path == NULL || edge == NULL) return false;
    for (size_t index = 0; index < ctx->active_count; index++) {
        if (strcmp(ctx->traffic_keys[index], traffic_key) == 0) continue;
        if (ctx->active_path_objects[index] != NULL && vpp_edge_used_by_path(edge, ctx->active_path_objects[index])) return true;
    }
    return false;
}

static void remember_active_path(en_vpp_command_ctx_t *ctx, const char *traffic_key, const en_path_t *path)
{
    for (size_t i = 0; i < ctx->active_count; i++) {
        if (strcmp(ctx->traffic_keys[i], traffic_key) == 0) {
            snprintf(ctx->active_paths[i], sizeof(ctx->active_paths[i]), "%s", path->path_id);
            ctx->active_path_objects[i] = path;
            return;
        }
    }
    if (ctx->active_count < EN_MAX_CANDIDATES) {
        size_t idx = ctx->active_count++;
        snprintf(ctx->traffic_keys[idx], sizeof(ctx->traffic_keys[idx]), "%s", traffic_key);
        snprintf(ctx->active_paths[idx], sizeof(ctx->active_paths[idx]), "%s", path->path_id);
        ctx->active_path_objects[idx] = path;
    }
}

static void forget_active_path(en_vpp_command_ctx_t *ctx, const char *traffic_key, const char *path_id)
{
    for (size_t i = 0; i < ctx->active_count; i++) {
        if (strcmp(ctx->traffic_keys[i], traffic_key) == 0 && strcmp(ctx->active_paths[i], path_id) == 0) {
            for (size_t j = i + 1; j < ctx->active_count; j++) {
                snprintf(ctx->traffic_keys[j - 1], sizeof(ctx->traffic_keys[j - 1]), "%s", ctx->traffic_keys[j]);
                snprintf(ctx->active_paths[j - 1], sizeof(ctx->active_paths[j - 1]), "%s", ctx->active_paths[j]);
                ctx->active_path_objects[j - 1] = ctx->active_path_objects[j];
            }
            ctx->active_count--;
            ctx->active_path_objects[ctx->active_count] = NULL;
            return;
        }
    }
}

static const en_tunnel_t *find_ctx_tunnel(const en_vpp_command_ctx_t *ctx, const char *tunnel_id)
{
    if (ctx == NULL || tunnel_id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < ctx->tunnel_count; i++) {
        if (strcmp(ctx->tunnels[i].tunnel_id, tunnel_id) == 0) {
            return &ctx->tunnels[i];
        }
    }
    return NULL;
}
