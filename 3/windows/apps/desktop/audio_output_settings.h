#pragma once
#include <QObject>
#include <QList>
#include <QString>

class QProcess;
class QTimer;
struct AudioOutputDevice {
    QString id;
    QString name;
    bool operator==(const AudioOutputDevice &) const = default;
};
namespace vtrpon {
inline constexpr auto kSystemDefaultPflDeviceId = "@system-default";
bool savedUseDefaultAudioOutputWhenMissing();
bool saveUseDefaultAudioOutputWhenMissing(bool enabled);
struct MissingAudioOutputs {
    bool program = false;
    bool pfl = false;
};
MissingAudioOutputs missingAudioOutputs(const QList<AudioOutputDevice> &devices,
    const QString &systemDefault, const QString &program, const QString &pfl);
QString savedProgramOutputDeviceId(); // Empty means system default.
QString savedEditPflOutputDeviceId(); // Empty means no device.
bool saveProgramOutputDeviceId(const QString &id);
bool saveEditPflOutputDeviceId(const QString &id);
bool sameAudioOutputDevice(const QString &program, const QString &pfl,
                           const QString &systemDefault);
}

// Enumeration happens in a disposable child, with no device activation in the
// Controller and no dependency on either player's audio pipeline.
class AudioOutputDeviceMonitor final : public QObject {
    Q_OBJECT
public:
    explicit AudioOutputDeviceMonitor(QObject *parent = nullptr);
    ~AudioOutputDeviceMonitor() override;
    QList<AudioOutputDevice> devices() const;
    QString defaultDeviceId() const;
    void setRefreshInterval(int intervalMs); // <= 0: externally scheduled checks.
    void refresh();
    void requestShutdown();
    bool shutdownComplete() const;
signals:
    void devicesChanged();
    void refreshed(bool success);
private:
    void readResult(int exitCode);
    QProcess *process_ = nullptr;
    QTimer *timer_ = nullptr;
    QTimer *timeout_ = nullptr;
    QList<AudioOutputDevice> devices_;
    QString defaultId_;
    bool enumerated_ = false;
    bool shuttingDown_ = false;
};
