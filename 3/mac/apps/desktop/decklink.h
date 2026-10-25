#pragma once
#include "decklink_media_api.h"
#include <QByteArray>
#include <QList>
#include <QString>
#include <memory>
#include <array>
typedef struct _GstElement GstElement;
typedef struct _GstBuffer GstBuffer;
struct ID3D11RenderTargetView;
namespace vtrpon {
bool isDeckLinkSource(const QString &path);
QString deckLinkSourcePath(const VtrPonDeckLinkStreamOptions &options);
bool deckLinkSourceOptions(const QString &path, VtrPonDeckLinkStreamOptions *out);
QString deckLinkAddonPath();
bool deckLinkAddonInstalled();
struct DeckLinkSettings {
    bool inputEnabled = true, outputEnabled = false;
    QString outputChannel;
    uint32_t outputMode = 0, inputMode = 0;
    bool compositeEnabled = true, fillEnabled = false, keyEnabled = false, premultipliedFill = false;
    QString fillChannel, keyChannel;
    uint32_t fillMode = 0, keyMode = 0;
    unsigned outputMask() const { return outputEnabled ? (compositeEnabled ? 1u : 0u) | (fillEnabled ? 2u : 0u) | (keyEnabled ? 4u : 0u) : 0; }
    QString channel(unsigned i) const { return i == 0 ? outputChannel : i == 1 ? fillChannel : keyChannel; }
    uint32_t mode(unsigned i) const { return i == 0 ? outputMode : i == 1 ? fillMode : keyMode; }
    bool operator==(const DeckLinkSettings &) const = default;
};
struct DeckLinkChannel {
    QString name, path;
    bool input = false;
    QList<VtrPonDeckLinkMode> modes;
};
struct DeckLinkState {
    enum class Availability { AddonMissing, Checking, RuntimeUnavailable, NoDevices, Ready };
    Availability availability = Availability::AddonMissing;
    QList<DeckLinkChannel> channels;
    QString error, outputError, readbackError;
    bool outputRunning = false;
    quint64 sentFrames = 0;
    struct Output { bool running = false; QString error; quint64 sentFrames = 0; };
    std::array<Output, 3> outputs{};
    QString syncWarning;
    bool pairSynchronized = false;
};
DeckLinkSettings savedDeckLinkSettings();
void saveDeckLinkSettings(const DeckLinkSettings &);
void applyDeckLinkSettings(const DeckLinkSettings &);
DeckLinkState deckLinkState();
bool deckLinkInputAllowed(const QString &, QString *error = nullptr);
class DeckLinkRuntime final {
public:
    DeckLinkRuntime();
    ~DeckLinkRuntime();
    void requestShutdown();
    bool shutdownComplete() const;
};
quint64 deckLinkOutputGeneration();
unsigned deckLinkOutputMask();
void publishDeckLinkVideo(QByteArray, int width, int height, quint64 generation, qint64 time100ns = 0);
void publishDeckLinkVideo(QByteArray composite, QByteArray alpha, int width, int height, quint64 generation, qint64 time100ns = 0);
void publishDeckLinkAudio(GstBuffer *, qint64 time100ns = 0);
void reportDeckLinkOutputError(const QString &);
class DeckLinkOutput final {
public:
    DeckLinkOutput();
    ~DeckLinkOutput();
    void publish(ID3D11RenderTargetView *, qint64 time100ns = 0);
    void publish(ID3D11RenderTargetView *, ID3D11RenderTargetView *alpha, qint64 time100ns = 0);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
// Multiple video/audio consumers of one channel share one hardware capture.
// Destruction only detaches a subscriber; SDK shutdown runs on its owner worker (MTA on Windows).
class DeckLinkReceiver final {
public:
    DeckLinkReceiver(const QString &, GstElement *appsrc, bool audioOnly);
    ~DeckLinkReceiver();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
