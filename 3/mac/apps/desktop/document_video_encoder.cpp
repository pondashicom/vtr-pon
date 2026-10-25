#include "document_video_encoder.h"
#include <QDebug>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace {

constexpr int kVideoWidth = 1920;
constexpr int kVideoHeight = 1080;
constexpr int kVideoFramesPerSecond = 30;
constexpr qint64 kMaximumVideoDurationMs = 24LL * 60LL * 60LL * 1000LL;
constexpr guint64 kVideoFrameBytes =
    static_cast<guint64>(kVideoWidth) * kVideoHeight * 4;
constexpr guint64 kAppSourceQueueLimitBytes = kVideoFrameBytes * 4;
constexpr qint64 kAppSourceStallTimeoutMs = 120000;

enum class VideoFramePushResult { Pushed, Canceled, Failed };

void setEncoderIntegerPropertyIfPresent(GstElement *element,
                                        const char *name, gint64 value) {
  if (!element) {
    return;
  }
  GParamSpec *property =
      g_object_class_find_property(G_OBJECT_GET_CLASS(element), name);
  if (!property) {
    return;
  }
  GValue typedValue = G_VALUE_INIT;
  g_value_init(&typedValue, G_PARAM_SPEC_VALUE_TYPE(property));
  if (G_VALUE_HOLDS_INT(&typedValue)) {
    g_value_set_int(&typedValue, static_cast<gint>(value));
  } else if (G_VALUE_HOLDS_UINT(&typedValue)) {
    g_value_set_uint(&typedValue, static_cast<guint>(value));
  } else if (G_VALUE_HOLDS_INT64(&typedValue)) {
    g_value_set_int64(&typedValue, value);
  } else if (G_VALUE_HOLDS_UINT64(&typedValue)) {
    g_value_set_uint64(&typedValue, static_cast<guint64>(value));
  } else if (G_VALUE_HOLDS_ENUM(&typedValue)) {
    g_value_set_enum(&typedValue, static_cast<gint>(value));
  } else {
    g_value_unset(&typedValue);
    return;
  }
  (void)g_param_value_validate(property, &typedValue);
  g_object_set_property(G_OBJECT(element), name, &typedValue);
  g_value_unset(&typedValue);
}

void setEncoderBooleanPropertyIfPresent(GstElement *element,
                                        const char *name, bool value) {
  if (!element) {
    return;
  }
  GParamSpec *property =
      g_object_class_find_property(G_OBJECT_GET_CLASS(element), name);
  if (!property || G_PARAM_SPEC_VALUE_TYPE(property) != G_TYPE_BOOLEAN) {
    return;
  }
  g_object_set(G_OBJECT(element), name, value ? TRUE : FALSE, nullptr);
}

QString takeGStreamerError(GstMessage *message) {
  GError *error = nullptr;
  gchar *debug = nullptr;
  gst_message_parse_error(message, &error, &debug);
  qWarning().noquote() << "GST_BUS"
                       << (error ? error->message : "Unknown error")
                       << (debug ? debug : "");
  const QString detail = QString::fromUtf8(
      error ? error->message : "Unknown GStreamer encoding error");
  g_clear_error(&error);
  g_free(debug);
  return detail;
}

VideoFramePushResult waitForAppSourceCapacity(
    GstElement *appSource, GstBus *bus,
    const std::atomic_bool *cancelRequested, QString *errorDetail) {
  QElapsedTimer stallTimer;
  stallTimer.start();
  while (true) {
    if (cancelRequested &&
        cancelRequested->load(std::memory_order_acquire)) {
      return VideoFramePushResult::Canceled;
    }
    GstMessage *message =
        bus ? gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR)
            : nullptr;
    if (message) {
      if (errorDetail) {
        *errorDetail = takeGStreamerError(message);
      }
      gst_message_unref(message);
      return VideoFramePushResult::Failed;
    }
    if (gst_app_src_get_current_level_bytes(GST_APP_SRC(appSource)) <
        kAppSourceQueueLimitBytes) {
      return VideoFramePushResult::Pushed;
    }
    if (stallTimer.elapsed() >= kAppSourceStallTimeoutMs) {
      if (errorDetail) {
        *errorDetail = QStringLiteral(
            "The H.264 encoder stopped consuming document frames.");
      }
      return VideoFramePushResult::Failed;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

VideoFramePushResult pushVideoFrame(
    GstElement *appSource, GstBus *bus, const QImage &image,
    qint64 frameIndex, const std::atomic_bool *cancelRequested,
    QString *errorDetail) {
  if (!appSource || image.width() != kVideoWidth ||
      image.height() != kVideoHeight ||
      image.format() != QImage::Format_ARGB32) {
    if (errorDetail) {
      *errorDetail = QStringLiteral("The video frame has an invalid format.");
    }
    return VideoFramePushResult::Failed;
  }
  const VideoFramePushResult capacity = waitForAppSourceCapacity(
      appSource, bus, cancelRequested, errorDetail);
  if (capacity != VideoFramePushResult::Pushed) {
    return capacity;
  }
  const gsize byteCount = static_cast<gsize>(image.sizeInBytes());
  GstBuffer *buffer = gst_buffer_new_allocate(nullptr, byteCount, nullptr);
  if (!buffer ||
      gst_buffer_fill(buffer, 0, image.constBits(), byteCount) != byteCount) {
    if (buffer) {
      gst_buffer_unref(buffer);
    }
    if (errorDetail) {
      *errorDetail = QStringLiteral("Could not allocate a video frame.");
    }
    return VideoFramePushResult::Failed;
  }
  const GstClockTime presentationTime = gst_util_uint64_scale(
      static_cast<guint64>(frameIndex), GST_SECOND,
      kVideoFramesPerSecond);
  const GstClockTime nextPresentationTime = gst_util_uint64_scale(
      static_cast<guint64>(frameIndex + 1), GST_SECOND,
      kVideoFramesPerSecond);
  GST_BUFFER_PTS(buffer) = presentationTime;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = nextPresentationTime - presentationTime;
  const GstFlowReturn flow =
      gst_app_src_push_buffer(GST_APP_SRC(appSource), buffer);
  if (flow != GST_FLOW_OK) {
    if (errorDetail) {
      *errorDetail = QStringLiteral(
          "The H.264 encoder stopped accepting document frames.");
    }
    return cancelRequested &&
                   cancelRequested->load(std::memory_order_acquire)
               ? VideoFramePushResult::Canceled
               : VideoFramePushResult::Failed;
  }
  return VideoFramePushResult::Pushed;
}

} // namespace

std::optional<DocumentVideoFramePlan> makeDocumentVideoFramePlan(
    int pageCount, const DocumentVideoSettings &settings,
    QString *errorDetail) {
  if (errorDetail) {
    errorDetail->clear();
  }
  const auto fail = [errorDetail](const QString &detail) {
    if (errorDetail) {
      *errorDetail = detail;
    }
    return std::optional<DocumentVideoFramePlan>{};
  };
  if (pageCount <= 0) {
    return fail(QStringLiteral("At least one document page is required."));
  }
  if (!std::isfinite(settings.pageDurationSeconds) ||
      settings.pageDurationSeconds < 0.1 ||
      settings.pageDurationSeconds > 600.0) {
    return fail(QStringLiteral("The page duration is invalid."));
  }
  if (settings.crossDissolveEnabled &&
      (!std::isfinite(settings.crossDissolveDurationSeconds) ||
       settings.crossDissolveDurationSeconds < 0.1 ||
       settings.crossDissolveDurationSeconds >
           settings.pageDurationSeconds)) {
    return fail(QStringLiteral(
        "Cross Dissolve duration must not exceed the page duration."));
  }

  DocumentVideoFramePlan plan;
  plan.framesPerSecond = kVideoFramesPerSecond;
  plan.framesPerPage = (std::max)(
      1, static_cast<int>(std::lround(settings.pageDurationSeconds *
                                      kVideoFramesPerSecond)));
  plan.transitionFrames = settings.crossDissolveEnabled
                              ? (std::min)(
                                    plan.framesPerPage,
                                    (std::max)(
                                        1, static_cast<int>(std::lround(
                                               settings.crossDissolveDurationSeconds *
                                               kVideoFramesPerSecond))))
                              : 0;
  plan.totalFrames = static_cast<qint64>(pageCount) * plan.framesPerPage;
  plan.durationMs = static_cast<qint64>(std::llround(
      static_cast<long double>(plan.totalFrames) * 1000.0L /
      kVideoFramesPerSecond));
  if (plan.durationMs <= 0 || plan.durationMs > kMaximumVideoDurationMs) {
    return fail(QStringLiteral(
        "The converted video would exceed the 24-hour duration limit."));
  }
  return plan;
}

DocumentVideoEncodeResult encodeDocumentVideo(
    const QStringList &paths, const QString &outputPath,
    const DocumentVideoSettings &settings,
    const std::atomic_bool *cancelRequested,
    const DocumentVideoProgress &progress) {
  DocumentVideoEncodeResult result;
  QString planError;
  const auto plan =
      makeDocumentVideoFramePlan(paths.size(), settings, &planError);
  if (!plan) {
    result.errorDetail = planError;
    return result;
  }
  if (cancelRequested &&
      cancelRequested->load(std::memory_order_acquire)) {
    result.canceled = true;
    return result;
  }

  GstElement *pipeline = gst_pipeline_new("document-video-encoder");
  GstElement *source = gst_element_factory_make("appsrc", "source");
  GstElement *convert = gst_element_factory_make("videoconvert", "convert");
  GstElement *rawCapsFilter =
      gst_element_factory_make("capsfilter", "encoder-input-format");
#ifdef __APPLE__
  // Use the LGPL applemedia bridge to the system VideoToolbox codec only.
  // Do not fall back to an automatically selected encoder (for example x264).
  GstElement *encoder = gst_element_factory_make("vtenc_h264", "encoder");
#else
  GstElement *encoder = gst_element_factory_make("mfh264enc", "encoder");
  const bool usingMediaFoundation = encoder != nullptr;
  if (!encoder) {
    encoder = gst_element_factory_make("openh264enc", "encoder");
  }
#endif
  GstElement *parser = gst_element_factory_make("h264parse", "parser");
  GstElement *h264CapsFilter =
      gst_element_factory_make("capsfilter", "mp4-h264-format");
  GstElement *muxer = gst_element_factory_make("mp4mux", "muxer");
  GstElement *sink = gst_element_factory_make("filesink", "sink");
  if (!pipeline || !source || !convert || !rawCapsFilter || !encoder ||
      !parser || !h264CapsFilter || !muxer || !sink) {
    result.errorDetail = QStringLiteral(
        "The GStreamer H.264/MP4 encoder components are unavailable. "
#ifdef __APPLE__
        "Apple VideoToolbox H.264 (vtenc_h264), h264parse and mp4mux are required.");
#else
        "Media Foundation H.264 or OpenH264 is required.");
#endif
    for (GstElement *element : {source, convert, rawCapsFilter, encoder,
                                parser, h264CapsFilter, muxer, sink}) {
      if (element) {
        gst_object_unref(element);
      }
    }
    if (pipeline) {
      gst_object_unref(pipeline);
    }
    return result;
  }

  g_object_set(G_OBJECT(source), "is-live", FALSE, "format", GST_FORMAT_TIME,
               "block", FALSE, "max-bytes", kAppSourceQueueLimitBytes,
               nullptr);
  const QByteArray outputPathUtf8 =
      QDir::toNativeSeparators(outputPath).toUtf8();
  g_object_set(G_OBJECT(sink), "location", outputPathUtf8.constData(),
               "sync", FALSE, nullptr);
#ifdef __APPLE__
  setEncoderIntegerPropertyIfPresent(encoder, "bitrate", 5000);
  setEncoderIntegerPropertyIfPresent(encoder, "max-keyframe-interval", 120);
  setEncoderBooleanPropertyIfPresent(encoder, "allow-frame-reordering", false);
#else
  if (usingMediaFoundation) {
    setEncoderIntegerPropertyIfPresent(encoder, "bitrate", 5000);
    setEncoderIntegerPropertyIfPresent(encoder, "gop-size", 120);
    setEncoderIntegerPropertyIfPresent(encoder, "quality-vs-speed", 67);
    setEncoderIntegerPropertyIfPresent(encoder, "rc-mode", 0);
    setEncoderBooleanPropertyIfPresent(encoder, "low-latency", true);
  } else {
    setEncoderIntegerPropertyIfPresent(encoder, "bitrate", 5000000);
    setEncoderIntegerPropertyIfPresent(encoder, "max-bitrate", 5000000);
    setEncoderIntegerPropertyIfPresent(encoder, "gop-size", 120);
    setEncoderIntegerPropertyIfPresent(encoder, "rate-control", 1);
    setEncoderIntegerPropertyIfPresent(encoder, "usage-type", 1);
    setEncoderBooleanPropertyIfPresent(encoder, "enable-frame-skip", false);
  }
#endif
  const auto *encoderFactory = gst_element_get_factory(encoder);
  qInfo() << "DOCUMENT_ENCODER" << gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(encoderFactory));
  setEncoderIntegerPropertyIfPresent(parser, "config-interval", -1);
  setEncoderBooleanPropertyIfPresent(muxer, "faststart", true);

  GstCaps *rawEncoderCaps = gst_caps_new_simple(
      "video/x-raw", "format", G_TYPE_STRING,
#ifdef __APPLE__
      "NV12", nullptr);
  if (rawEncoderCaps) gst_caps_set_simple(rawEncoderCaps, "colorimetry", G_TYPE_STRING, "bt709", nullptr);
#else
      usingMediaFoundation ? "NV12" : "I420", nullptr);
#endif
  GstCaps *h264Caps = gst_caps_from_string(
      "video/x-h264,stream-format=avc,alignment=au");
  if (!rawEncoderCaps || !h264Caps) {
    result.errorDetail =
        QStringLiteral("Could not create the H.264 encoder format.");
    if (rawEncoderCaps) {
      gst_caps_unref(rawEncoderCaps);
    }
    if (h264Caps) {
      gst_caps_unref(h264Caps);
    }
    for (GstElement *element : {source, convert, rawCapsFilter, encoder,
                                parser, h264CapsFilter, muxer, sink}) {
      gst_object_unref(element);
    }
    gst_object_unref(pipeline);
    return result;
  }
  g_object_set(G_OBJECT(rawCapsFilter), "caps", rawEncoderCaps, nullptr);
  g_object_set(G_OBJECT(h264CapsFilter), "caps", h264Caps, nullptr);
  gst_caps_unref(rawEncoderCaps);
  gst_caps_unref(h264Caps);

  GstCaps *caps = gst_caps_from_string(
      "video/x-raw,format=BGRA,width=1920,height=1080,"
      "framerate=30/1,pixel-aspect-ratio=1/1");
  if (!caps) {
    result.errorDetail =
        QStringLiteral("Could not create the document video format.");
    for (GstElement *element : {source, convert, rawCapsFilter, encoder,
                                parser, h264CapsFilter, muxer, sink}) {
      gst_object_unref(element);
    }
    gst_object_unref(pipeline);
    return result;
  }
  gst_app_src_set_caps(GST_APP_SRC(source), caps);
  gst_caps_unref(caps);
  gst_app_src_set_duration(
      GST_APP_SRC(source),
      gst_util_uint64_scale(static_cast<guint64>(plan->totalFrames),
                            GST_SECOND, kVideoFramesPerSecond));

  gst_bin_add_many(GST_BIN(pipeline), source, convert, rawCapsFilter, encoder,
                   parser, h264CapsFilter, muxer, sink, nullptr);
  if (!gst_element_link_many(source, convert, rawCapsFilter, encoder, parser,
                             h264CapsFilter, muxer, sink, nullptr)) {
    result.errorDetail =
        QStringLiteral("Could not link the H.264/MP4 encoding pipeline.");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return result;
  }
  GstBus *bus = gst_element_get_bus(pipeline);
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    GstMessage *message = bus ? gst_bus_timed_pop_filtered(
                                    bus, 0, GST_MESSAGE_ERROR)
                              : nullptr;
    result.errorDetail = message
                             ? takeGStreamerError(message)
                             : QStringLiteral(
                                   "Could not start the H.264/MP4 encoding pipeline.");
    if (message) {
      gst_message_unref(message);
    }
  }

  qint64 frameIndex = 0;
  QImage current;
  if (result.errorDetail.isEmpty()) {
    current =
        QImage(paths.constFirst()).convertToFormat(QImage::Format_ARGB32);
    if (current.size() != QSize(kVideoWidth, kVideoHeight)) {
      result.errorDetail = QStringLiteral(
          "A document page image is not the required 1920x1080 size.");
    }
  }

  for (int pageIndex = 0;
       result.errorDetail.isEmpty() && pageIndex < paths.size();
       ++pageIndex) {
    if (cancelRequested &&
        cancelRequested->load(std::memory_order_acquire)) {
      result.canceled = true;
      break;
    }
    QImage next;
    const bool hasNext = pageIndex + 1 < paths.size();
    if (hasNext) {
      next = QImage(paths.at(pageIndex + 1))
                 .convertToFormat(QImage::Format_ARGB32);
      if (next.size() != QSize(kVideoWidth, kVideoHeight)) {
        result.errorDetail = QStringLiteral(
            "A document page image is not the required 1920x1080 size.");
        break;
      }
    }

    const int transitionFrames = hasNext ? plan->transitionFrames : 0;
    const int holdFrames = plan->framesPerPage - transitionFrames;
    for (int frame = 0; frame < holdFrames; ++frame) {
      if (cancelRequested &&
          cancelRequested->load(std::memory_order_acquire)) {
        result.canceled = true;
        break;
      }
      const VideoFramePushResult pushResult = pushVideoFrame(
          source, bus, current, frameIndex, cancelRequested,
          &result.errorDetail);
      if (pushResult == VideoFramePushResult::Canceled) {
        result.canceled = true;
        break;
      }
      if (pushResult != VideoFramePushResult::Pushed) {
        break;
      }
      ++frameIndex;
      if (progress) {
        progress(frameIndex, plan->totalFrames);
      }
    }
    for (int frame = 0;
         !result.canceled && result.errorDetail.isEmpty() &&
         frame < transitionFrames;
         ++frame) {
      if (cancelRequested &&
          cancelRequested->load(std::memory_order_acquire)) {
        result.canceled = true;
        break;
      }
      QImage blended = current.copy();
      {
        QPainter painter(&blended);
        painter.setOpacity(static_cast<qreal>(frame + 1) /
                           transitionFrames);
        painter.drawImage(0, 0, next);
      }
      const VideoFramePushResult pushResult = pushVideoFrame(
          source, bus, blended, frameIndex, cancelRequested,
          &result.errorDetail);
      if (pushResult == VideoFramePushResult::Canceled) {
        result.canceled = true;
        break;
      }
      if (pushResult != VideoFramePushResult::Pushed) {
        break;
      }
      ++frameIndex;
      if (progress) {
        progress(frameIndex, plan->totalFrames);
      }
    }
    if (result.canceled || !result.errorDetail.isEmpty()) {
      break;
    }
    if (hasNext) {
      current = std::move(next);
    }
  }

  if (!result.canceled && result.errorDetail.isEmpty() &&
      frameIndex == plan->totalFrames) {
    if (gst_app_src_end_of_stream(GST_APP_SRC(source)) != GST_FLOW_OK) {
      result.errorDetail =
          QStringLiteral("Could not finish the H.264 video stream.");
    } else {
      QElapsedTimer completionTimer;
      completionTimer.start();
      while (result.errorDetail.isEmpty() && !result.succeeded &&
             completionTimer.elapsed() < 120000) {
        if (cancelRequested &&
            cancelRequested->load(std::memory_order_acquire)) {
          result.canceled = true;
          break;
        }
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 250 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS |
                                        GST_MESSAGE_ERROR));
        if (!message) {
          continue;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
          result.succeeded = true;
        } else {
          result.errorDetail = takeGStreamerError(message);
        }
        gst_message_unref(message);
      }
      if (!result.succeeded && !result.canceled &&
          result.errorDetail.isEmpty()) {
        result.errorDetail =
            QStringLiteral("The H.264/MP4 encoder did not finish in time.");
      }
    }
  }

  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (bus) {
    gst_object_unref(bus);
  }
  gst_object_unref(pipeline);
  if (result.succeeded) {
    result.durationMs = plan->durationMs;
    result.outputPath = QFileInfo(outputPath).absoluteFilePath();
  } else {
    QFile::remove(outputPath);
  }
  return result;
}
