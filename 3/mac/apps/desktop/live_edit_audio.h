#pragma once

#include "edit_player.h"
#include "playlist_item.h"
#include "edit_pfl_output.h"
#include "edit_pfl_protocol.h"
#include "async_pipeline_start.h"
#include "async_pipeline_stop.h"
#include "audio_effects.h"
#include "live_capture_audio.h"
#include "network_video.h"
#include "decklink.h"
#include <gst/gst.h>
#include <QDeadlineTimer>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

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
    if (pipeline_ && audioDeviceId_ == audioDeviceId) {
      setSettings(settings);
      return true;
    }
    stop();
#ifdef __APPLE__
    std::erase_if(stops_, [](const auto &job) { return job->succeeded(); });
    if (!stops_.empty()) {
      if (detail) *detail = QStringLiteral("The previous EDIT audio input is still stopping or could not stop safely. Retry after it stops.");
      return false;
    }
#endif
    if (audioDeviceId.isEmpty()) return true;
    if (!vtrpon::registerAudioEffects()) {
      if (detail) *detail = QStringLiteral("EDIT audio effects are unavailable.");
      return false;
    }
    const bool network = (vtrpon::isNetworkVideoSource(audioDeviceId) || vtrpon::isDeckLinkSource(audioDeviceId));
    if (!network && !vtrpon::registerLiveCaptureAudio()) {
      if (detail) *detail = QStringLiteral("The shared RAW audio input is unavailable.");
      return false;
    }
    const QString input = network
        ? QStringLiteral("appsrc name=edit-live-source is-live=true format=time do-timestamp=true block=false max-buffers=0 max-bytes=0 max-time=200000000 leaky-type=downstream ! ")
        : vtrpon::liveCaptureAudioInput(QStringLiteral("edit-live-source"));
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
#ifdef __APPLE__
    start_ = AsyncPipelineStart::start(pipeline_);
    startDeadline_.setRemainingTime(10000);
#else
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      if (detail) *detail = QStringLiteral("Could not start the EDIT live audio input.");
      stop();
      return false;
    }
#endif
    if (vtrpon::isDeckLinkSource(audioDeviceId))
      deckLinkReceiver_ = std::make_shared<vtrpon::DeckLinkReceiver>(audioDeviceId, source_, true);
    else if (network)
      networkReceiver_ = std::make_shared<vtrpon::NetworkVideoReceiver>(
          audioDeviceId, source_, true);
    feed_->setPlaying(true);
    audioDeviceId_ = audioDeviceId;
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
    audioDeviceId_.clear();
    if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
    if (pipeline_) {
      stops_.push_back(AsyncPipelineStop::start(pipeline_, {},
#ifdef __APPLE__
          AsyncPipelineStart::retire(start_)
#else
          {}
#endif
      ));
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    input_ = {};
    output_ = {};
    lastInputLevelNs_ = 0;
    lastOutputLevelNs_ = 0;
  }

  bool active() const { return pipeline_ != nullptr; }
  bool readyToStart() const {
    return std::all_of(stops_.begin(), stops_.end(),
                       [](const auto &job) { return job->done(); });
  }
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
#ifdef __APPLE__
    if (start_ && ((!start_->done() && startDeadline_.hasExpired()) ||
                   (start_->done() && !start_->succeeded()))) {
      if (error) *error = QStringLiteral("Could not open the selected Mac audio input. Check device and microphone permission.");
    }
#endif
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
  QString audioDeviceId_;
  std::shared_ptr<TapState> tap_;
#ifdef __APPLE__
  std::shared_ptr<AsyncPipelineStart> start_;
  QDeadlineTimer startDeadline_;
#endif
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
