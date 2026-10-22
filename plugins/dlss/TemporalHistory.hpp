// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <limits>

namespace RygtDlss {

class TemporalHistory {
public:
    bool NeedsReset(uint64_t frame, bool requested = false) const {
        return requested || !valid || last == std::numeric_limits<uint64_t>::max() || frame != last + 1;
    }
    void Commit(uint64_t frame) { last = frame; valid = true; }
    void Invalidate() { valid = false; }

private:
    uint64_t last{};
    bool valid{};
};

} // namespace RygtDlss
