// --- Common C++ Headers ---
#include <iostream>
#include <vector>
#include <string>
#include <limits>
#include <iomanip>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <mutex>

// --- Platform-Specific Includes ---
#ifdef _WIN32
    #define UNICODE
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #define _WIN32_WINNT 0x0601
    #include <windows.h>
    #include <hidsdi.h>
    #include <hidpi.h>
    #include <setupapi.h>
    #include <wtypes.h>
#else // Linux
    #include <libudev.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <linux/input.h>
    #include <sys/ioctl.h>
    #include <string.h>
    #include <errno.h>
    #include <poll.h>
    #include <cstdint>
    // Define LONG for Linux to match the Windows type used in shared code
    typedef int32_t LONG;
    #define BITS_PER_LONG (sizeof(long) * 8)
#endif

// --- Project-Specific Headers ---
#include "rtmidi/RtMidi.h"
#include "third_party/nlohmann/json.hpp"

// --- Namespaces and Constants ---
using json = nlohmann::json;
namespace fs = std::filesystem;
const std::string CONFIG_EXTENSION = ".hidmidi.json";

// --- Data Structures ---
struct ControlInfo {
    bool isButton = false;
    LONG logicalMin = 0;
    LONG logicalMax = 0;
    std::string name = "Unknown Control";

#ifdef _WIN32
    USAGE usagePage = 0;
    USAGE usage = 0;
#else // Linux
    uint16_t eventType = 0;
    uint16_t eventCode = 0;
#endif
};

struct MidiMappingConfig {
    std::string hidDevicePath;
    std::string hidDeviceName;
    ControlInfo control;
    std::string midiDeviceName;
    enum class MidiMessageType { NONE, NOTE_ON_OFF, CC } midiMessageType = MidiMessageType::NONE;
    int midiChannel = 0;
    int midiNoteOrCCNumber = 0;
    int midiValueNoteOnVelocity = 64;
    int midiValueCCOn = 127;
    int midiValueCCOff = 0;
    LONG calibrationMinHid = 0;
    LONG calibrationMaxHid = 0;
    bool calibrationDone = false;
    bool reverseAxis = false;
    int midiSendIntervalMs = 1;
};

// --- JSON Serialization ---
NLOHMANN_JSON_SERIALIZE_ENUM(MidiMappingConfig::MidiMessageType, {
    {MidiMappingConfig::MidiMessageType::NONE, nullptr},
    {MidiMappingConfig::MidiMessageType::NOTE_ON_OFF, "NoteOnOff"},
    {MidiMappingConfig::MidiMessageType::CC, "CC"}
})

void to_json(json& j, const ControlInfo& ctrl) {
    j = json{
        {"isButton", ctrl.isButton}, {"logicalMin", ctrl.logicalMin},
        {"logicalMax", ctrl.logicalMax}, {"name", ctrl.name}
    };
#ifdef _WIN32
    j["usagePage"] = ctrl.usagePage;
    j["usage"] = ctrl.usage;
#else
    j["eventType"] = ctrl.eventType;
    j["eventCode"] = ctrl.eventCode;
#endif
}

void from_json(const json& j, ControlInfo& ctrl) {
    j.at("isButton").get_to(ctrl.isButton);
    j.at("logicalMin").get_to(ctrl.logicalMin);
    j.at("logicalMax").get_to(ctrl.logicalMax);
    j.at("name").get_to(ctrl.name);
#ifdef _WIN32
    ctrl.usagePage = j.value("usagePage", 0);
    ctrl.usage = j.value("usage", 0);
#else
    ctrl.eventType = j.value("eventType", 0);
    ctrl.eventCode = j.value("eventCode", 0);
#endif
}

void to_json(json& j, const MidiMappingConfig& cfg) {
    j = json{
        {"hidDevicePath", cfg.hidDevicePath}, {"hidDeviceName", cfg.hidDeviceName},
        {"control", cfg.control}, {"midiDeviceName", cfg.midiDeviceName},
        {"midiMessageType", cfg.midiMessageType}, {"midiChannel", cfg.midiChannel},
        {"midiNoteOrCCNumber", cfg.midiNoteOrCCNumber}, {"midiValueNoteOnVelocity", cfg.midiValueNoteOnVelocity},
        {"midiValueCCOn", cfg.midiValueCCOn}, {"midiValueCCOff", cfg.midiValueCCOff},
        {"calibrationMinHid", cfg.calibrationMinHid}, {"calibrationMaxHid", cfg.calibrationMaxHid},
        {"calibrationDone", cfg.calibrationDone}, {"reverseAxis", cfg.reverseAxis},
        {"midiSendIntervalMs", cfg.midiSendIntervalMs}
    };
}

void from_json(const json& j, MidiMappingConfig& cfg) {
    j.at("hidDevicePath").get_to(cfg.hidDevicePath);
    j.at("hidDeviceName").get_to(cfg.hidDeviceName);
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
    cfg.midiSendIntervalMs = j.value("midiSendIntervalMs", 1);
}

// --- Global State ---
std::atomic<bool> g_quitFlag(false);
std::atomic<LONG> g_currentValue(0);
std::atomic<bool> g_valueChanged(false);
LONG g_previousValue = -1;
int g_lastSentMidiValue = -1;
RtMidiOut g_midiOut;
MidiMappingConfig g_currentConfig;
std::thread g_inputThread;
std::mutex g_consoleMutex;

// --- Forward Declarations ---
void ClearScreen();
int GetUserSelection(int maxValidChoice, int minValidChoice = 0);
void DisplayMonitoringOutput();
void ClearInputBuffer();
bool string_ends_with(const std::string& str, const std::string& suffix);
bool SaveConfiguration(const MidiMappingConfig& config, const std::string& filename);
bool LoadConfiguration(const std::string& filename, MidiMappingConfig& config);
std::vector<fs::path> ListConfigurations(const std::string& directory);
bool PerformCalibration();

// ===================================================================================
//
// PLATFORM-SPECIFIC IMPLEMENTATIONS
//
// ===================================================================================

#ifdef _WIN32

// --- Windows Data Structures ---
struct HidDeviceInfo {
    HANDLE handle = nullptr;
    std::string name = "Unknown Device";
    std::string path;
    PHIDP_PREPARSED_DATA preparsedData = nullptr;
    HIDP_CAPS caps = {};
    RID_DEVICE_INFO rawInfo = {};

    ~HidDeviceInfo() {
        if (preparsedData) HeapFree(GetProcessHeap(), 0, preparsedData);
    }
    HidDeviceInfo(const HidDeviceInfo&) = delete;
    HidDeviceInfo& operator=(const HidDeviceInfo&) = delete;
    HidDeviceInfo(HidDeviceInfo&& other) noexcept
        : handle(other.handle), name(std::move(other.name)), path(std::move(other.path)),
          preparsedData(other.preparsedData), caps(other.caps), rawInfo(other.rawInfo) {
        other.preparsedData = nullptr;
        other.handle = nullptr;
    }
    HidDeviceInfo() = default;
};

// --- Windows Helper Functions ---
std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

std::wstring StringToWString(const std::string& str) {
    if (str.empty()) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
    return wstrTo;
}

// --- Windows Device/Control Logic ---
std::vector<HidDeviceInfo> EnumerateHidDevices() {
    std::vector<HidDeviceInfo> hidDevices;
    UINT numDevices = 0;
    GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST));
    if (numDevices == 0) return hidDevices;

    auto deviceList = std::make_unique<RAWINPUTDEVICELIST[]>(numDevices);
    GetRawInputDeviceList(deviceList.get(), &numDevices, sizeof(RAWINPUTDEVICELIST));

    for (UINT i = 0; i < numDevices; ++i) {
        if (deviceList[i].dwType != RIM_TYPEHID) continue;

        RID_DEVICE_INFO deviceInfo;
        deviceInfo.cbSize = sizeof(RID_DEVICE_INFO);
        UINT size = sizeof(RID_DEVICE_INFO);
        if (GetRawInputDeviceInfo(deviceList[i].hDevice, RIDI_DEVICEINFO, &deviceInfo, &size) == (UINT)-1) continue;

        if (deviceInfo.hid.usUsagePage == 1 && (deviceInfo.hid.usUsage == 4 || deviceInfo.hid.usUsage == 5)) {
            auto info = std::make_unique<HidDeviceInfo>();
            info->handle = deviceList[i].hDevice;
            info->rawInfo = deviceInfo;

            UINT pathSize = 0;
            GetRawInputDeviceInfo(info->handle, RIDI_DEVICENAME, NULL, &pathSize);
            if (pathSize > 1) {
                std::wstring wpath(pathSize, L'\0');
                GetRawInputDeviceInfo(info->handle, RIDI_DEVICENAME, &wpath[0], &pathSize);
                info->path = WStringToString(wpath);
            }

            HANDLE hFile = CreateFileW(StringToWString(info->path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                wchar_t buffer[256] = {0};
                if (HidD_GetProductString(hFile, buffer, sizeof(buffer))) {
                    info->name = WStringToString(buffer);
                }
                CloseHandle(hFile);
            }

            UINT dataSize = 0;
            GetRawInputDeviceInfo(info->handle, RIDI_PREPARSEDDATA, NULL, &dataSize);
            if (dataSize > 0) {
                info->preparsedData = (PHIDP_PREPARSED_DATA)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dataSize);
                if (info->preparsedData && GetRawInputDeviceInfo(info->handle, RIDI_PREPARSEDDATA, info->preparsedData, &dataSize) == dataSize) {
                    if (HidP_GetCaps(info->preparsedData, &info->caps) == HIDP_STATUS_SUCCESS) {
                        hidDevices.push_back(std::move(*info));
                    }
                }
            }
        }
    }
    return hidDevices;
}

std::vector<ControlInfo> GetAvailableControls(PHIDP_PREPARSED_DATA pData, HIDP_CAPS& caps) {
    std::vector<ControlInfo> controls;
    // Get Buttons
    if (caps.NumberInputButtonCaps > 0) {
        auto buttonCapsVec = std::make_unique<HIDP_BUTTON_CAPS[]>(caps.NumberInputButtonCaps);
        USHORT capsLength = caps.NumberInputButtonCaps;
        if (HidP_GetButtonCaps(HidP_Input, buttonCapsVec.get(), &capsLength, pData) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < capsLength; ++i) {
                const auto& bCaps = buttonCapsVec[i];
                if (bCaps.IsRange) {
                    for (USAGE u = bCaps.Range.UsageMin; u <= bCaps.Range.UsageMax; ++u) {
                        ControlInfo ctrl;
                        ctrl.isButton = true; ctrl.usagePage = bCaps.UsagePage; ctrl.usage = u;
                        ctrl.name = "Button " + std::to_string(u);
                        controls.push_back(ctrl);
                    }
                } else {
                    ControlInfo ctrl;
                    ctrl.isButton = true; ctrl.usagePage = bCaps.UsagePage; ctrl.usage = bCaps.NotRange.Usage;
                    ctrl.name = "Button " + std::to_string(bCaps.NotRange.Usage);
                    controls.push_back(ctrl);
                }
            }
        }
    }
    // Get Axes/Values
    if (caps.NumberInputValueCaps > 0) {
        auto valueCapsVec = std::make_unique<HIDP_VALUE_CAPS[]>(caps.NumberInputValueCaps);
        USHORT capsLength = caps.NumberInputValueCaps;
        if (HidP_GetValueCaps(HidP_Input, valueCapsVec.get(), &capsLength, pData) == HIDP_STATUS_SUCCESS) {
            for (USHORT i = 0; i < capsLength; ++i) {
                const auto& vCaps = valueCapsVec[i];
                if (vCaps.IsRange) {
                    for (USAGE u = vCaps.Range.UsageMin; u <= vCaps.Range.UsageMax; ++u) {
                        ControlInfo ctrl;
                        ctrl.isButton = false; ctrl.usagePage = vCaps.UsagePage; ctrl.usage = u;
                        ctrl.logicalMin = vCaps.LogicalMin; ctrl.logicalMax = vCaps.LogicalMax;
                        ctrl.name = "Axis " + std::to_string(u);
                        controls.push_back(ctrl);
                    }
                } else {
                    ControlInfo ctrl;
                    ctrl.isButton = false; ctrl.usagePage = vCaps.UsagePage; ctrl.usage = vCaps.NotRange.Usage;
                    ctrl.logicalMin = vCaps.LogicalMin; ctrl.logicalMax = vCaps.LogicalMax;
                    ctrl.name = "Axis " + std::to_string(vCaps.NotRange.Usage);
                    controls.push_back(ctrl);
                }
            }
        }
    }
    return controls;
}

// --- Windows Input Monitoring ---
HWND g_messageWindow = nullptr;
RAWINPUTDEVICE g_rid;
PHIDP_PREPARSED_DATA g_preparsedData = nullptr;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_INPUT) {
        UINT dwSize = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
        auto lpb = std::make_unique<BYTE[]>(dwSize);
        if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.get(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) return 0;

        RAWINPUT* raw = (RAWINPUT*)lpb.get();
        if (raw->header.dwType == RIM_TYPEHID && g_preparsedData) {
            ULONG value = 0;
            if (g_currentConfig.control.isButton) {
                USAGE usage = g_currentConfig.control.usage;
                ULONG usageCount = 1;
                if (HidP_GetUsages(HidP_Input, g_currentConfig.control.usagePage, 0, &usage, &usageCount, g_preparsedData, (PCHAR)raw->data.hid.bRawData, raw->data.hid.dwSizeHid) == HIDP_STATUS_SUCCESS) {
                    value = 1;
                } else {
                    value = 0;
                }
            } else {
                HidP_GetUsageValue(HidP_Input, g_currentConfig.control.usagePage, 0, g_currentConfig.control.usage, &value, g_preparsedData, (PCHAR)raw->data.hid.bRawData, raw->data.hid.dwSizeHid);
            }
            if (static_cast<LONG>(value) != g_currentValue.load()) {
                g_currentValue = value;
                g_valueChanged = true;
            }
        }
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    if (uMsg == WM_DESTROY) {
        g_quitFlag = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void InputMonitorLoop() {
    WNDCLASS wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"JoystickMidiListener";
    if (!RegisterClass(&wc)) return;

    g_messageWindow = CreateWindowEx(0, wc.lpszClassName, L"Listener", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    if (!g_messageWindow) return;

    g_rid.usUsagePage = 1; // Generic Desktop
    g_rid.usUsage = 4;     // Joystick
    g_rid.dwFlags = RIDEV_INPUTSINK;
    g_rid.hwndTarget = g_messageWindow;
    RegisterRawInputDevices(&g_rid, 1, sizeof(g_rid));
    g_rid.usUsage = 5; // Gamepad
    RegisterRawInputDevices(&g_rid, 1, sizeof(g_rid));

    MSG msg;
    while (!g_quitFlag && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_preparsedData) HeapFree(GetProcessHeap(), 0, g_preparsedData);
    if (g_messageWindow) DestroyWindow(g_messageWindow);
    UnregisterClass(L"JoystickMidiListener", GetModuleHandle(NULL));
}

#else // --- Linux Implementation ---

struct HidDeviceInfo {
    std::string name;
    std::string path;
};

std::vector<HidDeviceInfo> EnumerateHidDevices() {
    std::vector<HidDeviceInfo> found_devices;
    struct udev *udev = udev_new();
    if (!udev) return found_devices;

    struct udev_enumerate *enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
        udev_unref(udev);
        return found_devices;
    }

    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_scan_devices(enumerate);

    struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
    struct udev_list_entry *dev_list_entry;

    udev_list_entry_foreach(dev_list_entry, devices) {
        const char *syspath = udev_list_entry_get_name(dev_list_entry);
        if (!syspath) continue;

        struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
        if (!dev) continue;

        const char* is_joystick = udev_device_get_property_value(dev, "ID_INPUT_JOYSTICK");
        if (is_joystick && strcmp(is_joystick, "1") == 0) {
            const char* dev_node = udev_device_get_devnode(dev);
            if (dev_node && (std::string(dev_node).find("/dev/input/event") != std::string::npos)) {
                HidDeviceInfo info;
                info.path = dev_node;
                const char* name = udev_device_get_property_value(dev, "ID_MODEL_FROM_DATABASE");
                if (!name) name = udev_device_get_property_value(dev, "NAME");
                info.name = name ? name : "Unnamed Joystick";
                found_devices.push_back(info);
            }
        }
        udev_device_unref(dev);
    }

    udev_enumerate_unref(enumerate);
    udev_unref(udev);
    return found_devices;
}

std::vector<ControlInfo> GetAvailableControls(const std::string& devicePath) {
    std::vector<ControlInfo> controls;
    int fd = open(devicePath.c_str(), O_RDONLY);
    if (fd < 0) return controls;

    unsigned long ev_bits[EV_MAX / BITS_PER_LONG + 1] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);

    auto test_bit = [](int bit, const unsigned long* array) {
        return (array[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1;
    };

    if (test_bit(EV_KEY, ev_bits)) {
        unsigned long key_bits[KEY_MAX / BITS_PER_LONG + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
        for (int code = BTN_JOYSTICK; code < KEY_MAX; ++code) {
            if (test_bit(code, key_bits)) {
                ControlInfo ctrl;
                ctrl.isButton = true; ctrl.eventType = EV_KEY; ctrl.eventCode = code;
                ctrl.logicalMin = 0; ctrl.logicalMax = 1;
                ctrl.name = "Button " + std::to_string(code - BTN_JOYSTICK);
                controls.push_back(ctrl);
            }
        }
    }
    if (test_bit(EV_ABS, ev_bits)) {
        unsigned long abs_bits[ABS_MAX / BITS_PER_LONG + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
        for (int code = 0; code < ABS_MAX; ++code) {
            if (test_bit(code, abs_bits)) {
                struct input_absinfo abs_info;
                if (ioctl(fd, EVIOCGABS(code), &abs_info) >= 0) {
                    ControlInfo ctrl;
                    ctrl.isButton = false; ctrl.eventType = EV_ABS; ctrl.eventCode = code;
                    ctrl.logicalMin = abs_info.minimum; ctrl.logicalMax = abs_info.maximum;
                    ctrl.name = "Axis " + std::to_string(code);
                    controls.push_back(ctrl);
                }
            }
        }
    }
    close(fd);
    return controls;
}

void InputMonitorLoop() {
    int fd = open(g_currentConfig.hidDevicePath.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        std::cerr << "\nError: Could not open device " << g_currentConfig.hidDevicePath << " in input thread. " << strerror(errno) << std::endl;
        return;
    }

    struct input_event ev;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (!g_quitFlag) {
        int ret = poll(&pfd, 1, 100);
        if (ret < 0 || !(pfd.revents & POLLIN)) continue;

        if (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == g_currentConfig.control.eventType && ev.code == g_currentConfig.control.eventCode) {
                if (static_cast<LONG>(ev.value) != g_currentValue.load()) {
                    g_currentValue = ev.value;
                    g_valueChanged = true;
                }
            }
        }
    }
    close(fd);
    std::cout << "\nInput monitoring thread finished." << std::endl;
}

#endif

// ===================================================================================
//
// CROSS-PLATFORM HELPER FUNCTIONS
//
// ===================================================================================

void ClearScreen() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
}

void ClearInputBuffer() {
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

bool string_ends_with(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int GetUserSelection(int maxValidChoice, int minValidChoice) {
    long long choice = -1;
    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) { g_quitFlag = true; return -1; }
        try {
            if (line.empty()) continue;
            size_t processedChars = 0;
            choice = std::stoll(line, &processedChars);
            if (processedChars == line.length() && choice >= minValidChoice && choice <= maxValidChoice) {
                return static_cast<int>(choice);
            } else {
                std::cout << "Invalid input. Please enter a whole number between " << minValidChoice << " and " << maxValidChoice << "." << std::endl;
            }
        } catch (const std::exception&) {
            std::cout << "Invalid input. Please enter a number." << std::endl;
        }
    }
}

void DisplayMonitoringOutput() {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    const int BAR_WIDTH = 30;
    const int DISPLAY_WIDTH = 80;
    std::stringstream ss;

    ss << "[" << std::left << std::setw(20) << g_currentConfig.control.name.substr(0, 20) << "] ";

    if (g_currentConfig.control.isButton) {
        ss << (g_currentValue.load() ? "[ ### ON ### ]" : "[ --- OFF -- ]");
    } else {
        double percentage = 0.0;
        LONG displayRangeMin = g_currentConfig.control.logicalMin;
        LONG displayRangeMax = g_currentConfig.control.logicalMax;

        if (g_currentConfig.calibrationDone) {
            displayRangeMin = g_currentConfig.calibrationMinHid;
            displayRangeMax = g_currentConfig.calibrationMaxHid;
        }

        LONG displayRange = displayRangeMax - displayRangeMin;
        if (displayRange > 0) {
            LONG clampedValue = std::max(displayRangeMin, std::min(displayRangeMax, g_currentValue.load()));
            percentage = static_cast<double>(clampedValue - displayRangeMin) * 100.0 / static_cast<double>(displayRange);
        } else if (g_currentValue.load() >= displayRangeMax) {
            percentage = 100.0;
        }

        int barLength = static_cast<int>((percentage / 100.0) * BAR_WIDTH + 0.5);
        barLength = std::max(0, std::min(BAR_WIDTH, barLength));

        std::string bar(barLength, '#');
        std::string empty(BAR_WIDTH - barLength, '-');

        ss << "|" << bar << empty << "| ";
        ss << std::fixed << std::setprecision(1) << std::setw(5) << percentage << "% ";
        ss << "(Raw:" << std::right << std::setw(6) << g_currentValue.load() << ")";
    }

    std::string outputStr = ss.str();
    if (outputStr.length() < DISPLAY_WIDTH) {
        outputStr.append(DISPLAY_WIDTH - outputStr.length(), ' ');
    } else if (outputStr.length() > DISPLAY_WIDTH) {
        outputStr.resize(DISPLAY_WIDTH);
    }
    std::cout << "\r" << outputStr << std::flush;
}

bool SaveConfiguration(const MidiMappingConfig& config, const std::string& filename) {
    try {
        json j = config;
        std::ofstream ofs(filename);
        if (!ofs.is_open()) {
            std::cerr << "Error: Could not open file for saving: " << filename << std::endl;
            return false;
        }
        ofs << std::setw(4) << j << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving config: " << e.what() << std::endl;
        return false;
    }
}

bool LoadConfiguration(const std::string& filename, MidiMappingConfig& config) {
    try {
        std::ifstream ifs(filename);
        if (!ifs.is_open()) return false;
        json j;
        ifs >> j;
        config = j.get<MidiMappingConfig>();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading config '" << filename << "': " << e.what() << std::endl;
        return false;
    }
}

std::vector<fs::path> ListConfigurations(const std::string& directory) {
    std::vector<fs::path> configFiles;
    try {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file() && string_ends_with(entry.path().string(), CONFIG_EXTENSION)) {
                configFiles.push_back(entry.path());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error listing configs: " << e.what() << std::endl;
    }
    std::sort(configFiles.begin(), configFiles.end());
    return configFiles;
}

bool PerformCalibration() {
    if (g_currentConfig.control.isButton) return true;

    auto do_countdown = [](const std::string& stageName) {
        for (int i = 5; i > 0; --i) {
            std::cout << "\rStarting " << stageName << " capture in " << i << " second(s)... " << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << "\r" << std::string(50, ' ') << "\r" << std::flush;
    };

    auto capture_hold_value = [](bool captureMin) -> LONG {
        LONG extremeValue = captureMin ? std::numeric_limits<LONG>::max() : std::numeric_limits<LONG>::min();
        auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(5);

        while (std::chrono::steady_clock::now() < endTime) {
            auto time_left = std::chrono::duration_cast<std::chrono::seconds>(endTime - std::chrono::steady_clock::now()).count();
            LONG current_val = g_currentValue.load();
            if (captureMin) extremeValue = std::min(extremeValue, current_val);
            else extremeValue = std::max(extremeValue, current_val);

            std::cout << "\rCapturing... HOLD! (" << time_left + 1 << "s) Current: " << current_val << " "
                      << (captureMin ? "Min: " : "Max: ") << extremeValue << "      " << std::flush;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::cout << std::endl;
        return extremeValue;
    };

    ClearScreen();
    std::cout << "--- Calibrating Axis: " << g_currentConfig.control.name << " ---\n\n";
    std::cout << "1. Move the control to its desired MINIMUM position.\n   Get ready!" << std::endl;
    do_countdown("MIN");
    g_currentConfig.calibrationMinHid = capture_hold_value(true);
    std::cout << "   Minimum value captured: " << g_currentConfig.calibrationMinHid << "\n\n";

    std::cout << "2. Move the control to its desired MAXIMUM position.\n   Get ready!" << std::endl;
    do_countdown("MAX");
    g_currentConfig.calibrationMaxHid = capture_hold_value(false);
    std::cout << "   Maximum value captured: " << g_currentConfig.calibrationMaxHid << "\n\n";

    if (g_currentConfig.calibrationMinHid > g_currentConfig.calibrationMaxHid) {
        std::cout << "Note: Min value was greater than Max value. Swapping." << std::endl;
        std::swap(g_currentConfig.calibrationMinHid, g_currentConfig.calibrationMaxHid);
    }
    g_currentConfig.calibrationDone = true;
    std::cout << "Calibration complete. Press Enter to continue." << std::endl;
    ClearInputBuffer();
    std::cin.get();
    return true;
}

// ===================================================================================
//
// MAIN APPLICATION
//
// ===================================================================================

int main() {
    ClearScreen();
    std::cout << "--- HID to MIDI Mapper ---\n\n";
    bool configLoaded = false;

    auto configFiles = ListConfigurations(".");
    if (!configFiles.empty()) {
        std::cout << "Found existing configurations:\n";
        for (size_t i = 0; i < configFiles.size(); ++i) {
            std::cout << "[" << i << "] " << configFiles[i].filename().string() << std::endl;
        }
        std::cout << "[" << configFiles.size() << "] Create New Configuration\n";
        int choice = GetUserSelection(configFiles.size(), 0);
        if (g_quitFlag) return 1;

        if (choice < (int)configFiles.size()) {
            if (LoadConfiguration(configFiles[choice].string(), g_currentConfig)) {
                std::cout << "Configuration loaded successfully." << std::endl;
                configLoaded = true;
            } else {
                std::cerr << "Failed to load configuration. Starting new setup." << std::endl;
            }
        }
    }

    if (!configLoaded) {
        ClearScreen();
        std::cout << "--- Step 1: Select HID Controller ---\n";
        auto available_devices = EnumerateHidDevices();
        if (available_devices.empty()) {
            std::cerr << "No joysticks found." << std::endl; return 1;
        }

        std::cout << "Available Controllers:\n";
        for (size_t i = 0; i < available_devices.size(); ++i) {
            std::cout << "[" << i << "] " << available_devices[i].name << " (" << available_devices[i].path << ")" << std::endl;
        }
        int dev_choice = GetUserSelection(available_devices.size() - 1, 0);
        if (g_quitFlag) return 1;

        g_currentConfig.hidDeviceName = available_devices[dev_choice].name;
        g_currentConfig.hidDevicePath = available_devices[dev_choice].path;

        ClearScreen();
        std::cout << "--- Step 2: Select Control to Map ---\n";
        #ifdef _WIN32
            // On Windows, we need the preparsed data from the selected device
            UINT dataSize = 0;
            GetRawInputDeviceInfo(available_devices[dev_choice].handle, RIDI_PREPARSEDDATA, NULL, &dataSize);
            if (dataSize > 0) {
                g_preparsedData = (PHIDP_PREPARSED_DATA)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dataSize);
                GetRawInputDeviceInfo(available_devices[dev_choice].handle, RIDI_PREPARSEDDATA, g_preparsedData, &dataSize);
            }
            auto available_controls = GetAvailableControls(g_preparsedData, available_devices[dev_choice].caps);
        #else
            auto available_controls = GetAvailableControls(g_currentConfig.hidDevicePath);
        #endif

        if (available_controls.empty()) {
            std::cerr << "No usable controls found on this device." << std::endl; return 1;
        }
        std::cout << "Available Controls:\n";
        for (size_t i = 0; i < available_controls.size(); ++i) {
            std::cout << "[" << i << "] " << available_controls[i].name << (available_controls[i].isButton ? " (Button)" : " (Axis)") << std::endl;
        }
        int ctrl_choice = GetUserSelection(available_controls.size() - 1, 0);
        g_currentConfig.control = available_controls[ctrl_choice];

        ClearScreen();
        std::cout << "--- Step 3: Select MIDI Output ---\n";
        unsigned int portCount = g_midiOut.getPortCount();
        if (portCount == 0) {
            std::cerr << "No MIDI output ports available." << std::endl; return 1;
        }
        for (unsigned int i = 0; i < portCount; ++i) {
            std::cout << "  [" << i << "]: " << g_midiOut.getPortName(i) << std::endl;
        }
        int midi_choice = GetUserSelection(portCount - 1, 0);
        g_midiOut.openPort(midi_choice);
        g_currentConfig.midiDeviceName = g_midiOut.getPortName(midi_choice);

        g_inputThread = std::thread(InputMonitorLoop);

        ClearScreen();
        std::cout << "--- Step 4: Configure MIDI Mapping ---\n";
        std::cout << "Select MIDI message type:\n[0] Note On/Off\n[1] CC\n";
        g_currentConfig.midiMessageType = (GetUserSelection(1, 0) == 0) ? MidiMappingConfig::MidiMessageType::NOTE_ON_OFF : MidiMappingConfig::MidiMessageType::CC;
        std::cout << "Enter MIDI Channel (1-16): ";
        g_currentConfig.midiChannel = GetUserSelection(16, 1) - 1;
        std::cout << "Enter MIDI Note/CC Number (0-127): ";
        g_currentConfig.midiNoteOrCCNumber = GetUserSelection(127, 0);

        if (g_currentConfig.midiMessageType == MidiMappingConfig::MidiMessageType::NOTE_ON_OFF) {
            std::cout << "Enter Note On Velocity (1-127): ";
            g_currentConfig.midiValueNoteOnVelocity = GetUserSelection(127, 1);
        } else {
            if (g_currentConfig.control.isButton) {
                std::cout << "Enter CC Value when Pressed (0-127): ";
                g_currentConfig.midiValueCCOn = GetUserSelection(127, 0);
                std::cout << "Enter CC Value when Released (0-127): ";
                g_currentConfig.midiValueCCOff = GetUserSelection(127, 0);
            } else {
                std::cout << "Reverse MIDI output? (0=No, 1=Yes): ";
                g_currentConfig.reverseAxis = (GetUserSelection(1, 0) == 1);
                PerformCalibration();
            }
        }
    } else { // Config was loaded
        #ifdef _WIN32
            // On Windows, we need to find the device and get its preparsed data
            auto devices = EnumerateHidDevices();
            bool found = false;
            for(auto& dev : devices) {
                if (dev.path == g_currentConfig.hidDevicePath) {
                    g_preparsedData = dev.preparsedData;
                    dev.preparsedData = nullptr; // Prevent destructor from freeing it
                    found = true;
                    break;
                }
            }
            if (!found) {
                std::cerr << "Configured HID device not found." << std::endl; return 1;
            }
        #endif
        g_inputThread = std::thread(InputMonitorLoop);
        unsigned int portCount = g_midiOut.getPortCount();
        int midi_port = -1;
        for (unsigned int i = 0; i < portCount; ++i) {
            if (g_midiOut.getPortName(i) == g_currentConfig.midiDeviceName) {
                midi_port = i;
                break;
            }
        }
        if (midi_port == -1) {
            std::cerr << "Configured MIDI port '" << g_currentConfig.midiDeviceName << "' not found." << std::endl;
            g_quitFlag = true;
            if (g_inputThread.joinable()) g_inputThread.join();
            return 1;
        }
        g_midiOut.openPort(midi_port);
    }

    if (!configLoaded) {
        ClearScreen();
        std::cout << "--- Step 5: Save Configuration ---\n";
        std::cout << "Enter filename to save (e.g., my_joystick.hidmidi.json), or leave blank to skip: ";
        std::string saveFilename;
        std::getline(std::cin, saveFilename);
        if (!saveFilename.empty()) {
            if (!string_ends_with(saveFilename, CONFIG_EXTENSION)) {
                saveFilename += CONFIG_EXTENSION;
            }
            if (SaveConfiguration(g_currentConfig, saveFilename)) {
                std::cout << "Configuration saved to " << saveFilename << std::endl;
            }
        }
    }

    ClearScreen();
    std::cout << "--- Monitoring Active ---\n";
    std::cout << "Device: " << g_currentConfig.hidDeviceName << std::endl;
    std::cout << "Control: " << g_currentConfig.control.name << std::endl;
    std::cout << "MIDI Port: " << g_currentConfig.midiDeviceName << std::endl;
    std::cout << "(Press Enter to exit on Linux, or close window)\n\n";

    auto lastDisplayTime = std::chrono::steady_clock::now();
    while (!g_quitFlag) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastDisplayTime > std::chrono::milliseconds(1000 / 60)) {
            DisplayMonitoringOutput();
            lastDisplayTime = now;
        }

        if (g_valueChanged.exchange(false)) {
            std::vector<unsigned char> message;
            if (g_currentConfig.control.isButton) {
                bool pressed = g_currentValue.load() != 0;
                if (pressed != (g_previousValue != 0)) {
                    if (g_currentConfig.midiMessageType == MidiMappingConfig::MidiMessageType::NOTE_ON_OFF) {
                        message = {(unsigned char)((pressed ? 0x90 : 0x80) | g_currentConfig.midiChannel), (unsigned char)g_currentConfig.midiNoteOrCCNumber, (unsigned char)(pressed ? g_currentConfig.midiValueNoteOnVelocity : 0)};
                    } else {
                        message = {(unsigned char)(0xB0 | g_currentConfig.midiChannel), (unsigned char)g_currentConfig.midiNoteOrCCNumber, (unsigned char)(pressed ? g_currentConfig.midiValueCCOn : g_currentConfig.midiValueCCOff)};
                    }
                    if (!message.empty()) g_midiOut.sendMessage(&message);
                }
            } else { // Axis
                if (g_currentConfig.calibrationDone) {
                    LONG range = g_currentConfig.calibrationMaxHid - g_currentConfig.calibrationMinHid;
                    if (range > 0) {
                        LONG clamped = std::max(g_currentConfig.calibrationMinHid, std::min(g_currentConfig.calibrationMaxHid, g_currentValue.load()));
                        double norm = (double)(clamped - g_currentConfig.calibrationMinHid) / range;
                        if (g_currentConfig.reverseAxis) norm = 1.0 - norm;
                        int midiVal = (int)(norm * 127.0 + 0.5);
                        if (midiVal != g_lastSentMidiValue) {
                            message = {(unsigned char)(0xB0 | g_currentConfig.midiChannel), (unsigned char)g_currentConfig.midiNoteOrCCNumber, (unsigned char)midiVal};
                            g_midiOut.sendMessage(&message);
                            g_lastSentMidiValue = midiVal;
                        }
                    }
                }
            }
            g_previousValue = g_currentValue.load();
        }

        #ifndef _WIN32
        {
            std::lock_guard<std::mutex> lock(g_consoleMutex);
            struct timeval tv = {0L, 0L};
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(0, &fds);
            if (select(1, &fds, NULL, NULL, &tv) > 0) {
                g_quitFlag = true;
            }
        }
        #endif
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::cout << "\n\nExiting..." << std::endl;
    if (g_inputThread.joinable()) g_inputThread.join();
    if (g_midiOut.isPortOpen()) g_midiOut.closePort();
    return 0;
}
