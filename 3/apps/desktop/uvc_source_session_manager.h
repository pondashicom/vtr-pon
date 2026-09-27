#pragma once

#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>

#include <memory>

typedef struct _GstContext GstContext;
typedef struct _GstElement GstElement;

class UvcSourceSessionManager final : public QObject
{
    Q_OBJECT

public:
    struct ProgramSource {
        GstElement *pipeline = nullptr;
        GstElement *appSink = nullptr;

        bool isValid() const { return pipeline && appSink; }
    };

    explicit UvcSourceSessionManager(bool use4kProgramOutput,
                                     QObject *parent = nullptr);
    ~UvcSourceSessionManager() override;

    UvcSourceSessionManager(const UvcSourceSessionManager &) = delete;
    UvcSourceSessionManager &operator=(const UvcSourceSessionManager &) = delete;

    void setProgramContext(GstContext *context);
    bool ensureSource(const QString &devicePath,
                      QString *errorDetail = nullptr);
    bool bindProgramDeck(int deckIndex, const QString &devicePath,
                         ProgramSource &source,
                         QString *errorDetail = nullptr);
    void releaseProgramDeck(int deckIndex);
    void setPreviewDevicePaths(const QSet<QString> &devicePaths);
    void setEditPreviewPath(const QString &devicePath);
    void setRegisteredDevicePaths(const QSet<QString> &devicePaths);
    void setAvailableDevicePaths(const QSet<QString> &devicePaths);
    bool sourceAvailable(const QString &devicePath) const;
    // Initial connection is neither frame-confirmed nor offline.
    bool sourceOffline(const QString &devicePath) const;
    void reportSourceFailure(const QString &devicePath, const QString &detail);
    QImage latestThumbnail(const QString &devicePath) const;
    bool sourceIsPlaying(const QString &devicePath) const;
    bool sourceIsRetiring(const QString &devicePath) const;
    bool sourceErrorAfter(const QString &devicePath,
                          quint64 previousGeneration,
                          quint64 &currentGeneration,
                          QString &errorDetail) const;
    void shutdown();
    void requestShutdown();
    bool shutdownComplete() const;

signals:
    void thumbnailReady(const QString &devicePath, const QImage &image);
    void editPreviewReady(const QString &devicePath, const QImage &image);
    void sourceError(const QString &devicePath, const QString &detail);
    void sourceAvailabilityChanged(const QString &devicePath, bool available);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
