#include "decklink.h"
#include "edit_session_controller.h"
#include "live_audio_source.h"
#include "wasapi_raw_audio_source.h"
#include "media_thumbnailer.h"
#include "edit_pfl_output.h"
#include "edit_pfl_protocol.h"
#include "async_pipeline_stop.h"
#include "audio_effects.h"
#include "audio_output_settings.h"

#include "edit_panel.h"
#include "playlist_model.h"
#include "playlist_panel.h"
#include "network_video.h"
#include "uvc_audio_mapping.h"
#include "uvc_source_session_manager.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>
#include <atomic>
#include <cmath>

class LiveEditAudio final {
public:
  struct TapState {
    std::shared_ptr<EditPflFeed> feed;
    std::atomic_bool active{true};
    GstElement *pipeline = nullptr; // Kept alive by the probe's owning pipeline.
    GstSegment segment;
    TapState() { gst_segment_init(&segment, GST_FORMAT_TIME); }
  };
  explicit LiveEditAudio(std::shared_ptr<EditPflFeed> feed)
      : feed_(std::move(feed)) {}
  ~LiveEditAudio() { stop(); }

  bool start(const QString &audioDeviceId, const PlaylistItemSettings &settings,
             QString *detail) {
    stop();
    if (audioDeviceId.isEmpty()) return true;
    if (!vtrpon::registerAudioEffects()) {
      if (detail) *detail = QStringLiteral("EDIT audio effects are unavailable.");
      return false;
    }
    const bool network = (vtrpon::isNetworkVideoSource(audioDeviceId) || vtrpon::isDeckLinkSource(audioDeviceId));
    if (!network && !vtrpon::registerWasapiRawAudioSource()) {
      if (detail) *detail = QStringLiteral("The shared RAW audio input is unavailable.");
      return false;
    }
    const QString input = network
        ? QStringLiteral("appsrc name=edit-live-source is-live=true format=time do-timestamp=true block=false max-buffers=0 max-bytes=0 max-time=200000000 leaky-type=downstream ! ")
        : QStringLiteral("vtrponwasapirawsrc name=edit-live-source ! ");
    const QString description = input + QStringLiteral(
        "queue max-size-buffers=0 max-size-bytes=0 max-size-time=200000000 ! "
        "audioconvert ! audioresample ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
        "level name=edit-live-input-level post-messages=true interval=20000000 ! "
        "vtrponaudioeffects name=edit-live-effects slot=edit ! "
        "volume name=edit-live-volume ! "
        "level name=edit-live-output-level post-messages=true interval=20000000 ! "
        "fakesink sync=true qos=false enable-last-sample=false");
    GError *parseError = nullptr;
    pipeline_ = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (parseError) {
      if (detail) *detail = QString::fromUtf8(parseError->message);
      g_error_free(parseError);
    }
    if (!pipeline_ || !GST_IS_PIPELINE(pipeline_)) {
      stop();
      return false;
    }
    source_ = gst_bin_get_by_name(GST_BIN(pipeline_), "edit-live-source");
    effects_ = gst_bin_get_by_name(GST_BIN(pipeline_), "edit-live-effects");
    volume_ = gst_bin_get_by_name(GST_BIN(pipeline_), "edit-live-volume");
    inputLevel_ = gst_bin_get_by_name(GST_BIN(pipeline_), "edit-live-input-level");
    outputLevel_ = gst_bin_get_by_name(GST_BIN(pipeline_), "edit-live-output-level");
    bus_ = gst_element_get_bus(pipeline_);
    if (!source_ || !effects_ || !volume_ || !inputLevel_ || !outputLevel_ ||
        !bus_) {
      if (detail) *detail = QStringLiteral("Could not create the EDIT live audio elements.");
      stop();
      return false;
    }
    if (!network) {
      const QByteArray id = audioDeviceId.toUtf8();
      g_object_set(source_, "device", id.constData(), nullptr);
    }
    setSettings(settings);
    tap_ = std::make_shared<TapState>();
    tap_->feed = feed_;
    tap_->pipeline = pipeline_;
    GstPad *pflPad = gst_element_get_static_pad(volume_, "sink");
    const gulong pflProbeId = pflPad
        ? gst_pad_add_probe(pflPad, static_cast<GstPadProbeType>(
              GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
              &LiveEditAudio::onPflAudio,
              new std::shared_ptr<TapState>(tap_),
              [](gpointer value) { delete static_cast<std::shared_ptr<TapState> *>(value); })
        : 0;
    if (pflPad) gst_object_unref(pflPad);
    if (pflProbeId == 0) {
      if (detail) *detail = QStringLiteral("Could not create the EDIT live PFL tap.");
      stop();
      return false;
    }
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      if (detail) *detail = QStringLiteral("Could not start the EDIT live audio input.");
      stop();
      return false;
    }
    if (vtrpon::isDeckLinkSource(audioDeviceId))
      deckLinkReceiver_ = std::make_shared<vtrpon::DeckLinkReceiver>(audioDeviceId, source_, true);
    else if (network)
      networkReceiver_ = std::make_shared<vtrpon::NetworkVideoReceiver>(
          audioDeviceId, source_, true);
    feed_->setPlaying(true);
    return true;
  }

  void stop() {
    if (tap_) tap_->active.store(false, std::memory_order_release);
    tap_.reset();
    feed_->setPlaying(false);
    feed_->flush();
    networkReceiver_.reset();
    deckLinkReceiver_.reset();
    for (auto *element : {source_, effects_, volume_, inputLevel_, outputLevel_})
      if (element) gst_object_unref(element);
    source_ = effects_ = volume_ = inputLevel_ = outputLevel_ = nullptr;
    if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
    if (pipeline_) {
      stops_.push_back(AsyncPipelineStop::start(pipeline_));
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    input_ = {};
    output_ = {};
    lastInputLevelNs_ = 0;
    lastOutputLevelNs_ = 0;
  }

  bool active() const { return pipeline_ != nullptr; }
  bool shutdownComplete() const {
    return std::all_of(stops_.begin(), stops_.end(),
                       [](const auto &job) { return job->done(); });
  }
  void setSettings(const PlaylistItemSettings &settings) {
    if (effects_) {
      const auto &eq = settings.nominalEqTenthsDb;
      vtrpon::setAudioEffectEqualizer(effects_, eq[0] / 10.0,
                                      eq[1] / 10.0, eq[2] / 10.0);
    }
    if (volume_) {
      const double gain = settings.nominalVolumeTenthsDb <= kMinimumItemVolumeTenthsDb
          ? 0.0 : std::pow(10.0, settings.nominalVolumeTenthsDb / 200.0);
      g_object_set(volume_, "volume", gain, nullptr);
    }
  }
  void processBus(QString *error) {
    if (!bus_) return;
    EditPlayer::AudioLevel inputPeak;
    EditPlayer::AudioLevel outputPeak;
    bool inputSeen = false;
    bool outputSeen = false;
    const auto mergePeak = [](EditPlayer::AudioLevel &target,
                              const EditPlayer::AudioLevel &value) {
      target.leftPeakDb = (std::max)(target.leftPeakDb, value.leftPeakDb);
      target.rightPeakDb = (std::max)(target.rightPeakDb, value.rightPeakDb);
      target.clipped = target.clipped || value.clipped;
    };
    while (GstMessage *message = gst_bus_pop(bus_)) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) {
        EditPlayer::AudioLevel value;
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(inputLevel_) &&
            readLevel(message, value)) {
          mergePeak(inputPeak, value);
          inputSeen = true;
        } else if (GST_MESSAGE_SRC(message) == GST_OBJECT(outputLevel_) &&
                   readLevel(message, value)) {
          mergePeak(outputPeak, value);
          outputSeen = true;
        }
      } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ||
                 GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        if (error) {
          GError *gstError = nullptr;
          gchar *debug = nullptr;
          if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            gst_message_parse_error(message, &gstError, &debug);
          *error = gstError ? QString::fromUtf8(gstError->message)
                            : QStringLiteral("EDIT live audio input ended.");
          g_clear_error(&gstError);
          g_free(debug);
        }
      }
      gst_message_unref(message);
    }
    const auto nowNs = EditPflFeed::nowNs();
    constexpr std::int64_t kLevelStaleNs = 200000000;
    if (inputSeen) {
      input_ = inputPeak;
      lastInputLevelNs_ = nowNs;
    } else if (lastInputLevelNs_ != 0 && nowNs - lastInputLevelNs_ > kLevelStaleNs) {
      input_ = {};
    }
    if (outputSeen) {
      output_ = outputPeak;
      lastOutputLevelNs_ = nowNs;
    } else if (lastOutputLevelNs_ != 0 && nowNs - lastOutputLevelNs_ > kLevelStaleNs) {
      output_ = {};
    }
    if (error && !error->isEmpty()) stop();
    stops_.erase(std::remove_if(stops_.begin(), stops_.end(),
        [](const auto &job) { return job->done() && job->succeeded(); }), stops_.end());
  }
  EditPlayer::AudioLevel inputLevel() const { return input_; }
  EditPlayer::AudioLevel outputLevel() const { return output_; }

private:
  static GstPadProbeReturn onPflAudio(GstPad *, GstPadProbeInfo *info,
                                      gpointer data) {
    const auto tap = *static_cast<std::shared_ptr<TapState> *>(data);
    if (!tap->active.load(std::memory_order_acquire)) return GST_PAD_PROBE_OK;
    const auto type = GST_PAD_PROBE_INFO_TYPE(info);
    if ((type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
      GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
      if (!event) return GST_PAD_PROBE_OK;
      if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
        const GstSegment *segment = nullptr;
        gst_event_parse_segment(event, &segment);
        if (segment && segment->format == GST_FORMAT_TIME) tap->segment = *segment;
        tap->feed->flush();
      } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START ||
                 GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
        tap->feed->flush();
      }
      return GST_PAD_PROBE_OK;
    }
    if ((type & GST_PAD_PROBE_TYPE_BUFFER) == 0 || tap->segment.rate != 1.0)
      return GST_PAD_PROBE_OK;
    auto *audio = tap->feed->audio();
    if (!audio || !audio->enabled.load() || !audio->playing.load())
      return GST_PAD_PROBE_OK;
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer || !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)))
      return GST_PAD_PROBE_OK;
    const auto running = gst_segment_to_running_time(
        &tap->segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
    if (!GST_CLOCK_TIME_IS_VALID(running)) return GST_PAD_PROBE_OK;
    GstClock *clock = gst_element_get_clock(tap->pipeline);
    if (!clock) return GST_PAD_PROBE_OK;
    const auto clockNow = gst_clock_get_time(clock);
    const auto base = gst_element_get_base_time(tap->pipeline);
    gst_object_unref(clock);
    if (!GST_CLOCK_TIME_IS_VALID(base)) return GST_PAD_PROBE_OK;
    const auto due = EditPflFeed::nowNs() + static_cast<std::int64_t>(base + running)
        - static_cast<std::int64_t>(clockNow);
    GstMapInfo map{};
    if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      constexpr auto bytesPerFrame = 2 * sizeof(float);
      if (map.size % bytesPerFrame == 0 && map.size <= 48000 * bytesPerFrame) {
        const auto *samples = reinterpret_cast<const float *>(map.data);
        const auto frames = static_cast<std::uint32_t>(map.size / bytesPerFrame);
        const auto epoch = audio->generation.load();
        for (std::uint32_t offset = 0; offset < frames; offset += vtrpon::pfl::kFrames) {
          if (!tap->active.load(std::memory_order_acquire)) break;
          const auto count = (std::min)(vtrpon::pfl::kFrames, frames - offset);
          if (!audio->push(samples + offset * 2, count,
              due + static_cast<std::int64_t>(offset) * 1000000000LL / 48000, epoch)) break;
        }
      }
      gst_buffer_unmap(buffer, &map);
    }
    return GST_PAD_PROBE_OK;
  }
  static bool readLevel(GstMessage *message, EditPlayer::AudioLevel &level) {
    const GstStructure *structure = gst_message_get_structure(message);
    if (!structure || !gst_structure_has_name(structure, "level")) return false;
    const GValue *values = gst_structure_get_value(structure, "peak");
    if (!values) return false;
    const GValue *left = nullptr;
    const GValue *right = nullptr;
    const GType type = G_VALUE_TYPE(values);
    static const GType glibArrayType = g_type_from_name("GValueArray");
    static const GType gstArrayType = g_type_from_name("GstValueArray");
    static const GType listType = g_type_from_name("GstValueList");
    if (glibArrayType != G_TYPE_INVALID && g_type_is_a(type, glibArrayType)) {
      const auto *array = static_cast<const GValueArray *>(g_value_get_boxed(values));
      if (array && array->n_values > 0) {
        left = &array->values[0];
        right = array->n_values > 1 ? &array->values[1] : left;
      }
    } else if (gstArrayType != G_TYPE_INVALID && g_type_is_a(type, gstArrayType)) {
      const guint count = gst_value_array_get_size(values);
      if (count > 0) {
        left = gst_value_array_get_value(values, 0);
        right = gst_value_array_get_value(values, count > 1 ? 1 : 0);
      }
    } else if (listType != G_TYPE_INVALID && g_type_is_a(type, listType)) {
      const guint count = gst_value_list_get_size(values);
      if (count > 0) {
        left = gst_value_list_get_value(values, 0);
        right = gst_value_list_get_value(values, count > 1 ? 1 : 0);
      }
    }
    if (!left || !right || !G_VALUE_HOLDS_DOUBLE(left) || !G_VALUE_HOLDS_DOUBLE(right))
      return false;
    const double l = g_value_get_double(left), r = g_value_get_double(right);
    level.leftPeakDb = std::isfinite(l) ? std::clamp(l, -60.0, 6.0) : -60.0;
    level.rightPeakDb = std::isfinite(r) ? std::clamp(r, -60.0, 6.0) : -60.0;
    level.clipped = l >= 0.0 || r >= 0.0;
    return true;
  }

  std::shared_ptr<EditPflFeed> feed_;
  std::shared_ptr<TapState> tap_;
  GstElement *pipeline_ = nullptr;
  GstElement *source_ = nullptr;
  GstElement *effects_ = nullptr;
  GstElement *volume_ = nullptr;
  GstElement *inputLevel_ = nullptr;
  GstElement *outputLevel_ = nullptr;
  GstBus *bus_ = nullptr;
  std::shared_ptr<vtrpon::NetworkVideoReceiver> networkReceiver_;
  std::shared_ptr<vtrpon::DeckLinkReceiver> deckLinkReceiver_;
  std::vector<std::shared_ptr<AsyncPipelineStop>> stops_;
  EditPlayer::AudioLevel input_;
  EditPlayer::AudioLevel output_;
  std::int64_t lastInputLevelNs_ = 0;
  std::int64_t lastOutputLevelNs_ = 0;
};

namespace {

constexpr double kScanRate = 2.0;
constexpr qint64 kEndOfStreamToleranceMs = 1000;
constexpr int kOrdinarySeekLockTicks = 10;
constexpr int kFrameSeekLockTicks = 60;
constexpr qint64 kOrdinarySeekToleranceMs = 1000;
constexpr int kInputSignalHoldMs = 250;
constexpr int kInputClipHoldMs = 500;
constexpr double kMeterFloorDb = -60.0;

double nominalVolumeGain(int volumeTenthsDb) {
  if (volumeTenthsDb <= kMinimumItemVolumeTenthsDb) {
    return 0.0;
  }
  return std::pow(10.0, static_cast<double>(volumeTenthsDb) / 200.0);
}

void appendEditScanLog(const QString &message) {
  QFile file(QCoreApplication::applicationDirPath() +
             QStringLiteral("/vtr-pon-edit-scan.log"));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    return;
  }
  QTextStream stream(&file);
  stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << " "
         << message << '\n';
}

QString formatTime(qint64 timeMs) {
  const qint64 totalCentiseconds = (std::max)(timeMs, qint64{0}) / 10;
  const qint64 centiseconds = totalCentiseconds % 100;
  const qint64 totalSeconds = totalCentiseconds / 100;
  const qint64 seconds = totalSeconds % 60;
  const qint64 totalMinutes = totalSeconds / 60;
  const qint64 minutes = totalMinutes % 60;
  const qint64 hours = totalMinutes / 60;
  return QStringLiteral("%1:%2:%3.%4")
      .arg(hours, 2, 10, QLatin1Char('0'))
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'))
      .arg(centiseconds, 2, 10, QLatin1Char('0'));
}

QString startModeName(PlaylistStartMode mode) {
  return mode == PlaylistStartMode::Play ? QStringLiteral("Play")
                                         : QStringLiteral("Pause");
}

QString endModeName(PlaylistEndMode mode) {
  switch (mode) {
  case PlaylistEndMode::Stop:
    return QStringLiteral("Stop");
  case PlaylistEndMode::Pause:
    return QStringLiteral("Pause");
  case PlaylistEndMode::Repeat:
    return QStringLiteral("Repeat");
  case PlaylistEndMode::Next:
    return QStringLiteral("Next");
  case PlaylistEndMode::Goto:
    return QStringLiteral("GOTO");
  }
  return QString();
}

QString repeatEndModeName(PlaylistRepeatEndMode mode) {
  switch (mode) {
  case PlaylistRepeatEndMode::Stop:
    return QStringLiteral("Stop");
  case PlaylistRepeatEndMode::Pause:
    return QStringLiteral("Pause");
  case PlaylistRepeatEndMode::Next:
    return QStringLiteral("Next");
  case PlaylistRepeatEndMode::Goto:
    return QStringLiteral("GOTO");
  }
  return QString();
}

bool usesGotoDestination(const PlaylistItemSettings &settings) {
  return settings.endMode == PlaylistEndMode::Goto ||
         (settings.endMode == PlaylistEndMode::Repeat &&
          settings.repeatEndMode == PlaylistRepeatEndMode::Goto);
}

bool usesAutomaticEndTransition(const PlaylistItemSettings &settings) {
  return settings.endMode == PlaylistEndMode::Next ||
          settings.endMode == PlaylistEndMode::Goto ||
          settings.endMode == PlaylistEndMode::Repeat;
}

} // namespace

EditSessionController::EditSessionController(EditPanel *panel,
                                             PlaylistPanel *playlistPanel,
                                             PlaylistModel *playlistModel,
                                              UvcSourceSessionManager *uvcSessions,
                                              UvcAudioMappingStore *uvcAudioMappings,
                                             QObject *parent)
    : QObject(parent), panel_(panel), playlistPanel_(playlistPanel),
      playlistModel_(playlistModel), uvcSessions_(uvcSessions),
      uvcAudioMappings_(uvcAudioMappings), player_(panel->videoSurface()),
      liveAudio_(std::make_unique<LiveEditAudio>(player_.pflFeed())) {
  pflOutput_ = new EditPflOutput(player_.pflFeed(), this);
  connect(pflOutput_, &EditPflOutput::stateChanged, this, [this] {
    if (panel_) panel_->setPflState(pflOutput_->enabled(),
        pflOutput_->canEnable() || pflOutput_->deviceId().isEmpty(), pflOutput_->statusText());
  });
  connect(panel, &EditPanel::pflRequested, this, &EditSessionController::pflRequested);
  connect(pflOutput_, &EditPflOutput::outputError, this, [this](const QString &message) {
    if (panel_) panel_->setStatus(message, true);
    emit pflOutputError(message);
  });
  pflOutput_->setDeviceId(vtrpon::savedEditPflOutputDeviceId());
  panel->setPflState(false, pflOutput_->canEnable() || pflOutput_->deviceId().isEmpty(),
                     pflOutput_->statusText());
  connect(playlistModel, &PlaylistModel::itemSettingsChanged, this,
          [this](quint64 itemId) {
            if (itemId == editItemId_) {
              updateEditorState();
            }
          });
  connect(playlistModel, &PlaylistModel::itemSettingsLockChanged, this,
          [this](quint64 itemId) {
            if (itemId == editItemId_) {
              updateEditorState();
            }
          });
  connect(playlistModel, &PlaylistModel::onAirStateChanged, this,
          [this] { updateEditorState(); });
  connect(playlistModel, &PlaylistModel::itemRemoved, this,
          [this] { updateEditorState(); });
  connect(playlistModel, &PlaylistModel::itemAdded, this,
          [this] { updateEditorState(); });
  connect(playlistModel, &PlaylistModel::itemMoved, this,
          [this] { updateEditorState(); });
  connect(playlistModel, &PlaylistModel::itemMovedToPlaylist, this,
          [this] { updateEditorState(); });
  connect(playlistModel, &PlaylistModel::modelReset, this,
          [this] { updateEditorState(); });
  connect(playlistModel, &PlaylistModel::itemMediaAvailabilityChanged, this,
          [this](quint64 itemId, bool offline) {
            if (panel_) panel_->setGotoTargetOffline(itemId, offline);
            if (itemId == editItemId_ && vtrpon::isLiveAudioSource(liveDevicePath_)) {
              liveAudio_->stop();
              liveAudioNeedsStart_ = !offline;
              resetAudioPresentation();
              if (panel_) panel_->setStatus(vtrpon::LocalizedText(offline
                  ? "input.audio.offline" : "input.audio.ready"), offline);
            }
          });
  connect(uvcSessions, &UvcSourceSessionManager::thumbnailReady, this,
          [this](const QString &path, const QImage &image) {
            if (panel_ && path == liveDevicePath_ && !livePreviewReady_ && !image.isNull())
              panel_->setLiveFrame(image);
          });
  connect(uvcSessions, &UvcSourceSessionManager::editPreviewReady, this,
          [this](const QString &path, const QImage &image) {
            if (panel_ && path == liveDevicePath_ && !image.isNull()) {
              livePreviewReady_ = true;
              panel_->setLiveFrame(image);
            }
          });
  connect(uvcSessions, &UvcSourceSessionManager::sourceError, this,
          [this](const QString &path, const QString &) {
            if (panel_ && path == liveDevicePath_) {
              panel_->setLiveFrame({});
            }
          });
  connect(uvcAudioMappings, &UvcAudioMappingStore::mappingChanged, this,
          [this](const QString &path, const QString &) {
            if (path != liveDevicePath_) return;
            liveAudio_->stop();
            liveAudioNeedsStart_ = true;
            timer_.start();
          });
  connect(&timer_, &QTimer::timeout, this, &EditSessionController::updateUi);
  timer_.setTimerType(Qt::PreciseTimer);
  timer_.setInterval(33);

  if (player_.isAvailable()) {
    panel_->setStatus(
        QStringLiteral("EDIT | Select an item, then click EDIT to load"));
  } else {
    panel_->setStatus(
        QStringLiteral("Could not create the EDIT player (ON AIR will continue)"),
        true);
  }
}

EditSessionController::~EditSessionController() { shutdown(); }

bool EditSessionController::isAvailable() const {
  return player_.isAvailable();
}

bool EditSessionController::deferItemRemoval(quint64 itemId, std::function<void()> continuation) {
  if (shutdown_ || itemId == 0 || editItemId_ != itemId) return false;
  // Repeated input must not finalize the unload ahead of the timer and strand
  // the first continuation. Keep the original operation; do not queue duplicates.
  if (removalPendingId_ != 0) return true;
  if (prepareForItemRemoval(itemId) || !player_.isBusy()) return false;
  panel_->setTransportEnabled(false);
  updateEditorState();
  removalPendingId_ = itemId;
  removalContinuations_.push_back(std::move(continuation));
  timer_.start();
  return true;
}

bool EditSessionController::prepareForItemRemoval(quint64 itemId) {
  if (itemId == 0 || editItemId_ != itemId) {
    return true;
  }
  liveDevicePath_.clear();
  livePreviewReady_ = false;
  liveAudioNeedsStart_ = false;
  liveAudio_->stop();
  if (uvcSessions_) uvcSessions_->setEditPreviewPath({});
  if (panel_) panel_->clearLiveFrame();
  if (!player_.unload()) {
    if (panel_ && !player_.isBusy()) {
      panel_->setStatus(QStringLiteral("Could not unload the item from EDIT."), true);
    }
    return false;
  }
  loadPending_ = false;
  timer_.stop();
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  reverseScanPositionMs_.reset();
  reverseScanBasePositionMs_ = 0;
  seekLockTicks_ = 0;
  suppressScanEosTicks_ = 0;
  scanDirection_ = 0;
  resumeAfterScan_ = false;
  editItemId_ = 0;
  editFileName_.clear();
  if (playlistModel_) {
    playlistModel_->clearEditItem();
  }
  if (panel_) {
    panel_->clearFile();
    panel_->setTransportEnabled(false);
    panel_->setTimelineDuration(-1);
    panel_->setTimelinePosition(0);
    panel_->setEditorState(std::nullopt, false, false);
    resetAudioPresentation();
    panel_->setStatus(QStringLiteral("EDIT unloaded."));
  }
  return true;
}

void EditSessionController::refreshWindowHandle() {
  player_.refreshWindowHandle();
}

void EditSessionController::requestShutdown() {
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  liveDevicePath_.clear();
  livePreviewReady_ = false;
  liveAudioNeedsStart_ = false;
  liveAudio_->stop();
  if (uvcSessions_) uvcSessions_->setEditPreviewPath({});
  pflOutput_->shutdown();
  timer_.stop();
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  reverseScanPositionMs_.reset();
  reverseScanBasePositionMs_ = 0;
  seekLockTicks_ = 0;
  suppressScanEosTicks_ = 0;
  scanDirection_ = 0;
  resumeAfterScan_ = false;
  if (playlistModel_) {
    playlistModel_->clearEditItem();
  }
  player_.requestShutdown();
}

bool EditSessionController::shutdownComplete() const {
  return player_.shutdownComplete() && liveAudio_->shutdownComplete() &&
         (!pflOutput_ || pflOutput_->shutdownComplete());
}

void EditSessionController::shutdown() {
  requestShutdown();
  player_.shutdown();
}

void EditSessionController::loadSelectedItem() {
  if (!playlistPanel_) {
    return;
  }
  const auto selected = playlistPanel_->selectedItemState();
  if (selected &&
      !playlistPanel_->allowItemAction(selected->id,
                                       PlaylistItemAction::Edit)) {
    return;
  }
  if (selected && selected->sourceType == PlaylistSourceType::File)
    loadItem(selected->id, selected->filePath);
  else if (selected && selected->sourceType == PlaylistSourceType::Uvc)
    loadLiveItem(*selected);
}

void EditSessionController::setPflEnabled(bool enabled) {
  if (!shutdown_) pflOutput_->setEnabled(enabled);
}

void EditSessionController::setPflOutputDeviceId(const QString &id) {
  if (!shutdown_) pflOutput_->setDeviceId(id);
}

void EditSessionController::play() {
  if (!liveDevicePath_.isEmpty()) return;
  if (!stopScanInternal()) {
    return;
  }
  runTransport(QStringLiteral("Play"), player_.play());
  if (panel_) {
    panel_->setPlaybackState(player_.isPlaying());
  }
}

void EditSessionController::pause() {
  if (!liveDevicePath_.isEmpty()) return;
  if (!stopScanInternal()) {
    return;
  }
  runTransport(QStringLiteral("Pause"), player_.pause());
  if (panel_) {
    panel_->setPlaybackState(player_.isPlaying());
  }
}

void EditSessionController::jumpToStart() {
  requestSeek(0, QStringLiteral("Go to start"));
}

void EditSessionController::jumpToIn() {
  if (!panel_ || !playlistModel_ || !player_.hasMedia()) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state) {
    panel_->setStatus(QStringLiteral("Could not go to the EDIT In point."), true);
    return;
  }
  requestSeek(state->settings.inPointMs, QStringLiteral("Go to In point"));
}

void EditSessionController::jumpToOut() {
  if (!panel_ || !playlistModel_ || !player_.hasMedia()) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  const qint64 duration = player_.durationMs();
  if (!state) {
    panel_->setStatus(QStringLiteral("Could not go to the EDIT Out point."), true);
    return;
  }
  const qint64 target =
      state->settings.outPointMs.value_or((std::max)(duration, qint64{0}));
  requestSeek(target, QStringLiteral("Go to Out point"));
}

void EditSessionController::jumpToEnd() {
  if (!liveDevicePath_.isEmpty()) return;
  const qint64 duration = player_.durationMs();
  if (duration < 0) {
    if (panel_) {
      panel_->setStatus(QStringLiteral("Could not determine the EDIT end position."), true);
    }
    return;
  }
  requestSeek(duration, QStringLiteral("Go to end"));
}

void EditSessionController::startScanBackward() { startScan(-1); }

void EditSessionController::startScanForward() { startScan(1); }

void EditSessionController::stopScan() {
  (void)stopScanInternal();
}

void EditSessionController::setInPoint() { setSelectedEditPoint(true); }

void EditSessionController::setOutPoint() { setSelectedEditPoint(false); }

void EditSessionController::setInPointTime(qint64 positionMs) {
  setSelectedEditPoint(true, positionMs);
}

void EditSessionController::setOutPointTime(qint64 positionMs) {
  setSelectedEditPoint(false, positionMs);
}

void EditSessionController::seek(qint64 positionMs) {
  if (!liveDevicePath_.isEmpty()) return;
  if (!stopScanInternal()) {
    return;
  }
  pendingSeekMs_ = positionMs;
  pendingSeekAccurate_ = false;
}

void EditSessionController::stepFrames(int frameCount) {
  if (frameCount == 0 || !panel_ || !player_.hasMedia()
      || !stopScanInternal()) {
    return;
  }
  if (player_.isPlaying() && !player_.pause()) {
    panel_->setStatus(QStringLiteral("Failed to pause EDIT for frame stepping."),
                      true);
    return;
  }

  const qint64 duration = player_.durationMs();
  const qint64 position = pendingSeekMs_.value_or(
      seekLockMs_.value_or(player_.positionMs()));
  if (position < 0) {
    panel_->setStatus(
        QStringLiteral("Could not determine the EDIT frame position."), true);
    return;
  }
  const qint64 requestedTarget =
      player_.frameStepTargetMs(position, frameCount);
  const qint64 target = duration >= 0
      ? std::clamp(requestedTarget, qint64{0}, duration)
      : (std::max)(requestedTarget, qint64{0});
  pendingSeekMs_ = target;
  pendingSeekAccurate_ = true;
  panel_->setTimelinePosition(target);
  panel_->setPlaybackState(false);
  resetAudioPresentation();
}

void EditSessionController::loadItem(quint64 itemId, const QString &path) {
  if (shutdown_ || removalPendingId_ != 0) return;
  const QFileInfo fileInfo(path);
  if (!panel_) {
    return;
  }
  if (!player_.isAvailable()) {
    panel_->setStatus(
        QStringLiteral("The EDIT player is unavailable (ON AIR will continue)"),
        true);
    return;
  }
  if (!fileInfo.exists() || !fileInfo.isFile()) {
    panel_->setStatus(QStringLiteral("The file to load into EDIT could not be found."),
                      true);
    return;
  }
  if (!player_.load(path)) {
    panel_->setTransportEnabled(player_.hasMedia());
    panel_->setStatus(QStringLiteral("Failed to load the file into EDIT."), true);
    return;
  }
  liveDevicePath_.clear();
  livePreviewReady_ = false;
  liveAudioNeedsStart_ = false;
  liveAudio_->stop();
  if (uvcSessions_) uvcSessions_->setEditPreviewPath({});
  panel_->clearLiveFrame();

  editItemId_ = itemId;
  editFileName_ = fileInfo.fileName();
  if (playlistModel_) {
    playlistModel_->setEditItemId(itemId);
  }
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  reverseScanPositionMs_.reset();
  reverseScanBasePositionMs_ = 0;
  seekLockTicks_ = 0;
  scanDirection_ = 0;
  resumeAfterScan_ = false;
  panel_->resetForLoadedMedia();
  panel_->setFile(editFileName_, fileInfo.absoluteFilePath());
  panel_->setStatusToolTip(fileInfo.absoluteFilePath());
  loadPending_ = true;
  panel_->setTransportEnabled(false);
  panel_->setStatus(QStringLiteral("EDIT loading | %1").arg(editFileName_));
  updateEditorState();
  timer_.start();
}

void EditSessionController::loadLiveItem(const PlaylistItemState &item) {
  if (shutdown_ || removalPendingId_ != 0 || !panel_ || !uvcSessions_) return;
  liveAudio_->stop();
  (void)player_.unload();
  loadPending_ = false;
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  reverseScanPositionMs_.reset();
  reverseScanBasePositionMs_ = 0;
  seekLockTicks_ = 0;
  scanDirection_ = 0;
  resumeAfterScan_ = false;
  editItemId_ = item.id;
  liveDevicePath_ = item.filePath;
  livePreviewReady_ = false;
  liveAudioNeedsStart_ = true;
  const bool audioOnly = vtrpon::isLiveAudioSource(item.filePath);
  uvcSessions_->setEditPreviewPath(audioOnly ? QString() : item.filePath);
  editFileName_ = item.displayName;
  playlistModel_->setEditItemId(item.id);
  panel_->resetForLoadedMedia();
  panel_->setFile(editFileName_, item.filePath);
  panel_->setStatusToolTip(item.filePath);
  panel_->setTransportEnabled(false);
  panel_->setTimelineDuration(-1);
  panel_->setTimelinePosition(0);
  panel_->setLiveFrame(audioOnly ? generateLiveAudioThumbnail(QSize(960, 540))
                               : uvcSessions_->latestThumbnail(item.filePath));
  resetAudioPresentation();
  panel_->setStatus(QStringLiteral("EDIT | %1").arg(editFileName_));
  updateEditorState();
  timer_.start();
}

void EditSessionController::updateEditorState() {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto editState = playlistModel_->itemState(editItemId_);
  // Playlist selection is only the browser cursor. Once loaded, editItemId_
  // remains the EDIT target while the operator views another item or tab.
  const bool editorAvailable =
      editState && (player_.hasMedia() || !liveDevicePath_.isEmpty()) && editItemId_ != 0;
  const bool editPointsEnabled =
      editorAvailable && !playlistModel_->itemEditPointsLocked(editItemId_);
  const bool otherSettingsEnabled =
      editorAvailable && !playlistModel_->itemSettingsLocked(editItemId_);
  const auto gotoTargets = playlistModel_->snapshot().items;
  QSet<quint64> offlineGotoTargets;
  for (const auto &target : gotoTargets) {
    if (playlistModel_->itemMediaOffline(target.id)) {
      offlineGotoTargets.insert(target.id);
    }
  }
  panel_->setEditorState(
      editState ? std::optional<PlaylistItemSettings>{editState->settings}
                : std::nullopt,
      editPointsEnabled, otherSettingsEnabled,
       gotoTargets, offlineGotoTargets,
       editState && editState->sourceType == PlaylistSourceType::Uvc);
  if (editState && !liveDevicePath_.isEmpty())
    liveAudio_->setSettings(editState->settings);
  if (editState && player_.hasMedia() && liveDevicePath_.isEmpty()) {
    (void)player_.setNominalVolume(
        nominalVolumeGain(editState->settings.nominalVolumeTenthsDb));
    (void)player_.setNominalDelayFrames(editState->settings.nominalAudioDelayFrames);
    const auto eq = editState->settings.nominalEqTenthsDb;
    (void)player_.setNominalEq(eq[0] / 10.0, eq[1] / 10.0, eq[2] / 10.0);
  }
}

void EditSessionController::setSelectedEditPoint(
    bool setInPoint, std::optional<qint64> requestedPositionMs) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  const char *blockedReason = nullptr;
  if (!state) {
    blockedReason = "edit.error.media_not_loaded";
  } else if (playlistModel_->itemEditPointsLocked(editItemId_)) {
    blockedReason = editItemId_ == playlistModel_->onAirItemId() ? "edit.error.points_locked"
        : editItemId_ == playlistModel_->bgmItemId() ? "edit.error.points_bgm_active"
        : state->sourceType != PlaylistSourceType::File ? "edit.error.points_file_only"
        : "edit.error.points_settings_locked";
  } else if (!player_.hasMedia()) {
    blockedReason = "edit.error.media_not_loaded";
  }
  if (blockedReason) {
    panel_->setStatus(vtrpon::LocalizedText(blockedReason), true);
    updateEditorState();
    return;
  }
  const qint64 position = requestedPositionMs ? *requestedPositionMs
                                              : player_.positionMs();
  const qint64 duration = player_.durationMs();
  if (position < 0) {
    panel_->setStatus(QStringLiteral("Could not determine the current EDIT position."),
                      true);
    return;
  }
  if (requestedPositionMs && duration >= 0 && position > duration) {
    panel_->setStatus(
        QStringLiteral("Set the In/Out point within the media duration."), true);
    updateEditorState();
    return;
  }

  PlaylistItemSettings settings = state->settings;
  if (setInPoint) {
    if ((duration >= 0 && position >= duration) ||
        (settings.outPointMs && position >= *settings.outPointMs)) {
      panel_->setStatus(
          QStringLiteral("Set the In point before the Out point and the end of the media."),
          true);
      updateEditorState();
      return;
    }
    settings.inPointMs = position;
  } else {
    if (position <= settings.inPointMs) {
      panel_->setStatus(QStringLiteral("Set the Out point after the In point."),
                        true);
      updateEditorState();
      return;
    }
    settings.outPointMs =
        duration >= 0 ? (std::min)(position, duration) : position;
  }
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the In/Out point."),
                      true);
    updateEditorState();
    return;
  }
  const qint64 savedPoint =
      setInPoint ? settings.inPointMs : *settings.outPointMs;
  panel_->setStatus(
      QStringLiteral("EDIT %1 %2 | %3")
          .arg(setInPoint ? QStringLiteral("In") : QStringLiteral("Out"),
               formatTime(savedPoint), editFileName_));
}

void EditSessionController::setStartMode(PlaylistStartMode mode) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (state && state->sourceType == PlaylistSourceType::Uvc) {
    updateEditorState();
    return;
  }
  if (!state || !player_.hasMedia() ||
      playlistModel_->itemSettingsLocked(editItemId_)) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.startMode = mode;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the Start Mode."),
                      true);
    return;
  }
  panel_->setStatus(QStringLiteral("EDIT Start Mode: %1 | %2")
                        .arg(startModeName(mode), editFileName_));
}

void EditSessionController::toggleStartMode() {
  const auto state = playlistModel_ ? playlistModel_->itemState(editItemId_) : std::nullopt;
  if (state) setStartMode(state->settings.startMode == PlaylistStartMode::Pause
                             ? PlaylistStartMode::Play : PlaylistStartMode::Pause);
}

void EditSessionController::toggleFadeOut() {
  const auto state = playlistModel_ ? playlistModel_->itemState(editItemId_) : std::nullopt;
  if (state) setFadeOut(!state->settings.fadeOutEnabled, state->settings.fadeOutDurationMs);
}

bool EditSessionController::resetEditor() {
  if (shutdown_) return false;
  if (deferItemRemoval(editItemId_, [this] { (void)resetEditor(); })) return true;
  if (!prepareForItemRemoval(editItemId_)) return false;
  setPflEnabled(false);
  emit pflRequested(false);
  return true;
}

void EditSessionController::setEndMode(PlaylistEndMode mode) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || !player_.hasMedia() ||
      playlistModel_->itemSettingsLocked(editItemId_)) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.endMode = mode;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the End Mode."),
                      true);
    return;
  }
  panel_->setStatus(QStringLiteral("EDIT End Mode: %1 | %2")
                        .arg(endModeName(mode), editFileName_));
}

void EditSessionController::setGotoTarget(quint64 itemId) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  const auto target = playlistModel_->itemState(itemId);
  if (!state || !player_.hasMedia() ||
      playlistModel_->itemSettingsLocked(editItemId_) ||
      !usesGotoDestination(state->settings) ||
      (itemId != 0 && (!target || playlistModel_->itemMediaOffline(itemId)))) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.gotoTargetItemId = itemId;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the GOTO destination."),
                      true);
    updateEditorState();
    return;
  }
  const QString destination =
      target ? QStringLiteral("P%1-%2 | %3")
                   .arg(target->playlistNumber)
                   .arg(target->itemNumber, 3, 10, QLatin1Char('0'))
                   .arg(target->displayName)
             : QStringLiteral("None");
  panel_->setStatus(QStringLiteral("EDIT GOTO: %1 | %2")
                        .arg(destination, editFileName_));
}

void EditSessionController::setRepeatOptions(
    int count, PlaylistRepeatEndMode endMode) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || !player_.hasMedia() ||
      playlistModel_->itemSettingsLocked(editItemId_) ||
      state->settings.endMode != PlaylistEndMode::Repeat) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.repeatCount = count;
  settings.repeatEndMode = endMode;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the repeat options."),
                      true);
    updateEditorState();
    return;
  }
  const QString countText = count == kInfiniteItemRepeatCount
                                ? QString(QChar(0x221e))
                                : QString::number(count);
  panel_->setStatus(QStringLiteral("EDIT Repeat: %1 | After: %2 | %3")
                        .arg(countText, repeatEndModeName(endMode),
                             editFileName_));
}

void EditSessionController::setFadeIn(bool enabled, qint64 durationMs) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || (!player_.hasMedia() && liveDevicePath_.isEmpty()) ||
      playlistModel_->itemSettingsLocked(editItemId_)) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.fadeInEnabled = enabled;
  settings.fadeInDurationMs = durationMs;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the fade-in settings."),
                      true);
    updateEditorState();
    return;
  }
  panel_->setStatus(
      QStringLiteral("EDIT Fade In: %1 (%2 s) | %3")
          .arg(enabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
          .arg(durationMs / 1000.0, 0, 'f', 1)
          .arg(editFileName_));
}

void EditSessionController::setFadeOut(bool enabled, qint64 durationMs) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || !player_.hasMedia() ||
      playlistModel_->itemSettingsLocked(editItemId_)) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.fadeOutEnabled = enabled;
  settings.fadeOutDurationMs = durationMs;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the fade-out settings."),
                      true);
    updateEditorState();
    return;
  }
  panel_->setStatus(
      QStringLiteral("EDIT Fade Out: %1 (%2 s) | %3")
          .arg(enabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
          .arg(durationMs / 1000.0, 0, 'f', 1)
          .arg(editFileName_));
}

void EditSessionController::setEndCrossfade(bool enabled,
                                            qint64 durationMs) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || !player_.hasMedia() ||
      playlistModel_->itemSettingsLocked(editItemId_) ||
      !usesAutomaticEndTransition(state->settings)) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.endCrossfadeEnabled = enabled;
  settings.endCrossfadeDurationMs = durationMs;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(
        QStringLiteral("Could not save the end crossfade settings."), true);
    updateEditorState();
    return;
  }
  panel_->setStatus(
      QStringLiteral("EDIT End Crossfade: %1 (%2 s) | %3")
          .arg(enabled ? QStringLiteral("ON") : QStringLiteral("OFF"))
          .arg(durationMs / 1000.0, 0, 'f', 1)
          .arg(editFileName_));
}

void EditSessionController::setNominalDelayFrames(int frames) {
  if (!panel_ || !playlistModel_) return;
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || !player_.hasMedia() || playlistModel_->itemSettingsLocked(editItemId_)) {
    updateEditorState();
    return;
  }
  auto settings = state->settings;
  settings.nominalAudioDelayFrames = frames;
  if (playlistModel_->updateItemSettings(state->id, settings))
    (void)player_.setNominalDelayFrames(frames);
  updateEditorState();
}

void EditSessionController::setNominalEq(int low, int mid, int high) {
  if (!panel_ || !playlistModel_) return;
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || (!player_.hasMedia() && liveDevicePath_.isEmpty()) ||
      playlistModel_->itemSettingsLocked(editItemId_)) {
    updateEditorState();
    return;
  }
  auto settings = state->settings;
  settings.nominalEqTenthsDb = {low, mid, high};
  if (playlistModel_->updateItemSettings(state->id, settings)) {
    if (liveDevicePath_.isEmpty())
      (void)player_.setNominalEq(low / 10.0, mid / 10.0, high / 10.0);
    else
      liveAudio_->setSettings(settings);
  }
  updateEditorState();
}

void EditSessionController::setNominalVolume(int volumeTenthsDb) {
  if (!panel_ || !playlistModel_) {
    return;
  }
  const auto state = playlistModel_->itemState(editItemId_);
  if (!state || (!player_.hasMedia() && liveDevicePath_.isEmpty()) ||
      playlistModel_->itemSettingsLocked(editItemId_)) {
    updateEditorState();
    return;
  }
  PlaylistItemSettings settings = state->settings;
  settings.nominalVolumeTenthsDb = volumeTenthsDb;
  if (!playlistModel_->updateItemSettings(state->id, settings)) {
    panel_->setStatus(QStringLiteral("Could not save the nominal volume."), true);
    updateEditorState();
    return;
  }
  if (liveDevicePath_.isEmpty())
    (void)player_.setNominalVolume(nominalVolumeGain(volumeTenthsDb));
  else
    liveAudio_->setSettings(settings);
  const QString value = volumeTenthsDb == kMinimumItemVolumeTenthsDb
                            ? QString(QChar(0x2212)) + QChar(0x221e)
                            : QStringLiteral("%1 dB")
                                  .arg(volumeTenthsDb / 10.0, 0, 'f', 1);
  panel_->setStatus(
      QStringLiteral("EDIT Nominal Volume: %1 | %2").arg(value, editFileName_));
}

void EditSessionController::runTransport(const QString &name, bool succeeded) {
  if (!panel_) {
    return;
  }
  if (!succeeded) {
    panel_->setStatus(QStringLiteral("EDIT %1 failed.").arg(name),
                      true);
    return;
  }
  panel_->setStatus(QStringLiteral("EDIT %1 | %2").arg(name, editFileName_));
}

void EditSessionController::resetAudioPresentation() {
  inputSignalUntilMs_ = 0;
  inputClipUntilMs_ = 0;
  if (panel_) {
    panel_->resetAudioPresentation();
  }
}

void EditSessionController::updateInputSignalIndicator(
    const EditPlayer::AudioLevel &inputLevel) {
  if (!panel_) {
    return;
  }
  if (!inputSignalClock_.isValid()) {
    inputSignalClock_.start();
  }
  const qint64 now = inputSignalClock_.elapsed();
  if ((std::max)(inputLevel.leftPeakDb, inputLevel.rightPeakDb) >
      kMeterFloorDb) {
    inputSignalUntilMs_ = now + kInputSignalHoldMs;
  }
  if (inputLevel.clipped) {
    inputClipUntilMs_ = now + kInputClipHoldMs;
  }
  panel_->setInputSignalAppearance(now < inputSignalUntilMs_,
                                   now < inputClipUntilMs_);
}

void EditSessionController::stopPlayback() {
  if (!liveDevicePath_.isEmpty()) return;
  if (!stopScanInternal()) {
    return;
  }
  if (!panel_) {
    return;
  }
  if (!player_.stop()) {
    panel_->setStatus(QStringLiteral("Failed to stop EDIT."), true);
    return;
  }
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  seekLockTicks_ = 0;
  panel_->setTimelinePosition(0);
  panel_->setPlaybackState(false);
  panel_->setStatus(QStringLiteral("EDIT Stop | %1").arg(editFileName_));
}

void EditSessionController::requestSeek(qint64 positionMs,
                                        const QString &name) {
  if (!liveDevicePath_.isEmpty()) return;
  if (!stopScanInternal()) {
    return;
  }
  if (!panel_ || !player_.hasMedia()) {
    return;
  }
  const qint64 duration = player_.durationMs();
  const qint64 target =
      duration >= 0 ? std::clamp(positionMs, qint64{0}, duration)
                    : (std::max)(positionMs, qint64{0});
  pendingSeekMs_ = target;
  pendingSeekAccurate_ = false;
  panel_->setTimelinePosition(target);
  panel_->setStatus(QStringLiteral("EDIT %1 | %2").arg(name, editFileName_));
}

void EditSessionController::startScan(int direction) {
  if (!panel_ || !player_.hasMedia() || direction == 0) {
    return;
  }
  const int normalizedDirection = direction < 0 ? -1 : 1;
  if (scanDirection_ == normalizedDirection) {
    return;
  }
  if (!stopScanInternal()) {
    return;
  }

  const qint64 position = player_.positionMs();
  const qint64 duration = player_.durationMs();
  if (position < 0 || duration <= 0) {
    panel_->setStatus(QStringLiteral("Could not determine the EDIT scan position."), true);
    return;
  }
  if ((normalizedDirection < 0 && position <= 0)
      || (normalizedDirection > 0 && position >= duration)) {
    panel_->setTimelinePosition(normalizedDirection < 0 ? 0 : duration);
    panel_->setStatus(
        QStringLiteral("EDIT %1 | %2")
            .arg(normalizedDirection < 0 ? QStringLiteral("Start")
                                         : QStringLiteral("End"),
                 editFileName_));
    return;
  }

  resumeAfterScan_ = player_.isPlaying();
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  seekLockTicks_ = 0;
  suppressScanEosTicks_ = 0;

  if (normalizedDirection < 0) {
    if (resumeAfterScan_ && !player_.pause()) {
      resumeAfterScan_ = false;
      panel_->setStatus(QStringLiteral("Failed to prepare EDIT for rewind."),
                        true);
      appendEditScanLog(QStringLiteral("start failed direction=-1"));
      return;
    }
    scanDirection_ = -1;
    reverseScanBasePositionMs_ = position;
    reverseScanPositionMs_ = position;
    reverseScanElapsed_.restart();
    appendEditScanLog(
        QStringLiteral("start direction=-1 pos=%1 rate=%2 playing=%3 "
                       "mode=timeline")
            .arg(position)
            .arg(kScanRate, 0, 'f', 1)
            .arg(resumeAfterScan_));
    resetAudioPresentation();
    panel_->setStatus(
        QStringLiteral("EDIT Rewind (position only) | %1").arg(editFileName_));
    return;
  }

  if (!player_.startForwardScan(kScanRate)) {
    resumeAfterScan_ = false;
    panel_->setStatus(QStringLiteral("EDIT scan failed."), true);
    appendEditScanLog(QStringLiteral("start failed direction=1"));
    return;
  }
  scanDirection_ = 1;
  appendEditScanLog(
      QStringLiteral("start direction=1 pos=%1 rate=%2 playing=%3 mode=video")
          .arg(position)
          .arg(kScanRate, 0, 'f', 1)
          .arg(resumeAfterScan_));
  panel_->setStatus(QStringLiteral("EDIT Fast-forward | %1").arg(editFileName_));
}

bool EditSessionController::stopScanInternal(bool boundaryReached) {
  if (scanDirection_ == 0) {
    return true;
  }

  const int direction = scanDirection_;
  const bool shouldResume = resumeAfterScan_ && !boundaryReached;
  qint64 position = -1;
  bool restored = false;
  if (direction < 0) {
    position = boundaryReached
        ? 0
        : reverseScanPositionMs_.value_or(reverseScanBasePositionMs_);
    restored = player_.seek(position);
    if (restored) {
      restored = shouldResume ? player_.play() : player_.pause();
    }
  } else {
    restored = player_.stopForwardScan(shouldResume, boundaryReached,
                                       position);
  }
  appendEditScanLog(
      QStringLiteral("stop direction=%1 pos=%2 resume=%3 boundary=%4 ok=%5")
          .arg(direction)
          .arg(position)
          .arg(shouldResume)
          .arg(boundaryReached)
          .arg(restored));

  suppressScanEosTicks_ = direction > 0 ? 8 : 0;
  scanDirection_ = 0;
  resumeAfterScan_ = false;
  reverseScanPositionMs_.reset();
  reverseScanBasePositionMs_ = 0;
  pendingSeekMs_.reset();
  seekLockMs_.reset();
  seekLockTicks_ = 0;
  if (position >= 0 && panel_) {
    panel_->setTimelinePosition(position);
  }
  if (direction < 0 && position >= 0) {
    seekLockMs_ = position;
    seekLockTicks_ = 10;
  }
  if (!restored) {
    if (panel_) {
      panel_->setStatus(QStringLiteral("Failed to restore normal EDIT playback."),
                        true);
    }
    return false;
  }
  if (panel_) {
    resetAudioPresentation();
    panel_->setStatus(
        QStringLiteral("EDIT %1 | %2")
            .arg(boundaryReached
                     ? QStringLiteral("Scan boundary")
                     : direction < 0 ? QStringLiteral("Rewind stopped")
                                     : QStringLiteral("Fast-forward stopped"),
                 editFileName_));
  }
  return true;
}

void EditSessionController::updateUi() {
  if (!panel_ || (!player_.isAvailable() && liveDevicePath_.isEmpty())) {
    return;
  }
  if (player_.isAvailable()) player_.processBusMessages();
  if (!liveDevicePath_.isEmpty() && removalPendingId_ == 0) {
    if (liveAudioNeedsStart_ && !player_.isBusy() && !playlistModel_->itemMediaOffline(editItemId_)) {
      liveAudioNeedsStart_ = false;
      const auto state = playlistModel_->itemState(editItemId_);
      if (state) {
        const QString audioDeviceId = vtrpon::isLiveAudioSource(liveDevicePath_)
            ? vtrpon::liveAudioDeviceId(liveDevicePath_)
            : (vtrpon::isNetworkVideoSource(liveDevicePath_) || vtrpon::isDeckLinkSource(liveDevicePath_))
            ? liveDevicePath_
            : uvcAudioMappings_ ? uvcAudioMappings_->audioDeviceIdFor(liveDevicePath_)
                                : QString();
        QString error;
        if (!liveAudio_->start(audioDeviceId, state->settings, &error)) {
          panel_->setStatusToolTip(error);
          panel_->setStatus(vtrpon::LocalizedText(
              vtrpon::isLiveAudioSource(liveDevicePath_) ? "input.audio.error" : vtrpon::isDeckLinkSource(liveDevicePath_) ? "decklink.input_error" : vtrpon::isNetworkVideoSource(liveDevicePath_)
                  ? "network_video.input_error" : "uvc.input_error"), true);
        }
      }
    }
    QString audioError;
    liveAudio_->processBus(&audioError);
    if (!audioError.isEmpty()) {
      panel_->setStatusToolTip(audioError);
      panel_->setStatus(vtrpon::LocalizedText(
          vtrpon::isLiveAudioSource(liveDevicePath_) ? "input.audio.error" : vtrpon::isDeckLinkSource(liveDevicePath_) ? "decklink.input_error" : vtrpon::isNetworkVideoSource(liveDevicePath_)
              ? "network_video.input_error" : "uvc.input_error"), true);
    }
    const auto output = liveAudio_->outputLevel();
    panel_->setAudioLevel(output.leftPeakDb, output.rightPeakDb, output.clipped);
    updateInputSignalIndicator(liveAudio_->inputLevel());
    return;
  }
  if (removalPendingId_ != 0) {
    if (player_.isBusy()) return;
    const auto id = removalPendingId_;
    removalPendingId_ = 0;
    auto continuations = std::move(removalContinuations_);
    removalContinuations_.clear();
    if (prepareForItemRemoval(id)) {
      for (const auto &continuation : continuations) continuation();
    }
    return;
  }
  if (loadPending_) {
    if (player_.isBusy()) return;
    loadPending_ = false;
    panel_->setTransportEnabled(player_.hasMedia());
    updateEditorState();
    if (player_.hasMedia()) panel_->setStatus(QStringLiteral("EDIT loaded | %1").arg(editFileName_));
  }
  panel_->setPlaybackState(scanDirection_ != 0 || player_.isPlaying());
  if (pendingSeekMs_) {
    const qint64 target = *pendingSeekMs_;
    const bool accurate = pendingSeekAccurate_;
    pendingSeekMs_.reset();
    pendingSeekAccurate_ = false;
    if (accurate ? player_.seekAccurate(target) : player_.seek(target)) {
      seekLockMs_ = target;
      seekLockTicks_ = accurate ? kFrameSeekLockTicks
                                : kOrdinarySeekLockTicks;
      seekLockToleranceMs_ = accurate
          ? (std::max)(qint64{2}, player_.frameDurationMs() / 3)
          : kOrdinarySeekToleranceMs;
    } else {
      panel_->setStatus(QStringLiteral("EDIT seek failed."), true);
    }
  }
  const qint64 duration = player_.durationMs();
  if (duration >= 0) {
    playlistModel_->setKnownMediaDuration(editItemId_, duration);
    panel_->setTimelineDuration(duration);
  }
  if (scanDirection_ < 0) {
    QString scanEventDetail;
    const EditPlayer::Event scanEvent = player_.takeEvent(scanEventDetail);
    if (scanEvent == EditPlayer::Event::Error) {
      pendingSeekMs_.reset();
      seekLockMs_.reset();
      seekLockTicks_ = 0;
      reverseScanPositionMs_.reset();
      reverseScanBasePositionMs_ = 0;
      scanDirection_ = 0;
      resumeAfterScan_ = false;
      panel_->setTransportEnabled(player_.hasMedia());
      updateEditorState();
      panel_->setStatusToolTip(scanEventDetail);
      panel_->setStatus(QStringLiteral("EDIT ERROR | Only EDIT playback was stopped"),
                        true);
      return;
    }
    const qint64 elapsedMs =
        reverseScanElapsed_.isValid() ? reverseScanElapsed_.elapsed() : 0;
    const qint64 deltaMs = static_cast<qint64>(elapsedMs * kScanRate);
    const qint64 reversePosition = (std::max)(
        reverseScanBasePositionMs_ - deltaMs, qint64{0});
    reverseScanPositionMs_ = reversePosition;
    panel_->setTimelinePosition(reversePosition);
    if (reversePosition <= 0) {
      (void)stopScanInternal(true);
      return;
    }
    resetAudioPresentation();
    return;
  }
  if (scanDirection_ != 0) {
    QString scanEventDetail;
    const EditPlayer::Event scanEvent = player_.takeEvent(scanEventDetail);
    if (scanEvent == EditPlayer::Event::Error) {
      const int failedDirection = scanDirection_;
      qint64 recoveryPosition = -1;
      const bool recovered = player_.stopForwardScan(
          false, false, recoveryPosition);
      appendEditScanLog(
          QStringLiteral("error direction=%1 pos=%2 recovery=%3")
              .arg(failedDirection)
              .arg(recoveryPosition)
              .arg(recovered));
      pendingSeekMs_.reset();
      seekLockMs_.reset();
      seekLockTicks_ = 0;
      scanDirection_ = 0;
      resumeAfterScan_ = false;
      if (recoveryPosition >= 0) {
        panel_->setTimelinePosition(recoveryPosition);
      }
      panel_->setTransportEnabled(player_.hasMedia());
      updateEditorState();
      panel_->setStatusToolTip(scanEventDetail);
      panel_->setStatus(QStringLiteral("EDIT ERROR | Only EDIT playback was stopped"),
                        true);
      return;
    }
    if (scanEvent == EditPlayer::Event::ScanBoundary
        || scanEvent == EditPlayer::Event::EndOfStream) {
      const qint64 eventPosition = player_.positionMs();
      appendEditScanLog(
          QStringLiteral("boundary direction=%1 pos=%2 duration=%3")
              .arg(scanDirection_)
              .arg(eventPosition)
              .arg(duration));
      (void)stopScanInternal(true);
      return;
    }
    const qint64 scanPosition = player_.positionMs();
    if (!panel_->isTimelineSliderDown() && scanPosition >= 0) {
      panel_->setTimelinePosition(scanPosition);
    }
    resetAudioPresentation();
    return;
  }
  const qint64 position = player_.positionMs();
  if (!panel_->isTimelineSliderDown() && position >= 0) {
    if (seekLockMs_
        && qAbs(position - *seekLockMs_) > seekLockToleranceMs_ &&
        --seekLockTicks_ > 0) {
      panel_->setTimelinePosition(*seekLockMs_);
    } else {
      seekLockMs_.reset();
      panel_->setTimelinePosition(position);
    }
  }
  const EditPlayer::AudioLevel level = player_.audioLevel();
  panel_->setAudioLevel(level.leftPeakDb, level.rightPeakDb, level.clipped);
  updateInputSignalIndicator(player_.inputAudioLevel());

  QString detail;
  const EditPlayer::Event event = player_.takeEvent(detail);
  if (event == EditPlayer::Event::Error) {
    pendingSeekMs_.reset();
    seekLockMs_.reset();
    seekLockTicks_ = 0;
    panel_->setTransportEnabled(player_.hasMedia());
    updateEditorState();
    panel_->setStatusToolTip(detail);
    panel_->setStatus(QStringLiteral("EDIT ERROR | Only EDIT playback was stopped"),
                      true);
  } else if (event == EditPlayer::Event::EndOfStream) {
    const qint64 eventPosition = player_.positionMs();
    const qint64 eventDuration = player_.durationMs();
    if (eventPosition >= 0 && eventDuration > 0
        && eventPosition < eventDuration - kEndOfStreamToleranceMs) {
      appendEditScanLog(
          QStringLiteral("ignore stale eos pos=%1 duration=%2")
              .arg(eventPosition)
              .arg(eventDuration));
      suppressScanEosTicks_ = 0;
      return;
    }
    if (suppressScanEosTicks_ > 0) {
      --suppressScanEosTicks_;
      return;
    }
    stopPlayback();
  }
  if (suppressScanEosTicks_ > 0) {
    --suppressScanEosTicks_;
  }
}
