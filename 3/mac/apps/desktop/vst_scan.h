#pragma once

#include <QCoreApplication>
#include <QDebug>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace vtrpon {

// The worker owns filesystem traversal and every scanner process. It receives
// only values and publishes snapshots; cancellation/destruction never waits
// for filesystem or vendor code on the Controller thread.
class VstScan final {
public:
    struct Snapshot {
        QJsonArray catalog;
        QString currentPath, error;
        int checked = 0, total = 0, skipped = 0;
        bool discovering = true, done = false;
        bool operator==(const Snapshot &) const = default;
    };
    static std::shared_ptr<VstScan> start(QString helper, QStringList roots, qint64 parentPid) {
        auto state = std::make_shared<VstScan>();
        try {
            std::thread([state, helper = std::move(helper), roots = std::move(roots), parentPid] {
                try { state->run(helper, roots, parentPid); }
                catch (...) {
                    std::lock_guard lock(state->mutex_);
                    state->snapshot_.error = QStringLiteral("The VST scan worker failed.");
                }
                std::lock_guard lock(state->mutex_);
                state->snapshot_.done = true;
            }).detach();
        } catch (...) {
            state->snapshot_.error = QStringLiteral("Could not start the VST scan worker.");
            state->snapshot_.done = true;
        }
        return state;
    }
    void cancel() { canceled_.store(true); }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

private:
    void run(const QString &helper, const QStringList &roots, qint64 parentPid) {
        QStringList directories = roots, paths;
        while (!directories.isEmpty() && !canceled_) {
            const QString directory = directories.takeLast();
            QDirIterator entries(directory, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
            while (!canceled_ && entries.hasNext()) {
                const QString path = entries.next();
                const auto info = entries.fileInfo();
                if (path.endsWith(QStringLiteral(".vst3"), Qt::CaseInsensitive)) {
                    paths.append(path); // A bundle is one module; do not walk its assets/binaries.
                } else if (info.isDir() && !info.isSymLink()) {
                    directories.append(path);
                }
            }
        }
        if (canceled_) return;
        paths.removeDuplicates();
        paths.sort(Qt::CaseInsensitive);
        {
            std::lock_guard lock(mutex_);
            snapshot_.discovering = false;
            snapshot_.total = paths.size();
        }
        QTemporaryDir temporary;
        if (!temporary.isValid()) {
            std::lock_guard lock(mutex_);
            snapshot_.error = temporary.errorString();
            return;
        }
        for (const auto &path : paths) {
            if (canceled_) return;
            {
                std::lock_guard lock(mutex_);
                snapshot_.currentPath = path;
            }
            qInfo().noquote() << "VST_SCAN_BEGIN" << path;
            QProcess process;
            process.setStandardOutputFile(QProcess::nullDevice());
            process.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
            process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
                args->flags |= CREATE_NO_WINDOW;
            });
#endif
            const QString result = temporary.filePath(QStringLiteral("scan.json"));
            QElapsedTimer deadline;
            deadline.start();
            process.start(helper, {QStringLiteral("--scan"), path, QStringLiteral("--result"), result,
                QStringLiteral("--parent"), QString::number(parentPid)});
            const bool started = process.waitForStarted(5000);
            while (started && process.state() != QProcess::NotRunning &&
                   !canceled_ && deadline.elapsed() < 20000) {
                process.waitForFinished(50);
            }
            const bool finished = started && process.state() == QProcess::NotRunning;
            if (!finished && process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(3000);
            }
            if (canceled_) return;
            QJsonArray found;
            QFile file(result);
            bool valid = finished && process.exitStatus() == QProcess::NormalExit &&
                         process.exitCode() == 0 && file.size() < 8 * 1024 * 1024 &&
                         file.open(QIODevice::ReadOnly);
            if (valid) {
                QJsonParseError error;
                const auto document = QJsonDocument::fromJson(file.readAll(), &error);
                valid = error.error == QJsonParseError::NoError && document.isArray();
                for (const auto &value : document.array()) {
                    auto item = value.toObject();
                    if (item.value("uid").toString().size() != 32 || item.value("name").toString().isEmpty()) continue;
                    item.insert("path", path);
                    found.append(item);
                }
            }
            file.close();
            QFile::remove(result);
            qInfo().noquote() << "VST_SCAN_END" << path << "valid=" << valid
                             << "elapsedMs=" << deadline.elapsed();
            {
                std::lock_guard lock(mutex_);
                for (const auto &item : found) snapshot_.catalog.append(item);
                ++snapshot_.checked;
                if (!valid) ++snapshot_.skipped;
            }
            if (!started) {
                std::lock_guard lock(mutex_);
                snapshot_.error = process.errorString();
                return;
            }
        }
    }
    std::atomic_bool canceled_{false};
    mutable std::mutex mutex_;
    Snapshot snapshot_;
};

} // namespace vtrpon
