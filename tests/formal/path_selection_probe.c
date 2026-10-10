#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 6) return 2;
    const char *scenario = argv[1];
    double active_value = strtod(argv[2], NULL);
    double alternative_value = strtod(argv[3], NULL);
    double hysteresis = strtod(argv[4], NULL);
    double limit = strtod(argv[5], NULL);
    en_controller_t *controller = calloc(1, sizeof(*controller));
    if (controller == NULL) return 2;
    controller->state.path_count = 2;
    controller->state.health_count = 2;
    for (size_t index = 0; index < 2; index++) {
        en_path_t *path = &controller->state.paths[index];
        en_copy_id(path->path_id, sizeof(path->path_id), index == 0 ? "active" : "alternative");
        en_copy_id(path->source, sizeof(path->source), "source");
        en_copy_id(path->destination, sizeof(path->destination), "destination");
        path->administrative_state = EN_ADMIN_ENABLED;
        en_path_health_t *health = &controller->state.health[index];
        en_copy_id(health->path_id, sizeof(health->path_id), path->path_id);
        health->state = EN_HEALTH_HEALTHY;
        health->rtt_ms = index == 0 ? active_value : alternative_value;
        health->packet_loss_percent = health->rtt_ms;
    }
    en_intent_t intent = {0};
    en_copy_id(intent.traffic.source, sizeof(intent.traffic.source), "source");
    en_copy_id(intent.traffic.destination, sizeof(intent.traffic.destination), "destination");
    intent.path_selection.mode = EN_SELECT_EVALUATED;
    intent.path_selection.candidate_count = 2;
    en_copy_id(intent.path_selection.candidates[0], sizeof(intent.path_selection.candidates[0]), "active");
    en_copy_id(intent.path_selection.candidates[1], sizeof(intent.path_selection.candidates[1]), "alternative");
    intent.path_selection.comparison_count = 1;
    intent.path_selection.comparison_order[0] = EN_COMPARE_LATENCY;
    en_path_constraints_t *constraints = &intent.path_selection.constraints;
    constraints->has_hysteresis_percent = true;
    constraints->hysteresis_percent = hysteresis;
    if (strcmp(scenario, "rtt") == 0) {
        constraints->has_max_rtt_ms = true;
        constraints->max_rtt_ms = limit;
    } else if (strcmp(scenario, "loss") == 0) {
        intent.path_selection.comparison_order[0] = EN_COMPARE_PACKET_LOSS;
        constraints->has_max_packet_loss_percent = true;
        constraints->max_packet_loss_percent = limit;
    } else if (strcmp(scenario, "disabled") == 0) {
        controller->state.paths[0].administrative_state = EN_ADMIN_DISABLED;
    } else if (strcmp(scenario, "absent") == 0) {
        intent.path_selection.candidate_count = 1;
        en_copy_id(intent.path_selection.candidates[0], sizeof(intent.path_selection.candidates[0]), "alternative");
    } else if (strcmp(scenario, "waypoint") == 0) {
        constraints->required_waypoint_count = 1;
        en_copy_id(constraints->required_waypoints[0], sizeof(constraints->required_waypoints[0]), "inspection");
        controller->state.paths[1].waypoint_count = 1;
        en_copy_id(controller->state.paths[1].waypoints[0], sizeof(controller->state.paths[1].waypoints[0]), "inspection");
    } else if (strcmp(scenario, "healthy") != 0) {
        free(controller);
        return 2;
    }
    char traffic_key[EN_MAX_TRAFFIC_KEY_LEN];
    en_make_traffic_key(&intent.traffic, traffic_key, sizeof(traffic_key));
    en_set_applied_path(controller, traffic_key, "active");
    en_selection_result_t result = {0};
    en_error_code_t error = en_select_path(controller, &intent, &result);
    printf("%d %s\n", error, result.selected_path);
    free(controller);
    return error == EN_ERR_NONE ? 0 : 1;
}
