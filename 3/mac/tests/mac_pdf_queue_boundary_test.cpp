#include "background_io_queue.h"
#include "controller_settings.h"
#include "document_import_queue.h"
#include "pdf_boundary_fixtures.h"
#include "pdf_import_service.h"
#include "playlist_model.h"
#include "user_info.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <functional>
#include <atomic>
#include <sys/stat.h>

namespace {
bool check(bool condition, const char *label) {
    qInfo() << (condition ? "PASS" : "FAIL") << label;
    return condition;
}
bool waitFor(const std::function<bool()> &condition, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents();
        if (condition()) return true;
        QThread::msleep(2);
    } while (timer.elapsed() < timeout);
    return false;
}
struct RestoreMode {
    QString path;
    mode_t original;
    explicit RestoreMode(const QString &value) : path(value) {
        struct stat info{};
        original = ::stat(QFile::encodeName(path).constData(), &info) == 0 ? (info.st_mode & 0777) : 0700;
    }
    ~RestoreMode() { ::chmod(QFile::encodeName(path).constData(), original); }
};
struct QueueFixture {
    PlaylistModel model;
    UserInfoCenter info;
    BackgroundIoQueue io;
    DocumentImportQueue queue{&model, nullptr, &info, &io};
    ~QueueFixture() {
        queue.requestShutdown();
        waitFor([this] { return queue.shutdownComplete(); });
        io.requestShutdown();
        waitFor([this] { return io.shutdownComplete(); });
    }
};

bool cancellationCleanupFailure(const QString &path, const QString &directory,
                                bool removePlaceholder, bool closeCleanupQueue = false) {
    const QString destination = QDir(directory).filePath(
        closeCleanupQueue ? "shutdown-cleanup" : (removePlaceholder ? "remove-item" : "retain-item"));
    const QString root = QDir(destination).filePath("PDF");
    if (!QDir().mkpath(root)) return false;
    RestoreMode restore{root};
    QFile sentinel(QDir(root).filePath("keep.txt"));
    if (!sentinel.open(QIODevice::WriteOnly) || sentinel.write("keep") != 4) return false;
    sentinel.close();
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue("files/documentConversionDirectory", destination);
    settings.sync();
    QueueFixture fixture;
    QString residual, warningDetail;
    bool cancelSent = false, denied = false;
    std::atomic_bool wrongThread{false};
    int ticks = 0;
    QObject observer; // Disconnect callbacks before their captured state is destroyed.
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start();
    QObject::connect(&fixture.info, &UserInfoCenter::messageChanged, &observer,
        [&](UserInfoSeverity severity, const QString &, const QString &, const QString &detail) {
            if (QThread::currentThread() != QApplication::instance()->thread()) {
                wrongThread.store(true);
                return;
            }
            if (severity == UserInfoSeverity::Warning && !residual.isEmpty() &&
                detail.contains(residual)) warningDetail = detail;
        }, Qt::DirectConnection);
    QObject::connect(&fixture.model, &PlaylistModel::itemConversionStateChanged, &observer,
        [&](quint64 id) {
            const auto state = fixture.model.itemConversionState(id);
            if (!state || state->stage != PlaylistItemConversionStage::Encoding || cancelSent) return;
            const auto entries = QDir(root).entryList({"*.staging"}, QDir::Dirs | QDir::NoDotAndDotDot);
            if (entries.size() != 1) return;
            residual = QDir(root).filePath(entries.first());
            denied = ::chmod(QFile::encodeName(root).constData(), 0500) == 0;
            QFile probe(QDir(root).filePath("permission-probe"));
            denied = !probe.open(QIODevice::WriteOnly | QIODevice::NewOnly) && denied;
            if (probe.isOpen()) { probe.close(); probe.remove(); }
            cancelSent = true;
            if (closeCleanupQueue) fixture.io.requestShutdown();
            fixture.queue.cancel(id, removePlaceholder);
        });
    PdfAddSettings video;
    video.method = PdfAddMethod::VideoConversion;
    video.pageDurationSeconds = 30;
    video.crossDissolveEnabled = false;
    QString error;
    bool ok = check(fixture.queue.enqueuePdf(path, video, 1, &error),
                    "queue accepts conversion for cleanup-failure cancellation");
    fixture.queue.startNext();
    ok &= check(waitFor([&] { return !fixture.queue.hasPendingImports(); }) && cancelSent && denied,
                "active cancellation completes after real output-directory permission failure");
    fixture.io.requestShutdown();
    ok &= check(waitFor([&] { return fixture.io.shutdownComplete(); }),
                "asynchronous cleanup finishes despite denied directory removal");
    ok &= check(waitFor([&] { return !warningDetail.isEmpty(); }, 500) && !wrongThread.load(),
                "cleanup failure reports residual path through Info-bar even after item removal");
    ok &= check(ticks > 5 && fixture.model.itemCount() == 0 && QDir(residual).exists(),
                "failed cleanup leaves no stale item and GUI heartbeat continues");
    ok &= check(sentinel.open(QIODevice::ReadOnly) && sentinel.readAll() == "keep",
                "queue cancellation preserves unrelated output");
    sentinel.close();
    const bool restored = ::chmod(QFile::encodeName(root).constData(), 0700) == 0;
    ok &= check(restored && PdfImportService::discardGeneratedArtifacts(residual, root) &&
                !QDir(residual).exists() && QFile::exists(sentinel.fileName()),
                "restoring permissions allows residual-only cleanup");
    return ok;
}
bool rejectedJobsRecover(const QString &path, const QString &directory) {
    const QString root = QDir(directory).filePath("rejected-queue");
    auto settings = vtrpon::makeControllerSettings();
    settings.setValue("files/documentConversionDirectory", root);
    settings.sync();
    const QString overLimit = QDir(directory).filePath("queue-1001.pdf");
    if (!PdfBoundaryFixtures::writePdf(overLimit, 1001)) return false;
    QueueFixture fixture;
    fixture.info.setDisplayDuration(0);
    int warnings = 0, ticks = 0;
    std::atomic_bool wrongThread{false};
    QObject observer; // Also protects timeout/failure teardown while fixture drains events.
    QObject::connect(&fixture.info, &UserInfoCenter::messageChanged, &observer,
        [&](UserInfoSeverity severity, const QString &, const QString &, const QString &detail) {
            if (QThread::currentThread() != QApplication::instance()->thread()) {
                wrongThread.store(true);
                return;
            }
            if (severity == UserInfoSeverity::Warning && !detail.isEmpty()) ++warnings;
        }, Qt::DirectConnection);
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start();
    PdfAddSettings native;
    QString error;
    bool ok = check(fixture.queue.enqueuePdf(path, native, 1, &error),
                    "queue accepts a valid PDF ahead of rejected jobs");
    const auto first = fixture.model.itemIdAt(1, 0);
    for (const auto method : {PdfAddMethod::Native, PdfAddMethod::PngSequence,
                              PdfAddMethod::VideoConversion}) {
        PdfAddSettings add;
        add.method = method;
        for (const auto &source : {PdfBoundaryFixtures::encryptedFixturePath(), overLimit})
            ok &= check(fixture.queue.enqueuePdf(source, add, 1, &error),
                        "invalid document reaches asynchronous worker validation");
    }
    ok &= check(fixture.queue.enqueuePdf(path, native, 1, &error),
                "valid PDF queues behind failed documents");
    const auto last = fixture.model.itemIdAt(1, 7);
    fixture.queue.startNext();
    ok &= check(waitFor([&] { return !fixture.queue.hasPendingImports(); }) &&
                fixture.model.itemCount(1) == 2 && fixture.model.itemIdAt(1, 0) == first &&
                fixture.model.itemIdAt(1, 1) == last,
                "encrypted and over-limit failures remove only failed placeholders and continue queue");
    ok &= check(waitFor([&] { return warnings == 6; }, 1000) && ticks > 0 && !wrongThread.load(),
                "all six failures notify on GUI thread while event loop remains responsive");
    ok &= check(!QDir(QDir(root).filePath("PDF")).exists(),
                "worker rejection creates no generated output root");
    return ok;
}
}

bool runPdfQueueBoundaryChecks(const QString &directory) {
    const QString path = QDir(directory).filePath("queue-boundary.pdf");
    if (!PdfBoundaryFixtures::writePdf(path, 2)) return false;
    bool ok = rejectedJobsRecover(path, directory);
    ok = cancellationCleanupFailure(path, directory, true) && ok;
    ok = cancellationCleanupFailure(path, directory, false) && ok;
    ok = cancellationCleanupFailure(path, directory, true, true) && ok;
    return ok;
}
