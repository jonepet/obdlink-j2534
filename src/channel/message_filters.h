#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "channel/channel_kind.h"
#include "j2534/j2534_defs.h"
#include "stn/stn_adapter.h"

namespace obdlink {

struct FilterMessages {
    const j2534::PassThruMsg* mask;
    const j2534::PassThruMsg* pattern;
    const j2534::PassThruMsg* flowControl;
};

// J2534 message filters of one channel: validation, receive-side matching, and the
// equivalent STN hardware filters and flow-control pairs.
class MessageFilters {
public:
    explicit MessageFilters(ChannelKind kind);

    long add(j2534::ULong type, const FilterMessages& messages, j2534::ULong& filterId);

    long remove(j2534::ULong filterId);

    void clear();

    bool acceptsFrame(std::span<const std::uint8_t> idAndData) const;

    bool acceptsIsoTpFrom(std::uint32_t ecuId, bool extended) const;

    bool hasFlowControlFor(std::uint32_t testerId, bool extended) const;

    std::vector<StnFilter> stnFlowControlFilters() const;

    std::vector<StnFilter> stnPassFilters(bool extended) const;

    std::vector<FlowControlPair> flowControlPairs() const;

private:
    struct Filter {
        j2534::ULong type;
        std::vector<std::uint8_t> mask;
        std::vector<std::uint8_t> pattern;
        std::vector<std::uint8_t> flowControl;
        bool extended;
    };

    long validate(j2534::ULong type, const FilterMessages& messages) const;
    long validateIsoTp(const FilterMessages& messages) const;
    std::vector<const Filter*> filtersOfType(j2534::ULong type) const;

    ChannelKind kind_;
    std::map<j2534::ULong, Filter> filters_;
    j2534::ULong nextId_ = 1;
};

}
