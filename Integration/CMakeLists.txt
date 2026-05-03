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

# 1. Configure Platform Sources and Libs
# GamepadCoreTestCommon is added by the parent Tests/CMakeLists.txt

set(GAMEPAD_CORE_BUILD_AUDIO_TESTS OFF)
if(GAMEPAD_CORE_AUDIO_ENABLED_BUILD AND TARGET opus)
    set(GAMEPAD_CORE_BUILD_AUDIO_TESTS ON)
endif()
if(AUTOMATED_TESTS)
    set(GAMEPAD_CORE_BUILD_AUDIO_TESTS OFF)
endif()
if(NOT GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    message(STATUS "GamepadCore Tests: audio integration targets disabled")
endif()


# 2. Create Executables
add_executable(test-gamepad-outputs
        Features/test_gamepad_outputs.cpp
)

# Audio Haptics Integration Test - Reads .wav file and sends to controller
if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    add_executable(test-audio-haptics
            Features/test_audio_haptics.cpp
    )

    # Channels Haptics Integration Test - Different WAV for different controllers
    add_executable(test-channels-haptics
            Features/test_channels_haptics.cpp
    )
endif()

# Gamepad Input Integration Test
add_executable(test-gamepad-inputs
        Features/test_gamepad_inputs.cpp
)

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
    target_include_directories(test-channels-haptics PRIVATE ${COMMON_INCLUDES})
endif()

# Register tests with CTest
if(BUILD_INTEGRATION_TESTS OR BUILD_TESTS)
    enable_testing()
    add_test(NAME GamepadOutputs COMMAND test-gamepad-outputs)
    add_test(NAME GamepadInputs COMMAND test-gamepad-inputs)
    if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
        add_test(NAME AudioHaptics COMMAND test-audio-haptics)
    endif()
endif()

# Add project root path as a compile definition
get_filename_component(PROJECT_ROOT_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    target_compile_definitions(test-audio-haptics PRIVATE GAMEPAD_CORE_PROJECT_ROOT="${PROJECT_ROOT_ABS}")
    target_compile_definitions(test-channels-haptics PRIVATE GAMEPAD_CORE_PROJECT_ROOT="${PROJECT_ROOT_ABS}")
endif()

# 4. Linking
# Link with static lib (GamepadCore) and Common Test Lib
target_link_libraries(test-gamepad-outputs
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)

if(GAMEPAD_CORE_BUILD_AUDIO_TESTS)
    target_link_libraries(test-audio-haptics
            PRIVATE
            GamepadCore
            GamepadCoreTestCommon
            opus
    )

    target_link_libraries(test-channels-haptics
            PRIVATE
            GamepadCore
            GamepadCoreTestCommon
    )
endif()

target_link_libraries(test-gamepad-inputs
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)
