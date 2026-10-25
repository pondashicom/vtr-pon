#include "media_import_queue.h"
#include "playlist_model.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <gst/gst.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#define CHECK(value) do { if (!(value)) { \
  std::cerr << "CHECK failed at " << __LINE__ << ": " << #value << '\n'; \
  return 1; } } while (false)

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 4000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < timeoutMs) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  return condition();
}

struct ProbeGate {
  std::atomic_bool entered{false}, release{false}, exited{false};
  std::atomic_int calls{0};
  std::atomic_bool wrongThread{false};
};

MediaDurationProbeResult successful(PlaylistMediaType type = PlaylistMediaType::Video) {
  MediaDurationProbeResult result;
  result.succeeded = true;
  result.mediaType = type;
  result.durationMs = 1234;
  return result;
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  gst_init(&argc, &argv);
  QTemporaryDir directory;
  CHECK(directory.isValid());
  const QString path = directory.filePath("clip.wav");
  {
    // A real one-second PCM fixture also exercises the production discoverer.
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4); stream << quint32(16036);
    stream.writeRawData("WAVEfmt ", 8); stream << quint32(16) << quint16(1)
        << quint16(1) << quint32(8000) << quint32(16000) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4); stream << quint32(16000);
    stream.writeRawData(QByteArray(16000, '\0').constData(), 16000);
    CHECK(stream.status() == QDataStream::Ok);
  }
  {
    PlaylistModel model;
    for (auto type : {PlaylistSourceType::Pdf, PlaylistSourceType::PowerPoint}) {
      // Pending native registration must not stat this nonexistent source.
      const auto id = model.addPendingMediaItem("missing-native-document", 1, type);
      CHECK(id != 0 && model.itemConversionInProgress(id));
      CHECK(model.itemState(id)->sourceType == type);
      model.setCueItemState(id, PlaylistCueState::Ready);
      CHECK(model.cueItemId() == 0);
      CHECK(model.setItemDisplayName(id, "native rename"));
      CHECK(model.completePendingNativeDocument(id));
      CHECK(!model.itemConversionInProgress(id));
      CHECK(model.itemState(id)->displayName == "native rename");
      CHECK(model.itemState(id)->settings.endMode == PlaylistEndMode::Pause);
      CHECK(!model.completePendingNativeDocument(id));
      const auto removed = model.addPendingMediaItem("removed-native-document", 1, type);
      CHECK(model.removeItem(removed));
      CHECK(!model.completePendingNativeDocument(removed));
    }
  }
  {
    PlaylistModel model;
    const int other = model.createPlaylist();
    CHECK(model.setActivePlaylistNumber(1));
    auto gate = std::make_shared<ProbeGate>();
    const auto guiThread = std::this_thread::get_id();
    MediaImportQueue queue(&model, nullptr, [gate, guiThread](const QString &) {
      if (std::this_thread::get_id() == guiThread) gate->wrongThread = true;
      const int call = ++gate->calls;
      if (call == 1) {
        gate->entered = true;
        while (!gate->release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return successful();
    });
    QList<quint64> ids;
    for (int i = 0; i < 100; ++i) ids.append(queue.enqueue(path, 1));
    CHECK(ids.front() && model.itemCount() == 100);
    CHECK(model.itemConversionInProgress(ids.front()));
    CHECK(!model.restoreSnapshot(model.snapshot()));
    model.setCueItemState(ids.front(), PlaylistCueState::Ready);
    model.setOnAirItemId(ids.front());
    model.setEditItemId(ids.front());
    model.setDskItemId(ids.front());
    CHECK(!model.cueItemId() && !model.onAirItemId() && !model.editItemId() && !model.dskItemId());
    CHECK(!model.updateItemSettings(ids.front(), PlaylistItemSettings{}));
    CHECK(waitFor([&] { return gate->entered.load(); }));
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();
    CHECK(waitFor([&] { return heartbeats >= 10; }));
    CHECK(gate->calls == 1 && !gate->wrongThread);
    CHECK(model.setActivePlaylistNumber(other));
    CHECK(model.setItemDisplayName(ids.front(), "renamed"));
    CHECK(model.removeItem(ids[1])); // Queued deletion must never get analyzed.
    gate->release = true;
    CHECK(waitFor([&] { return !queue.hasPendingImports(); }, 7000));
    CHECK(gate->calls == 99 && !gate->wrongThread);
    CHECK(model.itemCount() == 0);
    CHECK(model.itemState(ids.front())->playlistNumber == 1);
    CHECK(model.itemState(ids.front())->displayName == "renamed");
    CHECK(model.itemState(ids.front())->settings.outPointMs == 1234);
    CHECK(model.setActivePlaylistNumber(1));
    CHECK(model.itemCount() == 99);
    CHECK(model.indexOf(ids[2]) == 1 && model.indexOf(ids.back()) == 98);
    CHECK(model.canRestoreSnapshot(model.snapshot()));
  }
  {
    PlaylistModel model;
    int calls = 0;
    MediaImportQueue queue(&model, nullptr, [](const QString &) {
      return successful(PlaylistMediaType::StillImage);
    });
    QStringList failures;
    QObject::connect(&queue, &MediaImportQueue::importsFailed,
                     [&](const QStringList &details) { failures += details; ++calls; });
    const auto missing = queue.enqueue(directory.filePath("missing.mp4"), 1);
    const auto image = queue.enqueue(path, 1, 8500);
    CHECK(missing && image);
    CHECK(waitFor([&] { return calls == 1; }));
    CHECK(failures.size() == 1 && !model.itemState(missing));
    CHECK(model.itemState(image)->settings.outPointMs == 8500);
  }
  {
    PlaylistModel model;
    MediaImportQueue queue(&model);
    const auto audio = queue.enqueue(path, 1);
    const auto invalidImage = queue.enqueue(path, 1, 5000);
    QStringList failures;
    QObject::connect(&queue, &MediaImportQueue::importsFailed,
                     [&](const QStringList &details) { failures += details; });
    CHECK(waitFor([&] { return !queue.hasPendingImports() && !failures.isEmpty(); }));
    CHECK(model.itemState(audio)->mediaType == PlaylistMediaType::Audio);
    CHECK(model.itemState(audio)->settings.outPointMs == 1000);
    CHECK(!model.itemState(invalidImage) && failures.size() == 1);
  }
  {
    PlaylistModel model;
    auto gate = std::make_shared<ProbeGate>();
    auto queue = std::make_unique<MediaImportQueue>(&model, nullptr,
        [gate](const QString &) {
      gate->entered = true;
      while (!gate->release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      gate->exited = true;
      return successful();
    });
    const auto active = queue->enqueue(path, 1);
    CHECK(waitFor([&] { return gate->entered.load(); }));
    CHECK(model.removeItem(active));
    const auto later = queue->enqueue(path, 1);
    QElapsedTimer elapsed;
    elapsed.start();
    queue.reset(); // Must not wait for a blocked probe or leave placeholders.
    CHECK(elapsed.elapsed() < 500);
    CHECK(!model.itemState(later) && model.itemCount() == 0);
    gate->release = true;
    CHECK(waitFor([&] { return gate->exited.load(); }));
    CHECK(model.itemCount() == 0);
  }
  {
    PlaylistModel model;
    auto gate = std::make_shared<ProbeGate>();
    MediaImportQueue queue(&model, nullptr, [gate](const QString &) {
      gate->entered = true;
      while (!gate->release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      gate->exited = true;
      return successful();
    });
    const auto removed = queue.enqueue(path, 1);
    CHECK(waitFor([&] { return gate->entered.load(); }));
    CHECK(model.removeItem(removed));
    // Restore can reuse the deleted ID while its old worker is still running.
    PlaylistModelSnapshot snapshot = model.snapshot();
    PlaylistItemState replacement;
    replacement.id = removed;
    replacement.itemNumber = 1;
    replacement.playlistNumber = 1;
    replacement.filePath = path;
    replacement.displayName = "restored";
    replacement.mediaType = PlaylistMediaType::Audio;
    replacement.settings.outPointMs = 900;
    snapshot.items.append(replacement);
    CHECK(model.restoreSnapshot(snapshot));
    const auto next = queue.enqueue(path, 1);
    gate->release = true;
    CHECK(waitFor([&] { return !queue.hasPendingImports(); }));
    CHECK(model.itemState(removed)->displayName == "restored");
    CHECK(model.itemState(removed)->settings.outPointMs == 900);
    CHECK(model.itemState(next)->settings.outPointMs == 1234);
  }
  {
    PlaylistModel model;
    auto gate = std::make_shared<ProbeGate>();
    auto queue = std::make_unique<MediaImportQueue>(&model, nullptr,
        [gate](const QString &) {
          ++gate->calls;
          gate->entered = true;
          while (!gate->release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
          gate->exited = true;
          return successful();
        });
    // Always release the fixture before a failed assertion unwinds the test.
    struct ReleaseGate {
      std::shared_ptr<ProbeGate> gate;
      ~ReleaseGate() { gate->release = true; }
    } releaseGate{gate};
    const auto active = queue->enqueue(path, 1);
    const auto queued = queue->enqueue(path, 1);
    CHECK(active && queued && waitFor([&] { return gate->entered.load(); }));
    int failures = 0;
    QObject::connect(queue.get(), &MediaImportQueue::importsFailed,
                     [&] { ++failures; });
    QElapsedTimer elapsed;
    elapsed.start();
    queue->requestShutdown();
    queue->requestShutdown(); // Idempotent while a probe is still blocked.
    CHECK(elapsed.elapsed() < 500);
    CHECK(!queue->shutdownComplete());
    CHECK(!queue->hasPendingImports() && model.itemCount() == 0);
    CHECK(queue->enqueue(path, 1) == 0);
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();
    CHECK(waitFor([&] { return heartbeats >= 5; }));
    CHECK(!queue->shutdownComplete() && gate->calls == 1);
    gate->release = true;
    CHECK(waitFor([&] { return queue->shutdownComplete(); }));
    CHECK(gate->exited && gate->calls == 1 && failures == 0);
    CHECK(!model.itemState(active) && !model.itemState(queued));
    elapsed.restart();
    queue.reset();
    CHECK(elapsed.elapsed() < 500);
    QCoreApplication::processEvents();
    CHECK(model.itemCount() == 0);
  }
  std::cout << "Media import queue checks passed\n";
  return 0;
}
