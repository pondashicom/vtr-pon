#pragma once
#include <memory>
#include <utility>
#include <vector>

namespace vtrpon::vst {
// UI-thread only, after removing the published pointer. A reader may have
// loaded a shared_ptr but not yet acquired its processing lock.
template<class T> class DeferredRelease {
public:
    void retire(std::shared_ptr<T> &object) {
        if (object) pending_.push_back(std::move(object));
    }
    void collect() {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->use_count() == 1) it = pending_.erase(it);
            else ++it;
        }
    }
    bool empty() const { return pending_.empty(); }
private:
    std::vector<std::shared_ptr<T>> pending_;
};
} // namespace vtrpon::vst
