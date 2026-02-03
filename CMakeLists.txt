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

# 1. Configure Gamepad-Core (Core Logic)
option(LOAD_GAMEPAD_CORE "Load Gamepad-Core library" ON)
if(LOAD_GAMEPAD_CORE)
    add_subdirectory(Gamepad-Core)
endif()

# 2. Configure Platform Sources and Libs
add_subdirectory(Common)

# 3. Configure Integration Tests
option(BUILD_INTEGRATION_TESTS "Build integration tests" ON)
option(AUTOMATED_TESTS "Enable automated tests (timeout for CI)" OFF)

if(BUILD_INTEGRATION_TESTS)
    if(AUTOMATED_TESTS)
        add_compile_definitions(AUTOMATED_TESTS)
    endif()
    add_subdirectory(Integration)
endif()
