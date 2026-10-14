#pragma once

#include "device/device_settings.h"

namespace obdlink {

// Registry key of this pass-thru device, relative to HKLM; a 32-bit process reads it through
// the WOW6432Node view.
inline constexpr char DeviceRegistryKey[] = "SOFTWARE\\PassThruSupport.04.04\\OBDLink STN";

DeviceSettings loadRegistrySettings();

}
