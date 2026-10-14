#include <memory>

#include <windows.h>

#include "api/passthru_api.h"
#include "config/registry_settings.h"
#include "transport/win_serial_port.h"

namespace {

obdlink::PassThruApi& api()
{
    static obdlink::PassThruApi instance(
        obdlink::loadRegistrySettings,
        [](const obdlink::DeviceSettings& settings) -> std::unique_ptr<obdlink::ByteStream> {
            return std::make_unique<obdlink::WinSerialPort>(settings.portName, settings.hostBaudRate, settings.timing.commandTimeout);
        },
        OBDLINK_J2534_VERSION);
    return instance;
}

// An open device runs a scheduler thread; unloading the DLL under it would make FreeLibrary
// join that thread while holding the loader lock. Pinning keeps the DLL loaded until process exit.
bool pinModule()
{
    HMODULE module = nullptr;
    return GetModuleHandleExA(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCSTR>(&pinModule),
               &module)
        != 0;
}

template <typename Call>
long contained(Call call) noexcept
{
    try {
        return call();
    } catch (...) {
        return j2534::status::Failed;
    }
}

}

using j2534::PassThruMsg;
using j2534::ULong;

extern "C" {

long WINAPI PassThruOpen(void* pName, ULong* pDeviceID)
{
    return contained([&] {
        static const bool pinned = pinModule();
        if (!pinned) {
            return api().reportFailure("cannot pin the driver DLL in memory");
        }
        return api().open(pName, pDeviceID);
    });
}

long WINAPI PassThruClose(ULong DeviceID)
{
    return contained([&] { return api().close(DeviceID); });
}

long WINAPI PassThruConnect(ULong DeviceID, ULong ProtocolID, ULong Flags, ULong BaudRate, ULong* pChannelID)
{
    return contained([&] { return api().connect(DeviceID, ProtocolID, Flags, BaudRate, pChannelID); });
}

long WINAPI PassThruDisconnect(ULong ChannelID)
{
    return contained([&] { return api().disconnect(ChannelID); });
}

long WINAPI PassThruReadMsgs(ULong ChannelID, PassThruMsg* pMsg, ULong* pNumMsgs, ULong Timeout)
{
    return contained([&] { return api().readMessages(ChannelID, pMsg, pNumMsgs, Timeout); });
}

long WINAPI PassThruWriteMsgs(ULong ChannelID, PassThruMsg* pMsg, ULong* pNumMsgs, ULong)
{
    return contained([&] { return api().writeMessages(ChannelID, pMsg, pNumMsgs); });
}

long WINAPI PassThruStartPeriodicMsg(ULong ChannelID, PassThruMsg* pMsg, ULong* pMsgID, ULong TimeInterval)
{
    return contained([&] { return api().startPeriodic(ChannelID, pMsg, pMsgID, TimeInterval); });
}

long WINAPI PassThruStopPeriodicMsg(ULong ChannelID, ULong MsgID)
{
    return contained([&] { return api().stopPeriodic(ChannelID, MsgID); });
}

long WINAPI PassThruStartMsgFilter(
    ULong ChannelID,
    ULong FilterType,
    PassThruMsg* pMaskMsg,
    PassThruMsg* pPatternMsg,
    PassThruMsg* pFlowControlMsg,
    ULong* pFilterID)
{
    return contained([&] { return api().startFilter(ChannelID, FilterType, pMaskMsg, pPatternMsg, pFlowControlMsg, pFilterID); });
}

long WINAPI PassThruStopMsgFilter(ULong ChannelID, ULong FilterID)
{
    return contained([&] { return api().stopFilter(ChannelID, FilterID); });
}

long WINAPI PassThruSetProgrammingVoltage(ULong DeviceID, ULong PinNumber, ULong Voltage)
{
    return contained([&] { return api().setProgrammingVoltage(DeviceID, PinNumber, Voltage); });
}

long WINAPI PassThruReadVersion(ULong DeviceID, char* pFirmwareVersion, char* pDllVersion, char* pApiVersion)
{
    return contained([&] { return api().readVersion(DeviceID, pFirmwareVersion, pDllVersion, pApiVersion); });
}

long WINAPI PassThruGetLastError(char* pErrorDescription)
{
    return contained([&] { return api().lastError(pErrorDescription); });
}

long WINAPI PassThruIoctl(ULong ChannelID, ULong IoctlID, void* pInput, void* pOutput)
{
    return contained([&] { return api().ioctl(ChannelID, IoctlID, pInput, pOutput); });
}

}
