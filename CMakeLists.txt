cmake_minimum_required(VERSION 3.16)
project(modern_osc VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(MODERN_OSC_BUILD_UDP "Build the native UDP socket adapter" ON)
option(MODERN_OSC_BUILD_TESTS "Build conformance and integration tests" OFF)
option(MODERN_OSC_BUILD_EXAMPLES "Build the OSC UDP example" OFF)
option(MODERN_OSC_BUILD_FUZZER "Build the ASan/UBSan mutation test" OFF)

add_library(modern_osc_core STATIC src/packet.cpp src/pattern.cpp)
add_library(modern_osc::core ALIAS modern_osc_core)
target_compile_features(modern_osc_core PUBLIC cxx_std_17)
target_include_directories(modern_osc_core PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
set_target_properties(modern_osc_core PROPERTIES EXPORT_NAME core POSITION_INDEPENDENT_CODE ON)
install(TARGETS modern_osc_core EXPORT modern_osc_targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(MODERN_OSC_BUILD_UDP)
    add_library(modern_osc_udp STATIC src/udp.cpp)
    add_library(modern_osc::udp ALIAS modern_osc_udp)
    target_link_libraries(modern_osc_udp PUBLIC modern_osc_core)
    if(WIN32)
        target_link_libraries(modern_osc_udp PRIVATE ws2_32)
        target_compile_definitions(modern_osc_udp PRIVATE _WIN32_WINNT=0x0600 NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    set_target_properties(modern_osc_udp PROPERTIES EXPORT_NAME udp POSITION_INDEPENDENT_CODE ON)
    install(TARGETS modern_osc_udp EXPORT modern_osc_targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

set(modern_osc_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/modern_osc")
configure_package_config_file(cmake/modern_osc-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/modern_osc-config.cmake"
    INSTALL_DESTINATION "${modern_osc_package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/modern_osc-config-version.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(EXPORT modern_osc_targets FILE modern_osc-targets.cmake NAMESPACE modern_osc::
    DESTINATION "${modern_osc_package_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/modern_osc-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/modern_osc-config-version.cmake"
    DESTINATION "${modern_osc_package_dir}")
install(DIRECTORY include/modern_osc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/modern_osc)

if(MODERN_OSC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MODERN_OSC_BUILD_EXAMPLES AND MODERN_OSC_BUILD_UDP)
    add_executable(modern_osc_echo examples/echo.cpp)
    target_link_libraries(modern_osc_echo PRIVATE modern_osc::udp)
endif()

if(MODERN_OSC_BUILD_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "The fuzz target requires a Clang toolchain with ASan and UBSan")
    endif()
    add_executable(modern_osc_fuzz tests/fuzz.cpp src/packet.cpp src/pattern.cpp)
    target_include_directories(modern_osc_fuzz PRIVATE include)
    target_compile_features(modern_osc_fuzz PRIVATE cxx_std_17)
    target_compile_options(modern_osc_fuzz PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(modern_osc_fuzz PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
endif()
