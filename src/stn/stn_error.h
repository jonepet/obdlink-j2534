#pragma once

#include <stdexcept>

namespace obdlink {

class StnError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class StnTimeout : public StnError {
public:
    using StnError::StnError;
};

class StnBaudRateRejected : public StnError {
public:
    using StnError::StnError;
};

}
