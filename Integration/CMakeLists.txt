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


# 2. Create Executables
add_executable(test-gamepad-outputs
        Features/test_gamepad_outputs.cpp
)

# Audio Haptics Integration Test - Reads .wav file and sends to controller
add_executable(test-audio-haptics
        Features/test_audio_haptics.cpp
)

# Channels Haptics Integration Test - Different WAV for different controllers
add_executable(test-channels-haptics
        Features/test_channels_haptics.cpp
)

# Gamepad Input Integration Test
add_executable(test-gamepad-inputs
        Features/test_gamepad_inputs.cpp
)

# 3. Configure Includes
# The test needs to find Core and Common Test headers
# Use CMAKE_CURRENT_SOURCE_DIR to be robust when included as submodule
set(COMMON_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Public"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Private"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Common"
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_include_directories(test-gamepad-outputs PRIVATE ${COMMON_INCLUDES})
target_include_directories(test-audio-haptics PRIVATE ${COMMON_INCLUDES})
target_include_directories(test-channels-haptics PRIVATE ${COMMON_INCLUDES})
target_include_directories(test-gamepad-inputs PRIVATE ${COMMON_INCLUDES})

# Register tests with CTest
if(BUILD_INTEGRATION_TESTS OR BUILD_TESTS)
    enable_testing()
    add_test(NAME GamepadOutputs COMMAND test-gamepad-outputs)
    add_test(NAME AudioHaptics COMMAND test-audio-haptics)
    add_test(NAME GamepadInputs COMMAND test-gamepad-inputs)
endif()

# Add project root path as a compile definition
target_compile_definitions(test-audio-haptics PRIVATE GAMEPAD_CORE_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_definitions(test-channels-haptics PRIVATE GAMEPAD_CORE_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")

# 4. Linking
# Link with static lib (GamepadCore) and Common Test Lib
target_link_libraries(test-gamepad-outputs
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)

target_link_libraries(test-audio-haptics
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)

target_link_libraries(test-channels-haptics
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)

target_link_libraries(test-gamepad-inputs
        PRIVATE
        GamepadCore
        GamepadCoreTestCommon
)
