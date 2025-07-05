#!/bin/bash

# Exit on any error
set -e

# --- Dependency Fetching ---

# Download and extract RtMidi if not exists
if [ ! -d "rtmidi" ]; then
    echo "Downloading RtMidi..."
    wget -O rtmidi.zip https://github.com/thestk/rtmidi/archive/refs/heads/master.zip
    
    echo "Extracting RtMidi..."
    unzip -q rtmidi.zip
    
    # Create rtmidi directory and copy necessary files
    mkdir -p rtmidi
    cp rtmidi-master/*.h rtmidi/
    cp rtmidi-master/*.cpp rtmidi/
    
    # Cleanup
    rm rtmidi.zip
    rm -rf rtmidi-master
fi

# Download nlohmann/json if not exists
JSON_DIR="third_party/nlohmann"
JSON_PATH="$JSON_DIR/json.hpp"
if [ ! -f "$JSON_PATH" ]; then
    echo "Downloading nlohmann/json.hpp..."
    mkdir -p "$JSON_DIR"
    wget -O "$JSON_PATH" https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
    echo "json.hpp downloaded successfully."
else
    echo "json.hpp already exists. Skipping download."
fi


# --- Build Process ---

# Create build directory
mkdir -p build
cd build

# Configure the project with CMake
echo "Configuring project with CMake..."
cmake ..

# Build the project
echo "Building project..."
make

echo ""
echo "Build complete! Executable: build/JoystickMIDI"
cd ..
