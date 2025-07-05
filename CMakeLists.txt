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

# Include directories for dependencies
include_directories(
    ${CMAKE_SOURCE_DIR}/rtmidi
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann
)

# --- Platform Specific Configuration ---

if(WIN32)
    # --- Windows Specific Configuration ---
    message(STATUS "Configuring for Windows")
    # Add preprocessor definition for RtMidi on Windows
    add_definitions(-D__WINDOWS_MM__)

    # Define system libraries needed on Windows
    # winmm: Windows MultiMedia API (for MIDI)
    # hid:   Human Interface Device API (for joystick input)
    # setupapi: Device installation functions (often used with HID)
    set(SYSTEM_LIBS winmm hid setupapi)

    # MinGW-specific settings (Compiler flags and static linking)
    if(MINGW)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
    endif()

elseif(UNIX)
    # --- Linux Specific Configuration ---
    message(STATUS "Configuring for Linux")
    # Add preprocessor definition for RtMidi on Linux (ALSA)
    add_definitions(-D__LINUX_ALSA__)

    # Find required libraries
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    pkg_check_modules(UDEV REQUIRED libudev)

    # Define system libraries needed on Linux
    # asound: ALSA library for MIDI
    # udev:   For device enumeration
    # pthread: Required by RtMidi on Linux
    set(SYSTEM_LIBS ${ALSA_LIBRARIES} ${UDEV_LIBRARIES} pthread)

    # Add include directories for system libs
    include_directories(${ALSA_INCLUDE_DIRS} ${UDEV_INCLUDE_DIRS})

    # Set compiler flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
endif()


# Add RtMidi library
add_library(rtmidi STATIC
    ${CMAKE_SOURCE_DIR}/rtmidi/RtMidi.cpp
    ${CMAKE_SOURCE_DIR}/rtmidi/rtmidi_c.cpp
)

# Main executable
add_executable(JoystickMIDI main.cpp)

# Link the executable against RtMidi and the platform-specific system libraries
target_link_libraries(JoystickMIDI PRIVATE rtmidi ${SYSTEM_LIBS})

# Output directory configuration (Places the executable in the build root)
set_target_properties(JoystickMIDI PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)

install(TARGETS JoystickMIDI DESTINATION bin)
