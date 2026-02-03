# Copyright (c) 2025 Rafael Valoto. All Rights Reserved.

cmake_minimum_required(VERSION 3.20)
project(GamepadCoreTestCommon)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TEST_COMMON_SOURCES "")

if(WIN32)
    list(APPEND TEST_COMMON_SOURCES
            Platform/windows/windows_device_info.cpp
    )
    add_compile_definitions(UNICODE _UNICODE)
elseif(UNIX AND NOT APPLE)
    list(APPEND TEST_COMMON_SOURCES
            Platform/linux/linux_device_info.cpp
    )
endif()

add_library(GamepadCoreTestCommon STATIC
    ${TEST_COMMON_SOURCES}
)

# Use relative paths from CMAKE_CURRENT_SOURCE_DIR to be robust as submodule
set(CORE_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Public")
set(CORE_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../Source/Private")

target_include_directories(GamepadCoreTestCommon PUBLIC
        .
        ${CORE_PUBLIC}
        ${CORE_PRIVATE}
)

target_compile_definitions(GamepadCoreTestCommon PUBLIC BUILD_GAMEPAD_CORE_TESTS)

if(WIN32)
    target_link_libraries(GamepadCoreTestCommon PUBLIC
        Setupapi
        Hid
        ole32
        oleaut32
        uuid
        winmm
    )
elseif(UNIX AND NOT APPLE)
    find_package(SDL2 REQUIRED)
    target_link_libraries(GamepadCoreTestCommon PUBLIC ${SDL2_LIBRARIES})
    target_include_directories(GamepadCoreTestCommon PUBLIC ${SDL2_INCLUDE_DIRS})
endif()

target_link_libraries(GamepadCoreTestCommon PUBLIC GamepadCore)
