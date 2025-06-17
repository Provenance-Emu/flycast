cmake_minimum_required(VERSION 3.16)
project(FlycastTests)



# --- GoogleTest Setup ---
include(FetchContent)
FetchContent_Declare(
  googletest
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/core/deps/googletest
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(CTest)
include(GoogleTest)

# --- Core Feature Options for Tests ---
# These mirror options from the main CMakeLists.txt to control the build configuration.
option(ENABLE_SH4_IR "Enable SH4 IR for tests" ON)
option(ENABLE_DYNAREC "Enable Dynarec for tests (AICA/DSP/ARM7)" OFF)
option(FAST_MMU "Enable fast MMU for SH4 IR" ON)
option(FPU_INTERP_FALLBACK "Enable FPU interpreter fallback for SH4 IR" ON)
option(USE_OPENMP "Use OpenMP for parallelization" ON)

# The root CMakeLists.txt incorrectly forces SH4_IR_JIT when ENABLE_SH4_IR is ON.
# For tests, we want the IR interpreter, not the JIT recompiler.
# This removes the definition to prevent the JIT from being enabled.
get_property(defs TARGET flycast_lib PROPERTY COMPILE_DEFINITIONS)
list(REMOVE_ITEM defs "SH4_IR_JIT")
set_property(TARGET flycast_lib PROPERTY COMPILE_DEFINITIONS ${defs})

# --- Source File List ---
set(flycast_tests_sources
    main.cpp
    src/AicaArmTest.cpp
    #src/CheatManagerTest.cpp
    #src/ConfigFileTest.cpp
    src/MmuTest.cpp
    src/Sh4InterpreterTest.cpp
    #src/div32_test.cpp
    #src/serialize_test.cpp
    #src/test_stubs.cpp
)

# --- Executable Definition ---
add_executable(flycast_tests ${flycast_tests_sources})
set_target_properties(flycast_tests PROPERTIES MACOSX_BUNDLE OFF)

target_link_libraries(flycast_tests PRIVATE
    flycast_lib
    gtest
    SDL2-static
)

if(APPLE)
    target_link_libraries(flycast_tests PRIVATE
        Syphon
        "-F/System/Library/PrivateFrameworks"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Cocoa"
        "-framework MultitouchSupport"
        "$ENV{HOME}/VulkanSDK/macOS/lib/libMoltenVK.dylib"
    )
endif()

if(USE_VULKAN AND Vulkan_FOUND)
    target_link_libraries(flycast_tests PRIVATE Vulkan::Vulkan)
    # These definitions are required for the test executable itself, not just the library
    target_compile_definitions(flycast_tests PRIVATE VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
    if(APPLE)
        target_compile_definitions(flycast_tests PRIVATE VK_USE_PLATFORM_METAL_EXT)
    endif()
endif()

# --- Include Directories ---
target_include_directories(flycast_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/hw/sh4
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/deps
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/deps/nowide/include
)

# --- Compile Definitions ---
target_compile_definitions(flycast_tests PRIVATE
    FLYCAST_TEST_BUILD
    DEBUGFAST # Ensure MAX_LOGLEVEL is LDEBUG for tests
    FEAT_NO_NETWORKING
    FEAT_NO_GGPO
    FEAT_NO_MINIUPNPC
    FEAT_NO_RCHEEVOS
    VIDEO_ROUTING
)
if(ENABLE_SH4_IR)
    target_compile_definitions(flycast_tests PRIVATE SH4_IR_ENABLED)
endif()
if(ENABLE_DYNAREC)
    target_compile_definitions(flycast_tests PRIVATE AREC_ENABLED DSPREC_ENABLED)
else()
    target_compile_definitions(flycast_tests PRIVATE TARGET_NO_REC=1)
endif()
if(FAST_MMU AND ENABLE_SH4_IR)
    target_compile_definitions(flycast_tests PRIVATE FAST_MMU)
endif()
if(FPU_INTERP_FALLBACK AND ENABLE_SH4_IR)
    target_compile_definitions(flycast_tests PRIVATE FPU_INTERP_FALLBACK)
endif()

# --- Test Discovery ---
gtest_discover_tests(flycast_tests)

# --- Link Libraries ---
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_package(libzip REQUIRED)

target_link_libraries(flycast_tests PRIVATE
    gtest_main
    flycast_lib
)

if(USE_OPENMP)
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(flycast_tests PRIVATE OpenMP::OpenMP_CXX)
    endif()
endif()

# --- CTest Registration ---
add_test(NAME FlycastUnitTests COMMAND flycast_tests)
