if(NOT DEFINED EVENTNET_NETNS_PLAN OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "EVENTNET_NETNS_PLAN, SOURCE_DIR, and OUTPUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}/vlan-100" "${OUTPUT_DIR}/vlan-200")

execute_process(
    COMMAND "${EVENTNET_NETNS_PLAN}" --intent intent-vlan-100 --out-dir "${OUTPUT_DIR}/vlan-100" "${SOURCE_DIR}/samples/vlan-vrf-isolation.yaml"
    RESULT_VARIABLE status_100
    OUTPUT_VARIABLE output_100
    ERROR_VARIABLE error_100)
if(NOT status_100 EQUAL 0)
    message(FATAL_ERROR "VLAN 100 plan generation failed: ${output_100}${error_100}")
endif()

execute_process(
    COMMAND "${EVENTNET_NETNS_PLAN}" --intent intent-vlan-200 --out-dir "${OUTPUT_DIR}/vlan-200" "${SOURCE_DIR}/samples/vlan-vrf-isolation.yaml"
    RESULT_VARIABLE status_200
    OUTPUT_VARIABLE output_200
    ERROR_VARIABLE error_200)
if(NOT status_200 EQUAL 0)
    message(FATAL_ERROR "VLAN 200 plan generation failed: ${output_200}${error_200}")
endif()

file(READ "${OUTPUT_DIR}/vlan-100/selected-path.txt" summary_100)
file(READ "${OUTPUT_DIR}/vlan-200/selected-path.txt" summary_200)
file(READ "${OUTPUT_DIR}/vlan-100/vpp-netns-route-plan.sh" plan_100)
file(READ "${OUTPUT_DIR}/vlan-100/vpp-route-plan.sh" generic_plan_100)
string(FIND "${summary_100}" "table: 100" table_100)
string(FIND "${summary_100}" "table: 200" mixed_100)
string(FIND "${summary_200}" "table: 200" table_200)
string(FIND "${summary_200}" "table: 100" mixed_200)
string(FIND "${plan_100}" "ensure_vpp_table 100" plan_table)
string(FIND "${plan_100}" "set_vlan_interface_table host-vpp-site-a.100 100" plan_interface_a)
string(FIND "${plan_100}" "set_vlan_interface_table host-vpp-site-b.100 100" plan_interface_b)
string(FIND "${plan_100}" "ip route add 10.10.2.0/24 table 100" plan_route)
string(FIND "${generic_plan_100}" "ip route add 10.10.2.0/24 table 100" generic_route)
if(table_100 LESS 0 OR mixed_100 GREATER -1 OR table_200 LESS 0 OR mixed_200 GREATER -1 OR
   plan_table LESS 0 OR plan_interface_a LESS 0 OR plan_interface_b LESS 0 OR plan_route LESS 0 OR generic_route LESS 0 OR
   plan_table GREATER plan_interface_a OR plan_interface_a GREATER plan_interface_b OR plan_interface_b GREATER plan_route)
    message(FATAL_ERROR "VLAN plans do not keep FIB tables isolated")
endif()

message(STATUS "VLAN VRF isolation plan passed")
