#include "config/registry_settings.h"

#include <array>
#include <chrono>

#include <windows.h>

namespace obdlink {

namespace {

DWORD readDword(const char* name, DWORD fallback)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    const auto result = RegGetValueA(HKEY_LOCAL_MACHINE, DeviceRegistryKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
    return result == ERROR_SUCCESS ? value : fallback;
}

std::string readString(const char* name)
{
    std::array<char, MAX_PATH> buffer {};
    DWORD size = buffer.size();
    const auto result = RegGetValueA(HKEY_LOCAL_MACHINE, DeviceRegistryKey, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
    return result == ERROR_SUCCESS ? std::string(buffer.data()) : std::string {};
}

std::chrono::milliseconds readMilliseconds(const char* name, DWORD fallback)
{
    return std::chrono::milliseconds(readDword(name, fallback));
}

}

DeviceSettings loadRegistrySettings()
{
    DeviceSettings settings;
    settings.portName = readString("ComPort");
    settings.hostBaudRate = readDword("HostBaudRate", 115200);
    settings.adapterBaudRate = readDword("AdapterBaudRate", 2000000);
    settings.logPath = readString("LogFile");
    settings.timing = AdapterTiming {
        readMilliseconds("CommandTimeoutMs", 1000),
        readMilliseconds("ResetTimeoutMs", 3000),
        // ISO 14229-2 P2*Server_max.
        std::chrono::milliseconds(5000),
        readMilliseconds("BusyProbeMs", 300),
    };
    return settings;
}

}
