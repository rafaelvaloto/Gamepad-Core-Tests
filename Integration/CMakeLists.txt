cmake_minimum_required(VERSION 3.20)

project(GamepadCoreIntegrationTest)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# IMPORTANT: Add Unicode definitions to avoid conversion errors from CHAR* to LPCWSTR
# and to ensure linking with correct Windows API versions (CreateFileW, etc)
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
endif()

set(GAMEPAD_CORE_BUILD_AUDIO_TESTS OFF)
if(TARGET opus)
    set(GAMEPAD_CORE_BUILD_AUDIO_TESTS ON)
endif()

if(AUTOMATED_TESTS)
    set(GAMEPAD_CORE_BUILD_AUDIO_TESTS OFF)
endif()

# 2. Create Executables
add_executable(test-gamepad-outputs
        Features/test_gamepad_outputs.cpp
)

# Gamepad Input Integration Test
add_executable(test-gamepad-inputs
        Features/test_gamepad_inputs.cpp
)

# Audio Haptics Integration Test - Reads .wav file and sends to controller
if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    add_executable(test-audio-haptics
            Features/test_audio_haptics.cpp
    )
endif()

# 3. Configure Includes
# GamepadCore and GamepadCoreTestCommon propagate their includes when linked
set(COMMON_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common"
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_include_directories(test-gamepad-outputs PRIVATE ${COMMON_INCLUDES})
target_include_directories(test-gamepad-inputs PRIVATE ${COMMON_INCLUDES})
if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    target_include_directories(test-audio-haptics PRIVATE ${COMMON_INCLUDES})
endif()

# Register tests with CTest
if(AUTOMATED_TESTS)
    enable_testing()
    add_test(NAME GamepadOutputs COMMAND test-gamepad-outputs)
    add_test(NAME GamepadInputs COMMAND test-gamepad-inputs)
endif()

# Add project root path as a compile definition
get_filename_component(PROJECT_ROOT_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    target_compile_definitions(test-audio-haptics PRIVATE GAMEPAD_CORE_PROJECT_ROOT="${PROJECT_ROOT_ABS}")
endif()

# Link with static lib (GamepadCore) and Common Test Lib
target_link_libraries(test-gamepad-outputs
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)
target_link_libraries(test-gamepad-inputs
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)

if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    target_link_libraries(test-audio-haptics
            PRIVATE
            opus
            GamepadCore
            GamepadCoreTestCommon
    )
endif()
