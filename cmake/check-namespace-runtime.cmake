set(OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/namespace-runtime-test")
file(REMOVE_RECURSE "${OUTPUT_DIR}")
execute_process(
    COMMAND "${EVENTNET_NETNS_PLAN}" --intent intent-gre-vpp-data --out-dir "${OUTPUT_DIR}"
        "${SOURCE_DIR}/samples/gre-vpp-data-plane.yaml"
    RESULT_VARIABLE PLAN_RESULT
    OUTPUT_VARIABLE PLAN_OUTPUT
    ERROR_VARIABLE PLAN_ERROR
)
if(NOT PLAN_RESULT EQUAL 0)
    message(FATAL_ERROR "namespace runtime plan generation failed: ${PLAN_OUTPUT}${PLAN_ERROR}")
endif()

file(READ "${OUTPUT_DIR}/vpp-route-plan.sh" VPP_PLAN)
file(READ "${OUTPUT_DIR}/vpp-netns-route-plan.sh" VPP_NETNS_PLAN)
if(NOT VPP_PLAN MATCHES "run_vpp_node site-a")
    message(FATAL_ERROR "site-a VPP dispatch missing")
endif()
if(NOT VPP_NETNS_PLAN MATCHES "run_vpp_node site-a")
    message(FATAL_ERROR "site-a netns VPP dispatch missing")
endif()
if(NOT VPP_NETNS_PLAN MATCHES "/run/ibuki-vpp-ns/site-a/cli.sock")
    message(FATAL_ERROR "site-a VPP socket missing")
endif()
if(NOT VPP_NETNS_PLAN MATCHES "/run/ibuki-vpp-ns/site-b/cli.sock")
    message(FATAL_ERROR "site-b VPP socket missing")
endif()
if(NOT VPP_NETNS_PLAN MATCHES "run_vpp_node site-b ip route add 10\\.10\\.1\\.0/24 via 10\\.255\\.0\\.1 gre0")
    message(FATAL_ERROR "site-b GRE return route missing or has a CIDR next hop")
endif()
if(NOT VPP_NETNS_PLAN MATCHES "run_vpp_node site-b create gre tunnel src 172\\.16\\.2\\.2 dst 172\\.16\\.1\\.1 instance 0")
    message(FATAL_ERROR "site-b GRE tunnel creation missing")
endif()
if(NOT VPP_NETNS_PLAN MATCHES "run_vpp_node site-b set interface ip address gre0 10\\.255\\.0\\.2/30")
    message(FATAL_ERROR "site-b GRE address missing")
endif()
message(STATUS "Namespace runtime VPP dispatch checks passed")
