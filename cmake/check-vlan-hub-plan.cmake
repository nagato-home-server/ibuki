if(NOT DEFINED EVENTNET_NETNS_PLAN OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "EVENTNET_NETNS_PLAN, SOURCE_DIR, and OUTPUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
execute_process(
    COMMAND "${EVENTNET_NETNS_PLAN}" --intent intent-vlan-hub --out-dir "${OUTPUT_DIR}"
        "${SOURCE_DIR}/samples/vpp-vlan-hub-netns.yaml"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Hub VLAN plan generation failed: ${output}${error}")
endif()

file(READ "${OUTPUT_DIR}/vpp-netns-route-plan.sh" plan)
string(FIND "${plan}" "ensure_vpp_table 100" table_position)
string(FIND "${plan}" "ensure_vlan_subinterface host-vpp-site-a 100" site_a_position)
string(FIND "${plan}" "ensure_vlan_subinterface host-vpp-hub-1 100" hub_position)
string(FIND "${plan}" "ensure_vlan_subinterface host-vpp-hub-b 100" hub_b_position)
string(FIND "${plan}" "ensure_vlan_subinterface host-vpp-site-b 100" site_b_position)
string(FIND "${plan}" "ensure_unmatched_vlan_acl host-vpp-hub-1" acl_position)
string(FIND "${plan}" "ip route add 10.10.2.0/24 table 100" route_position)
string(FIND "${plan}" "ip route add 10.10.2.0/24 table 100 via 172.16.103.2 host-vpp-hub-b.100" hub_route_position)
if(table_position LESS 0 OR site_a_position LESS 0 OR hub_position LESS 0 OR hub_b_position LESS 0 OR site_b_position LESS 0 OR
   acl_position LESS 0 OR route_position LESS 0 OR table_position GREATER site_a_position OR
   site_a_position GREATER hub_position OR hub_position GREATER hub_b_position OR hub_b_position GREATER site_b_position OR
   site_b_position GREATER acl_position OR acl_position GREATER route_position OR hub_route_position LESS 0)
    message(FATAL_ERROR "Hub VLAN plan does not keep preparation order")
endif()
foreach(interface IN ITEMS host-vpp-site-a host-vpp-hub-1 host-vpp-site-b)
    string(FIND "${plan}" "ensure_vlan_subinterface ${interface} 100" interface_position)
    if(interface_position LESS 0)
        message(FATAL_ERROR "Missing VLAN sub-interface preparation for ${interface}")
    endif()
endforeach()
message(STATUS "Hub VLAN waypoint plan passed")
