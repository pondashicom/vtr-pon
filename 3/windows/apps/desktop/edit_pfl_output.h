#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include <cstdint>

namespace vtrpon::pfl { struct SharedAudio; }

// Shared with the EDIT tap only. Contains no QObject or Program reference.
class EditPflFeed final {
public:
    EditPflFeed();
    ~EditPflFeed();
    EditPflFeed(const EditPflFeed &) = delete;
    EditPflFeed &operator=(const EditPflFeed &) = delete;
    bool available() const;
    QString mappingName() const;
    vtrpon::pfl::SharedAudio *audio() const;
    void setPlaying(bool playing);
    void flush();
    static std::int64_t nowNs();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class EditPflOutput final : public QObject {
    Q_OBJECT
public:
    explicit EditPflOutput(std::shared_ptr<EditPflFeed> feed, QObject *parent = nullptr);
    ~EditPflOutput() override;
    std::shared_ptr<EditPflFeed> feed() const;
    void setDeviceId(const QString &id);
    void setEnabled(bool enabled);
    void shutdown();
    bool shutdownComplete() const;
    bool enabled() const;
    QString deviceId() const;
    bool canEnable() const;
    QString statusText() const;
signals:
    void stateChanged();
    void outputError(const QString &message);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
