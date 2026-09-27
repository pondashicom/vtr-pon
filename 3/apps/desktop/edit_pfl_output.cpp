#include "edit_pfl_output.h"
#include "edit_pfl_protocol.h"
#include "language_text.h"
#include <QDebug>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <new>
#ifdef _WIN32
#include <windows.h>
#endif

class EditPflFeed::Impl {
public:
    QString name;
    vtrpon::pfl::SharedAudio *audio = nullptr;
#ifdef _WIN32
    HANDLE mapping = nullptr;
#endif
};
EditPflFeed::EditPflFeed() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    impl_->name = QStringLiteral("Local\\VTRPON-EDIT-PFL-%1")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    impl_->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(vtrpon::pfl::SharedAudio),
        reinterpret_cast<LPCWSTR>(impl_->name.utf16()));
    if (impl_->mapping) {
        void *memory = MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                    sizeof(vtrpon::pfl::SharedAudio));
        if (memory) impl_->audio = new (memory) vtrpon::pfl::SharedAudio;
    }
#endif
}
EditPflFeed::~EditPflFeed() {
#ifdef _WIN32
    if (impl_->audio) UnmapViewOfFile(impl_->audio);
    if (impl_->mapping) CloseHandle(impl_->mapping);
#endif
}
bool EditPflFeed::available() const { return impl_->audio != nullptr; }
QString EditPflFeed::mappingName() const { return impl_->name; }
vtrpon::pfl::SharedAudio *EditPflFeed::audio() const { return impl_->audio; }
void EditPflFeed::flush() {
    if (impl_->audio) impl_->audio->generation.fetch_add(1);
}
void EditPflFeed::setPlaying(bool playing) {
    if (impl_->audio && impl_->audio->playing.exchange(playing ? 1U : 0U)
                            != (playing ? 1U : 0U)) flush();
}
std::int64_t EditPflFeed::nowNs() {
#ifdef _WIN32
    static const auto frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (counter.QuadPart / frequency) * 1000000000LL
        + (counter.QuadPart % frequency) * 1000000000LL / frequency;
#else
    return 0;
#endif
}

class EditPflOutput::Impl {
public:
    Impl(EditPflOutput *owner, std::shared_ptr<EditPflFeed> source) : q(owner), feed(std::move(source)) {
        process = new QProcess(owner);
#ifdef _WIN32
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.BasicLimitInformation.ActiveProcessLimit = 1;
        limits.ProcessMemoryLimit = 256ULL * 1024 * 1024;
        if (job && !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            CloseHandle(job); job = nullptr;
        }
        process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
        QObject::connect(process, &QProcess::started, owner, [this] {
            HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                       static_cast<DWORD>(process->processId()));
            const bool assigned = child && job && AssignProcessToJobObject(job, child);
            if (child) CloseHandle(child);
            if (!assigned) { fail("edit.pfl.failed"); stop(false); process->kill(); }
        });
#endif
        QObject::connect(
            process, &QProcess::readyReadStandardOutput, owner, [this] {
              qInfo().noquote()
                  << "PFL stdout"
                  << QString::fromUtf8(process->readAllStandardOutput());
            });
        QObject::connect(
            process, &QProcess::readyReadStandardError, owner, [this] {
              qWarning().noquote()
                  << "PFL stderr"
                  << QString::fromUtf8(process->readAllStandardError());
            });
        QObject::connect(process, &QProcess::finished, owner,
                         [this](int exitCode, QProcess::ExitStatus exitStatus) {
                           qInfo() << "PFL process finished" << exitCode
                                   << exitStatus;
                           const bool expected = stopping;
                           stopping = false;
                           if (!expected && !closed)
                             fail("edit.pfl.failed");
                           if (!closed && restart) {
                             restart = false;
                             start();
                           } else if (!closed && expected)
                             emit q->stateChanged();
                         });
        QObject::connect(process, &QProcess::errorOccurred, owner, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && !closed) fail("edit.pfl.failed");
        });
        timer = new QTimer(owner);
        timer->setInterval(100);
        QObject::connect(timer, &QTimer::timeout, owner, [this] { poll(); });
        timer->start();
    }
    void fail(const char *key) {
        requested = false;
        ready = false;
        statusKey = key;
        if (auto *a = feed->audio()) { a->enabled = 0; feed->flush(); }
        emit q->stateChanged();
        emit q->outputError(vtrpon::uiText(key));
    }
    void start() {
        if (closed || device.isEmpty() || !requested || !feed->available()) return;
        if (process->state() != QProcess::NotRunning) return;
        const auto helper = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("vtr-pon-pfl-output.exe"));
        if (!QFileInfo::exists(helper)) { fail("edit.pfl.helper_missing"); return; }
        auto *a = feed->audio();
        // The previous reader is gone. Only it owns read while running.
        a->read = a->write.load();
        a->quit = 0;
        a->enabled = 0;
        a->state = static_cast<std::uint32_t>(vtrpon::pfl::State::Opening);
        a->error = 0;
        a->heartbeatNs = 0;
        feed->flush();
        startedNs = EditPflFeed::nowNs();
        ready = false;
        statusKey = "edit.pfl.opening";
        qInfo() << "PFL starting device=" << device;
        process->start(helper, {feed->mappingName(), device,
            QString::number(QCoreApplication::applicationPid())});
        emit q->stateChanged();
    }
    void stop(bool startAgain) {
        restart = startAgain;
        ready = false;
        if (auto *a = feed->audio()) { a->enabled = 0; a->quit = 1; feed->flush(); }
        if (process->state() != QProcess::NotRunning) {
            stopping = true;
            stopNs = EditPflFeed::nowNs();
        } else if (restart) { restart = false; start(); }
    }
    void poll() {
        if (closed) return;
        auto *a = feed->audio();
        if (!a || process->state() == QProcess::NotRunning) return;
        const auto now = EditPflFeed::nowNs();
        if (stopping) {
            // Kill only this private PFL child, asynchronously. Never wait on
            // its driver or call any Program/EDIT pipeline state function.
            if (now - stopNs > 1500000000LL) process->kill();
            return;
        }
        const auto state = static_cast<vtrpon::pfl::State>(a->state.load());
        if (state == vtrpon::pfl::State::Failed) {
          qWarning() << "PFL failed HRESULT=" << Qt::hex << a->error.load()
                     << "device=" << device;
          fail("edit.pfl.failed");
          stop(false);
          return;
        }
        const auto heartbeat = a->heartbeatNs.load();
        if ((state == vtrpon::pfl::State::Opening && now - startedNs > 5000000000LL)
            || (state != vtrpon::pfl::State::Opening
                && now - (heartbeat != 0 ? heartbeat : startedNs) > 2000000000LL)) {
            fail("edit.pfl.failed"); stop(false); return;
        }
        if (state == vtrpon::pfl::State::Ready && !ready) {
          qInfo() << "PFL ready device=" << device;
          ready = true;
          a->enabled = requested ? 1U : 0U;
          statusKey = requested ? "edit.pfl.on" : "edit.pfl.off";
          emit q->stateChanged();
        }
    }
    EditPflOutput *q;
    std::shared_ptr<EditPflFeed> feed;
    QProcess *process = nullptr;
    QTimer *timer = nullptr;
    QString device;
    const char *statusKey = "edit.pfl.no_device";
    bool requested = false, ready = false, stopping = false, restart = false, closed = false;
    std::int64_t startedNs = 0, stopNs = 0;
#ifdef _WIN32
    HANDLE job = nullptr;
#endif
};
EditPflOutput::EditPflOutput(std::shared_ptr<EditPflFeed> feed, QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, std::move(feed))) {}
EditPflOutput::~EditPflOutput() {
    shutdown();
    // Disconnect lambdas before Impl dies (QProcess is a QObject child).
    impl_->process->disconnect(this);
    impl_->timer->disconnect(this);
#ifdef _WIN32
    if (impl_->job) CloseHandle(impl_->job);
#endif
}
std::shared_ptr<EditPflFeed> EditPflOutput::feed() const { return impl_->feed; }
bool EditPflOutput::enabled() const { return impl_->requested && impl_->ready && !impl_->stopping; }
QString EditPflOutput::deviceId() const { return impl_->device; }
bool EditPflOutput::canEnable() const {
    return !impl_->closed && impl_->feed->available() && !impl_->device.isEmpty()
        && !impl_->stopping && !(impl_->requested && !impl_->ready
                               && impl_->process->state() != QProcess::NotRunning);
}
QString EditPflOutput::statusText() const { return vtrpon::uiText(impl_->statusKey); }
void EditPflOutput::setDeviceId(const QString &id) {
    if (impl_->closed || id == impl_->device) return;
    impl_->device = id;
    if (id.isEmpty()) impl_->requested = false;
    impl_->statusKey = id.isEmpty() ? "edit.pfl.no_device" : "edit.pfl.off";
    impl_->stop(impl_->requested);
    emit stateChanged();
}
void EditPflOutput::setEnabled(bool value) {
    if (impl_->closed) return;
    if (value && impl_->stopping && !impl_->device.isEmpty() && impl_->feed->available()) {
        impl_->requested = true;
        impl_->restart = true;
        emit stateChanged();
        return;
    }
    impl_->requested = value && canEnable();
    impl_->feed->flush();
    if (auto *a = impl_->feed->audio()) a->enabled = enabled() ? 1U : 0U;
    if (impl_->requested && !impl_->ready && !impl_->stopping) impl_->start();
    else {
        impl_->statusKey = !canEnable() ? "edit.pfl.no_device"
            : enabled() ? "edit.pfl.on" : "edit.pfl.off";
        emit stateChanged();
    }
}
void EditPflOutput::shutdown() {
    if (impl_->closed) return;
    impl_->closed = true;
    impl_->requested = false;
    impl_->stop(false);
    impl_->timer->stop();
    // No driver teardown runs in the Controller. QProcess::kill addresses
    // exactly this child; the child also exits if its parent handle signals.
    if (impl_->process->state() != QProcess::NotRunning) impl_->process->kill();
}

bool EditPflOutput::shutdownComplete() const {
    return impl_->closed && impl_->process->state() == QProcess::NotRunning;
}
