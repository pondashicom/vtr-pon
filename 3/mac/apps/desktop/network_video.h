#pragma once
#include "network_video_source.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <array>
#include <memory>

typedef struct _GstElement GstElement;
typedef struct _GstBuffer GstBuffer;
struct ID3D11RenderTargetView;

namespace vtrpon {
enum class NetworkVideoKind { Composite, Fill, Key, Alpha };
constexpr unsigned networkVideoBit(NetworkVideoKind kind) { return 1u << unsigned(kind); }
struct NetworkVideoSettings {
    bool outputEnabled = false, inputEnabled = false;
    QString outputName;
    bool compositeEnabled = true, fillEnabled = false, keyEnabled = false, alphaEnabled = false;
    bool premultipliedFill = false;
    unsigned outputMask() const {
        return (compositeEnabled ? 1u : 0u) | (fillEnabled ? 2u : 0u)
            | (keyEnabled ? 4u : 0u) | (alphaEnabled ? 8u : 0u);
    }
    bool operator==(const NetworkVideoSettings &) const = default;
};
struct NetworkVideoSource {
    QString name, path;
    bool operator==(const NetworkVideoSource &) const = default;
};
struct NetworkVideoState {
    struct Output {
        QString name, error;
        bool ready = false, running = false;
        quint64 sentFrames = 0;
    };
    std::array<Output, 4> outputs;
    QList<NetworkVideoSource> sources;
    QString error;
    bool outputRunning = false, inputRunning = false;
    quint64 sentFrames = 0;
};
QString networkVideoOutputName(const NetworkVideoSettings &settings, NetworkVideoKind kind);
QString networkVideoAddonPath();
bool networkVideoAddonInstalled();
NetworkVideoSettings savedNetworkVideoSettings();
void applyNetworkVideoSettings(const NetworkVideoSettings &settings);
void saveNetworkVideoSettings(const NetworkVideoSettings &settings);
NetworkVideoState networkVideoState();
bool networkVideoInputAllowed(const QString &path, QString *error = nullptr);

class NetworkVideoRuntime final {
public:
    NetworkVideoRuntime();
    ~NetworkVideoRuntime();
    void requestShutdown();
    bool shutdownComplete() const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Nonblocking, bounded publications. Called on the compositor/audio threads.
quint64 networkVideoOutputGeneration();
unsigned networkVideoOutputMask();
qint64 networkVideoTimecode();
// Both buffers belong to the same render. Alpha pixels are premultiplied BGRA.
void publishNetworkVideo(QByteArray composite, QByteArray alpha, int width, int height,
                         quint64 generation, qint64 timecode);
void publishNetworkAudio(GstBuffer *buffer, qint64 timecode = 0);
void reportNetworkVideoOutputError(const QString &message);

class NetworkVideoOutput final {
public:
    NetworkVideoOutput();
    ~NetworkVideoOutput();
    void publish(ID3D11RenderTargetView *target, ID3D11RenderTargetView *alpha = nullptr,
                 qint64 timecode = 0);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Holds its appsrc reference until the bounded receive loop exits. Destroy only
// on the existing asynchronous session-retirement worker, never in a UI callback.
class NetworkVideoReceiver final {
public:
    NetworkVideoReceiver(const QString &path, GstElement *appsrc, bool audioOnly);
    ~NetworkVideoReceiver();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
