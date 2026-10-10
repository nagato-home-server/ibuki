#include "internal.h"

#include <assert.h>
#include <string.h>

size_t nondet_size(void);

void harness_candidates(void)
{
    en_controller_t controller;
    controller.state.path_count = 0;
    controller.state.health_count = 0;
    controller.state.applied_count = 0;
    controller.state.node_count = 0;
    en_intent_t intent = {0};
    en_selection_result_t result = {0};
    intent.path_selection.mode = EN_SELECT_PRIORITY;
    intent.path_selection.candidate_count = nondet_size();
    __CPROVER_assume(intent.path_selection.candidate_count <= EN_MAX_CANDIDATES + 1);
    en_error_code_t error = en_select_path(&controller, &intent, &result);
    if (intent.path_selection.candidate_count > EN_MAX_CANDIDATES) {
        assert(error == EN_ERR_INVALID_ARGUMENT);
    }
    assert(result.candidate_count <= EN_MAX_CANDIDATES);
    assert(result.excluded_count <= EN_MAX_CANDIDATES);
}

void harness_comparisons(void)
{
    en_controller_t controller;
    controller.state.health_count = 0;
    controller.state.applied_count = 0;
    controller.state.node_count = 0;
    en_intent_t intent = {0};
    en_selection_result_t result = {0};
    controller.state.path_count = 2;
    memset(controller.state.paths, 0, 2 * sizeof(en_path_t));
    strcpy(controller.state.paths[0].path_id, "first");
    strcpy(controller.state.paths[1].path_id, "second");
    intent.path_selection.mode = EN_SELECT_EVALUATED;
    intent.path_selection.candidate_count = 2;
    strcpy(intent.path_selection.candidates[0], "first");
    strcpy(intent.path_selection.candidates[1], "second");
    intent.path_selection.comparison_count = nondet_size();
    __CPROVER_assume(intent.path_selection.comparison_count <= EN_MAX_COMPARISONS + 1);
    en_error_code_t error = en_select_path(&controller, &intent, &result);
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
