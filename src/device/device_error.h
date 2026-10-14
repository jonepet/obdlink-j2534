#pragma once

#include "stn/stn_error.h"

namespace obdlink {

class DeviceUnavailable : public StnError {
public:
    using StnError::StnError;
};

class TransmitFailed : public StnError {
public:
    using StnError::StnError;
};

}
