cmake_minimum_required(VERSION 3.30)
project(NRICompatibility LANGUAGES CXX)

# Configure independently: cmake -S Tests/Compatibility -B _Build/Compatibility.
# Backend/dependency options remain owned by NRI and can be passed on the CLI.
if(NOT TARGET NRI)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/NRI")
endif()

if(NOT NRI_ENABLE_VK_SUPPORT)
    message(FATAL_ERROR "NRICompatibilityBaseline requires NRI_ENABLE_VK_SUPPORT")
endif()

option(NRI_COMPATIBILITY_FORCE_LEGACY_SYNC "Disable synchronization2 in the standalone test device" OFF)
if(NRI_COMPATIBILITY_FORCE_LEGACY_SYNC)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=1)
endif()

add_executable(NRICompatibilityBaseline baseline.cpp)
target_compile_definitions(NRICompatibilityBaseline PRIVATE NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)

enable_testing()
add_executable(NRILegacySynchronizationTests legacy_sync.cpp)
target_compile_features(NRILegacySynchronizationTests PRIVATE cxx_std_17)
include(FetchContent)
FetchContent_GetProperties(vulkan_headers SOURCE_DIR vulkan_headers_SOURCE_DIR)
target_include_directories(NRILegacySynchronizationTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../Source/VK" "${vulkan_headers_SOURCE_DIR}/include")
add_test(NAME NRILegacySynchronizationTests COMMAND NRILegacySynchronizationTests)
# Test-only provenance: emit what NRI actually enabled, not a second probe device.
target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_DIAGNOSTICS=1)
target_compile_features(NRICompatibilityBaseline PRIVATE cxx_std_17)
target_link_libraries(NRICompatibilityBaseline PRIVATE NRI)
set_target_properties(NRICompatibilityBaseline PROPERTIES CXX_EXTENSIONS OFF)

find_package(Git QUIET)
set(NRI_COMPATIBILITY_SOURCE_COMMIT "unknown")
if(Git_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../.."
        OUTPUT_VARIABLE NRI_COMPATIBILITY_SOURCE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
target_compile_definitions(NRICompatibilityBaseline PRIVATE
    NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
    NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")

if(NOT NRI_STATIC_LIBRARY)
    add_custom_command(TARGET NRICompatibilityBaseline POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityBaseline>"
        VERBATIM)
endif()
