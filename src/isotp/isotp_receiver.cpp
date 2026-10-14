#include "isotp/isotp_receiver.h"

#include <algorithm>
#include <array>

namespace obdlink {

namespace {

constexpr std::size_t FirstFrameHeaderBytes = 2;

}

std::vector<IsoTpEvent> IsoTpReceiver::accept(const CanFrame& frame)
{
    if (frame.data.empty()) {
        return {};
    }
    using Handler = std::vector<IsoTpEvent> (IsoTpReceiver::*)(const CanFrame&);
    static constexpr std::array<Handler, 3> handlersByPciType {
        &IsoTpReceiver::acceptSingle,
        &IsoTpReceiver::acceptFirst,
        &IsoTpReceiver::acceptConsecutive,
    };
    const std::size_t pciType = frame.data[0] >> 4;
    if (pciType >= handlersByPciType.size()) {
        return {};
    }
    return (this->*handlersByPciType[pciType])(frame);
}

void IsoTpReceiver::reset()
{
    assemblies_.clear();
}

std::vector<IsoTpEvent> IsoTpReceiver::acceptSingle(const CanFrame& frame)
{
    const std::size_t length = frame.data[0] & 0x0F;
    if (length == 0 || length > MaxSingleFramePayload || length >= frame.data.size()) {
        return {};
    }
    assemblies_.erase({ frame.id, frame.extended });
    return { IsoTpMessage { frame.id, frame.extended, { frame.data.begin() + 1, frame.data.begin() + 1 + length } } };
}

std::vector<IsoTpEvent> IsoTpReceiver::acceptFirst(const CanFrame& frame)
{
    if (frame.data.size() < FirstFrameHeaderBytes) {
        return {};
    }
    const std::size_t length = (static_cast<std::size_t>(frame.data[0] & 0x0F) << 8) | frame.data[1];
    if (length <= MaxSingleFramePayload) {
        return {};
    }
    Assembly assembly;
    assembly.expectedLength = length;
    const auto available = std::min(frame.data.size() - FirstFrameHeaderBytes, length);
    assembly.payload.assign(frame.data.begin() + FirstFrameHeaderBytes, frame.data.begin() + FirstFrameHeaderBytes + available);
    assemblies_[{ frame.id, frame.extended }] = std::move(assembly);
    return { IsoTpFirstFrame { frame.id, frame.extended } };
}

std::vector<IsoTpEvent> IsoTpReceiver::acceptConsecutive(const CanFrame& frame)
{
    const auto found = assemblies_.find({ frame.id, frame.extended });
    if (found == assemblies_.end()) {
        return {};
    }
    auto& assembly = found->second;
    const std::uint8_t sequence = frame.data[0] & 0x0F;
    if (sequence != assembly.nextSequence) {
        assemblies_.erase(found);
        return {};
    }
    assembly.nextSequence = (assembly.nextSequence + 1) & 0x0F;
    const auto remaining = assembly.expectedLength - assembly.payload.size();
    const auto available = std::min(frame.data.size() - 1, remaining);
    assembly.payload.insert(assembly.payload.end(), frame.data.begin() + 1, frame.data.begin() + 1 + available);
    if (assembly.payload.size() < assembly.expectedLength) {
        return {};
    }
    IsoTpMessage message { frame.id, frame.extended, std::move(assembly.payload) };
    assemblies_.erase(found);
    return { std::move(message) };
}

}
