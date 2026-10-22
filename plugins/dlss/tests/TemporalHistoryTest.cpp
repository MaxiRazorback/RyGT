// SPDX-License-Identifier: MIT
#include "../TemporalHistory.hpp"
#include <stdexcept>

static void Check(bool value, const char* description) {
    if (!value) throw std::runtime_error(description);
}

int main() {
    RygtDlss::TemporalHistory history;
    Check(history.NeedsReset(0), "first frame must reset");
    history.Commit(0);
    Check(!history.NeedsReset(1), "consecutive frame must retain history");
    Check(history.NeedsReset(1, true), "explicit camera cut must reset");
    Check(history.NeedsReset(0), "duplicate frame must reset");
    Check(history.NeedsReset(2), "skipped guest frame must reset");
    history.Commit(8);
    Check(history.NeedsReset(7), "rewound timeline must reset");
    history.Invalidate();
    Check(history.NeedsReset(9), "failed evaluation or recreated feature must reset");
    history.Commit(9);
    Check(!history.NeedsReset(10), "successful recovery must retain consecutive history");
    history.Commit(UINT64_MAX);
    Check(history.NeedsReset(0), "frame overflow must reset");
}
