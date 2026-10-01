#pragma once
#include <QDateTime>
#include <chrono>
namespace vtrpon {
inline qint64 mediaTimecode100ns() {
    static const auto start = std::chrono::steady_clock::now();
    static const qint64 epoch = QDateTime::currentMSecsSinceEpoch() * 10000;
    return epoch + std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count() / 100;
}
}
