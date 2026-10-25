#include "controller_settings.h"
#include "audio_output_settings.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
QSettings settings() { return vtrpon::makeControllerSettings(); }
bool save(const QString &key, const QString &id) {
    auto store = settings();
    store.setValue(key, id); store.sync();
    return store.status() == QSettings::NoError;
}
}
namespace vtrpon {
bool savedUseDefaultAudioOutputWhenMissing() {
    return settings().value(QStringLiteral("audio/useDefaultWhenMissing"), false).toBool();
}
bool saveUseDefaultAudioOutputWhenMissing(bool enabled) {
    auto store = settings();
    store.setValue(QStringLiteral("audio/useDefaultWhenMissing"), enabled);
    store.sync();
    return store.status() == QSettings::NoError;
}
MissingAudioOutputs missingAudioOutputs(const QList<AudioOutputDevice> &devices,
    const QString &systemDefault, const QString &program, const QString &pfl) {
    const auto available = [&devices](const QString &id) {
        return !id.isEmpty() && std::any_of(devices.cbegin(), devices.cend(),
            [&id](const AudioOutputDevice &device) {
                return device.id.compare(id, Qt::CaseInsensitive) == 0;
            });
    };
    return {!available(program.isEmpty() ? systemDefault : program),
            !pfl.isEmpty() && !available(pfl == QLatin1StringView(kSystemDefaultPflDeviceId)
                ? systemDefault : pfl)};
}
QString savedProgramOutputDeviceId() { return settings().value(QStringLiteral("audio/programOutputDeviceId")).toString(); }
QString savedEditPflOutputDeviceId() { return settings().value(QStringLiteral("audio/editPflOutputDeviceId")).toString(); }
bool saveProgramOutputDeviceId(const QString &id) { return save(QStringLiteral("audio/programOutputDeviceId"), id); }
bool saveEditPflOutputDeviceId(const QString &id) { return save(QStringLiteral("audio/editPflOutputDeviceId"), id); }
bool sameAudioOutputDevice(const QString &program, const QString &pfl, const QString &systemDefault) {
    const auto resolved = program.isEmpty() ? systemDefault : program;
    const auto resolvedPfl = pfl == QLatin1StringView(kSystemDefaultPflDeviceId) ? systemDefault : pfl;
    return !pfl.isEmpty() && !resolved.isEmpty() && resolved.compare(resolvedPfl, Qt::CaseInsensitive) == 0;
}
}
AudioOutputDeviceMonitor::AudioOutputDeviceMonitor(QObject *parent) : QObject(parent) {
    process_ = new QProcess(this);
#ifdef _WIN32
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    process_->setStandardErrorFile(QProcess::nullDevice());
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    timeout_->setInterval(3000);
    connect(timeout_, &QTimer::timeout, process_, &QProcess::kill);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!shuttingDown_ && error == QProcess::FailedToStart) {
            timeout_->stop();
            emit refreshed(false);
        }
    });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { readResult(code); });
    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &AudioOutputDeviceMonitor::refresh);
    timer_->start();
    refresh();
}
AudioOutputDeviceMonitor::~AudioOutputDeviceMonitor() {
    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) process_->kill();
}
QList<AudioOutputDevice> AudioOutputDeviceMonitor::devices() const { return devices_; }
QString AudioOutputDeviceMonitor::defaultDeviceId() const { return defaultId_; }
void AudioOutputDeviceMonitor::setRefreshInterval(int intervalMs) {
    if (intervalMs <= 0) {
        timer_->stop();
        return;
    }
    timer_->setInterval(qMax(1000, intervalMs));
    timer_->start();
}
void AudioOutputDeviceMonitor::refresh() {
    if (shuttingDown_ || process_->state() != QProcess::NotRunning) return;
    timeout_->start();
    process_->start(QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("vtr-pon-pfl-output.exe")), {QStringLiteral("--list")});
}
void AudioOutputDeviceMonitor::readResult(int exitCode) {
    timeout_->stop();
    if (shuttingDown_) return;
    const auto result = QJsonDocument::fromJson(process_->readAllStandardOutput());
    if (exitCode != 0 || !result.isObject()
        || !result.object().value(QStringLiteral("devices")).isArray()) {
        emit refreshed(false);
        return;
    }
    const auto object = result.object();
    QList<AudioOutputDevice> devices;
    for (const auto &entry : object.value(QStringLiteral("devices")).toArray()) {
        const auto value = entry.toObject();
        const auto id = value.value(QStringLiteral("id")).toString();
        const auto name = value.value(QStringLiteral("name")).toString();
        if (!id.isEmpty() && !name.isEmpty()) devices.append({id, name});
    }
    const auto defaultId = object.value(QStringLiteral("default")).toString();
    if (!enumerated_ || devices != devices_ || defaultId != defaultId_) {
        enumerated_ = true;
        devices_ = devices; defaultId_ = defaultId;
        emit devicesChanged();
    }
    emit refreshed(true);
}

void AudioOutputDeviceMonitor::requestShutdown() {
    shuttingDown_ = true;
    timer_->stop();
    timeout_->stop();
    if (process_->state() != QProcess::NotRunning) process_->kill();
}
bool AudioOutputDeviceMonitor::shutdownComplete() const { return process_->state() == QProcess::NotRunning; }
