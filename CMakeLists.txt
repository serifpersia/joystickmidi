cmake_minimum_required(VERSION 3.15)
project(JoystickMIDI VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type configuration
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/rtmidi)

# --- Windows Specific Configuration ---
# Add preprocessor definition for RtMidi on Windows
add_definitions(-D__WINDOWS_MM__)

# Define system libraries needed on Windows
# winmm: Windows MultiMedia API (for MIDI)
# hid:   Human Interface Device API (likely for joystick input)
# setupapi: Device installation functions (often used with HID)
set(SYSTEM_LIBS winmm hid setupapi)

# MinGW-specific settings (Compiler flags and static linking)
if(MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    # Optional: Keep static linking if desired, otherwise remove the next line
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()
# --- End Windows Specific Configuration ---


# Add RtMidi library
add_library(rtmidi STATIC
    ${CMAKE_SOURCE_DIR}/rtmidi/RtMidi.cpp
    ${CMAKE_SOURCE_DIR}/rtmidi/rtmidi_c.cpp
)

# Main executable
add_executable(JoystickMIDI main.cpp)

# Link the executable against RtMidi and the Windows system libraries
target_link_libraries(JoystickMIDI PRIVATE rtmidi ${SYSTEM_LIBS})

# Output directory configuration (Places the EXE in the build root)
set_target_properties(JoystickMIDI PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)

install(TARGETS JoystickMIDI DESTINATION bin)