// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace RygtDlss {

struct Resolution {
    std::uint32_t inputWidth;
    std::uint32_t inputHeight;
    std::uint32_t outputWidth;
    std::uint32_t outputHeight;

    bool operator==(const Resolution& other) const {
        return inputWidth == other.inputWidth && inputHeight == other.inputHeight &&
               outputWidth == other.outputWidth && outputHeight == other.outputHeight;
    }
};

template <typename TMode>
class ModeCache {
public:
    template <typename TQuery>
    std::optional<TMode> Get(const Resolution& resolution, TQuery&& query) {
        if (entry && entry->resolution == resolution) {
            return entry->mode;
        }
        const auto mode = std::forward<TQuery>(query)();
        if (mode) {
            entry = Entry{resolution, *mode};
        }
        return mode;
    }

    void Reset() {
        entry.reset();
    }

private:
    struct Entry {
        Resolution resolution;
        TMode mode;
    };

    std::optional<Entry> entry;
};

} // namespace RygtDlss
