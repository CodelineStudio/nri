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

option(NRI_COMPATIBILITY_RENDERPASS1 "Test Vulkan1.1 with only native timeline and host-query-reset extensions" OFF)
option(NRI_COMPATIBILITY_RENDERPASS2 "Test Vulkan1.1 with native renderpass2, timeline and host-query-reset extensions" OFF)
if(NRI_COMPATIBILITY_RENDERPASS1 AND NRI_COMPATIBILITY_RENDERPASS2)
    message(FATAL_ERROR "Select only one renderpass fixture profile")
endif()
if(NRI_COMPATIBILITY_RENDERPASS2)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_RENDERPASS2=1)
endif()
if(NRI_COMPATIBILITY_RENDERPASS1)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_RENDERPASS1=1)
endif()

option(NRI_COMPATIBILITY_BASIC_OPTIONALS "Disable address/indexing/indirect-count optional extensions on the Vulkan1.1 test profile" OFF)
if(NRI_COMPATIBILITY_BASIC_OPTIONALS)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_BASIC_OPTIONALS=1)
endif()

option(NRI_COMPATIBILITY_INDIVIDUAL_FEATURES "Exercise pre-1.2 individual feature/property structures on the test device" OFF)
if(NRI_COMPATIBILITY_INDIVIDUAL_FEATURES)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_INDIVIDUAL_FEATURES=1)
endif()

set(NRI_COMPATIBILITY_API_MINOR 0 CACHE STRING "Test-only instance API ceiling: 0=native, 1=Vulkan1.1, 2=Vulkan1.2, 3=Vulkan1.3")
if(NOT NRI_COMPATIBILITY_API_MINOR MATCHES "^[0-3]$")
    message(FATAL_ERROR "API_MINOR must be 0, 1, 2 or 3")
endif()
target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_API_MINOR=${NRI_COMPATIBILITY_API_MINOR})

option(NRI_COMPATIBILITY_FORCE_LEGACY_SYNC "Disable synchronization2 in the standalone test device" OFF)
if(NRI_COMPATIBILITY_FORCE_LEGACY_SYNC)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=1)
endif()

add_executable(NRICompatibilityBaseline baseline.cpp)
add_executable(NRICompatibilityFenceSequences fence_sequences.cpp)
find_package(Threads REQUIRED)
target_link_libraries(NRICompatibilityFenceSequences PRIVATE NRI Threads::Threads)
target_compile_features(NRICompatibilityFenceSequences PRIVATE cxx_std_17)
set_target_properties(NRICompatibilityFenceSequences PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(NRICompatibilityFenceSequences PRIVATE NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)
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
target_compile_definitions(NRICompatibilityFenceSequences PRIVATE
    NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
    NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")

if(NOT NRI_STATIC_LIBRARY)
    add_custom_command(TARGET NRICompatibilityFenceSequences POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityFenceSequences>"
        VERBATIM)
    add_custom_command(TARGET NRICompatibilityBaseline POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityBaseline>"
        VERBATIM)
endif()
