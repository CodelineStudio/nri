cmake_minimum_required(VERSION 3.30)
project(NRICompatibility LANGUAGES C CXX)

# Configure independently: cmake -S Tests/Compatibility -B _Build/Compatibility.
# Backend/dependency options remain owned by NRI and can be passed on the CLI.
if(NOT TARGET NRI)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." "${CMAKE_CURRENT_BINARY_DIR}/NRI")
endif()

if(NOT NRI_ENABLE_VK_SUPPORT)
    message(FATAL_ERROR "NRICompatibilityBaseline requires NRI_ENABLE_VK_SUPPORT")
endif()

option(NRI_COMPATIBILITY_MAPPED_RANGES "Exercise explicit mapped range calls on coherent host memory for validation" OFF)
target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_MAPPED_RANGES=$<BOOL:${NRI_COMPATIBILITY_MAPPED_RANGES}>)

option(NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY "Force legal dedicated allocations for memory-failure qualification" OFF)
option(NRI_COMPATIBILITY_MEMORY_FAILURES "Inject one buffer/image allocation failure and one buffer map failure" OFF)
if(NRI_COMPATIBILITY_MEMORY_FAILURES AND NOT NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY)
    message(FATAL_ERROR "Memory failure qualification requires FORCE_DEDICATED_MEMORY")
endif()
target_compile_definitions(NRI_VK PRIVATE
    NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY=$<BOOL:${NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY}>
    NRI_COMPATIBILITY_MEMORY_FAILURES=$<BOOL:${NRI_COMPATIBILITY_MEMORY_FAILURES}>)

option(NRI_COMPATIBILITY_NO_INDIRECT_FIRST_INSTANCE "Disable only indirect first instance" OFF)
option(NRI_COMPATIBILITY_NO_SHADER_DRAW_PARAMETERS "Disable only shader draw parameters" OFF)
if(NRI_COMPATIBILITY_NO_INDIRECT_FIRST_INSTANCE)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_NO_INDIRECT_FIRST_INSTANCE=1)
endif()
if(NRI_COMPATIBILITY_NO_SHADER_DRAW_PARAMETERS)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_NO_SHADER_DRAW_PARAMETERS=1)
endif()
option(NRI_COMPATIBILITY_NO_INDIRECT_OPTIONALS "Disable multi-draw, indirect first instance and shader draw parameters on the actual test device" OFF)
if(NRI_COMPATIBILITY_NO_INDIRECT_OPTIONALS)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_NO_INDIRECT_OPTIONALS=1)
endif()
option(NRI_COMPATIBILITY_NO_TIMELINE "Disable native timeline features and test binary value-fence scheduling" OFF)
option(NRI_COMPATIBILITY_LEGACY_WSI "Disable surface capabilities2/maintenance and retain swapchain in renderpass profiles" OFF)
option(NRI_COMPATIBILITY_ACQUIRE_RESULTS "Inject acquire TIMEOUT/NOT_READY/SUBOPTIMAL and acquire/present OUT_OF_DATE/SURFACE_LOST" OFF)
option(NRI_COMPATIBILITY_PRESENT_OOM "Reject the first maintenance1 present before enqueue with out-of-device-memory" OFF)
option(NRI_COMPATIBILITY_PRESENT_TIMEOUT "Return timeout on the first maintenance1 retirement wait, retaining native objects for retry" OFF)
if(NRI_COMPATIBILITY_PRESENT_TIMEOUT)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_PRESENT_TIMEOUT=1)
endif()
if(NRI_COMPATIBILITY_PRESENT_OOM)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_PRESENT_OOM=1)
endif()
if(NRI_COMPATIBILITY_LEGACY_WSI)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_LEGACY_WSI=1)
endif()
if(NRI_COMPATIBILITY_ACQUIRE_RESULTS)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_ACQUIRE_RESULTS=1)
endif()
option(NRI_COMPATIBILITY_NO_HOST_RESET "Disable native host query reset and test ordered synchronous reset emulation" OFF)
if(NRI_COMPATIBILITY_NO_HOST_RESET)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_NO_HOST_RESET=1)
endif()
set(NRI_COMPATIBILITY_FAIL_SUBMIT 0 CACHE STRING "Test-only scheduler native-submit ordinal to fail with device-memory exhaustion (0=off)")
if(NOT NRI_COMPATIBILITY_FAIL_SUBMIT MATCHES "^[0-9]+$")
    message(FATAL_ERROR "FAIL_SUBMIT must be a nonnegative integer")
endif()
target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_FAIL_SUBMIT=${NRI_COMPATIBILITY_FAIL_SUBMIT})
if(NRI_COMPATIBILITY_NO_TIMELINE)
    target_compile_definitions(NRI_VK PRIVATE NRI_COMPATIBILITY_NO_TIMELINE=1)
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
add_executable(NRICompatibilityQueryReset query_reset.cpp)
find_package(Threads REQUIRED)
target_link_libraries(NRICompatibilityFenceSequences PRIVATE NRI Threads::Threads)
target_link_libraries(NRICompatibilityQueryReset PRIVATE NRI Threads::Threads)
target_compile_features(NRICompatibilityQueryReset PRIVATE cxx_std_17)
set_target_properties(NRICompatibilityQueryReset PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(NRICompatibilityQueryReset PRIVATE NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)
target_compile_features(NRICompatibilityFenceSequences PRIVATE cxx_std_17)
set_target_properties(NRICompatibilityFenceSequences PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(NRICompatibilityFenceSequences PRIVATE NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)
target_compile_definitions(NRICompatibilityBaseline PRIVATE NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)

enable_testing()
add_executable(NRIBufferValidationTests buffer_validation.cpp)
target_compile_features(NRIBufferValidationTests PRIVATE cxx_std_17)
set_target_properties(NRIBufferValidationTests PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(NRIBufferValidationTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../Include" "${CMAKE_CURRENT_LIST_DIR}/../../Source/Validation")
add_test(NAME NRIBufferValidationTests COMMAND NRIBufferValidationTests)
add_executable(NRILegacySynchronizationTests legacy_sync.cpp)
target_compile_features(NRILegacySynchronizationTests PRIVATE cxx_std_17)
include(FetchContent)
FetchContent_GetProperties(vulkan_headers SOURCE_DIR vulkan_headers_SOURCE_DIR)
add_executable(NRIFenceQueryTests fence_query.cpp)
target_compile_features(NRIFenceQueryTests PRIVATE cxx_std_17)
set_target_properties(NRIFenceQueryTests PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(NRIFenceQueryTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../Include" "${CMAKE_CURRENT_LIST_DIR}/../../Source/VK" "${vulkan_headers_SOURCE_DIR}/include")
add_test(NAME NRIFenceQueryTests COMMAND NRIFenceQueryTests)
target_include_directories(NRICompatibilityQueryReset PRIVATE "${vulkan_headers_SOURCE_DIR}/include")
target_include_directories(NRILegacySynchronizationTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../Source/VK" "${vulkan_headers_SOURCE_DIR}/include")
add_test(NAME NRILegacySynchronizationTests COMMAND NRILegacySynchronizationTests)
add_executable(NRIPipelineCacheHeaderTests pipeline_cache_header.cpp)
target_compile_features(NRIPipelineCacheHeaderTests PRIVATE cxx_std_17)
target_include_directories(NRIPipelineCacheHeaderTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../Source/VK" "${vulkan_headers_SOURCE_DIR}/include")
add_test(NAME NRIPipelineCacheHeaderTests COMMAND NRIPipelineCacheHeaderTests)
add_executable(NRIMappedMemoryRangeTests mapped_memory_range.cpp)
target_compile_features(NRIMappedMemoryRangeTests PRIVATE cxx_std_17)
set_target_properties(NRIMappedMemoryRangeTests PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(NRIMappedMemoryRangeTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../Source/VK" "${vulkan_headers_SOURCE_DIR}/include")
add_test(NAME NRIMappedMemoryRangeTests COMMAND NRIMappedMemoryRangeTests)
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

target_compile_definitions(NRICompatibilityQueryReset PRIVATE
    NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
    NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")

add_executable(NRICompatibilityAllocationLoad allocation_load.cpp)
target_compile_features(NRICompatibilityAllocationLoad PRIVATE cxx_std_17)
set_target_properties(NRICompatibilityAllocationLoad PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(NRICompatibilityAllocationLoad PRIVATE NRI)
target_compile_definitions(NRICompatibilityAllocationLoad PRIVATE
    NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
    NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)
if(NOT NRI_STATIC_LIBRARY)
    add_custom_command(TARGET NRICompatibilityAllocationLoad POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityAllocationLoad>"
        VERBATIM)
endif()

add_executable(NRICompatibilityWrappedMemory wrapped_memory.cpp)
target_compile_features(NRICompatibilityWrappedMemory PRIVATE cxx_std_17)
set_target_properties(NRICompatibilityWrappedMemory PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(NRICompatibilityWrappedMemory PRIVATE NRI)
target_include_directories(NRICompatibilityWrappedMemory PRIVATE "${vulkan_headers_SOURCE_DIR}/include")
target_compile_definitions(NRICompatibilityWrappedMemory PRIVATE
    NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
    NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    NRI_COMPATIBILITY_MAPPED_RANGES=$<BOOL:${NRI_COMPATIBILITY_MAPPED_RANGES}>
    NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)

add_executable(NRICompatibilityMemoryLifetime memory_lifetime.cpp)
target_compile_features(NRICompatibilityMemoryLifetime PRIVATE cxx_std_17)
set_target_properties(NRICompatibilityMemoryLifetime PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(NRICompatibilityMemoryLifetime PRIVATE NRI)
target_compile_definitions(NRICompatibilityMemoryLifetime PRIVATE
    NRI_COMPATIBILITY_MAPPED_RANGES=$<BOOL:${NRI_COMPATIBILITY_MAPPED_RANGES}>
    NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
    NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
    NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>
    NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY=$<BOOL:${NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY}>
    NRI_COMPATIBILITY_MEMORY_FAILURES=$<BOOL:${NRI_COMPATIBILITY_MEMORY_FAILURES}>)

if(WIN32)
    add_executable(NRICompatibilityMultiPresentation multi_presentation_win32.cpp)
    target_compile_features(NRICompatibilityMultiPresentation PRIVATE cxx_std_17)
    set_target_properties(NRICompatibilityMultiPresentation PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(NRICompatibilityMultiPresentation PRIVATE NRI user32)
    target_compile_definitions(NRICompatibilityMultiPresentation PRIVATE
        NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
        NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
        NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)
    if(NOT NRI_STATIC_LIBRARY)
        add_custom_command(TARGET NRICompatibilityMultiPresentation POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityMultiPresentation>"
            VERBATIM)
    endif()
    add_executable(NRICompatibilityPresentation presentation_win32.cpp)
    target_compile_features(NRICompatibilityPresentation PRIVATE cxx_std_17)
    set_target_properties(NRICompatibilityPresentation PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(NRICompatibilityPresentation PRIVATE NRI Threads::Threads user32)
    target_compile_definitions(NRICompatibilityPresentation PRIVATE
        NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
        NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
        NRI_COMPATIBILITY_DEFERRED_PRESENT=$<OR:$<BOOL:${NRI_COMPATIBILITY_NO_TIMELINE}>,$<BOOL:${NRI_COMPATIBILITY_NO_HOST_RESET}>>
        NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>
        NRI_COMPATIBILITY_ACQUIRE_RESULTS=$<BOOL:${NRI_COMPATIBILITY_ACQUIRE_RESULTS}>)

    if(NOT NRI_STATIC_LIBRARY)
        add_custom_command(TARGET NRICompatibilityPresentation POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityPresentation>"
            VERBATIM)
    endif()
endif()

set(NRI_COMPATIBILITY_SLANGC "" CACHE FILEPATH "Pinned Slang compiler for shader execution qualification")
set(NRI_COMPATIBILITY_SPIRV_VAL "" CACHE FILEPATH "Pinned spirv-val executable")
set(NRI_COMPATIBILITY_SPIRV_DIS "" CACHE FILEPATH "Pinned spirv-dis executable")
set(NRI_COMPATIBILITY_SLANG_SDK "" CACHE PATH "Optional pinned Slang SDK for API input qualification")
if(NRI_COMPATIBILITY_SLANG_SDK)
    find_package(slang CONFIG REQUIRED PATHS "${NRI_COMPATIBILITY_SLANG_SDK}/cmake" NO_DEFAULT_PATH)
    add_executable(NRICompatibilitySlangInputs slang_inputs.cpp)
    target_compile_features(NRICompatibilitySlangInputs PRIVATE cxx_std_17)
    set_target_properties(NRICompatibilitySlangInputs PROPERTIES CXX_EXTENSIONS OFF)
    target_link_libraries(NRICompatibilitySlangInputs PRIVATE slang::slang)
    if(WIN32)
        add_custom_target(NRICompatibilitySlangRuntime
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:NRICompatibilitySlangInputs>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:slang::slang>" "$<TARGET_FILE_DIR:NRICompatibilitySlangInputs>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:slang::slang-glslang>" "$<TARGET_FILE_DIR:NRICompatibilitySlangInputs>"
            VERBATIM)
        add_dependencies(NRICompatibilitySlangInputs NRICompatibilitySlangRuntime)
    endif()
endif()
if(NRI_COMPATIBILITY_SLANGC OR NRI_COMPATIBILITY_SPIRV_VAL OR NRI_COMPATIBILITY_SPIRV_DIS)
    foreach(tool NRI_COMPATIBILITY_SLANGC NRI_COMPATIBILITY_SPIRV_VAL NRI_COMPATIBILITY_SPIRV_DIS)
        if(NOT EXISTS "${${tool}}")
            message(FATAL_ERROR "Shader qualification requires ${tool}")
        endif()
    endforeach()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(shader_output "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(shader_input_arguments "")
    set(shader_input_dependencies "")
    if(TARGET NRICompatibilitySlangInputs)
        list(APPEND shader_input_arguments --input-driver "$<TARGET_FILE:NRICompatibilitySlangInputs>")
        list(APPEND shader_input_dependencies NRICompatibilitySlangInputs "$<TARGET_FILE:slang::slang>" "$<TARGET_FILE:slang::slang-glslang>")
    endif()
    set(shader_runtime_dependencies "")
    if(WIN32)
        get_filename_component(slang_binary_dir "${NRI_COMPATIBILITY_SLANGC}" DIRECTORY)
        foreach(runtime gfx.dll slang-compiler.dll slang-glsl-module.dll slang-glslang.dll slang-llvm.dll slang-rt.dll slang.dll)
            if(EXISTS "${slang_binary_dir}/${runtime}")
                list(APPEND shader_runtime_dependencies "${slang_binary_dir}/${runtime}")
            endif()
        endforeach()
    endif()
    add_custom_command(OUTPUT "${shader_output}/compiled_shaders.h"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/compile_shaders.py"
            --slangc "${NRI_COMPATIBILITY_SLANGC}" --spirv-val "${NRI_COMPATIBILITY_SPIRV_VAL}"
            --spirv-dis "${NRI_COMPATIBILITY_SPIRV_DIS}" --output "${shader_output}" ${shader_input_arguments}
        DEPENDS compile_shaders.py shader_toolchain.json shaders/common.slang shaders/shared_math.slang shaders/compute.slang shaders/indirect.slang shaders/draw.slang shaders/graphics_state.slang
            "${NRI_COMPATIBILITY_SLANGC}" "${NRI_COMPATIBILITY_SPIRV_VAL}" "${NRI_COMPATIBILITY_SPIRV_DIS}"
            ${shader_runtime_dependencies} ${shader_input_dependencies}
        VERBATIM)
    add_executable(NRICompatibilityInflightResources inflight_resources.cpp "${shader_output}/compiled_shaders.h")
    target_compile_features(NRICompatibilityInflightResources PRIVATE cxx_std_17)
    set_target_properties(NRICompatibilityInflightResources PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(NRICompatibilityInflightResources PRIVATE "${shader_output}")
    target_link_libraries(NRICompatibilityInflightResources PRIVATE NRI)
    target_compile_definitions(NRICompatibilityInflightResources PRIVATE
        NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
        NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
        NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)
    if(NOT NRI_STATIC_LIBRARY)
        add_custom_command(TARGET NRICompatibilityInflightResources POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityInflightResources>"
            VERBATIM)
    endif()
    add_executable(NRICompatibilityShaderExecution shader_execution.cpp draw_capabilities_c.c graphics_state.cpp graphics_state.h pipeline_cache_io.cpp pipeline_cache_io.h "${shader_output}/compiled_shaders.h")
    target_compile_features(NRICompatibilityShaderExecution PRIVATE cxx_std_17)
    set_target_properties(NRICompatibilityShaderExecution PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(NRICompatibilityShaderExecution PRIVATE "${shader_output}")
    target_link_libraries(NRICompatibilityShaderExecution PRIVATE NRI)
    target_compile_definitions(NRICompatibilityShaderExecution PRIVATE
        NRI_COMPATIBILITY_SOURCE_COMMIT="${NRI_COMPATIBILITY_SOURCE_COMMIT}"
        NRI_COMPATIBILITY_COMPILER="${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}"
        NRI_COMPATIBILITY_FORCE_LEGACY_SYNC=$<BOOL:${NRI_COMPATIBILITY_FORCE_LEGACY_SYNC}>)
    if(NOT NRI_STATIC_LIBRARY)
        add_custom_command(TARGET NRICompatibilityShaderExecution POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityShaderExecution>"
            VERBATIM)
    endif()
endif()

if(NOT NRI_STATIC_LIBRARY)
    add_custom_command(TARGET NRICompatibilityWrappedMemory POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityWrappedMemory>"
        VERBATIM)
    add_custom_command(TARGET NRICompatibilityMemoryLifetime POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityMemoryLifetime>"
        VERBATIM)
    add_custom_command(TARGET NRICompatibilityQueryReset POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityQueryReset>"
        VERBATIM)
    add_custom_command(TARGET NRICompatibilityFenceSequences POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityFenceSequences>"
        VERBATIM)
    add_custom_command(TARGET NRICompatibilityBaseline POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:NRI>" "$<TARGET_FILE_DIR:NRICompatibilityBaseline>"
        VERBATIM)
endif()
