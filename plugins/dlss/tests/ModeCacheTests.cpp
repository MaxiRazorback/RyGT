// SPDX-License-Identifier: MIT

#include "../ModeCache.hpp"

#include <cstdio>
#include <stdexcept>

namespace {

enum class Mode { Quality, Balanced, Performance };

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void StableResolutionDoesNotQueryAgain() {
    RygtDlss::ModeCache<Mode> cache;
    const RygtDlss::Resolution resolution{1920, 1080, 2560, 1440};
    int queries = 0;
    const auto query = [&]() {
        ++queries;
        return std::optional<Mode>{Mode::Quality};
    };
    for (int frame = 0; frame < 10000; ++frame) {
        Check(cache.Get(resolution, query) == Mode::Quality, "cached mode changed");
    }
    Check(queries == 1, "stable resolution repeated the NGX query path");
}

void EveryDimensionIsPartOfTheKey() {
    const RygtDlss::Resolution original{1920, 1080, 2560, 1440};
    const RygtDlss::Resolution changes[] = {
        {1280, 1080, 2560, 1440},
        {1920, 720, 2560, 1440},
        {1920, 1080, 3840, 1440},
        {1920, 1080, 2560, 2160},
    };
    for (const auto& changed : changes) {
        RygtDlss::ModeCache<Mode> cache;
        int queries = 0;
        const auto query = [&]() {
            return std::optional<Mode>{queries++ == 0 ? Mode::Quality : Mode::Performance};
        };
        Check(cache.Get(original, query) == Mode::Quality, "initial mode was not selected");
        Check(cache.Get(changed, query) == Mode::Performance, "resize reused a stale mode");
        Check(cache.Get(changed, query) == Mode::Performance, "resized mode was not cached");
        Check(queries == 2, "resize did not query exactly once");
    }
}

void FailedQueryCanRecover() {
    RygtDlss::ModeCache<Mode> cache;
    const RygtDlss::Resolution resolution{1920, 1080, 3840, 2160};
    int queries = 0;
    const auto query = [&]() -> std::optional<Mode> {
        if (++queries < 3) {
            return std::nullopt;
        }
        return Mode::Performance;
    };
    Check(!cache.Get(resolution, query), "failed query reported support");
    Check(!cache.Get(resolution, query), "failed query was cached as supported");
    Check(cache.Get(resolution, query) == Mode::Performance, "failed query could not recover");
    Check(cache.Get(resolution, query) == Mode::Performance, "recovered result was not cached");
    Check(queries == 3, "failure was cached or recovery kept querying");
}

void UnsupportedResizePreservesThePreviousResolution() {
    RygtDlss::ModeCache<Mode> cache;
    const RygtDlss::Resolution supported{1920, 1080, 2560, 1440};
    const RygtDlss::Resolution unsupported{1920, 1080, 1280, 720};
    cache.Get(supported, []() { return std::optional<Mode>{Mode::Balanced}; });
    Check(!cache.Get(unsupported, []() { return std::optional<Mode>{}; }),
          "unsupported resolution reused the previous result");
    int queries = 0;
    Check(cache.Get(supported, [&]() {
        ++queries;
        return std::optional<Mode>{Mode::Performance};
    }) == Mode::Balanced,
          "unsupported resize changed the previous mode");
    Check(queries == 0, "previous successful selection was queried again");
}

void NewDeviceSessionQueriesAgain() {
    RygtDlss::ModeCache<Mode> cache;
    const RygtDlss::Resolution resolution{1920, 1080, 2560, 1440};
    cache.Get(resolution, []() { return std::optional<Mode>{Mode::Quality}; });
    cache.Reset();
    int queries = 0;
    const auto query = [&]() {
        ++queries;
        return std::optional<Mode>{Mode::Balanced};
    };
    Check(cache.Get(resolution, query) == Mode::Balanced, "new device reused a stale selection");
    Check(cache.Get(resolution, query) == Mode::Balanced, "new device result was not cached");
    Check(queries == 1, "device reset did not invalidate the selection");
    cache.Reset();
    cache.Reset();
}

} // namespace

int main() {
    try {
        StableResolutionDoesNotQueryAgain();
        EveryDimensionIsPartOfTheKey();
        FailedQueryCanRecover();
        UnsupportedResizePreservesThePreviousResolution();
        NewDeviceSessionQueriesAgain();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("DLSS mode cache: 5 regression scenarios passed");
    return 0;
}
