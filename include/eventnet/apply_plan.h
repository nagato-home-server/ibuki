#ifndef EVENTNET_APPLY_PLAN_H
#define EVENTNET_APPLY_PLAN_H

#include "eventnet/yaml_config.h"

#define EN_MAX_PLAN_COMMANDS 128
#define EN_MAX_COMMAND_LEN 512
#define EN_MAX_CONFIG_TEXT 8192

typedef struct {
    char commands[EN_MAX_PLAN_COMMANDS][EN_MAX_COMMAND_LEN];
    size_t command_count;
    char rollback_commands[EN_MAX_PLAN_COMMANDS][EN_MAX_COMMAND_LEN];
    size_t rollback_command_count;
    char command_rollbacks[EN_MAX_PLAN_COMMANDS][EN_MAX_COMMAND_LEN];
    bool command_has_rollback[EN_MAX_PLAN_COMMANDS];
    bool has_command_rollbacks;
    char swanctl_conf[EN_MAX_CONFIG_TEXT];
} en_apply_plan_t;

en_error_code_t en_apply_plan_from_config(
    const en_yaml_config_t *config,
    const en_intent_t *intent,
    const en_path_t *selected_path,
    en_apply_plan_t *plan
);

en_error_code_t en_apply_plan_from_config_with_file(
    const en_yaml_config_t *config,
    const en_intent_t *intent,
    const en_path_t *selected_path,
    const char *swanctl_conf_filename,
    en_apply_plan_t *plan
);

en_error_code_t en_apply_plan_write_swanctl_conf(
    const en_apply_plan_t *plan,
    const char *filename
);

en_error_code_t en_apply_plan_write_shell_script(
    const en_apply_plan_t *plan,
    const char *filename
);

en_error_code_t en_apply_plan_run(
    const en_apply_plan_t *plan,
    bool dry_run
);

#endif
