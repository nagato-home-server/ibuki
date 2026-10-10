#include "internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

size_t nondet_size(void);

void harness_candidates(void)
{
    en_controller_t storage;
    en_controller_t *controller = &storage;
    controller->state.path_count = 0;
    controller->state.health_count = 0;
    controller->state.applied_count = 0;
    controller->state.node_count = 0;
    en_intent_t intent = {0};
    en_selection_result_t result = {0};
    intent.path_selection.mode = EN_SELECT_PRIORITY;
    intent.path_selection.candidate_count = nondet_size();
    __CPROVER_assume(intent.path_selection.candidate_count <= EN_MAX_CANDIDATES + 1);
    en_error_code_t error = en_select_path(controller, &intent, &result);
    if (intent.path_selection.candidate_count > EN_MAX_CANDIDATES) {
        assert(error == EN_ERR_INVALID_ARGUMENT);
    }
    assert(result.candidate_count <= EN_MAX_CANDIDATES);
    assert(result.excluded_count <= EN_MAX_CANDIDATES);
}

void harness_comparisons(void)
{
    en_controller_t storage;
    en_controller_t *controller = &storage;
    controller->state.health_count = 0;
    controller->state.applied_count = 0;
    controller->state.node_count = 0;
    en_intent_t intent = {0};
    en_selection_result_t result = {0};
    controller->state.path_count = 2;
    for (size_t index = 0; index < 2; index++) {
        controller->state.paths[index].administrative_state = EN_ADMIN_ENABLED;
        controller->state.paths[index].waypoint_count = 0;
        controller->state.paths[index].segment_count = 0;
    }
    strcpy(controller->state.paths[0].path_id, "first");
    strcpy(controller->state.paths[1].path_id, "second");
    intent.path_selection.mode = EN_SELECT_EVALUATED;
    intent.path_selection.candidate_count = 2;
    strcpy(intent.path_selection.candidates[0], "first");
    strcpy(intent.path_selection.candidates[1], "second");
    intent.path_selection.comparison_count = nondet_size();
    __CPROVER_assume(intent.path_selection.comparison_count <= EN_MAX_COMPARISONS + 1);
    for (size_t index = 0; index < EN_MAX_COMPARISONS; index++) {
        intent.path_selection.comparison_order[index] = EN_COMPARE_PATH_ID;
    }
    en_error_code_t error = en_select_path(controller, &intent, &result);
    if (intent.path_selection.comparison_count > EN_MAX_COMPARISONS) {
        assert(error == EN_ERR_INVALID_ARGUMENT);
    } else {
        assert(error == EN_ERR_NONE);
    }
}

void harness_copy(void)
{
    char destination[EN_MAX_ID_LEN];
    char source[EN_MAX_ID_LEN + 1];
    for (size_t index = 0; index < sizeof(source); index++) source[index] = 'a';
    source[sizeof(source) - 1] = '\0';
    size_t capacity = nondet_size();
    __CPROVER_assume(capacity <= sizeof(destination));
    en_copy_id(destination, capacity, source);
    if (capacity > 0) assert(destination[capacity - 1] == '\0');
}

void harness_count_predicate(void)
{
    en_intent_t intent;
    intent.path_selection.candidate_count = nondet_size();
    intent.path_selection.comparison_count = nondet_size();
    intent.path_selection.constraints.forbidden_waypoint_count = nondet_size();
    intent.path_selection.constraints.required_waypoint_count = nondet_size();
    intent.path_selection.constraints.required_capability_count = nondet_size();
    bool valid = en_intent_counts_valid(&intent);
    assert(valid == (intent.path_selection.candidate_count <= EN_MAX_CANDIDATES &&
        intent.path_selection.comparison_count <= EN_MAX_COMPARISONS &&
        intent.path_selection.constraints.forbidden_waypoint_count <= EN_MAX_WAYPOINTS &&
        intent.path_selection.constraints.required_waypoint_count <= EN_MAX_WAYPOINTS &&
        intent.path_selection.constraints.required_capability_count <= EN_MAX_CAPABILITIES));
    assert(!en_intent_counts_valid(NULL));
}

void harness_reject_invalid_counts(void)
{
    en_controller_t *controller = malloc(1);
    __CPROVER_assume(controller != NULL);
    for (size_t field = 0; field < 5; field++) {
        en_intent_t intent = {0};
        en_selection_result_t result;
        if (field == 0) intent.path_selection.candidate_count = EN_MAX_CANDIDATES + 1;
        else if (field == 1) intent.path_selection.comparison_count = EN_MAX_COMPARISONS + 1;
        else if (field == 2) intent.path_selection.constraints.forbidden_waypoint_count = EN_MAX_WAYPOINTS + 1;
        else if (field == 3) intent.path_selection.constraints.required_waypoint_count = EN_MAX_WAYPOINTS + 1;
        else intent.path_selection.constraints.required_capability_count = EN_MAX_CAPABILITIES + 1;
        assert(en_select_path(controller, &intent, &result) == EN_ERR_INVALID_ARGUMENT);
        assert(result.candidate_count == 0);
        assert(result.excluded_count == 0);
    }
    free(controller);
}
