# JoystickMIDI 🎮➡️🎹

A simple **Windows-only** utility to map HID joystick/gamepad inputs (axes/buttons) to MIDI messages (Note On/Off or CC).

[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?style=flat-square&logo=windows)](https://www.microsoft.com/windows)

## Features

*   Map joystick/gamepad buttons and axes to MIDI Note On/Off or Control Change (CC) messages.
*   Configure MIDI channel, note/CC number, and output values.
*   Axis calibration (min/max detection) and reversal.
*   Save and load configurations (`.hidmidi.json`).
*   Simple console interface.

## Requirements

*   Windows 7 or later.
*   A C++ Compiler (MinGW or Visual Studio).
*   CMake.
*   Git.
*   An HID-compliant joystick or gamepad.
*   A MIDI output device (virtual or physical).

## Getting Started (Build from Source)

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/serifpersia/joystickmidi-win.git
    cd joystickmidi-win
    ```
2.  **Run the build script:**
    ```batch
    build.bat
    ```
    *   This script will check for CMake and a compiler. It may offer to install them using `winget` if not found.
    *   It downloads dependencies (RtMidi, nlohmann/json) if needed.
    *   It configures and builds the project using CMake.
    *   The final executable will be in the `build` directory (`build\JoystickMIDI.exe`).

## Usage

1.  Run `build\JoystickMIDI.exe` from your command line or by double-clicking.
2.  **First Run / New Configuration:**
    *   Follow the on-screen prompts to:
        *   Select your HID controller.
        *   Choose the specific button or axis you want to map.
        *   Select your MIDI output port.
        *   Configure the MIDI message type (Note/CC), channel, number, and values.
        *   Calibrate the axis range if mapping an axis.
        *   Save the configuration to a `.hidmidi.json` file.
3.  **Load Configuration:** If `.hidmidi.json` files exist in the same directory, you'll be prompted to load one or create a new configuration.
4.  **Monitoring:** Once configured (or loaded), the application will monitor the selected input and send MIDI messages accordingly. Press `Ctrl+C` or close the console window to exit.

## Get Latest Release

[![Latest Release](https://img.shields.io/github/v/release/serifpersia/joystickmidi-win?label=latest%20release&style=flat-square&logo=github)](https://github.com/serifpersia/joystickmidi-win/releases/latest)

Click the badge above to download the latest pre-compiled version.