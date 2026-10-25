#pragma once

#include <QImage>
#include <mutex>
#include <utility>

// One latest image and at most one queued wakeup per UVC session.
class UvcThumbnailMailbox final {
public:
    bool publish(QImage image) {
        std::scoped_lock lock(mutex_);
        latest_ = std::move(image);
        return !std::exchange(pending_, true);
    }

    QImage consume() {
        std::scoped_lock lock(mutex_);
        pending_ = false;
        return latest_;
    }

    QImage latest() const {
        std::scoped_lock lock(mutex_);
        return latest_;
    }

private:
    mutable std::mutex mutex_;
    QImage latest_;
    bool pending_ = false;
};
