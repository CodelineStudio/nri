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

add_executable(NRICompatibilityBaseline baseline.cpp)
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
