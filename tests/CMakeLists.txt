add_executable(modern_osc_conformance conformance.cpp)
target_link_libraries(modern_osc_conformance PRIVATE modern_osc::core)
add_test(NAME modern_osc_conformance COMMAND modern_osc_conformance)

if(MODERN_OSC_BUILD_UDP)
    add_executable(modern_osc_udp_test udp.cpp)
    target_link_libraries(modern_osc_udp_test PRIVATE modern_osc::udp)
    add_test(NAME modern_osc_udp_test COMMAND modern_osc_udp_test)
    set_tests_properties(modern_osc_udp_test PROPERTIES TIMEOUT 15)
endif()

add_executable(modern_osc_subscription subscription.cpp)
target_link_libraries(modern_osc_subscription PRIVATE modern_osc::core)
add_test(NAME modern_osc_subscription COMMAND modern_osc_subscription)
