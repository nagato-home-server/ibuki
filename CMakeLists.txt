cmake_minimum_required(VERSION 3.16)

project(eventnet_controller_c C)

option(EVENTNET_ENABLE_VPP_API "Enable build-time VPP Binary API dependency checks" OFF)
option(EVENTNET_ENABLE_STRONGSWAN_VICI "Enable build-time strongSwan libvici dependency checks" OFF)
option(EVENTNET_ENABLE_HARDENING "Enable baseline Linux compiler and linker hardening" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(UNIX AND EVENTNET_ENABLE_HARDENING)
    add_compile_options(-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2)
    add_link_options(-Wl,-z,relro,-z,now)
endif()

add_library(eventnet_controller
    third_party/yyjson/yyjson.c
    src/json_output.c
    src/audit.c
    src/command_adapters.c
    src/controller.c
    src/health_probe.c
    src/path_selection.c
    src/apply_plan.c
    src/render_commands.c
    src/state.c
    src/yaml_config.c
    src/strongswan_adapter_mock.c
    src/topology.c
    src/transition.c
    src/telemetry.c
    src/strongswan_observer.c
    src/strongswan_vici_adapter.c
    src/vpp_observer.c
    src/vpp_api_adapter.c
    src/vpp_api_transport.c
    src/vpp_adapter_mock.c
)

target_include_directories(eventnet_controller PUBLIC include third_party/yyjson)
if(UNIX)
    target_link_libraries(eventnet_controller PUBLIC m)
endif()

if(EVENTNET_ENABLE_VPP_API)
    set(VPP_API_INCLUDE_PATHS /usr/include /usr/local/include)
    set(VPP_API_LIBRARY_PATHS /usr/lib /usr/local/lib /usr/lib64 /usr/local/lib64)
    if(CMAKE_LIBRARY_ARCHITECTURE)
        list(APPEND VPP_API_LIBRARY_PATHS "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
    endif()
    if(DEFINED ENV{VPP_PREFIX} AND NOT "$ENV{VPP_PREFIX}" STREQUAL "")
        list(APPEND VPP_API_INCLUDE_PATHS "$ENV{VPP_PREFIX}/include")
        list(APPEND VPP_API_LIBRARY_PATHS "$ENV{VPP_PREFIX}/lib")
    endif()
    find_path(VPP_API_INCLUDE_DIR NAMES vapi/vapi.h PATHS ${VPP_API_INCLUDE_PATHS}
        PATH_SUFFIXES vpp vpp-api vpp_plugins/vpp-api/client vpp_plugins/vpp-api)
    find_library(VPP_API_LIBRARY NAMES vapi vapiclient vppapiclient PATHS ${VPP_API_LIBRARY_PATHS})
    if(NOT VPP_API_INCLUDE_DIR OR NOT VPP_API_LIBRARY)
        message(FATAL_ERROR "EVENTNET_ENABLE_VPP_API requires vapi/vapi.h and libvapi, libvapiclient, or libvppapiclient")
    endif()
    target_include_directories(eventnet_controller PUBLIC ${VPP_API_INCLUDE_DIR})
    target_link_libraries(eventnet_controller PUBLIC ${VPP_API_LIBRARY})
    target_compile_definitions(eventnet_controller PUBLIC EVENTNET_ENABLE_VPP_API=1)
    add_executable(eventnet_vpp_api_transport_probe examples/vpp_api_transport_probe.c)
    target_link_libraries(eventnet_vpp_api_transport_probe PRIVATE eventnet_controller)
    message(STATUS "VPP Binary API enabled: ${VPP_API_INCLUDE_DIR}, ${VPP_API_LIBRARY}")
else()
    message(STATUS "VPP Binary API disabled; using vppctl adapter")
endif()

if(EVENTNET_ENABLE_STRONGSWAN_VICI)
    find_path(STRONGSWAN_VICI_INCLUDE_DIR NAMES libvici.h PATHS /usr/include /usr/local/include PATH_SUFFIXES strongswan)
    set(STRONGSWAN_VICI_LIBRARY_PATHS /usr/lib /usr/local/lib /usr/lib64 /usr/local/lib64)
    if(CMAKE_LIBRARY_ARCHITECTURE)
        list(APPEND STRONGSWAN_VICI_LIBRARY_PATHS "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}")
    endif()
    find_library(STRONGSWAN_VICI_LIBRARY NAMES vici PATHS ${STRONGSWAN_VICI_LIBRARY_PATHS})
    if(NOT STRONGSWAN_VICI_INCLUDE_DIR OR NOT STRONGSWAN_VICI_LIBRARY)
        message(FATAL_ERROR "EVENTNET_ENABLE_STRONGSWAN_VICI requires libvici.h and libvici")
    endif()
    target_sources(eventnet_controller PRIVATE src/strongswan_vici_client.c)
    target_include_directories(eventnet_controller PRIVATE ${STRONGSWAN_VICI_INCLUDE_DIR})
    target_link_libraries(eventnet_controller PRIVATE ${STRONGSWAN_VICI_LIBRARY})
    target_compile_definitions(eventnet_controller PUBLIC EVENTNET_ENABLE_STRONGSWAN_VICI=1)
    add_executable(eventnet_strongswan_vici_probe examples/strongswan_vici_probe.c)
    target_link_libraries(eventnet_strongswan_vici_probe PRIVATE eventnet_controller)
    add_executable(eventnet_strongswan_vici_controller_probe examples/strongswan_vici_controller_probe.c)
    target_link_libraries(eventnet_strongswan_vici_controller_probe PRIVATE eventnet_controller)
    message(STATUS "strongSwan libvici enabled: ${STRONGSWAN_VICI_INCLUDE_DIR}, ${STRONGSWAN_VICI_LIBRARY}")
else()
    message(STATUS "strongSwan libvici disabled; using existing VICI callback/CLI adapters")
endif()

if(MSVC)
    target_compile_options(eventnet_controller PRIVATE /W4)
else()
    target_compile_options(eventnet_controller PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(eventnet_demo examples/demo.c)
target_link_libraries(eventnet_demo PRIVATE eventnet_controller)

add_executable(eventnet_yaml_demo examples/yaml_demo.c)
target_link_libraries(eventnet_yaml_demo PRIVATE eventnet_controller)

add_executable(eventnet_netns_plan examples/netns_plan.c)
target_link_libraries(eventnet_netns_plan PRIVATE eventnet_controller)

add_executable(eventnet_scenario examples/eventnet_scenario.c)
target_link_libraries(eventnet_scenario PRIVATE eventnet_controller)

add_executable(eventnet_agent examples/eventnet_agent.c)
target_link_libraries(eventnet_agent PRIVATE eventnet_controller)

add_executable(eventnetd examples/eventnetd.c)
target_link_libraries(eventnetd PRIVATE eventnet_controller)

add_executable(eventnet_swanctl_observer examples/swanctl_observer.c)
target_link_libraries(eventnet_swanctl_observer PRIVATE eventnet_controller)

add_executable(eventnet_vpp_observer examples/vpp_observer.c)
target_link_libraries(eventnet_vpp_observer PRIVATE eventnet_controller)

add_executable(eventnet_vpp_interface_observer examples/vpp_interface_observer.c)
target_link_libraries(eventnet_vpp_interface_observer PRIVATE eventnet_controller)

enable_testing()
add_executable(eventnet_tests tests/test_controller.c)
target_link_libraries(eventnet_tests PRIVATE eventnet_controller)
add_test(NAME eventnet_tests COMMAND eventnet_tests)
add_test(NAME eventnet_scenario_direct
    COMMAND eventnet_scenario ${CMAKE_CURRENT_SOURCE_DIR}/samples/linux-vm-netns.yaml --step direct-ok)
add_test(NAME eventnet_scenario_fallback
    COMMAND eventnet_scenario ${CMAKE_CURRENT_SOURCE_DIR}/samples/linux-vm-netns.yaml --step direct-failed)
add_test(NAME eventnet_yaml_route_variants
    COMMAND eventnet_yaml_demo --validate-only ${CMAKE_CURRENT_SOURCE_DIR}/samples/route-examples.yaml)
add_test(NAME eventnet_yaml_invalid_routes
    COMMAND eventnet_yaml_demo --validate-only ${CMAKE_CURRENT_SOURCE_DIR}/samples/route-invalid-examples.yaml)
set_tests_properties(eventnet_yaml_invalid_routes PROPERTIES WILL_FAIL TRUE)
add_test(NAME eventnet_yaml_multiport_vpp_edges
    COMMAND eventnet_yaml_demo --validate-only ${CMAKE_CURRENT_SOURCE_DIR}/samples/vpp-vlan-hub-netns.yaml)
add_test(NAME eventnet_yaml_legacy_no_segment_plan
    COMMAND eventnet_netns_plan --intent intent-legacy-no-segment
        --out-dir ${CMAKE_CURRENT_BINARY_DIR}/legacy-no-segment-plan
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/agent-legacy-no-segment.yaml)
set_tests_properties(eventnet_yaml_legacy_no_segment_plan PROPERTIES
    PASS_REGULAR_EXPRESSION "runtime files")
add_test(NAME eventnet_yaml_explicit_no_segment_rejected
    COMMAND eventnet_netns_plan --intent intent-explicit-no-segment
        --out-dir ${CMAKE_CURRENT_BINARY_DIR}/explicit-no-segment-plan
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/legacy-explicit-no-segment.yaml)
set_tests_properties(eventnet_yaml_explicit_no_segment_rejected PROPERTIES
    WILL_FAIL TRUE)
add_test(NAME eventnet_yaml_empty_no_segment_rejected
    COMMAND eventnet_yaml_demo --validate-only
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/invalid-empty-no-segment.yaml)
set_tests_properties(eventnet_yaml_empty_no_segment_rejected PROPERTIES
    WILL_FAIL TRUE)
add_test(NAME eventnet_agent_simulated
    COMMAND eventnet_agent --path path-direct --source site-a --target 203.0.113.9
        --count 1 --interval-ms 0 --simulate 12.5 0.0)
set_tests_properties(eventnet_agent_simulated PROPERTIES
    PASS_REGULAR_EXPRESSION "ibuki.telemetry.path_health.v1")
add_test(NAME eventnet_agent_yaml_simulated
    COMMAND eventnet_agent --yaml ${CMAKE_CURRENT_SOURCE_DIR}/samples/linux-vm-netns.yaml
        --intent intent-a-b --count 1 --interval-ms 0 --simulate 12.5 0.0)
set_tests_properties(eventnet_agent_yaml_simulated PROPERTIES
    PASS_REGULAR_EXPRESSION "path-direct")
add_test(NAME eventnet_agent_yaml_explicit_simulated
    COMMAND eventnet_agent --yaml ${CMAKE_CURRENT_SOURCE_DIR}/samples/route-examples.yaml
        --intent intent-legacy-single-route --count 1 --interval-ms 0 --simulate 12.5 0.0)
set_tests_properties(eventnet_agent_yaml_explicit_simulated PROPERTIES
    PASS_REGULAR_EXPRESSION "path-legacy-single-route")
add_test(NAME eventnet_agent_yaml_evaluated_simulated
    COMMAND eventnet_agent --yaml ${CMAKE_CURRENT_SOURCE_DIR}/samples/route-examples.yaml
        --intent intent-evaluated-stability --count 1 --interval-ms 0 --simulate 12.5 0.0)
set_tests_properties(eventnet_agent_yaml_evaluated_simulated PROPERTIES
    PASS_REGULAR_EXPRESSION "path-legacy-single-route")
add_test(NAME eventnet_agent_yaml_relay_terminal_simulated
    COMMAND eventnet_agent --yaml ${CMAKE_CURRENT_SOURCE_DIR}/samples/route-examples.yaml
        --intent intent-relay-chain-routes --count 1 --interval-ms 0 --simulate 12.5 0.0)
set_tests_properties(eventnet_agent_yaml_relay_terminal_simulated PROPERTIES
    PASS_REGULAR_EXPRESSION "203.0.113.18")
add_test(NAME eventnet_agent_yaml_legacy_no_segment_simulated
    COMMAND eventnet_agent --yaml ${CMAKE_CURRENT_SOURCE_DIR}/samples/agent-legacy-no-segment.yaml
        --intent intent-legacy-no-segment --count 1 --interval-ms 0 --simulate 12.5 0.0)
set_tests_properties(eventnet_agent_yaml_legacy_no_segment_simulated PROPERTIES
    PASS_REGULAR_EXPRESSION "192.0.2.2")
add_test(NAME eventnet_file_replay_legacy_no_segment
    COMMAND eventnetd ${CMAKE_CURRENT_SOURCE_DIR}/samples/agent-legacy-no-segment.yaml
        --intent intent-legacy-no-segment
        --telemetry ${CMAKE_CURRENT_SOURCE_DIR}/samples/telemetry-legacy-no-segment.jsonl
        --batch-size 1 --count 1 --max-age-ms 0)
set_tests_properties(eventnet_file_replay_legacy_no_segment PROPERTIES
    PASS_REGULAR_EXPRESSION "selected_path: path-legacy-no-segment")
add_test(NAME eventnet_file_replay
    COMMAND eventnetd ${CMAKE_CURRENT_SOURCE_DIR}/samples/linux-vm-netns.yaml
        --intent intent-a-b --telemetry ${CMAKE_CURRENT_SOURCE_DIR}/samples/telemetry-replay.jsonl
        --batch-size 3 --count 1 --max-age-ms 0)
set_tests_properties(eventnet_file_replay PROPERTIES
    PASS_REGULAR_EXPRESSION "selected_path: path-direct")
add_test(NAME eventnet_file_replay_fallback
    COMMAND eventnetd ${CMAKE_CURRENT_SOURCE_DIR}/samples/linux-vm-netns.yaml
        --intent intent-a-b --telemetry ${CMAKE_CURRENT_SOURCE_DIR}/samples/telemetry-replay-fallback.jsonl
        --batch-size 3 --count 2 --max-age-ms 0)
set_tests_properties(eventnet_file_replay_fallback PROPERTIES
    PASS_REGULAR_EXPRESSION "selected_path: path-direct;selected_path: path-via-hub")
add_test(NAME eventnet_file_replay_incomplete
    COMMAND eventnetd ${CMAKE_CURRENT_SOURCE_DIR}/samples/linux-vm-netns.yaml
        --intent intent-a-b --telemetry ${CMAKE_CURRENT_SOURCE_DIR}/samples/telemetry-replay.jsonl
        --batch-size 3 --count 2 --max-age-ms 0)
set_tests_properties(eventnet_file_replay_incomplete PROPERTIES
    WILL_FAIL TRUE)
add_test(NAME eventnet_state_wrong_intent
    COMMAND eventnetd ${CMAKE_CURRENT_SOURCE_DIR}/samples/linux-vm-netns.yaml
        --intent intent-a-b --telemetry ${CMAKE_CURRENT_SOURCE_DIR}/samples/telemetry-replay.jsonl
        --batch-size 3 --count 1 --max-age-ms 0
        --state-file ${CMAKE_CURRENT_SOURCE_DIR}/samples/state-wrong-intent.tsv)
set_tests_properties(eventnet_state_wrong_intent PROPERTIES
    WILL_FAIL TRUE)
add_test(NAME eventnet_state_wrong_path
    COMMAND eventnetd ${CMAKE_CURRENT_SOURCE_DIR}/samples/route-examples.yaml
        --intent intent-explicit-single-route --telemetry ${CMAKE_CURRENT_SOURCE_DIR}/samples/telemetry-replay.jsonl
        --batch-size 1 --count 1 --max-age-ms 0
        --state-file ${CMAKE_CURRENT_SOURCE_DIR}/samples/state-wrong-path.tsv)
set_tests_properties(eventnet_state_wrong_path PROPERTIES
    WILL_FAIL TRUE)
add_test(NAME eventnet_state_disabled_node
    COMMAND eventnetd ${CMAKE_CURRENT_SOURCE_DIR}/samples/state-disabled-node.yaml
        --intent intent-a-b --telemetry ${CMAKE_CURRENT_SOURCE_DIR}/samples/telemetry-replay.jsonl
        --batch-size 1 --count 1 --max-age-ms 0
        --state-file ${CMAKE_CURRENT_SOURCE_DIR}/samples/state-disabled-node.tsv)
set_tests_properties(eventnet_state_disabled_node PROPERTIES
    WILL_FAIL TRUE)
add_test(NAME eventnet_yaml_vlan_vrf_isolation
    COMMAND ${CMAKE_COMMAND}
        -DEVENTNET_NETNS_PLAN=$<TARGET_FILE:eventnet_netns_plan>
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/vlan-vrf-isolation-test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-vlan-vrf-isolation.cmake)
add_test(NAME eventnet_yaml_vlan_vrf_conflict
    COMMAND eventnet_netns_plan --intent intent-vlan-conflict
        --out-dir ${CMAKE_CURRENT_BINARY_DIR}/vlan-vrf-conflict-test
        ${CMAKE_CURRENT_SOURCE_DIR}/samples/vlan-vrf-conflict.yaml)
set_tests_properties(eventnet_yaml_vlan_vrf_conflict PROPERTIES WILL_FAIL TRUE)
add_test(NAME eventnet_yaml_vlan_hub_waypoint
    COMMAND ${CMAKE_COMMAND}
        -DEVENTNET_NETNS_PLAN=$<TARGET_FILE:eventnet_netns_plan>
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/vlan-hub-plan-test
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check-vlan-hub-plan.cmake)

install(TARGETS eventnetd
    RUNTIME DESTINATION libexec/ibuki
)
install(TARGETS
    eventnet_agent
    eventnet_yaml_demo
    eventnet_netns_plan
    eventnet_scenario
    eventnet_swanctl_observer
    eventnet_vpp_observer
    eventnet_vpp_interface_observer
    RUNTIME DESTINATION bin
)
if(UNIX)
    install(FILES deploy/ibuki-eventnetd.service DESTINATION lib/systemd/system)
endif()
if(EVENTNET_ENABLE_VPP_API)
    install(TARGETS eventnet_vpp_api_transport_probe RUNTIME DESTINATION bin)
endif()
