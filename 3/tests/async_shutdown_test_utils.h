#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

// During terminal teardown only pump Qt and the completion API. Playback bus
// polling would touch backend state now owned by the shutdown worker.
template <typename Predicate>
bool awaitShutdown(Predicate completed, int timeoutMs = 30000)
{
    QElapsedTimer deadline;
    deadline.start();
    do {
        QCoreApplication::processEvents();
        if (completed()) return true;
        QThread::msleep(5);
    } while (deadline.elapsed() < timeoutMs);
    return false;
}
