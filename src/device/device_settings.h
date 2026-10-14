#pragma once

#include <string>

#include "stn/stn_adapter.h"

namespace obdlink {

struct DeviceSettings {
    std::string portName;
    unsigned hostBaudRate = 0;
    unsigned adapterBaudRate = 0;
    AdapterTiming timing {};
    std::string logPath;
};

}
