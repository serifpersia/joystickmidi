#define UNICODE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define _WIN32_WINNT 0x0601 // Minimum Windows version (Windows 7)

#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>   // For device interface functions
#include <iostream>
#include <vector>
#include <string>
#include <limits>       // For numeric_limits
#include <iomanip>      // For std::setw, std::fixed, std::setprecision
#include <memory>       // For std::unique_ptr, std::make_unique
#include <algorithm>    // For std::find, std::max, std::min, std::swap
#include <cstdlib>      // For system("cls"), system("pause")
#include <chrono>       // For timing (steady_clock, milliseconds)
#include <thread>       // For std::this_thread::sleep_for
#include <sstream>      // For std::wstringstream
#include <fstream>      // For file I/O (ofstream, ifstream)
#include <filesystem>   // For listing config files (C++17)
#include "rtmidi/RtMidi.h" // Include RtMidi header
#include "third_party/nlohmann/json.hpp" // Include nlohmann/json header

// Use nlohmann json namespace
using json = nlohmann::json;
namespace fs = std::filesystem;

// --- Constants ---
constexpr int TARGET_FPS = 60; // Target rate for console display updates
constexpr std::chrono::milliseconds FRAME_DURATION(1000 / TARGET_FPS);
constexpr int DISPLAY_WIDTH = 80; // Width for console output line
const std::string CONFIG_EXTENSION = ".hidmidi.json"; // Specific extension for config files
constexpr int DEFAULT_MIDI_SEND_INTERVAL_MS = 1; // Default MIDI send interval in ms
const wchar_t* HID_MIDI_INPUT_LISTENER_CLASS = L"HidMidiInputListenerWindowClass"; // Use a single class name

// --- Enums ---
enum class MidiMessageType {
    NONE,
    NOTE_ON_OFF, // For buttons
    CC           // For buttons or axes
};

NLOHMANN_JSON_SERIALIZE_ENUM(MidiMessageType, {
    {MidiMessageType::NONE, nullptr}, // Represent NONE as null in JSON
    {MidiMessageType::NOTE_ON_OFF, "NoteOnOff"},
    {MidiMessageType::CC, "CC"}
})

// --- Structures ---

// Structure to hold information about a detected HID device
struct HidDeviceInfo {
    HANDLE handle = nullptr; // Raw Input device handle
    std::wstring name = L"Unknown Device";
    std::wstring path; // Device path, used for identification
    PHIDP_PREPARSED_DATA preparsedData = nullptr; // Needs cleanup via HeapFree
    HIDP_CAPS caps = {}; // Device capabilities
    std::vector<HIDP_BUTTON_CAPS> buttonCaps; // Button capabilities
    std::vector<HIDP_VALUE_CAPS> valueCaps; // Value (axis) capabilities
    RID_DEVICE_INFO rawInfo = {}; // Store raw device info for registration details

    // Destructor to clean up preparsed data
    ~HidDeviceInfo() {
        if (preparsedData) {
            HeapFree(GetProcessHeap(), 0, preparsedData);
            preparsedData = nullptr;
        }
        // handle is managed elsewhere (e.g., g_selectedDeviceHandle) or is null
    }
    // Delete copy constructor/assignment to prevent double-free
    HidDeviceInfo(const HidDeviceInfo&) = delete;
    HidDeviceInfo& operator=(const HidDeviceInfo&) = delete;

    // Move constructor
    HidDeviceInfo(HidDeviceInfo&& other) noexcept
        : handle(other.handle), name(std::move(other.name)), path(std::move(other.path)),
          preparsedData(other.preparsedData), caps(other.caps),
          buttonCaps(std::move(other.buttonCaps)), valueCaps(std::move(other.valueCaps)),
          rawInfo(other.rawInfo)
    {
        // Null out the moved-from object's pointer to prevent it from freeing the memory
        other.preparsedData = nullptr;
        other.handle = nullptr; // Handle ownership is also transferred implicitly
    }
    // Move assignment operator
    HidDeviceInfo& operator=(HidDeviceInfo&& other) noexcept {
        if (this != &other) {
             // Free existing resource if any
             if (preparsedData) HeapFree(GetProcessHeap(), 0, preparsedData);
             // Transfer ownership
             handle = other.handle; // Transfer handle ownership
             name = std::move(other.name);
             path = std::move(other.path);
             preparsedData = other.preparsedData;
             caps = other.caps;
             buttonCaps = std::move(other.buttonCaps);
             valueCaps = std::move(other.valueCaps);
             rawInfo = other.rawInfo;
             // Null out the moved-from object's pointers
             other.preparsedData = nullptr;
             other.handle = nullptr; // Null out handle in source
        }
        return *this;
    }
    // Default constructor
    HidDeviceInfo() = default;
};

// Structure to hold selected control info (part of the config)
struct HidControlInfo {
    bool isButton = false;
    USAGE usagePage = 0;
    USAGE usage = 0;
    LONG logicalMin = 0; // Original logical min from HID report descriptor
    LONG logicalMax = 0; // Original logical max from HID report descriptor
    std::wstring name = L"Unknown Control"; // User-friendly name
};

// Structure holding the complete mapping configuration (saved/loaded)
struct MidiMappingConfig {
    std::wstring hidDevicePath;       // To find the device on load
    std::wstring hidDeviceName;       // For display/reference
    HidControlInfo control;           // Details of the specific HID control being mapped
    std::string midiDeviceName;       // To find the MIDI port on load
    MidiMessageType midiMessageType = MidiMessageType::NONE;
    int midiChannel = 0;              // 0-15 (saved as 0-15, often displayed as 1-16)
    int midiNoteOrCCNumber = 0;       // 0-127
    int midiValueNoteOnVelocity = 64; // For Note On (1-127 recommended)
    int midiValueCCOn = 127;          // For Button CC when pressed (0-127)
    int midiValueCCOff = 0;           // For Button CC when released (0-127)
    LONG calibrationMinHid = 0;       // Captured min HID value during calibration (for Axis->CC)
    LONG calibrationMaxHid = 0;       // Captured max HID value during calibration (for Axis->CC)
    bool calibrationDone = false;     // Flag indicating if calibration was performed for this axis
    bool reverseAxis = false;         // Flag to reverse MIDI output for axes
    int midiSendIntervalMs = DEFAULT_MIDI_SEND_INTERVAL_MS; // Configurable MIDI send interval

    // Explicit default constructor
    MidiMappingConfig() = default;
};

// --- JSON Serialization/Deserialization Prototypes ---
void to_json(json& j, const MidiMappingConfig& cfg);
void from_json(const json& j, MidiMappingConfig& cfg);
void to_json(json& j, const HidControlInfo& ctrl);
void from_json(const json& j, HidControlInfo& ctrl);

// --- Global State ---
// HID related
HANDLE g_selectedDeviceHandle = nullptr; // Handle to the specific HID device selected for monitoring
PHIDP_PREPARSED_DATA g_selectedDevicePreparsedData = nullptr; // Global copy of preparsed data for WindowProc
HWND g_messageWindow = nullptr; // Handle to the *single* hidden message-only window
bool g_quitFlag = false; // Flag to signal the main loop to exit
bool g_windowClassRegistered = false; // Track if the single window class is registered
// Input state
LONG g_currentValue = 0; // The latest raw value read from the selected HID control
LONG g_previousValue = -1; // The raw value from the previous loop iteration (used for change detection)
bool g_valueChanged = false; // Flag set by WindowProc when g_currentValue changes
// MIDI Output
RtMidiOut g_midiOut; // RtMidi object for sending MIDI messages
// Current Configuration
MidiMappingConfig g_currentConfig; // Holds the active config (loaded from file or created by user)
// MIDI Timing/Rate Limiting
std::chrono::steady_clock::time_point g_lastMidiSendTime; // Timestamp of the last MIDI message sent
int g_lastSentMidiValue = -1; // The last *calculated* MIDI value (0-127) sent for axes/CC buttons
// Raw Input Device Info for Registration
RAWINPUTDEVICE g_rid; // Store the RID info needed for registration/unregistration


// --- Forward Declarations ---
// Utility Helpers
std::wstring GetHidDeviceName(HANDLE hHidDevice);
std::wstring GetUsageName(USAGE usagePage, USAGE usage);
void ClearScreen();
int GetUserSelection(int maxValidChoice, int minValidChoice = 0);
void DisplayMonitoringOutput(const COORD& startPos, HANDLE hConsole);
std::string WStringToString(const std::wstring& wstr);
std::wstring StringToWString(const std::string& str);
void ClearInputBuffer(); // Helper to clear std::cin
// Console cursor RAII Helper Class
class ConsoleCursorHider;
// Core Logic Helpers
bool ListMidiOutputPorts(RtMidiOut& midiOut);
int SelectMidiOutputPort(RtMidiOut& midiOut);
bool PerformCalibration(); // Modifies g_currentConfig
bool SaveConfiguration(const MidiMappingConfig& config, const std::string& filename);
bool LoadConfiguration(const std::string& filename, MidiMappingConfig& config); // Modifies config
std::vector<fs::path> ListConfigurations(const std::string& directory);
void SendMidiMessage(); // Uses global state to send MIDI
// NEW: Window/Input setup helpers
bool SetupInputWindowAndRegistration(HINSTANCE hInstance);
void CleanupInputWindowAndRegistration(HINSTANCE hInstance);

// --- Console Cursor RAII Helper ---
class ConsoleCursorHider {
    HANDLE hConsole_;
    BOOL originallyVisible_;
    CONSOLE_CURSOR_INFO initialInfo_{};
public:
    ConsoleCursorHider(HANDLE hConsole) : hConsole_(hConsole), originallyVisible_(FALSE) {
        if (hConsole_ != INVALID_HANDLE_VALUE && hConsole_ != NULL) {
            if (GetConsoleCursorInfo(hConsole_, &initialInfo_)) {
                originallyVisible_ = initialInfo_.bVisible;
                if (originallyVisible_) {
                    CONSOLE_CURSOR_INFO info = initialInfo_;
                    info.bVisible = FALSE;
                    SetConsoleCursorInfo(hConsole_, &info);
                }
            }
        }
    }
    ~ConsoleCursorHider() {
        if (hConsole_ != INVALID_HANDLE_VALUE && hConsole_ != NULL && originallyVisible_) {
             CONSOLE_CURSOR_INFO info = initialInfo_;
             info.bVisible = TRUE;
             SetConsoleCursorInfo(hConsole_, &info);
        }
    }
    ConsoleCursorHider(const ConsoleCursorHider&) = delete;
    ConsoleCursorHider& operator=(const ConsoleCursorHider&) = delete;
    ConsoleCursorHider(ConsoleCursorHider&&) = delete;
    ConsoleCursorHider& operator=(ConsoleCursorHider&&) = delete;
};


// --- Window Procedure ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_INPUT: {
            // Check if we are ready to process input (device selected, data parsed, config valid)
            if (g_selectedDeviceHandle == nullptr || g_selectedDevicePreparsedData == nullptr || g_currentConfig.midiMessageType == MidiMessageType::NONE) {
                 return DefWindowProc(hwnd, uMsg, wParam, lParam);
            }
            UINT dwSize = 0;
            // Get the size of the input data.
            GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
            if (dwSize == 0) return 0;

            auto lpb = std::make_unique<BYTE[]>(dwSize);
            if (!lpb) return 0;

            // Get the raw input data.
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.get(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
                return 0;
            }

            RAWINPUT* raw = (RAWINPUT*)lpb.get();

            // Check if it's from the specific HID device we care about
            if (raw->header.dwType == RIM_TYPEHID && raw->header.hDevice == g_selectedDeviceHandle)
            {
                NTSTATUS status;
                ULONG usageValue = 0;
                bool valueReadSuccess = false;
                const auto& ctrl = g_currentConfig.control;

                if (ctrl.isButton) {
                    ULONG usageCount = 1; // Check for the presence of *one* usage
                    USAGE usageToCheck = ctrl.usage;
                    status = HidP_GetUsages(HidP_Input, ctrl.usagePage, 0, &usageToCheck, &usageCount,
                                            g_selectedDevicePreparsedData, (PCHAR)raw->data.hid.bRawData,
                                            raw->data.hid.dwSizeHid * raw->data.hid.dwCount);

                    // HIDP_STATUS_SUCCESS means the button IS pressed.
                    // HIDP_STATUS_USAGE_NOT_FOUND means the button is NOT pressed (or not in this report).
                    // Other statuses are generally errors, except BUFFER_TOO_SMALL (which implies found)
                    if (status == HIDP_STATUS_SUCCESS || status == HIDP_STATUS_BUFFER_TOO_SMALL) {
                         usageValue = 1; // Button is ON
                         valueReadSuccess = true;
                    } else if (status == HIDP_STATUS_USAGE_NOT_FOUND) {
                         usageValue = 0; // Button is OFF
                         valueReadSuccess = true;
                    } else if (status == HIDP_STATUS_INCOMPATIBLE_REPORT_ID) {
                         // Ignore this report, it doesn't match the expected format for this usage/page
                    } else {
                         // Consider other statuses potential issues, but don't halt everything
                         // std::cerr << "HidP_GetUsages unexpected status: " << std::hex << status << std::dec << std::endl;
                    }
                } else { // Axis/Value
                    status = HidP_GetUsageValue(HidP_Input, ctrl.usagePage, 0, ctrl.usage, &usageValue,
                                                g_selectedDevicePreparsedData, (PCHAR)raw->data.hid.bRawData,
                                                raw->data.hid.dwSizeHid * raw->data.hid.dwCount);
                    if (status == HIDP_STATUS_SUCCESS) {
                         valueReadSuccess = true;
                    } else if (status == HIDP_STATUS_INCOMPATIBLE_REPORT_ID) {
                        // Ignore this report
                    } else if (status != HIDP_STATUS_USAGE_NOT_FOUND) { // Don't log not found errors for axes
                        // std::cerr << "HidP_GetUsageValue failed: Status=" << std::hex << status << std::dec << std::endl;
                    }
                }

                // Check if the value actually changed
                if (valueReadSuccess && static_cast<LONG>(usageValue) != g_currentValue) {
                    g_currentValue = static_cast<LONG>(usageValue);
                    g_valueChanged = true; // Signal main loop
                }
            }
            // Let the default procedure handle cleanup for lParam if needed
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        } // end case WM_INPUT

		case WM_DESTROY:
			// This now signals the *application* is exiting via the window being destroyed
			std::cout << "\nWM_DESTROY received for main input window. Signaling quit." << std::endl;
			g_quitFlag = true;
			PostQuitMessage(0); // Ensures the main message loop terminates if GetMessage is used
			return 0;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}


// --- Helper Function Implementations ---

void ClearScreen() {
    #ifdef _WIN32
        system("cls");
    #else
        system("clear");
    #endif
}

void ClearInputBuffer() {
    if (std::cin.fail()) {
        std::cin.clear();
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

std::wstring GetHidDeviceName(HANDLE hHidDevice) {
    wchar_t buffer[256] = {0};
    std::wstring result = L"Unknown HID Device";
    bool nameFound = false;

    if (HidD_GetProductString(hHidDevice, buffer, sizeof(buffer))) {
        std::wstring name = buffer;
        size_t first = name.find_first_not_of(L" \t\n\r\f\v");
        if (first != std::wstring::npos) {
            size_t last = name.find_last_not_of(L" \t\n\r\f\v");
            name = name.substr(first, (last - first + 1));
            if (!name.empty()) {
                 result = name;
                 nameFound = true;
            }
        }
    }
    if (!nameFound && HidD_GetManufacturerString(hHidDevice, buffer, sizeof(buffer))) {
        std::wstring name = buffer;
        size_t first = name.find_first_not_of(L" \t\n\r\f\v");
        if (first != std::wstring::npos) {
            size_t last = name.find_last_not_of(L" \t\n\r\f\v");
            name = name.substr(first, (last - first + 1));
             if (!name.empty()) {
                result = name + L" (Manufacturer)";
            }
        }
    }
    return result;
}

std::wstring GetUsageName(USAGE usagePage, USAGE usage) {
    // (Implementation unchanged)
    if (usagePage == 0x02) { // Simulation Controls Page
        switch (usage) {
            case 0xBA: return L"Rudder"; case 0xBB: return L"Throttle";
            case 0xC4: return L"Accelerator"; case 0xC5: return L"Brake";
            default: break;
        }
    } else if (usagePage == 0x05) { // Game Controls Page
        switch (usage) {
             case 0x20: return L"POV Hat Up"; case 0x21: return L"POV Hat Down";
             case 0x22: return L"POV Hat Right"; case 0x23: return L"POV Hat Left";
             case 0x24: return L"POV Hat Press";
             default: break;
        }
    } else if (usagePage == 0x01) { // Generic Desktop Page
        switch (usage) {
            case 0x01: return L"Pointer"; case 0x02: return L"Mouse";
            case 0x04: return L"Joystick"; case 0x05: return L"Gamepad";
            case 0x06: return L"Keyboard"; case 0x07: return L"Keypad";
            case 0x30: return L"X Axis"; case 0x31: return L"Y Axis"; case 0x32: return L"Z Axis";
            case 0x33: return L"Rx Axis"; case 0x34: return L"Ry Axis"; case 0x35: return L"Rz Axis";
            case 0x36: return L"Slider"; case 0x37: return L"Dial"; case 0x38: return L"Wheel";
            case 0x39: return L"Hat Switch";
            case 0x80: return L"System Control";
            default: break;
        }
    } else if (usagePage == 0x09) { // Button Page
        return L"Button " + std::to_wstring(usage);
    } else if (usagePage == 0x0C) { // Consumer Page
        switch (usage) {
             case 0xE9: return L"Volume Up"; case 0xEA: return L"Volume Down";
             case 0xB0: return L"Play"; case 0xB1: return L"Pause";
             case 0xB5: return L"Next Track"; case 0xB6: return L"Prev Track";
             default: break;
        }
    }

    // Fallback for unknown usages
    std::wstringstream ss_page, ss_usage;
    ss_page << std::hex << std::setw(2) << std::setfill(L'0') << usagePage;
    ss_usage << std::hex << std::setw(2) << std::setfill(L'0') << usage;
    return L"Usage(UP:0x" + ss_page.str() + L", U:0x" + ss_usage.str() + L")";
}

void DisplayMonitoringOutput(const COORD& startPos, HANDLE hConsole) {
    // (Implementation unchanged)
    const int BAR_WIDTH = 30;
    std::wstringstream wss;

    wss << L"[" << std::left << std::setw(20) << g_currentConfig.control.name.substr(0, 20) << L"] "; // Limit name width

    if (g_currentConfig.control.isButton) {
        wss << (g_currentValue ? L"[ ### ON ### ]" : L"[ --- OFF -- ]");
    } else { // Axis
        double percentage = 0.0;
        LONG displayRangeMin = g_currentConfig.control.logicalMin;
        LONG displayRangeMax = g_currentConfig.control.logicalMax;

        // Use calibrated range if available and valid
        if (g_currentConfig.calibrationDone) {
             displayRangeMin = g_currentConfig.calibrationMinHid;
             displayRangeMax = g_currentConfig.calibrationMaxHid;
        }

        LONG displayRange = displayRangeMax - displayRangeMin;
        if (displayRange > 0) {
             LONG clampedValue = std::max(displayRangeMin, std::min(displayRangeMax, g_currentValue));
             percentage = static_cast<double>(clampedValue - displayRangeMin) * 100.0 / static_cast<double>(displayRange);
        } else if (g_currentValue >= displayRangeMax) { // Handle case where min == max
             percentage = 100.0;
        } else {
             percentage = 0.0;
        }

        int barLength = static_cast<int>((percentage / 100.0) * BAR_WIDTH + 0.5);
        barLength = std::max(0, std::min(BAR_WIDTH, barLength));

        std::wstring bar(barLength, L'#');
        std::wstring empty(BAR_WIDTH - barLength, L'-');

        wss << L"|" << bar << empty << L"| ";
        wss << std::fixed << std::setprecision(1) << std::setw(5) << percentage << L"% ";
        wss << L"(Raw:" << std::right << std::setw(6) << g_currentValue << L")";
    }

    std::wstring outputStr = wss.str();
    if (outputStr.length() < DISPLAY_WIDTH) {
        outputStr.append(DISPLAY_WIDTH - outputStr.length(), L' ');
    } else if (outputStr.length() > DISPLAY_WIDTH) {
        outputStr.resize(DISPLAY_WIDTH);
    }

    if (hConsole != INVALID_HANDLE_VALUE && hConsole != NULL) {
        DWORD charsWritten;
        SetConsoleCursorPosition(hConsole, startPos);
        WriteConsoleW(hConsole, outputStr.c_str(), (DWORD)outputStr.length(), &charsWritten, NULL);
    }
}

int GetUserSelection(int maxValidChoice, int minValidChoice /*=0*/) {
    // (Implementation unchanged)
    long long choice = -1;
    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
             std::cerr << "Input error or EOF detected. Exiting." << std::endl;
             g_quitFlag = true; // Signal exit
             return -1;
        }

        try {
            size_t processedChars = 0;
            if (line.empty() || line.find_first_not_of(" \t") == std::string::npos) {
                 std::cout << "Empty input. Please enter a number." << std::endl;
                 continue;
            }
            choice = std::stoll(line, &processedChars);

            if (processedChars > 0 && processedChars == line.length() && choice >= minValidChoice && choice <= maxValidChoice) {
                return static_cast<int>(choice);
            } else {
                std::cout << "Invalid input. Please enter a whole number between " << minValidChoice << " and " << maxValidChoice << "." << std::endl;
            }
        } catch (const std::invalid_argument&) {
            std::cout << "Invalid input. Please enter a number." << std::endl;
        } catch (const std::out_of_range&) {
            std::cout << "Input out of range. Please enter a number between " << minValidChoice << " and " << maxValidChoice << "." << std::endl;
        }
    }
}

bool ListMidiOutputPorts(RtMidiOut& midiOut) {
    // (Implementation unchanged)
     unsigned int nPorts = 0;
     try {
          nPorts = midiOut.getPortCount();
     } catch (const RtMidiError& error) {
          std::cerr << "RtMidiError getting port count: " << error.getMessage() << std::endl;
          return false;
     }

    std::cout << "Available MIDI Output ports:\n";
    if (nPorts == 0) {
        std::cout << "  No MIDI output ports available." << std::endl;
        return true;
    }
    bool success = true;
    for (unsigned int i = 0; i < nPorts; ++i) {
        try {
            std::string portName = midiOut.getPortName(i);
            std::cout << "  [" << i << "]: " << portName << std::endl;
        } catch (const RtMidiError& error) {
            std::cerr << "  [" << i << "]: Error getting port name: " << error.getMessage() << std::endl;
            success = false;
        }
    }
    return success;
}

int SelectMidiOutputPort(RtMidiOut& midiOut) {
    // (Implementation unchanged)
    if (!ListMidiOutputPorts(midiOut)) {
        std::cerr << "Failed to list MIDI ports properly." << std::endl;
    }
     unsigned int portCount = 0;
     try {
          portCount = midiOut.getPortCount();
     } catch (...) {
         std::cerr << "Error getting MIDI port count after listing." << std::endl;
         return -1;
     }

     if (portCount == 0) {
         return -1;
     }

    std::cout << "\nSelect MIDI Output port number: ";
    int selection = GetUserSelection(portCount - 1, 0);
    if (g_quitFlag) return -1;
    return selection;
}

// *** NEW ***: Setup window and raw input registration
bool SetupInputWindowAndRegistration(HINSTANCE hInstance) {
    // --- Register the Window Class ---
    // Ensure it's not already registered (though UnregisterClass is called on cleanup)
    UnregisterClass(HID_MIDI_INPUT_LISTENER_CLASS, hInstance); // Try unregistering first, might fail harmlessly

    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc; // Use the single global WindowProc
    wc.hInstance = hInstance;
    wc.lpszClassName = HID_MIDI_INPUT_LISTENER_CLASS;
    // No need for CS_OWNDC, CS_HREDRAW, CS_VREDRAW for a message-only window

    if (!RegisterClass(&wc)) {
         std::cerr << "Error: Failed to register the input listener window class. Error: " << GetLastError() << std::endl;
         return false;
     }
    g_windowClassRegistered = true; // Mark as registered

    // --- Create the Message-Only Window ---
    g_messageWindow = CreateWindowEx(
        0,                          // Optional window styles.
        HID_MIDI_INPUT_LISTENER_CLASS, // Window class
        L"HID MIDI Input Listener", // Window text (Not visible)
        0,                          // Window style (None for message-only)
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, // Position and size (ignored)
        HWND_MESSAGE,               // Parent window (Message-Only)
        NULL,                       // Menu
        hInstance,                  // Instance handle
        NULL                        // Additional application data
    );

    if (!g_messageWindow) {
        std::cerr << "Error: Failed to create the message-only input window. Error: " << GetLastError() << std::endl;
        CleanupInputWindowAndRegistration(hInstance); // Attempt cleanup
        return false;
    }

    // --- Register for Raw Input ---
    if (g_selectedDeviceHandle == nullptr) {
         std::cerr << "Error: Cannot register raw input - No HID device selected." << std::endl;
         CleanupInputWindowAndRegistration(hInstance);
         return false;
    }

    // Get the necessary UsagePage and Usage from the device handle
    RID_DEVICE_INFO rawInfo;
    UINT sizeRI = sizeof(rawInfo);
    rawInfo.cbSize = sizeRI;
    if (GetRawInputDeviceInfo(g_selectedDeviceHandle, RIDI_DEVICEINFO, &rawInfo, &sizeRI) != sizeRI || sizeRI == 0) {
        std::cerr << "Error: Failed to get Raw Input Info for registration. Error: " << GetLastError() << std::endl;
        CleanupInputWindowAndRegistration(hInstance);
        return false;
    }

    // Store RID for later unregistration and setup registration struct
    g_rid.usUsagePage = rawInfo.hid.usUsagePage;
    g_rid.usUsage = rawInfo.hid.usUsage;
    g_rid.dwFlags = RIDEV_INPUTSINK; // Receive input even when not foreground, deliver to specific window
    g_rid.hwndTarget = g_messageWindow; // Target our message window

    if (RegisterRawInputDevices(&g_rid, 1, sizeof(g_rid)) == FALSE) {
        std::cerr << "Error: Failed to register raw input device. Error: " << GetLastError() << std::endl;
        CleanupInputWindowAndRegistration(hInstance);
        return false;
    }

    std::cout << "Input window and raw input registration successful." << std::endl;
    return true;
}

// *** NEW ***: Cleanup window and raw input registration
void CleanupInputWindowAndRegistration(HINSTANCE hInstance) {
     std::cout << "Cleaning up input window and registration..." << std::endl;

     // --- Unregister Raw Input ---
     // Need valid g_rid data from successful registration
     if (g_rid.hwndTarget != NULL) { // Check if registration likely succeeded
        RAWINPUTDEVICE rid_remove = g_rid; // Copy registration details
        rid_remove.dwFlags = RIDEV_REMOVE; // Flag for removal
        rid_remove.hwndTarget = NULL;      // Target must be NULL for removal

        std::cout << "Unregistering raw input device (UP:0x" << std::hex << g_rid.usUsagePage << ", U:0x" << g_rid.usUsage << std::dec << ")..." << std::endl;
        if (RegisterRawInputDevices(&rid_remove, 1, sizeof(rid_remove)) == FALSE) {
            std::cerr << "Warning: Failed to unregister raw input device (Error: " << GetLastError() << ")." << std::endl;
        }
        // Clear g_rid to prevent reuse attempts
        g_rid = {};
     } else {
         // std::cout << "Skipping raw input unregistration (was not registered or target was null)." << std::endl;
     }

     // --- Destroy Window ---
     if (g_messageWindow) {
          std::cout << "Destroying message window..." << std::endl;
          DestroyWindow(g_messageWindow);
          g_messageWindow = nullptr;
     }

     // --- Unregister Class ---
     if (g_windowClassRegistered) {
         std::cout << "Unregistering window class..." << std::endl;
         if (!UnregisterClass(HID_MIDI_INPUT_LISTENER_CLASS, hInstance)) {
              std::cerr << "Warning: Failed to unregister window class (Error: " << GetLastError() << ")." << std::endl;
         }
         g_windowClassRegistered = false;
     }
}

// Perform Axis Calibration - **REVISED: Added 5-second countdown before capture**
bool PerformCalibration() {
    if (g_currentConfig.control.isButton) return true; // No calibration needed for buttons

    // Check if the necessary global window is ready
    if (!g_messageWindow) {
        std::cerr << "Error: Cannot perform calibration - Input window not set up." << std::endl;
        return false;
    }
     if (g_selectedDeviceHandle == nullptr || g_selectedDevicePreparsedData == nullptr) {
         std::cerr << "Error: Cannot calibrate without selected HID device and preparsed data." << std::endl;
         return false;
    }

    constexpr int PRE_CAPTURE_DELAY_SECONDS = 5;        // Delay before starting capture
    constexpr int CALIBRATION_HOLD_DURATION_MS = 10000; // Hold for 10 seconds (as requested previously)
    constexpr int CALIBRATION_UPDATE_INTERVAL_MS = 100; // Update display every 100ms

    MSG msg;
    bool success = true; // Assume success initially for each stage
    HANDLE hConsoleCalib = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbiCalib;
    COORD startPosMinDisplay, startPosMaxDisplay; // For printing the final captured value
    COORD countdownStartPos;                       // For overwriting countdown message

    // --- Helper lambda for the countdown ---
    auto performCountdown = [&](const std::string& stageName) -> bool {
        if (!GetConsoleScreenBufferInfo(hConsoleCalib, &csbiCalib)) {
            std::cerr << "Warning: Could not get console buffer info for countdown." << std::endl;
            countdownStartPos = {0, 0}; // Default position
        } else {
            countdownStartPos = csbiCalib.dwCursorPosition;
        }

        for (int i = PRE_CAPTURE_DELAY_SECONDS; i > 0; --i) {
            SetConsoleCursorPosition(hConsoleCalib, countdownStartPos);
            std::cout << "Starting " << stageName << " capture in " << i << " second(s)...      " << std::flush; // Padding spaces
            // Sleep, but check for quit flag periodically (e.g., every 100ms) during the second
            for(int ms = 0; ms < 1000; ms += 100) {
                if(g_quitFlag) break;
                 // Briefly check messages to allow WM_QUIT processing if needed
                 while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                     if (msg.message == WM_QUIT) {
                         g_quitFlag = true; break;
                     }
                     if (msg.hwnd == g_messageWindow || msg.hwnd == NULL) {
                         TranslateMessage(&msg);
                         DispatchMessage(&msg);
                     }
                 }
                 if(g_quitFlag) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
             if (g_quitFlag) break; // Exit countdown loop if quit detected
        }

        // Clear the countdown line after finishing or aborting
        SetConsoleCursorPosition(hConsoleCalib, countdownStartPos);
        std::cout << std::string(DISPLAY_WIDTH, ' ') << std::flush; // Clear the line
        SetConsoleCursorPosition(hConsoleCalib, countdownStartPos); // Reset cursor

        return !g_quitFlag; // Return true if countdown completed without quitting
    };


    // --- Helper lambda for the hold process ---
    // (This lambda remains largely the same)
    auto captureHoldValue = [&](bool captureMin) -> LONG {
        LONG extremeValue = captureMin ? std::numeric_limits<LONG>::max() : std::numeric_limits<LONG>::min();
        bool valueCaptured = false;
        LONG initialValue = g_currentValue; // Capture value before starting
        LONG lastReadValue = initialValue;

        g_valueChanged = false; // Reset flag before starting hold

        auto startTime = std::chrono::steady_clock::now();
        auto endTime = startTime + std::chrono::milliseconds(CALIBRATION_HOLD_DURATION_MS);
        auto lastUpdateTime = startTime;

        COORD displayLineStart = {0,0};
        if(GetConsoleScreenBufferInfo(hConsoleCalib, &csbiCalib)) {
             displayLineStart = csbiCalib.dwCursorPosition;
        }

        // Move cursor to start of line *before* printing the holding message
        SetConsoleCursorPosition(hConsoleCalib, displayLineStart);
        std::cout << "Capturing... (   s) Current: " << std::setw(6) << lastReadValue
                  << " " << (captureMin ? "Min: " : "Max: ") << "..."
                  << "              " << std::flush; // Extra spaces to overwrite previous text

        while (std::chrono::steady_clock::now() < endTime && !g_quitFlag) {
            // Process messages for our *single* input window
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                 if (msg.message == WM_QUIT) {
                     g_quitFlag = true; break;
                 }
                 // We are interested in messages for our window or thread messages
                 if (msg.hwnd == g_messageWindow || msg.hwnd == NULL) {
                     TranslateMessage(&msg);
                     DispatchMessage(&msg); // This calls WindowProc, which updates g_currentValue and g_valueChanged
                 }
             }
             if (g_quitFlag) break; // Check if WM_QUIT was processed

             // Check if WindowProc detected a change
             if (g_valueChanged) {
                 lastReadValue = g_currentValue; // Update display value
                 if (captureMin) {
                     extremeValue = std::min(extremeValue, g_currentValue);
                 } else {
                     extremeValue = std::max(extremeValue, g_currentValue);
                 }
                 valueCaptured = true;
                 g_valueChanged = false; // Reset the flag after processing
             }

             // Update console display periodically
             auto now = std::chrono::steady_clock::now();
             if (now - lastUpdateTime > std::chrono::milliseconds(CALIBRATION_UPDATE_INTERVAL_MS)) {
                // Get time remaining for display
                auto timeRemaining = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - now);
                int secondsLeft = static_cast<int>(timeRemaining.count() / 1000.0 + 0.5);

                // Move cursor back to start of the line to overwrite
                SetConsoleCursorPosition(hConsoleCalib, displayLineStart);
                std::cout << "Capturing... (" << std::setw(3) << std::max(0, secondsLeft) << "s) Current: " << std::setw(6) << lastReadValue
                          << " " << (captureMin ? "Min: " : "Max: ") << std::setw(6)
                          << (valueCaptured ? extremeValue : lastReadValue) // Show extreme seen if any change occurred
                          << "              " << std::flush; // Overwrite previous line fully
                lastUpdateTime = now;
             }

            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Small sleep to avoid busy-waiting
        } // End while holding

        // Clear the "Capturing..." line after holding finishes or is aborted
        SetConsoleCursorPosition(hConsoleCalib, displayLineStart);
        std::cout << std::string(DISPLAY_WIDTH, ' ') << std::flush;
        SetConsoleCursorPosition(hConsoleCalib, displayLineStart); // Reset cursor to start of cleared line

        if (g_quitFlag) return -1; // Indicate quit request

        // If no change was detected *at all* during the hold, return the value from *before* the hold began.
        if (!valueCaptured) {
             std::cerr << "\nWarning: No input change detected during hold period. Using value from before hold: " << initialValue << std::endl;
             // Manually advance cursor to next line if warning is printed
              if(GetConsoleScreenBufferInfo(hConsoleCalib, &csbiCalib)) {
                  SetConsoleCursorPosition(hConsoleCalib, {0, static_cast<SHORT>(csbiCalib.dwCursorPosition.Y + 1)});
              }
             return initialValue;
        }

        // Otherwise, return the most extreme value seen during the hold.
        return extremeValue;
    };


    // --- Capture Minimum ---
    std::cout << "\n--- Calibrating Minimum ---" << std::endl;
    std::cout << "1. Move the control [" << WStringToString(g_currentConfig.control.name).c_str() << "] fully to its desired MINIMUM position." << std::endl;
    std::cout << "   Get ready! " << std::endl; // Prompt to get ready

    // Still check for quit flags etc. that might have been set during prompts
    if (g_quitFlag) { success = false; }

    // Perform countdown *before* asking user to hold
    if (success) {
        success = performCountdown("MIN");
    }

    if (success) {
        std::cout << "\n   OK. Now HOLD the control steady at the MINIMUM position for "
                  << (CALIBRATION_HOLD_DURATION_MS / 1000) << " seconds." << std::endl;
        // Store cursor position *before* calling captureHoldValue (after countdown line is cleared and "HOLD" message is printed)
        if(GetConsoleScreenBufferInfo(hConsoleCalib, &csbiCalib)) startPosMinDisplay = csbiCalib.dwCursorPosition;

        LONG capturedMin = captureHoldValue(true); // Call helper for min

        if (capturedMin == -1 && g_quitFlag) {
            success = false; // Quit detected within captureHoldValue
        } else if (success) { // Only proceed if not quit and no prior error
            g_currentConfig.calibrationMinHid = capturedMin;
            // Set cursor position *after* captureHoldValue finishes (it clears its own line)
            SetConsoleCursorPosition(hConsoleCalib, startPosMinDisplay);
            std::cout << "   Minimum value captured: " << g_currentConfig.calibrationMinHid << "      " << std::endl;
        }
    }

    // --- Capture Maximum ---
    if (success) {
        std::cout << "\n--- Calibrating Maximum ---" << std::endl;
        std::cout << "2. Move the control [" << WStringToString(g_currentConfig.control.name).c_str() << "] fully to its desired MAXIMUM position." << std::endl;
        std::cout << "   Get ready! " << std::endl; // Prompt to get ready

        if (g_quitFlag) { success = false; } // Check again before countdown

        // Perform countdown
        if(success) {
             success = performCountdown("MAX");
        }

        if (success) {
            std::cout << "\n   OK. Now HOLD the control steady at the MAXIMUM position for "
                      << (CALIBRATION_HOLD_DURATION_MS / 1000) << " seconds." << std::endl;
             // Store cursor position *before* calling captureHoldValue
             if(GetConsoleScreenBufferInfo(hConsoleCalib, &csbiCalib)) startPosMaxDisplay = csbiCalib.dwCursorPosition;

            LONG capturedMax = captureHoldValue(false); // Call helper for max

            if (capturedMax == -1 && g_quitFlag) {
                success = false; // Quit detected within captureHoldValue
            } else if (success) { // Only proceed if not quit and no prior error
                g_currentConfig.calibrationMaxHid = capturedMax;
                 // Set cursor position *after* captureHoldValue finishes
                 SetConsoleCursorPosition(hConsoleCalib, startPosMaxDisplay);
                 std::cout << "   Maximum value captured: " << g_currentConfig.calibrationMaxHid << "      " << std::endl;
            }
        }
    }

    // --- Final result reporting ---
    if (g_quitFlag) {
        std::cerr << "\nCalibration aborted due to exit request." << std::endl;
        return false;
    } else if (!success) {
         std::cerr << "\nCalibration failed due to input error or issue during capture/countdown." << std::endl;
         return false;
    }

    // Perform sanity check and mark as done only if fully successful
    if (g_currentConfig.calibrationMinHid > g_currentConfig.calibrationMaxHid) {
        std::cout << "\nNote: Min value was greater than Max value. Swapping." << std::endl;
        std::swap(g_currentConfig.calibrationMinHid, g_currentConfig.calibrationMaxHid);
         std::cout << "  New Min: " << g_currentConfig.calibrationMinHid << ", New Max: " << g_currentConfig.calibrationMaxHid << std::endl;
    } else if (g_currentConfig.calibrationMinHid == g_currentConfig.calibrationMaxHid) {
        std::cerr << "\nWarning: Minimum and Maximum calibrated values are the same ("
                  << g_currentConfig.calibrationMinHid << ").\n         MIDI output might be fixed or have very limited range." << std::endl;
        // Still mark as done, user might intend this, or can recalibrate.
    }

    g_currentConfig.calibrationDone = true;
    std::cout << "\nCalibration complete." << std::endl;
    return true; // Return success
}

std::string WStringToString(const std::wstring& wstr) {
    // (Implementation unchanged)
     if (wstr.empty()) return std::string();
     int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
     if (size_needed <= 0) { return ""; }
     std::string strTo(size_needed, 0);
     WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
     return strTo;
}

std::wstring StringToWString(const std::string& str) {
    // (Implementation unchanged)
     if (str.empty()) return std::wstring();
     int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
      if (size_needed <= 0) { return L""; }
     std::wstring wstrTo(size_needed, 0);
     MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
     return wstrTo;
}

// --- JSON Serialization/Deserialization Definitions ---
// (Implementations unchanged)
void to_json(json& j, const HidControlInfo& ctrl) {
    j = json{
        {"isButton", ctrl.isButton},
        {"usagePage", ctrl.usagePage},
        {"usage", ctrl.usage},
        {"logicalMin", ctrl.logicalMin},
        {"logicalMax", ctrl.logicalMax},
        {"name", WStringToString(ctrl.name)}
    };
}
void from_json(const json& j, HidControlInfo& ctrl) {
    j.at("isButton").get_to(ctrl.isButton);
    j.at("usagePage").get_to(ctrl.usagePage);
    j.at("usage").get_to(ctrl.usage);
    j.at("logicalMin").get_to(ctrl.logicalMin);
    j.at("logicalMax").get_to(ctrl.logicalMax);
    ctrl.name = StringToWString(j.at("name").get<std::string>());
}

void to_json(json& j, const MidiMappingConfig& cfg) {
    j = json{
        {"hidDevicePath", WStringToString(cfg.hidDevicePath)},
        {"hidDeviceName", WStringToString(cfg.hidDeviceName)},
        {"control", cfg.control},
        {"midiDeviceName", cfg.midiDeviceName},
        {"midiMessageType", cfg.midiMessageType},
        {"midiChannel", cfg.midiChannel},
        {"midiNoteOrCCNumber", cfg.midiNoteOrCCNumber},
        {"midiValueNoteOnVelocity", cfg.midiValueNoteOnVelocity},
        {"midiValueCCOn", cfg.midiValueCCOn},
        {"midiValueCCOff", cfg.midiValueCCOff},
        {"calibrationMinHid", cfg.calibrationMinHid},
        {"calibrationMaxHid", cfg.calibrationMaxHid},
        {"calibrationDone", cfg.calibrationDone},
        {"reverseAxis", cfg.reverseAxis},
        {"midiSendIntervalMs", cfg.midiSendIntervalMs}
    };
}
void from_json(const json& j, MidiMappingConfig& cfg) {
    cfg.hidDevicePath = StringToWString(j.at("hidDevicePath").get<std::string>());
    cfg.hidDeviceName = StringToWString(j.at("hidDeviceName").get<std::string>());
    j.at("control").get_to(cfg.control);
    j.at("midiDeviceName").get_to(cfg.midiDeviceName);
    j.at("midiMessageType").get_to(cfg.midiMessageType);
    j.at("midiChannel").get_to(cfg.midiChannel);
    j.at("midiNoteOrCCNumber").get_to(cfg.midiNoteOrCCNumber);

    cfg.midiValueNoteOnVelocity = j.value("midiValueNoteOnVelocity", 64);
    cfg.midiValueCCOn = j.value("midiValueCCOn", 127);
    cfg.midiValueCCOff = j.value("midiValueCCOff", 0);
    cfg.calibrationMinHid = j.value("calibrationMinHid", 0);
    cfg.calibrationMaxHid = j.value("calibrationMaxHid", 0);
    cfg.calibrationDone = j.value("calibrationDone", false);
    cfg.reverseAxis = j.value("reverseAxis", false);
    cfg.midiSendIntervalMs = j.value("midiSendIntervalMs", DEFAULT_MIDI_SEND_INTERVAL_MS);
    if (cfg.midiSendIntervalMs <= 0) {
        std::cerr << "Warning: Loaded MIDI Send Interval (" << cfg.midiSendIntervalMs
                  << "ms) is invalid. Resetting to default (" << DEFAULT_MIDI_SEND_INTERVAL_MS
                  << "ms)." << std::endl;
        cfg.midiSendIntervalMs = DEFAULT_MIDI_SEND_INTERVAL_MS;
    }
}
// --- End JSON ---


bool SaveConfiguration(const MidiMappingConfig& config, const std::string& filename) {
    // (Implementation unchanged)
    std::ofstream ofs;
    try {
        json j = config;
        ofs.open(filename);
        if (!ofs.is_open()) {
             std::cerr << "Error: Could not open file for saving: " << filename << std::endl;
             return false;
        }
        ofs << std::setw(4) << j << std::endl;
        ofs.close();
        return !ofs.fail();
    } catch (const json::exception& e) {
        std::cerr << "JSON serialization error while saving config: " << e.what() << std::endl;
        if (ofs.is_open()) ofs.close();
        return false;
    } catch (const std::ios_base::failure& e) {
         std::cerr << "File I/O error while saving config to '" << filename << "': " << e.what() << std::endl;
         if (ofs.is_open()) ofs.close();
         return false;
    } catch (const std::exception& e) {
         std::cerr << "Unexpected error while saving config: " << e.what() << std::endl;
         if (ofs.is_open()) ofs.close();
         return false;
    }
}

bool LoadConfiguration(const std::string& filename, MidiMappingConfig& config) {
    // (Implementation unchanged)
     std::ifstream ifs;
     try {
        ifs.open(filename);
        if (!ifs.is_open()) {
             return false;
        }
        json j;
        ifs >> j;
        ifs.close();

        if (!j.is_object()) {
             std::cerr << "Error: Configuration file '" << filename << "' does not contain a valid JSON object." << std::endl;
             return false;
        }
        if (!j.contains("hidDevicePath") || !j.contains("control") || !j.contains("midiDeviceName")) {
             std::cerr << "Error: Configuration file '" << filename << "' is missing essential fields." << std::endl;
             return false;
        }
        config = j.get<MidiMappingConfig>();
        return true;
    } catch (const json::parse_error& e) {
        std::cerr << "JSON parsing error in file '" << filename << "':\n  " << e.what() << "\n  at byte " << e.byte << std::endl;
        if(ifs.is_open()) ifs.close();
        return false;
    } catch (const json::type_error& e) {
        std::cerr << "JSON type error in file '" << filename << "':\n  " << e.what() << std::endl;
         if(ifs.is_open()) ifs.close();
        return false;
    } catch (const json::exception& e) {
         std::cerr << "JSON data error in file '" << filename << "':\n  " << e.what() << std::endl;
         if(ifs.is_open()) ifs.close();
         return false;
    } catch (const std::ios_base::failure& e) {
        std::cerr << "File I/O error while loading config from '" << filename << "': " << e.what() << std::endl;
        if(ifs.is_open()) ifs.close();
        return false;
    } catch (const std::exception& e) {
        std::cerr << "Unexpected error loading config '" << filename << "': " << e.what() << std::endl;
         if(ifs.is_open()) ifs.close();
        return false;
    }
}

std::vector<fs::path> ListConfigurations(const std::string& directory) {
    std::vector<fs::path> configFiles;
    try {
        fs::path dirPath = fs::absolute(fs::u8path(directory));
        if (!fs::exists(dirPath)) {
             return configFiles;
        }
        if (!fs::is_directory(dirPath)) {
             return configFiles;
        }

        for (const auto& entry : fs::directory_iterator(dirPath)) {
             if (entry.is_regular_file()) {
                 std::string filename_str = entry.path().filename().string(); // Get the full filename

                 // Check if the filename *ends with* CONFIG_EXTENSION
                 if (filename_str.length() >= CONFIG_EXTENSION.length() &&
                     filename_str.substr(filename_str.length() - CONFIG_EXTENSION.length()) == CONFIG_EXTENSION)
                 {
                     configFiles.push_back(entry.path());
                 }
             }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error listing configs in '" << directory << "': " << e.what() << std::endl;
    } catch (const std::exception& e) {
         std::cerr << "Error listing configs: " << e.what() << std::endl;
    }
    std::sort(configFiles.begin(), configFiles.end());
    return configFiles;
}

void SendMidiMessage() {
    // (Implementation unchanged)
    if (!g_midiOut.isPortOpen() || g_currentConfig.midiMessageType == MidiMessageType::NONE) {
        return;
    }

    std::vector<unsigned char> message;
    message.reserve(3);

    try {
        const auto& cfg = g_currentConfig;

        if (cfg.control.isButton) {
            bool currentlyPressed = (g_currentValue != 0);
            bool wasPressed = (g_previousValue != 0);

            if (currentlyPressed != wasPressed) { // Send only on state change
                if (cfg.midiMessageType == MidiMessageType::NOTE_ON_OFF) {
                    if (currentlyPressed) {
                        message = { (unsigned char)(0x90 | (cfg.midiChannel & 0x0F)),
                                    (unsigned char)(cfg.midiNoteOrCCNumber & 0x7F),
                                    (unsigned char)(cfg.midiValueNoteOnVelocity & 0x7F) };
                    } else {
                         message = { (unsigned char)(0x80 | (cfg.midiChannel & 0x0F)),
                                     (unsigned char)(cfg.midiNoteOrCCNumber & 0x7F),
                                     (unsigned char)(0x00) }; // Note Off Velocity often 0
                    }
                    g_midiOut.sendMessage(&message);
                    g_lastSentMidiValue = -1; // Reset axis tracking
                }
                else if (cfg.midiMessageType == MidiMessageType::CC) {
                     unsigned char ccValue = (unsigned char)((currentlyPressed ? cfg.midiValueCCOn
                                                                              : cfg.midiValueCCOff) & 0x7F);
                     // Check if value actually changed (important if On/Off values are the same)
                     // For buttons mapped to CC, we still only send on *state change*, not continuously.
                     message = { (unsigned char)(0xB0 | (cfg.midiChannel & 0x0F)),
                                 (unsigned char)(cfg.midiNoteOrCCNumber & 0x7F),
                                 ccValue };
                     g_midiOut.sendMessage(&message);
                     g_lastSentMidiValue = ccValue; // Update last sent value for CC buttons too
                }
            }
        }
        else { // Axis
            // Check if message type is CC and calibration was done
            if (cfg.midiMessageType == MidiMessageType::CC && cfg.calibrationDone)
            {
                LONG hidRange = cfg.calibrationMaxHid - cfg.calibrationMinHid;
                double normalizedValue = 0.0;

                // Calculate normalized value (0.0 to 1.0) based on calibrated range
                if (hidRange > 0) {
                     // Clamp the current value to the calibrated range first
                     LONG clampedValue = std::max(cfg.calibrationMinHid, std::min(cfg.calibrationMaxHid, g_currentValue));
                     normalizedValue = static_cast<double>(clampedValue - cfg.calibrationMinHid) / static_cast<double>(hidRange);
                } else {
                     // Handle edge case where min == max (avoid division by zero)
                     normalizedValue = (g_currentValue >= cfg.calibrationMaxHid) ? 1.0 : 0.0;
                }

                // Apply reversal if configured
                if (cfg.reverseAxis) {
                    normalizedValue = 1.0 - normalizedValue;
                }

                // Convert normalized value to MIDI value (0-127)
                int midiValue = static_cast<int>(normalizedValue * 127.0 + 0.5); // Add 0.5 for rounding
                midiValue = std::max(0, std::min(127, midiValue)); // Clamp to valid MIDI range

                // Send MIDI message only if the calculated MIDI value has changed
                if (midiValue != g_lastSentMidiValue) {
                     message = { (unsigned char)(0xB0 | (cfg.midiChannel & 0x0F)),
                                 (unsigned char)(cfg.midiNoteOrCCNumber & 0x7F),
                                 (unsigned char)(midiValue) };
                     g_midiOut.sendMessage(&message);
                     g_lastSentMidiValue = midiValue; // Update the last sent MIDI value
                }
            } else if (cfg.midiMessageType == MidiMessageType::CC && !cfg.calibrationDone) {
                // Optional: Handle uncalibrated axis? Could map logical range, but might be confusing.
                // For now, do nothing if not calibrated.
            }
        }
    } catch (const RtMidiError& error) {
         // Limit error spamming
         static std::chrono::steady_clock::time_point lastErrorTime;
         auto now = std::chrono::steady_clock::now();
         if (now - lastErrorTime > std::chrono::seconds(5)) {
             std::cerr << "\nRtMidiError sending message: " << error.getMessage() << std::endl;
             lastErrorTime = now;
         }
    } catch (const std::exception& e) {
         std::cerr << "\nError sending MIDI message: " << e.what() << std::endl;
    }
}


// --- Main Application Logic ---
int main() {
    HINSTANCE hInstance = GetModuleHandle(NULL);
    SetConsoleTitle(L"JoystickMIDI");
    // SetConsoleOutputCP(CP_UTF8); // Optional

    HANDLE hHeap = GetProcessHeap();
    if (!hHeap) {
        std::cerr << "Fatal Error: Could not get process heap." << std::endl;
        return 1;
    }

    bool configLoaded = false;
    std::string loadedConfigFilename;
    // bool exitAfterSave = false; // REMOVED

    // --- Configuration Handling (Load or Create New) ---
    ClearScreen();
    std::cout << "--- HID to MIDI Mapper ---\n\n";

    auto configFiles = ListConfigurations(".");
    int configChoice = -1;

    if (!configFiles.empty()) {
        std::cout << "Found existing configurations:\n";
        for (size_t i = 0; i < configFiles.size(); ++i) {
            std::cout << "[" << i << "] " << configFiles[i].filename().u8string() << std::endl;
        }
        std::cout << "[" << configFiles.size() << "] Create New Configuration\n";
        std::cout << "\nEnter number to load or create new: ";
        configChoice = GetUserSelection(static_cast<int>(configFiles.size()), 0);
        if (g_quitFlag) return 1;

        if (configChoice >= 0 && configChoice < static_cast<int>(configFiles.size())) {
            loadedConfigFilename = configFiles[configChoice].u8string();
            std::cout << "\nLoading configuration: " << loadedConfigFilename << "..." << std::endl;
            if (LoadConfiguration(loadedConfigFilename, g_currentConfig)) {
                std::cout << "Configuration loaded successfully." << std::endl;
                configLoaded = true;
            } else {
                std::cerr << "Failed to load configuration '" << loadedConfigFilename << "'.\nPlease check the file or create a new one." << std::endl;
                g_currentConfig = MidiMappingConfig{}; // Reset config
                configLoaded = false;
                 std::cout << "\nPress Enter to continue and create a new configuration..." << std::endl;
                 ClearInputBuffer(); std::cin.get();
                 if(g_quitFlag || std::cin.fail()) return 1;
                 ClearScreen(); // Clear before starting new config
                 std::cout << "\nStarting new configuration setup..." << std::endl;
            }
        } else {
             ClearScreen(); // Clear before starting new config
             std::cout << "\nStarting new configuration setup..." << std::endl;
             g_currentConfig = MidiMappingConfig{};
             configLoaded = false;
        }
    } else {
        std::cout << "No existing configurations found. Starting new setup..." << std::endl;
        g_currentConfig = MidiMappingConfig{};
        configLoaded = false;
    }
    Sleep(configLoaded ? 1000 : 1500); // Pause briefly

    // --- Device Enumeration & Selection (HID & MIDI) ---
    std::vector<HidDeviceInfo> hidDevices;
    int selectedHidDeviceIndex = -1;
    int selectedMidiPortIndex = -1;

    // === Step 1 & 2 (Combined): Find/Select HID Device and Control ===
    if (!configLoaded) {
        ClearScreen();
        std::cout << "--- Step 1: Select HID Controller ---\n\n";
        std::cout << "Enumerating HID Game Controllers...\n" << std::endl;
        UINT numDevices = 0;
        if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0) {
             std::cerr << "Error getting number of raw input devices." << std::endl; system("pause"); return 1;
        }
        if (numDevices == 0) {
             std::cerr << "No raw input devices found." << std::endl; system("pause"); return 1;
        }

        auto deviceList = std::make_unique<RAWINPUTDEVICELIST[]>(numDevices);
        if (!deviceList) { std::cerr << "Memory allocation failed for device list." << std::endl; system("pause"); return 1; }

        if (GetRawInputDeviceList(deviceList.get(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
             std::cerr << "Error getting raw input device list." << std::endl; system("pause"); return 1;
        }

        // Enumerate and collect suitable HID devices
        for (UINT i = 0; i < numDevices; ++i) {
            if (deviceList[i].dwType != RIM_TYPEHID) continue;

            RID_DEVICE_INFO deviceInfo;
            deviceInfo.cbSize = sizeof(RID_DEVICE_INFO);
            UINT size = sizeof(RID_DEVICE_INFO);

            if (GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_DEVICEINFO, &deviceInfo, &size) != size || size == 0) {
                continue;
            }

            // Filter for Joysticks (Usage=4) and Gamepads (Usage=5) on the Generic Desktop Page (UsagePage=1)
            if (deviceInfo.hid.usUsagePage == 1 && (deviceInfo.hid.usUsage == 4 || deviceInfo.hid.usUsage == 5))
            {
                auto infoPtr = std::make_unique<HidDeviceInfo>();
                if (!infoPtr) { std::cerr << "Memory allocation failed for HidDeviceInfo." << std::endl; continue; }
                HidDeviceInfo& info = *infoPtr;

                info.handle = deviceList[i].hDevice;
                info.rawInfo = deviceInfo; // Store raw info

                // Get Device Path
                UINT pathSize = 0;
                GetRawInputDeviceInfo(info.handle, RIDI_DEVICENAME, NULL, &pathSize);
                if (pathSize > 1) {
                    info.path.resize(pathSize);
                    if (GetRawInputDeviceInfo(info.handle, RIDI_DEVICENAME, info.path.data(), &pathSize) == (UINT)-1) {
                         info.path = L"Unknown Path";
                    } else {
                         info.path.resize(wcsnlen(info.path.c_str(), pathSize)); // Trim null chars
                    }
                } else {
                     info.path = L"Unknown Path";
                }

                // Get Preparsed Data (Important!)
                UINT dataSize = 0;
                if (GetRawInputDeviceInfo(info.handle, RIDI_PREPARSEDDATA, NULL, &dataSize) == 0 && dataSize > 0) {
                    info.preparsedData = (PHIDP_PREPARSED_DATA)HeapAlloc(hHeap, HEAP_ZERO_MEMORY, dataSize);
                    if (!info.preparsedData) { std::cerr << "HeapAlloc failed for preparsed data." << std::endl; continue; }

                    if (GetRawInputDeviceInfo(info.handle, RIDI_PREPARSEDDATA, info.preparsedData, &dataSize) == dataSize) {
                        // Get Capabilities (Optional here, but good practice)
                        if (HidP_GetCaps(info.preparsedData, &info.caps) == HIDP_STATUS_SUCCESS) {
                            // Get detailed button/value caps (needed for control selection later)
                            if (info.caps.NumberInputButtonCaps > 0) {
                                info.buttonCaps.resize(info.caps.NumberInputButtonCaps);
                                USHORT capsLength = info.caps.NumberInputButtonCaps;
                                if (HidP_GetButtonCaps(HidP_Input, info.buttonCaps.data(), &capsLength, info.preparsedData) != HIDP_STATUS_SUCCESS) {
                                     info.buttonCaps.clear();
                                }
                            }
                            if (info.caps.NumberInputValueCaps > 0) {
                                info.valueCaps.resize(info.caps.NumberInputValueCaps);
                                USHORT capsLength = info.caps.NumberInputValueCaps;
                                if (HidP_GetValueCaps(HidP_Input, info.valueCaps.data(), &capsLength, info.preparsedData) != HIDP_STATUS_SUCCESS) {
                                     info.valueCaps.clear();
                                }
                            }
                             // Check if device has *any* input controls we can use
                             if (info.buttonCaps.empty() && info.valueCaps.empty()) {
                                  // No usable input controls found for this device
                                  // No need to HeapFree preparsedData here, unique_ptr destructor handles it
                                  continue; // Skip this device
                             }
                        } else {
                             // Failed to get caps, might be problematic
                             // No need to HeapFree preparsedData here, unique_ptr destructor handles it
                             continue; // Skip this device
                        }
                    } else {
                         // Failed to get preparsed data content
                         // No need to HeapFree preparsedData here, unique_ptr destructor handles it
                         continue; // Skip this device
                    }
                } else {
                     // Failed to get preparsed data size or size is 0
                     continue; // Skip this device
                }


                // Get Device Name (Best effort)
                HANDLE hHidFile = CreateFileW(info.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                if (hHidFile != INVALID_HANDLE_VALUE) {
                     info.name = GetHidDeviceName(hHidFile);
                     CloseHandle(hHidFile);
                } else {
                     info.name = L"Unknown HID (CreateFile failed: " + std::to_wstring(GetLastError()) + L")";
                }

                // If all checks passed, move the valid device info into our list
                hidDevices.push_back(std::move(*infoPtr));
            }
        } // End for loop enumerating devices

        if (hidDevices.empty()) {
            std::cerr << "\nNo suitable HID Controllers (Joysticks/Gamepads) with usable inputs found." << std::endl;
            std::cerr << "Ensure your controller is connected and recognized by Windows." << std::endl;
            system("pause"); return 1;
        }

        // --- Select HID Device ---
        std::cout << "\nSelect Controller:\n";
        for (size_t i = 0; i < hidDevices.size(); ++i) {
            std::wcout << L"[" << i << L"] " << hidDevices[i].name << std::endl;
        }
        std::cout << "\nEnter number: ";
        selectedHidDeviceIndex = GetUserSelection(static_cast<int>(hidDevices.size()) - 1, 0);
        if (g_quitFlag || selectedHidDeviceIndex < 0) {
             // Cleanup handled by destructors if we exit here
             return 1;
        }

        // Move selected device info into global state, removing it from the vector
        // Note: This transfers ownership of the preparsedData pointer as well.
        { // Scope to ensure selectedDevice goes out of scope after move
            HidDeviceInfo selectedDevice = std::move(hidDevices[selectedHidDeviceIndex]);
            g_currentConfig.hidDevicePath = selectedDevice.path;
            g_currentConfig.hidDeviceName = selectedDevice.name;
            g_selectedDeviceHandle = selectedDevice.handle; // Keep the handle
            selectedDevice.handle = nullptr; // Prevent selectedDevice destructor from closing if needed
            g_selectedDevicePreparsedData = selectedDevice.preparsedData; // Transfer ownership of data pointer
            selectedDevice.preparsedData = nullptr; // Prevent selectedDevice destructor from freeing
        }
        hidDevices.clear(); // Clear the rest (destructors handle their resources)

        if (!g_selectedDevicePreparsedData || g_selectedDeviceHandle == nullptr) {
             std::cerr << "Error: Failed to properly acquire selected HID device resources." << std::endl;
             if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; }
             system("pause"); return 1;
        }

        // --- Select HID Control ---
        ClearScreen();
        std::cout << "--- Step 2: Select Control to Map ---\n\n";
        std::wcout << L"Device: " << g_currentConfig.hidDeviceName << std::endl << std::endl;
        std::cout << "Available Controls:\n";

        std::vector<HidControlInfo> availableControls;
        int controlIndex = 0;
        HIDP_CAPS caps;
        std::vector<HIDP_BUTTON_CAPS> buttonCapsVec;
        std::vector<HIDP_VALUE_CAPS> valueCapsVec;

        // Get caps again from the global preparsed data
        if (HidP_GetCaps(g_selectedDevicePreparsedData, &caps) != HIDP_STATUS_SUCCESS) {
             std::cerr << "Error: Cannot get capabilities from selected device preparsed data." << std::endl;
             if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; }
             system("pause"); return 1;
        }

        // Get detailed Button caps
        if (caps.NumberInputButtonCaps > 0) {
            buttonCapsVec.resize(caps.NumberInputButtonCaps);
            USHORT capsLength = caps.NumberInputButtonCaps;
            if(HidP_GetButtonCaps(HidP_Input, buttonCapsVec.data(), &capsLength, g_selectedDevicePreparsedData) != HIDP_STATUS_SUCCESS){
                buttonCapsVec.clear();
                std::cerr << "Warning: Failed to get detailed button capabilities." << std::endl;
            }
        }
        // Get detailed Value caps
         if (caps.NumberInputValueCaps > 0) {
            valueCapsVec.resize(caps.NumberInputValueCaps);
            USHORT capsLength = caps.NumberInputValueCaps;
             if(HidP_GetValueCaps(HidP_Input, valueCapsVec.data(), &capsLength, g_selectedDevicePreparsedData) != HIDP_STATUS_SUCCESS){
                 valueCapsVec.clear();
                 std::cerr << "Warning: Failed to get detailed value capabilities." << std::endl;
             }
        }

        // List Buttons
        for (const auto& bCaps : buttonCapsVec) {
            if (bCaps.IsRange) {
                for (USAGE usage = bCaps.Range.UsageMin; usage <= bCaps.Range.UsageMax; ++usage) {
                    HidControlInfo ctrl; ctrl.isButton = true; ctrl.usagePage = bCaps.UsagePage; ctrl.usage = usage;
                    ctrl.logicalMin = 0; ctrl.logicalMax = 1; // Buttons are typically 0 or 1
                    ctrl.name = GetUsageName(ctrl.usagePage, ctrl.usage);
                    // Add index for multiple buttons sharing the base name
                    if (usage != bCaps.Range.UsageMin && ctrl.name == GetUsageName(ctrl.usagePage, bCaps.Range.UsageMin)) {
                         ctrl.name += L" (" + std::to_wstring(usage - bCaps.Range.UsageMin + 1) + L")";
                    }
                    std::wcout << L"[" << std::setw(3) << controlIndex++ << L"] " << ctrl.name << L" (Button)" << std::endl;
                    availableControls.push_back(ctrl);
                }
            } else {
                 HidControlInfo ctrl; ctrl.isButton = true; ctrl.usagePage = bCaps.UsagePage; ctrl.usage = bCaps.NotRange.Usage;
                 ctrl.logicalMin = 0; ctrl.logicalMax = 1;
                 ctrl.name = GetUsageName(ctrl.usagePage, ctrl.usage);
                 std::wcout << L"[" << std::setw(3) << controlIndex++ << L"] " << ctrl.name << L" (Button)" << std::endl;
                 availableControls.push_back(ctrl);
            }
        }
        // List Values/Axes
        for (const auto& vCaps : valueCapsVec) {
             if (vCaps.BitSize <= 1) continue; // Skip single-bit values (likely buttons reported as values)
             if (vCaps.IsRange) {
                 for (USAGE usage = vCaps.Range.UsageMin; usage <= vCaps.Range.UsageMax; ++usage) {
                    HidControlInfo ctrl; ctrl.isButton = false; ctrl.usagePage = vCaps.UsagePage; ctrl.usage = usage;
                    ctrl.logicalMin = vCaps.LogicalMin; ctrl.logicalMax = vCaps.LogicalMax;
                    ctrl.name = GetUsageName(ctrl.usagePage, ctrl.usage);
                     if (usage != vCaps.Range.UsageMin && ctrl.name == GetUsageName(ctrl.usagePage, vCaps.Range.UsageMin)) {
                         ctrl.name += L" (" + std::to_wstring(usage - vCaps.Range.UsageMin + 1) + L")";
                    }
                    std::wcout << L"[" << std::setw(3) << controlIndex++ << L"] " << ctrl.name << L" (Axis/Value: " << ctrl.logicalMin << "-" << ctrl.logicalMax << ")" << std::endl;
                    availableControls.push_back(ctrl);
                 }
            } else {
                 HidControlInfo ctrl; ctrl.isButton = false; ctrl.usagePage = vCaps.UsagePage; ctrl.usage = vCaps.NotRange.Usage;
                 ctrl.logicalMin = vCaps.LogicalMin; ctrl.logicalMax = vCaps.LogicalMax;
                 ctrl.name = GetUsageName(ctrl.usagePage, ctrl.usage);
                 std::wcout << L"[" << std::setw(3) << controlIndex++ << L"] " << ctrl.name << L" (Axis/Value: " << ctrl.logicalMin << "-" << ctrl.logicalMax << ")" << std::endl;
                 availableControls.push_back(ctrl);
            }
        }

        if (availableControls.empty()) {
            std::cerr << "\nNo usable Button or Axis controls found for this device." << std::endl;
            if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr;}
            system("pause"); return 1;
        }

        std::cout << "\nEnter number of the control to map: ";
        int controlChoice = GetUserSelection(static_cast<int>(availableControls.size()) - 1, 0);
        if (g_quitFlag || controlChoice < 0) {
             if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr;}
             return 1;
        }
        g_currentConfig.control = availableControls[controlChoice];

    } else { // Config loaded, find the device and get preparsed data
        ClearScreen();
        std::cout << "--- Loading Configuration ---" << std::endl;
        std::cout << "Finding configured HID device: ";
        std::wcout << g_currentConfig.hidDeviceName << L" (" << g_currentConfig.hidDevicePath << L")" << std::endl;
        UINT numDevices = 0;
        GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST));
        if (numDevices > 0) {
             auto deviceList = std::make_unique<RAWINPUTDEVICELIST[]>(numDevices);
             if (deviceList && GetRawInputDeviceList(deviceList.get(), &numDevices, sizeof(RAWINPUTDEVICELIST)) != (UINT)-1) {
                 for (UINT i = 0; i < numDevices; ++i) {
                     if (deviceList[i].dwType != RIM_TYPEHID) continue;
                     UINT pathSize = 0;
                     GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_DEVICENAME, NULL, &pathSize);
                     if (pathSize > 1) {
                         std::wstring currentPath;
                         currentPath.resize(pathSize);
                         if (GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_DEVICENAME, currentPath.data(), &pathSize) != (UINT)-1) {
                             currentPath.resize(wcsnlen(currentPath.c_str(), pathSize));
                             if (currentPath == g_currentConfig.hidDevicePath) {
                                 g_selectedDeviceHandle = deviceList[i].hDevice; // Found the handle
                                 std::cout << "HID Device found." << std::endl;
                                 break;
                             }
                         }
                     }
                 }
             }
        }
         if (g_selectedDeviceHandle == nullptr) {
            std::wcerr << L"Error: Configured HID device '" << g_currentConfig.hidDeviceName
                       << L"' with path '" << g_currentConfig.hidDevicePath << L"' not found.\nPlease check connection or create a new configuration." << std::endl;
             system("pause"); return 1;
         }

         // Device handle found, now get the preparsed data
         std::cout << "Retrieving HID capabilities..." << std::endl;
         UINT dataSizeSelected = 0;
         // Free any potentially stale preparsed data first
         if (g_selectedDevicePreparsedData) {
              HeapFree(hHeap, 0, g_selectedDevicePreparsedData);
              g_selectedDevicePreparsedData = nullptr;
         }
         // Get size
         if (GetRawInputDeviceInfo(g_selectedDeviceHandle, RIDI_PREPARSEDDATA, NULL, &dataSizeSelected) == 0 && dataSizeSelected > 0) {
            // Allocate
            g_selectedDevicePreparsedData = (PHIDP_PREPARSED_DATA)HeapAlloc(hHeap, HEAP_ZERO_MEMORY, dataSizeSelected);
            if (!g_selectedDevicePreparsedData) {
                std::cerr << "Error: Failed to allocate memory for preparsed data." << std::endl;
                system("pause"); return 1;
            }
            // Get data
            if (GetRawInputDeviceInfo(g_selectedDeviceHandle, RIDI_PREPARSEDDATA, g_selectedDevicePreparsedData, &dataSizeSelected) != dataSizeSelected) {
                 HeapFree(hHeap, 0, g_selectedDevicePreparsedData);
                 g_selectedDevicePreparsedData = nullptr;
                 std::cerr << "Error: Failed to get preparsed data for configured HID device." << std::endl;
                 system("pause"); return 1;
            }
            std::cout << "HID capabilities retrieved." << std::endl;
         } else {
            std::cerr << "Error: Failed to get preparsed data size for configured HID device (Size=" << dataSizeSelected << "). Error: " << GetLastError() << std::endl;
            system("pause"); return 1;
         }
          Sleep(1000);
    } // End HID Device/Control Selection

    // === Setup the Input Window and Raw Input Registration ===
    // Do this *after* g_selectedDeviceHandle is confirmed valid and g_selectedDevicePreparsedData is acquired
    ClearScreen();
    std::cout << "Initializing input listener..." << std::endl;
    if (!SetupInputWindowAndRegistration(hInstance)) {
        std::cerr << "Failed to initialize input system. Exiting." << std::endl;
        // Cleanup might have already been attempted in the function
        if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; }
        system("pause");
        return 1;
    }
    Sleep(500);


    // === Step 3: Select MIDI Output Device ===
    if (!configLoaded) {
         ClearScreen();
         std::cout << "--- Step 3: Select MIDI Output ---\n\n";
         selectedMidiPortIndex = SelectMidiOutputPort(g_midiOut);
         if (g_quitFlag || selectedMidiPortIndex < 0) {
             std::cerr << "No MIDI output port selected. Exiting." << std::endl;
             CleanupInputWindowAndRegistration(hInstance);
             if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr;}
             return 1;
         }
         try {
              g_currentConfig.midiDeviceName = g_midiOut.getPortName(selectedMidiPortIndex);
         } catch (const RtMidiError& err) {
             std::cerr << "Warning: Could not retrieve name for selected MIDI port " << selectedMidiPortIndex << ": " << err.getMessage() << std::endl;
             g_currentConfig.midiDeviceName = "MIDI Output " + std::to_string(selectedMidiPortIndex);
         }
    } else { // Config loaded, find the MIDI port
        ClearScreen(); // Clear screen after input init message
        std::cout << "--- Loading Configuration ---" << std::endl;
        std::cout << "Finding configured MIDI Output port: " << g_currentConfig.midiDeviceName << std::endl;
        unsigned int portCount = 0;
        try { portCount = g_midiOut.getPortCount(); } catch(...) {}
        selectedMidiPortIndex = -1;
        for (unsigned int i = 0; i < portCount; ++i) {
            try {
                if (g_midiOut.getPortName(i) == g_currentConfig.midiDeviceName) {
                    selectedMidiPortIndex = i;
                    break;
                }
            } catch (const RtMidiError &error) {
                 std::cerr << "Warning: RtMidiError checking port " << i << " name: " << error.getMessage() << std::endl;
            }
        }

         if (selectedMidiPortIndex < 0) {
             std::cerr << "Warning: Configured MIDI Output port '" << g_currentConfig.midiDeviceName
                       << "' not found. Please select an available port." << std::endl;
             selectedMidiPortIndex = SelectMidiOutputPort(g_midiOut);
             if (g_quitFlag || selectedMidiPortIndex < 0) {
                  std::cerr << "No MIDI output port selected during fallback. Exiting." << std::endl;
                  CleanupInputWindowAndRegistration(hInstance);
                  if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; }
                  return 1;
             }
             // Update config with the newly selected port name
             try {
                  g_currentConfig.midiDeviceName = g_midiOut.getPortName(selectedMidiPortIndex);
                  std::cout << "Using newly selected MIDI port: " << g_currentConfig.midiDeviceName << std::endl;
             } catch(...){
                 std::cerr << "Warning: Failed to get name for newly selected MIDI port." << std::endl;
                 g_currentConfig.midiDeviceName = "MIDI Output " + std::to_string(selectedMidiPortIndex);
             }
         } else {
             std::cout << "MIDI Port found." << std::endl;
         }
          Sleep(1000);
    }

     // Open the selected/found MIDI port
    try {
        g_midiOut.openPort(selectedMidiPortIndex);
        std::cout << "\nOpened MIDI Port: " << g_currentConfig.midiDeviceName << std::endl;
    } catch (const RtMidiError& error) {
        std::cerr << "Error opening MIDI port: " << error.getMessage() << std::endl;
        CleanupInputWindowAndRegistration(hInstance);
        if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; }
        system("pause");
        return 1;
    }
    Sleep(500);


    // === Step 4 & 5: Configure MIDI Mapping Details & Calibrate ===
    if (!configLoaded) {
        ClearScreen();
        std::cout << "--- Step 4: Configure MIDI Mapping ---\n\n";
        std::wcout << L"Mapping Control: " << g_currentConfig.control.name << std::endl;

        if (g_currentConfig.control.isButton) {
            std::cout << "Select MIDI message type for Button:\n";
            std::cout << "[0] Note On/Off\n";
            std::cout << "[1] CC (Control Change)\n";
            int typeChoice = GetUserSelection(1, 0);
             if (g_quitFlag || typeChoice < 0) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }

            if (typeChoice == 0) { // Note On/Off
                g_currentConfig.midiMessageType = MidiMessageType::NOTE_ON_OFF;
                std::cout << "Enter MIDI Channel (1-16): ";
                g_currentConfig.midiChannel = GetUserSelection(16, 1) - 1;
                 if (g_quitFlag || g_currentConfig.midiChannel < -1) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
                std::cout << "Enter MIDI Note Number (0-127): ";
                g_currentConfig.midiNoteOrCCNumber = GetUserSelection(127, 0);
                 if (g_quitFlag || g_currentConfig.midiNoteOrCCNumber < 0) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
                std::cout << "Enter Note On Velocity (1-127): ";
                g_currentConfig.midiValueNoteOnVelocity = GetUserSelection(127, 1);
                if (g_quitFlag || g_currentConfig.midiValueNoteOnVelocity < 1) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
            } else { // CC
                g_currentConfig.midiMessageType = MidiMessageType::CC;
                std::cout << "Enter MIDI Channel (1-16): ";
                g_currentConfig.midiChannel = GetUserSelection(16, 1) - 1;
                if (g_quitFlag || g_currentConfig.midiChannel < -1) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
                std::cout << "Enter MIDI CC Number (0-127): ";
                g_currentConfig.midiNoteOrCCNumber = GetUserSelection(127, 0);
                 if (g_quitFlag || g_currentConfig.midiNoteOrCCNumber < 0) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
                std::cout << "Enter CC Value when Button Pressed (0-127): ";
                g_currentConfig.midiValueCCOn = GetUserSelection(127, 0);
                 if (g_quitFlag || g_currentConfig.midiValueCCOn < 0) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
                std::cout << "Enter CC Value when Button Released (0-127): ";
                g_currentConfig.midiValueCCOff = GetUserSelection(127, 0);
                 if (g_quitFlag || g_currentConfig.midiValueCCOff < 0) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
            }
        } else { // Axis/Value
            std::cout << "Mapping Axis/Value to MIDI CC.\n";
            g_currentConfig.midiMessageType = MidiMessageType::CC;
            std::cout << "Enter MIDI Channel (1-16): ";
            g_currentConfig.midiChannel = GetUserSelection(16, 1) - 1;
             if (g_quitFlag || g_currentConfig.midiChannel < -1) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
            std::cout << "Enter MIDI CC Number (0-127): ";
            g_currentConfig.midiNoteOrCCNumber = GetUserSelection(127, 0);
             if (g_quitFlag || g_currentConfig.midiNoteOrCCNumber < 0) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }

            std::cout << "Reverse MIDI output? (0=No: Min->" << (g_currentConfig.reverseAxis ? 127:0) << ", Max->" << (g_currentConfig.reverseAxis ? 0:127) << " / 1=Yes): ";
             int reverseChoice = GetUserSelection(1, 0);
             if (g_quitFlag || reverseChoice < 0) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
             g_currentConfig.reverseAxis = (reverseChoice == 1);

            ClearScreen();
            std::cout << "--- Step 5: Calibrate Axis ---\n\n";
             std::wcout << L"Calibrating: " << g_currentConfig.control.name << std::endl;
             std::cout << "Axis Hardware Logical Range: " << g_currentConfig.control.logicalMin
                       << " to " << g_currentConfig.control.logicalMax << std::endl;

            // Use the revised PerformCalibration (no temp window)
            if (!PerformCalibration()) { // Modifies g_currentConfig calibration values
                 std::cerr << "Calibration failed or was aborted. Exiting." << std::endl;
                 if (g_midiOut.isPortOpen()) g_midiOut.closePort();
                 CleanupInputWindowAndRegistration(hInstance);
                 if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; }
                 system("pause"); return 1;
            }
            // Calibration successful if we reach here
             std::cout << "\nCalibration parameters set:" << std::endl;
             std::cout << "  Min HID Value Captured: " << g_currentConfig.calibrationMinHid << std::endl;
             std::cout << "  Max HID Value Captured: " << g_currentConfig.calibrationMaxHid << std::endl;
             std::cout << "Press Enter to continue to save configuration...";
             ClearInputBuffer();
             std::cin.get();
             if (g_quitFlag || std::cin.fail()) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
        }
    } // End MIDI Config/Calibration (!configLoaded)


    // === Step 6: Save Configuration (if newly created) ===
     if (!configLoaded) {
         ClearScreen();
          std::cout << "--- Step 6: Save Configuration ---\n\n";
          std::string saveFilename;
          std::cout << "Enter filename to save configuration (e.g., my_joystick_mapping" << CONFIG_EXTENSION << "): ";

          while (true) {
               std::cout << "> ";
               if (!std::getline(std::cin, saveFilename)) {
                    std::cerr << "\nInput error or EOF. Cannot read filename." << std::endl;
                    g_quitFlag = true;
                    saveFilename = ""; break;
               }
               // Trim whitespace
               saveFilename.erase(0, saveFilename.find_first_not_of(" \t\n\r\f\v"));
               saveFilename.erase(saveFilename.find_last_not_of(" \t\n\r\f\v") + 1);
               if (!saveFilename.empty()) { break; }
               else { std::cout << "Filename cannot be empty. Please enter a name." << std::endl; }
          }
           if(g_quitFlag) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1;}

          if (!saveFilename.empty()) {
              // Ensure correct extension
              if (saveFilename.length() < CONFIG_EXTENSION.length() ||
                  saveFilename.substr(saveFilename.length() - CONFIG_EXTENSION.length()) != CONFIG_EXTENSION)
              {
                   saveFilename += CONFIG_EXTENSION;
              }

              if (SaveConfiguration(g_currentConfig, saveFilename)) {
                   std::cout << "Configuration saved successfully to " << saveFilename << std::endl;
                   // ** REMOVED: exitAfterSave = true; **
                   std::cout << "\nProceeding directly to monitoring..." << std::endl;
                   Sleep(1500); // Brief pause
              } else {
                   std::cerr << "Warning: Failed to save configuration to " << saveFilename << ". Continuing without saving." << std::endl;
                   std::cout << "\nPress Enter to start monitoring (unsaved configuration)..." << std::endl;
                   ClearInputBuffer(); std::cin.get();
                   if (g_quitFlag || std::cin.fail()) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
              }
          } else {
               std::cout << "Skipping save due to empty filename or input error." << std::endl;
               std::cout << "\nPress Enter to start monitoring (unsaved configuration)..." << std::endl;
               ClearInputBuffer(); std::cin.get();
                if (g_quitFlag || std::cin.fail()) { if (g_midiOut.isPortOpen()) g_midiOut.closePort(); CleanupInputWindowAndRegistration(hInstance); if (g_selectedDevicePreparsedData) { HeapFree(hHeap, 0, g_selectedDevicePreparsedData); g_selectedDevicePreparsedData = nullptr; } return 1; }
          }

          // ** REMOVED: The entire if (exitAfterSave) block **
          // The flow now naturally continues to the monitoring loop below

     } // End if (!configLoaded)


    // --- Monitoring Loop ---
    ClearScreen();
    std::cout << "--- Monitoring Active ---" << std::endl;
    std::wcout << L"Device: " << g_currentConfig.hidDeviceName << std::endl;
    std::wcout << L"Control: " << g_currentConfig.control.name << std::endl;
    std::cout << "MIDI Port: " << g_currentConfig.midiDeviceName << std::endl;
    std::cout << "Mapping: ";
    if (g_currentConfig.midiMessageType == MidiMessageType::NOTE_ON_OFF) {
         std::cout << "Note On/Off (Ch: " << g_currentConfig.midiChannel + 1 << ", Note: " << g_currentConfig.midiNoteOrCCNumber << ", Vel: " << g_currentConfig.midiValueNoteOnVelocity << ")";
    } else if (g_currentConfig.midiMessageType == MidiMessageType::CC) {
         if (g_currentConfig.control.isButton) {
             std::cout << "CC Button (Ch: " << g_currentConfig.midiChannel + 1 << ", CC: " << g_currentConfig.midiNoteOrCCNumber << ", OnVal: " << g_currentConfig.midiValueCCOn << ", OffVal: " << g_currentConfig.midiValueCCOff << ")";
         } else { // Axis
              if (g_currentConfig.calibrationDone) {
                    std::cout << "CC Axis (Ch: " << g_currentConfig.midiChannel + 1 << ", CC: " << g_currentConfig.midiNoteOrCCNumber << ", Range: " << g_currentConfig.calibrationMinHid << "-" << g_currentConfig.calibrationMaxHid << " -> "
                              << (g_currentConfig.reverseAxis ? "127-0" : "0-127")
                              << (g_currentConfig.reverseAxis ? " [Reversed]" : "");
              } else {
                   std::cout << "CC Axis (Ch: " << g_currentConfig.midiChannel + 1 << ", CC: " << g_currentConfig.midiNoteOrCCNumber << ", Range: UNCALIBRATED! Using Logical Range "
                             << g_currentConfig.control.logicalMin << "-" << g_currentConfig.control.logicalMax
                             << (g_currentConfig.reverseAxis ? " [Reversed])" : ")");
              }
         }
    } else { std::cout << "None"; }
    std::cout << std::endl;
    std::cout << "MIDI Send Interval: " << g_currentConfig.midiSendIntervalMs << "ms" << std::endl << std::endl;
    std::cout << "(Press Ctrl+C or close console window to exit)\n" << std::endl;


    HANDLE hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    COORD monitoringStartPos = {0, 0};
    if (GetConsoleScreenBufferInfo(hConsoleOutput, &csbi)) {
        monitoringStartPos = csbi.dwCursorPosition; // Get position *after* printing headers
    } else { std::cerr << "Warning: Could not get console buffer info. Display might flicker." << std::endl; }

    ConsoleCursorHider cursorHider(hConsoleOutput); // Hide cursor for clean display
    auto lastDisplayUpdateTime = std::chrono::steady_clock::now();
    MSG msg = {};
    g_previousValue = g_currentValue; // Initialize previous value
    g_lastSentMidiValue = -1;         // Ensure first MIDI value is sent
    g_valueChanged = true;           // Force initial update/send
    const std::chrono::milliseconds midiSendInterval(g_currentConfig.midiSendIntervalMs > 0 ? g_currentConfig.midiSendIntervalMs : 1);
    g_lastMidiSendTime = std::chrono::steady_clock::now() - midiSendInterval; // Allow immediate first send

    while (!g_quitFlag) {
        // Process Windows messages (including WM_INPUT and WM_QUIT)
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                 g_quitFlag = true; break;
            }
            // Only dispatch messages intended for our window or thread messages
            if (msg.hwnd == g_messageWindow || msg.hwnd == NULL) {
                 TranslateMessage(&msg);
                 DispatchMessage(&msg); // Calls WindowProc if it's for g_messageWindow
            }
        }
        if (g_quitFlag) break; // Exit loop if WM_QUIT was processed

        auto now = std::chrono::steady_clock::now();
        auto displayElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastDisplayUpdateTime);
        auto midiElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastMidiSendTime);

        bool displayUpdateNeeded = displayElapsed >= FRAME_DURATION;
        // Send MIDI if the value changed AND the minimum interval has passed
        bool canSendMidi = g_valueChanged && midiElapsed >= midiSendInterval;

        if (displayUpdateNeeded) {
            DisplayMonitoringOutput(monitoringStartPos, hConsoleOutput);
            lastDisplayUpdateTime = now;
        }

        if (canSendMidi) {
            SendMidiMessage(); // Function now handles change detection internally for axes
            g_lastMidiSendTime = now;
            g_previousValue = g_currentValue; // Update previous value *after* sending
            g_valueChanged = false;           // Reset the flag *after* processing the change
        }

        // Prevent busy-waiting, yield CPU slice
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } // End while (!g_quitFlag)

    // --- Cleanup ---
    std::cout << "\n\nExiting monitoring loop..." << std::endl;

    // Cleanup Window and Raw Input Registration
    CleanupInputWindowAndRegistration(hInstance);

    // Cleanup MIDI
    if (g_midiOut.isPortOpen()) {
         std::cout << "Sending MIDI All Notes Off / Reset Controllers..." << std::endl;
         std::vector<unsigned char> message(3);
         // Send to all channels just in case
         for(int ch = 0; ch < 16; ++ch) {
             message = { (unsigned char)(0xB0 | ch), 120, 0 }; // All Sound Off
             try { g_midiOut.sendMessage(&message); } catch(...) {}
             std::this_thread::sleep_for(std::chrono::microseconds(500));
             message = { (unsigned char)(0xB0 | ch), 121, 0 }; // Reset All Controllers
             try { g_midiOut.sendMessage(&message); } catch(...) {}
             std::this_thread::sleep_for(std::chrono::microseconds(500));
             message = { (unsigned char)(0xB0 | ch), 123, 0 }; // All Notes Off
             try { g_midiOut.sendMessage(&message); } catch(...) {}
             std::this_thread::sleep_for(std::chrono::microseconds(500));
         }
        g_midiOut.closePort();
        std::cout << "MIDI Port closed." << std::endl;
    }

    // Cleanup HID Resources
    std::cout << "Releasing HID resources..." << std::endl;
    if (g_selectedDevicePreparsedData) {
         HeapFree(hHeap, 0, g_selectedDevicePreparsedData);
         g_selectedDevicePreparsedData = nullptr;
         std::cout << "HID resources released." << std::endl;
    }
    // Note: g_selectedDeviceHandle doesn't need explicit closing here,
    // as it's a raw input handle managed by the system.

    std::cout << "\nMonitoring stopped." << std::endl;

    std::cout << "Press Enter to exit." << std::endl;
    ClearInputBuffer(); // Ensure buffer is clear before final wait
    std::cin.get();

    return 0;
}