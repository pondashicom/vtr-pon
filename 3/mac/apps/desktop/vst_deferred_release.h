#pragma once
#include <memory>
#include <utility>
#include <vector>

namespace vtrpon::vst {
// Used only by the owning UI thread, after removing the published pointer.
// Audio callbacks may still hold a reference, including before taking their
// try-lock. Keep an owner until those callbacks have released it, so unmapping
// shared memory and closing descriptors never falls to the last audio reader.
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
