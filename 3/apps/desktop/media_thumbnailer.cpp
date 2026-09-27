#include "media_thumbnailer.h"
#include "uvc_input_guard.h"
#include <QDebug>

#if defined(VTRPON_POWERPOINT_SUPPORT) && defined(_WIN32)
#include "powerpoint_source_session_manager.h"
#endif
#if defined(VTRPON_PDF_SUPPORT)
#include "pdf_source_session_manager.h"
#endif

#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QUrl>
#include <QVector>
#include <QtLogging>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace {

constexpr int kMinimumAudioWaveformSampleRate = 2000;
constexpr int kPreferredAudioWaveformSampleRate = 48000;
constexpr qint64 kMaximumAudioWaveformSamples = 16 * 1000 * 1000;
constexpr int kAudioWaveformSupersampling = 2;
constexpr qint64 kMaximumWaveformDurationMs = 2 * 60 * 60 * 1000;
constexpr qint64 kMaximumWaveformGenerationMs = 30000;
constexpr GstClockTime kPipelineStartupTimeout = 5 * GST_SECOND;
constexpr GstClockTime kPipelineSampleTimeout = 5 * GST_SECOND;

struct GstObjectDeleter {
  void operator()(GstElement *element) const {
    if (element) {
      gst_object_unref(element);
    }
  }
};

struct GstSampleDeleter {
  void operator()(GstSample *sample) const {
    if (sample) {
      gst_sample_unref(sample);
    }
  }
};

struct GstMessageDeleter {
  void operator()(GstMessage *message) const {
    if (message) {
      gst_message_unref(message);
    }
  }
};

struct PipelineDeleter {
  void operator()(GstElement *pipeline) const {
    if (!pipeline) {
      return;
    }
    if (gst_element_set_state(pipeline, GST_STATE_NULL) ==
        GST_STATE_CHANGE_FAILURE) {
      qCritical() << "Thumbnail pipeline refused NULL state; retaining it to "
                     "avoid freeing a live GStreamer backend.";
      return;
    }
    GstState currentState = GST_STATE_VOID_PENDING;
    if (gst_element_get_state(pipeline, &currentState, nullptr,
                              2 * GST_SECOND) != GST_STATE_CHANGE_SUCCESS ||
        currentState != GST_STATE_NULL) {
      qCritical() << "Thumbnail pipeline did not confirm NULL state; retaining "
                     "it to avoid freeing a live GStreamer backend.";
      return;
    }
    gst_object_unref(pipeline);
  }
};

using GstElementPtr = std::unique_ptr<GstElement, GstObjectDeleter>;
using GstMessagePtr = std::unique_ptr<GstMessage, GstMessageDeleter>;
using GstPipelinePtr = std::unique_ptr<GstElement, PipelineDeleter>;
using GstSamplePtr = std::unique_ptr<GstSample, GstSampleDeleter>;

bool cancellationRequested(const std::atomic_bool *cancelRequested) {
  return cancelRequested &&
         cancelRequested->load(std::memory_order_acquire);
}

int audioWaveformSampleRate(std::optional<qint64> durationMs) {
  if (!durationMs || *durationMs <= 0) {
    return kPreferredAudioWaveformSampleRate;
  }
  const qint64 durationSeconds = (std::max<qint64>)(1, *durationMs / 1000);
  const qint64 boundedRate = kMaximumAudioWaveformSamples / durationSeconds;
  return static_cast<int>(std::clamp<qint64>(
      boundedRate, kMinimumAudioWaveformSampleRate,
      kPreferredAudioWaveformSampleRate));
}

GstStateChangeReturn waitForState(
    GstElement *pipeline, GstClockTime timeout,
    const std::atomic_bool *cancelRequested) {
  QElapsedTimer elapsed;
  elapsed.start();
  const qint64 timeoutMs = static_cast<qint64>(timeout / GST_MSECOND);
  while (elapsed.elapsed() < timeoutMs) {
    if (cancellationRequested(cancelRequested)) {
      return GST_STATE_CHANGE_FAILURE;
    }
    const GstStateChangeReturn stateResult = gst_element_get_state(
        pipeline, nullptr, nullptr, GST_MSECOND * 100);
    if (stateResult != GST_STATE_CHANGE_ASYNC) {
      return stateResult;
    }
  }
  return GST_STATE_CHANGE_ASYNC;
}

GstSample *pullSampleWithCancellation(
    GstAppSink *sink, bool preroll, GstClockTime timeout,
    const std::atomic_bool *cancelRequested) {
  QElapsedTimer elapsed;
  elapsed.start();
  const qint64 timeoutMs = static_cast<qint64>(timeout / GST_MSECOND);
  while (elapsed.elapsed() < timeoutMs && !gst_app_sink_is_eos(sink)) {
    if (cancellationRequested(cancelRequested)) {
      return nullptr;
    }
    GstSample *sample = preroll
        ? gst_app_sink_try_pull_preroll(sink, GST_MSECOND * 100)
        : gst_app_sink_try_pull_sample(sink, GST_MSECOND * 100);
    if (sample) {
      return sample;
    }
  }
  return nullptr;
}

QString takeGErrorDetail(GError *&error) {
  if (!error) {
    return QString();
  }
  const QString detail = QString::fromUtf8(error->message);
  g_clear_error(&error);
  return detail;
}

QString pipelineErrorDetail(GstElement *pipeline,
                            const QString &fallbackDetail) {
  if (!pipeline) {
    return fallbackDetail;
  }
  GstBus *bus = gst_element_get_bus(pipeline);
  if (!bus) {
    return fallbackDetail;
  }
  GstMessagePtr message(gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR));
  gst_object_unref(bus);
  if (!message) {
    return fallbackDetail;
  }

  GError *error = nullptr;
  gchar *debug = nullptr;
  gst_message_parse_error(message.get(), &error, &debug);
  qWarning().noquote() << "GST_BUS"
                       << (error ? error->message : "Unknown error")
                       << (debug ? debug : "");
  const QString detail = takeGErrorDetail(error);
  g_free(debug);
  return detail.isEmpty() ? fallbackDetail : detail;
}

QImage createTypeCard(const QString &filePath, PlaylistMediaType mediaType,
                      const QSize &targetSize, bool failed = false) {
  QImage image(targetSize, QImage::Format_RGBA8888);
  image.fill(failed ? QColor(QStringLiteral("#3d2023")) : Qt::black);

  QString typeName;
  switch (mediaType) {
  case PlaylistMediaType::Video:
    typeName = QStringLiteral("VIDEO");
    break;
  case PlaylistMediaType::Audio:
    typeName = QStringLiteral("AUDIO");
    break;
  case PlaylistMediaType::StillImage:
    typeName = QStringLiteral("IMAGE");
    break;
  case PlaylistMediaType::Presentation:
    typeName = QStringLiteral("PPTX");
    break;
  case PlaylistMediaType::Pdf:
    typeName = QStringLiteral("PDF");
    break;
  case PlaylistMediaType::Unknown:
    typeName = QStringLiteral("MEDIA");
    break;
  }

  const QString extension = QFileInfo(filePath).suffix().toUpper();
  QPainter painter(&image);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setPen(failed ? QColor(QStringLiteral("#ffc1c4"))
                        : QColor(QStringLiteral("#d4dadd")));
  QFont font = painter.font();
  font.setBold(true);
  font.setPointSizeF(8.0);
  painter.setFont(font);
  painter.drawText(image.rect().adjusted(3, 3, -3, -15),
                   Qt::AlignCenter, failed ? QStringLiteral("Failed")
                                           : typeName);
  font.setBold(false);
  font.setPointSizeF(6.5);
  painter.setFont(font);
  painter.setPen(QColor(QStringLiteral("#879095")));
  painter.drawText(image.rect().adjusted(3, 3, -3, -3),
                   Qt::AlignHCenter | Qt::AlignBottom,
                   extension.isEmpty() ? typeName : extension);
  return image;
}

MediaThumbnailResult failedResult(const QString &filePath,
                                  PlaylistMediaType mediaType,
                                  const QSize &targetSize,
                                  const QString &detail) {
  MediaThumbnailResult result;
  result.image = createTypeCard(filePath, mediaType, targetSize, true);
  result.mediaType = mediaType;
  result.errorDetail = detail;
  return result;
}

bool configureSourceUri(GstElement *pipeline, const QString &filePath,
                        QString &errorDetail) {
  GstElementPtr source(gst_bin_get_by_name(GST_BIN(pipeline), "source"));
  if (!source) {
    errorDetail = QStringLiteral("Could not create the thumbnail input.");
    return false;
  }
  const QByteArray uri = QUrl::fromLocalFile(filePath).toEncoded();
  g_object_set(source.get(), "uri", uri.constData(), nullptr);
  return true;
}

GstPipelinePtr createPipeline(const QByteArray &description,
                              QString &errorDetail) {
  GError *error = nullptr;
  GstPipelinePtr pipeline(gst_parse_launch(description.constData(), &error));
  errorDetail = takeGErrorDetail(error);
  if (!pipeline && errorDetail.isEmpty()) {
    errorDetail = QStringLiteral("Could not create the thumbnail pipeline.");
  }
  return pipeline;
}

QImage imageFromVideoSample(GstSample *sample, QString &errorDetail) {
  GstCaps *caps = gst_sample_get_caps(sample);
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstVideoInfo videoInfo;
  gst_video_info_init(&videoInfo);
  if (!caps || !buffer || !gst_video_info_from_caps(&videoInfo, caps)) {
    errorDetail = QStringLiteral("Could not determine the video frame format.");
    return QImage();
  }

  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, &videoInfo, buffer, GST_MAP_READ)) {
    errorDetail = QStringLiteral("Could not read the video frame.");
    return QImage();
  }

  const int width = static_cast<int>(GST_VIDEO_INFO_WIDTH(&videoInfo));
  const int height = static_cast<int>(GST_VIDEO_INFO_HEIGHT(&videoInfo));
  QImage image(width, height, QImage::Format_RGBA8888);
  const auto *source = static_cast<const uchar *>(
      GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
  const int sourceStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  const qsizetype bytesPerRow = (std::min)(
      image.bytesPerLine(), static_cast<qsizetype>(std::abs(sourceStride)));
  for (int row = 0; row < height; ++row) {
    std::memcpy(image.scanLine(row), source + row * sourceStride,
                static_cast<std::size_t>(bytesPerRow));
  }
  gst_video_frame_unmap(&frame);
  return image;
}

QString sourceCodecName(GstElement *sink, const char *codecTag) {
  // The decoded caps describe raw pixels/samples, so read the source codec
  // from the stream tags retained by the existing thumbnail pipeline.
  GstPad *pad = gst_element_get_static_pad(sink, "sink");
  if (!pad) return {};
  QString codec;
  for (guint index = 0; ; ++index) {
    GstEvent *event = gst_pad_get_sticky_event(pad, GST_EVENT_TAG, index);
    if (!event) break;
    GstTagList *tags = nullptr;
    gst_event_parse_tag(event, &tags);
    gchar *value = nullptr;
    if (gst_tag_list_get_string(tags, codecTag, &value)) {
      codec = QString::fromUtf8(value).trimmed();
      g_free(value);
    }
    gst_event_unref(event);
    if (!codec.isEmpty()) break;
  }
  gst_object_unref(pad);
  if (codec.startsWith(QStringLiteral("H.264"))) return QStringLiteral("H.264");
  if (codec.startsWith(QStringLiteral("H.265"))) return QStringLiteral("H.265");
  if (codec.startsWith(QStringLiteral("MPEG-4 AAC"))) return QStringLiteral("AAC");
  if (codec.startsWith(QStringLiteral("Uncompressed")) &&
      codec.contains(QStringLiteral("PCM"))) return QStringLiteral("PCM");
  return codec;
}

MediaThumbnailResult generateVideoThumbnail(const QString &filePath,
                                            const QSize &targetSize,
                                            const std::atomic_bool *cancelRequested) {
  const QByteArray pipelineDescription = QStringLiteral(
      "uridecodebin name=source ! queue max-size-buffers=2 ! videoconvert ! "
      "videoscale name=thumbnail_scale add-borders=true ! "
      "video/x-raw,format=RGBA,width=%1,height=%2,pixel-aspect-ratio=1/1 ! "
      "appsink name=thumbnail_sink sync=false max-buffers=1 drop=true")
                                             .arg(targetSize.width())
                                             .arg(targetSize.height())
                                             .toUtf8();
  QString errorDetail;
  GstPipelinePtr pipeline = createPipeline(pipelineDescription, errorDetail);
  if (!pipeline || !configureSourceUri(pipeline.get(), filePath, errorDetail)) {
    return failedResult(filePath, PlaylistMediaType::Video, targetSize,
                        errorDetail);
  }

  GstElementPtr sinkElement(
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "thumbnail_sink"));
  if (!sinkElement) {
    return failedResult(
        filePath, PlaylistMediaType::Video, targetSize,
        QStringLiteral("Could not create the video thumbnail output."));
  }
  auto *sink = GST_APP_SINK(sinkElement.get());

  if (gst_element_set_state(pipeline.get(), GST_STATE_PAUSED) ==
      GST_STATE_CHANGE_FAILURE) {
    return failedResult(
        filePath, PlaylistMediaType::Video, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Could not open the video.")));
  }
  const GstStateChangeReturn stateResult = waitForState(
      pipeline.get(), kPipelineStartupTimeout, cancelRequested);
  if (stateResult == GST_STATE_CHANGE_FAILURE ||
      stateResult == GST_STATE_CHANGE_ASYNC) {
    return failedResult(
        filePath, PlaylistMediaType::Video, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Failed to load the video.")));
  }

  gint64 duration = GST_CLOCK_TIME_NONE;
  if (gst_element_query_duration(pipeline.get(), GST_FORMAT_TIME, &duration) &&
      GST_CLOCK_TIME_IS_VALID(duration) && duration > 0) {
    const gint64 target = (std::min)(GST_SECOND,
        (std::max)(GST_MSECOND * 100, duration / 10));
    (void)gst_element_seek_simple(
        pipeline.get(), GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
        target);
  }

  GstSamplePtr sample(pullSampleWithCancellation(
      sink, true, kPipelineSampleTimeout, cancelRequested));
  if (!sample) {
    (void)gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
    sample.reset(pullSampleWithCancellation(
        sink, false, kPipelineSampleTimeout, cancelRequested));
  }
  if (!sample) {
    return failedResult(
        filePath, PlaylistMediaType::Video, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Could not retrieve a video frame.")));
  }

  QImage image = imageFromVideoSample(sample.get(), errorDetail);
  if (image.isNull()) {
    return failedResult(filePath, PlaylistMediaType::Video, targetSize,
                        errorDetail);
  }

  MediaThumbnailResult result;
  result.image = std::move(image);
  result.mediaType = PlaylistMediaType::Video;
  result.succeeded = true;
  result.sourceCodec = sourceCodecName(sinkElement.get(), GST_TAG_VIDEO_CODEC);
  if (GST_CLOCK_TIME_IS_VALID(duration) && duration > 0) {
    result.sourceDurationMs = duration / GST_MSECOND;
  }
  // Read the negotiated input size before thumbnail scaling.
  GstElementPtr scale(gst_bin_get_by_name(GST_BIN(pipeline.get()),
                                         "thumbnail_scale"));
  GstPad *scaleSink = scale ? gst_element_get_static_pad(scale.get(), "sink") : nullptr;
  GstCaps *sourceCaps = scaleSink ? gst_pad_get_current_caps(scaleSink) : nullptr;
  GstVideoInfo sourceInfo;
  gst_video_info_init(&sourceInfo);
  if (sourceCaps && gst_video_info_from_caps(&sourceInfo, sourceCaps)) {
    result.sourceSize = QSize(GST_VIDEO_INFO_WIDTH(&sourceInfo),
                              GST_VIDEO_INFO_HEIGHT(&sourceInfo));
  }
  if (sourceCaps) gst_caps_unref(sourceCaps);
  if (scaleSink) gst_object_unref(scaleSink);
  return result;
}

MediaThumbnailResult generateUvcThumbnailInternal(
    const QString &devicePath, const QSize &targetSize,
    const std::atomic_bool *cancelRequested) {
  QString rejection;
  if (!vtrpon::uvcInputAllowed(devicePath, &rejection)) {
    return failedResult(devicePath, PlaylistMediaType::Video, targetSize, rejection);
  }
  const QByteArray pipelineDescription = QStringLiteral(
      "mfvideosrc name=source do-timestamp=true ! decodebin ! "
      "queue max-size-buffers=2 leaky=downstream ! videoconvert ! "
      "videoscale add-borders=true ! "
      "video/x-raw,format=RGBA,width=%1,height=%2,pixel-aspect-ratio=1/1 ! "
      "appsink name=thumbnail_sink sync=false max-buffers=1 drop=true")
                                             .arg(targetSize.width())
                                             .arg(targetSize.height())
                                             .toUtf8();
  QString errorDetail;
  GstPipelinePtr pipeline = createPipeline(pipelineDescription, errorDetail);
  if (!pipeline) {
    return failedResult(devicePath, PlaylistMediaType::Video, targetSize,
                        errorDetail);
  }
  GstElementPtr source(
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "source"));
  GstElementPtr sinkElement(
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "thumbnail_sink"));
  if (!source || !sinkElement || !GST_IS_APP_SINK(sinkElement.get())) {
    return failedResult(
        devicePath, PlaylistMediaType::Video, targetSize,
        QStringLiteral("Could not create the UVC thumbnail input or output."));
  }
  const QByteArray encodedPath = devicePath.toUtf8();
  g_object_set(source.get(), "device-path", encodedPath.constData(), nullptr);

  if (gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    return failedResult(
        devicePath, PlaylistMediaType::Video, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Could not open the UVC device.")));
  }
  const GstStateChangeReturn stateResult = waitForState(
      pipeline.get(), kPipelineStartupTimeout, cancelRequested);
  if (stateResult == GST_STATE_CHANGE_FAILURE ||
      stateResult == GST_STATE_CHANGE_ASYNC) {
    return failedResult(
        devicePath, PlaylistMediaType::Video, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Failed to start the UVC device.")));
  }

  GstSamplePtr sample(pullSampleWithCancellation(
      GST_APP_SINK(sinkElement.get()), false, kPipelineSampleTimeout,
      cancelRequested));
  if (!sample) {
    return failedResult(
        devicePath, PlaylistMediaType::Video, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Could not retrieve a UVC frame.")));
  }
  QImage image = imageFromVideoSample(sample.get(), errorDetail);
  if (image.isNull()) {
    return failedResult(devicePath, PlaylistMediaType::Video, targetSize,
                        errorDetail);
  }

  MediaThumbnailResult result;
  result.image = std::move(image);
  result.mediaType = PlaylistMediaType::Video;
  result.succeeded = true;
  return result;
}

MediaThumbnailResult generateAudioThumbnail(
    const QString &filePath, std::optional<qint64> durationMs,
    const QSize &targetSize, const std::atomic_bool *cancelRequested) {
  if (durationMs && *durationMs > kMaximumWaveformDurationMs) {
    MediaThumbnailResult result;
    result.image = createTypeCard(filePath, PlaylistMediaType::Audio,
                                  targetSize);
    result.mediaType = PlaylistMediaType::Audio;
    result.succeeded = true;
    return result;
  }

  const int waveformSampleRate = audioWaveformSampleRate(durationMs);
  const QByteArray pipelineDescription = QStringLiteral(
      "uridecodebin name=source ! queue max-size-buffers=16 ! audioconvert ! "
      "audioresample ! "
      "audio/x-raw,format=F32LE,channels=1,rate=%1,layout=interleaved ! "
      "appsink name=thumbnail_sink sync=false max-buffers=64 drop=false")
                                             .arg(waveformSampleRate)
                                             .toUtf8();
  QString errorDetail;
  GstPipelinePtr pipeline = createPipeline(pipelineDescription, errorDetail);
  if (!pipeline || !configureSourceUri(pipeline.get(), filePath, errorDetail)) {
    return failedResult(filePath, PlaylistMediaType::Audio, targetSize,
                        errorDetail);
  }

  GstElementPtr sinkElement(
      gst_bin_get_by_name(GST_BIN(pipeline.get()), "thumbnail_sink"));
  if (!sinkElement) {
    return failedResult(
        filePath, PlaylistMediaType::Audio, targetSize,
        QStringLiteral("Could not create the audio waveform output."));
  }
  auto *sink = GST_APP_SINK(sinkElement.get());

  if (gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    return failedResult(
        filePath, PlaylistMediaType::Audio, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Could not open the audio.")));
  }
  const GstStateChangeReturn stateResult = waitForState(
      pipeline.get(), kPipelineStartupTimeout, cancelRequested);
  if (stateResult == GST_STATE_CHANGE_FAILURE ||
      stateResult == GST_STATE_CHANGE_ASYNC) {
    return failedResult(
        filePath, PlaylistMediaType::Audio, targetSize,
        pipelineErrorDetail(pipeline.get(),
                            QStringLiteral("Failed to load the audio.")));
  }

  gint64 durationNs = GST_CLOCK_TIME_NONE;
  gint64 sourceDurationNs = GST_CLOCK_TIME_NONE;
  (void)gst_element_query_duration(pipeline.get(), GST_FORMAT_TIME,
                                   &sourceDurationNs);
  if (durationMs && *durationMs > 0) {
    durationNs = *durationMs * GST_MSECOND;
  } else {
    (void)gst_element_query_duration(pipeline.get(), GST_FORMAT_TIME,
                                     &durationNs);
  }
  if (!GST_CLOCK_TIME_IS_VALID(durationNs) || durationNs <= 0) {
    return failedResult(
        filePath, PlaylistMediaType::Audio, targetSize,
        QStringLiteral("Could not determine the audio duration."));
  }

  const QSize waveformRenderSize = targetSize * kAudioWaveformSupersampling;
  QVector<float> representativeSamples(targetSize.width(), 0.0F);
  QVector<bool> hasRepresentativeSample(targetSize.width(), false);
  bool receivedSamples = false;
  QElapsedTimer elapsed;
  elapsed.start();
  quint64 fallbackSampleOffset = 0;
  while (elapsed.elapsed() < kMaximumWaveformGenerationMs) {
    if (cancellationRequested(cancelRequested)) {
      return failedResult(
          filePath, PlaylistMediaType::Audio, targetSize,
          QStringLiteral("Audio waveform generation was canceled."));
    }
    GstSamplePtr sample(
        gst_app_sink_try_pull_sample(sink, GST_MSECOND * 250));
    if (!sample) {
      if (gst_app_sink_is_eos(sink)) {
        break;
      }
      GstBus *bus = gst_element_get_bus(pipeline.get());
      GstMessagePtr errorMessage(
          bus ? gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR) : nullptr);
      if (bus) {
        gst_object_unref(bus);
      }
      if (errorMessage) {
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(errorMessage.get(), &error, &debug);
        qWarning().noquote()
            << "GST_BUS" << (error ? error->message : "Unknown error")
            << (debug ? debug : "");
        errorDetail = takeGErrorDetail(error);
        g_free(debug);
        break;
      }
      continue;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample.get());
    GstMapInfo map = GST_MAP_INFO_INIT;
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      continue;
    }
    receivedSamples = true;
    const auto *samples = reinterpret_cast<const float *>(map.data);
    const quint64 sampleCount = map.size / sizeof(float);
    const GstClockTime bufferPts = GST_BUFFER_PTS(buffer);
    for (quint64 sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
      const long double absoluteSampleIndex =
          static_cast<long double>(fallbackSampleOffset + sampleIndex);
      const long double sampleTime =
          GST_CLOCK_TIME_IS_VALID(bufferPts)
              ? static_cast<long double>(bufferPts) +
                        static_cast<long double>(sampleIndex) * GST_SECOND /
                        waveformSampleRate
              : absoluteSampleIndex * GST_SECOND /
                    waveformSampleRate;
      const int column = std::clamp(
          static_cast<int>(sampleTime * targetSize.width() / durationNs), 0,
          targetSize.width() - 1);
      if (!hasRepresentativeSample[column]) {
        representativeSamples[column] = std::isfinite(samples[sampleIndex])
                                            ? samples[sampleIndex] : 0.0F;
        hasRepresentativeSample[column] = true;
      }
    }
    fallbackSampleOffset += sampleCount;
    gst_buffer_unmap(buffer, &map);
  }

  if (!receivedSamples || !errorDetail.isEmpty() ||
      elapsed.elapsed() >= kMaximumWaveformGenerationMs) {
    if (errorDetail.isEmpty()) {
      errorDetail = elapsed.elapsed() >= kMaximumWaveformGenerationMs
                        ? QStringLiteral("Audio waveform generation timed out.")
                        : QStringLiteral("Could not retrieve audio samples.");
    }
    return failedResult(filePath, PlaylistMediaType::Audio, targetSize,
                        errorDetail);
  }

  QImage renderedWaveform(waveformRenderSize, QImage::Format_RGBA8888);
  renderedWaveform.fill(Qt::black);
  QPainter waveformPainter(&renderedWaveform);
  waveformPainter.setRenderHint(QPainter::Antialiasing, true);
  waveformPainter.setPen(
      QPen(QColor(QStringLiteral("#254d2c")), kAudioWaveformSupersampling));
  const int center = waveformRenderSize.height() / 2;
  waveformPainter.drawLine(0, center, waveformRenderSize.width() - 1, center);
  QPen waveformPen(QColor(QStringLiteral("#4db858")),
                   kAudioWaveformSupersampling, Qt::SolidLine, Qt::FlatCap,
                   Qt::RoundJoin);
  waveformPainter.setPen(waveformPen);
  const int maximumAmplitude = (std::max)(1, center);
  QPainterPath waveformPath;
  float previousSample = 0.0F;
  for (int column = 0; column < representativeSamples.size(); ++column) {
    if (hasRepresentativeSample[column]) {
      previousSample = representativeSamples[column];
    }
    const int amplitude = qRound(
        std::clamp(previousSample, -1.0F, 1.0F) * maximumAmplitude);
    const QPointF point(column * kAudioWaveformSupersampling,
                        center - amplitude);
    if (column == 0) {
      waveformPath.moveTo(point);
    } else {
      waveformPath.lineTo(point);
    }
  }
  waveformPainter.drawPath(waveformPath);
  waveformPainter.end();

  QImage image = renderedWaveform.scaled(
      targetSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  QPainter painter(&image);

  QFont extensionFont = painter.font();
  extensionFont.setPointSizeF(6.5);
  painter.setFont(extensionFont);
  painter.setPen(QColor(QStringLiteral("#aeb6b9")));
  painter.drawText(image.rect().adjusted(3, 3, -3, -3),
                   Qt::AlignHCenter | Qt::AlignBottom,
                   QFileInfo(filePath).suffix().toUpper());

  MediaThumbnailResult result;
  result.image = std::move(image);
  result.mediaType = PlaylistMediaType::Audio;
  result.succeeded = true;
  result.sourceCodec = sourceCodecName(sinkElement.get(), GST_TAG_AUDIO_CODEC);
  if (GST_CLOCK_TIME_IS_VALID(sourceDurationNs) && sourceDurationNs > 0) {
    result.sourceDurationMs = sourceDurationNs / GST_MSECOND;
  }
  return result;
}

MediaThumbnailResult generateStillImageThumbnail(const QString &filePath,
                                                 const QSize &targetSize,
                                                 const std::atomic_bool *cancelRequested) {
  if (cancellationRequested(cancelRequested)) {
    return failedResult(
        filePath, PlaylistMediaType::StillImage, targetSize,
        QStringLiteral("Still-image thumbnail generation was canceled."));
  }
  QImageReader reader(filePath);
  reader.setAutoTransform(true);
  const QSize sourceSize = reader.size();
  if (sourceSize.isValid()) {
    reader.setScaledSize(sourceSize.scaled(targetSize * 2,
                                           Qt::KeepAspectRatio));
  }
  const QImage source = reader.read();
  if (source.isNull()) {
    return failedResult(
        filePath, PlaylistMediaType::StillImage, targetSize,
        reader.errorString().isEmpty()
            ? QStringLiteral("Could not read the still image.")
            : reader.errorString());
  }

  const QImage scaled = source.scaled(targetSize, Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
  QImage image(targetSize, QImage::Format_RGBA8888);
  image.fill(Qt::black);
  QPainter painter(&image);
  painter.drawImage((targetSize.width() - scaled.width()) / 2,
                    (targetSize.height() - scaled.height()) / 2, scaled);

  MediaThumbnailResult result;
  result.image = std::move(image);
  result.mediaType = PlaylistMediaType::StillImage;
  result.succeeded = true;
  result.sourceSize = sourceSize.isValid() ? sourceSize : source.size();
  return result;
}

MediaThumbnailResult generateMediaThumbnailInternal(
    const QString &filePath, PlaylistMediaType mediaType,
    std::optional<qint64> durationMs, const QSize &targetSize,
    const std::atomic_bool *cancelRequested) {
  if (filePath.isEmpty() || targetSize.width() <= 0 ||
      targetSize.height() <= 0) {
    return failedResult(
        filePath, mediaType, QSize((std::max)(1, targetSize.width()),
                                  (std::max)(1, targetSize.height())),
        QStringLiteral("The thumbnail request is invalid."));
  }

  switch (mediaType) {
  case PlaylistMediaType::Video:
    return generateVideoThumbnail(filePath, targetSize, cancelRequested);
  case PlaylistMediaType::Audio:
    return generateAudioThumbnail(filePath, durationMs, targetSize,
                                  cancelRequested);
  case PlaylistMediaType::StillImage:
    return generateStillImageThumbnail(filePath, targetSize,
                                       cancelRequested);
  case PlaylistMediaType::Presentation:
    {
#if defined(VTRPON_POWERPOINT_SUPPORT) && defined(_WIN32)
      if (cancellationRequested(cancelRequested)) {
        return failedResult(filePath, mediaType, targetSize,
                            QStringLiteral("Thumbnail generation was canceled."));
      }
      QString errorDetail;
      QImage image =
          generatePowerPointThumbnail(filePath, targetSize, &errorDetail);
      if (image.isNull()) {
        if (isPowerPointNativeSourceActiveError(errorDetail)) {
          // Native Program playback has priority over Office automation. This
          // is a temporary availability condition, not a broken playlist
          // item, so keep a neutral PPTX card instead of caching Failed.
          MediaThumbnailResult result;
          result.image = createTypeCard(filePath, mediaType, targetSize);
          result.mediaType = PlaylistMediaType::Presentation;
          result.succeeded = true;
          return result;
        }
        return failedResult(
            filePath, mediaType, targetSize,
            errorDetail.isEmpty()
                ? QStringLiteral("Could not export the first PowerPoint slide.")
                : errorDetail);
      }
      MediaThumbnailResult result;
      result.image = std::move(image);
      result.mediaType = PlaylistMediaType::Presentation;
      result.succeeded = true;
      return result;
#else
      MediaThumbnailResult result;
      result.image = createTypeCard(filePath, mediaType, targetSize);
      result.mediaType = PlaylistMediaType::Presentation;
      result.succeeded = true;
      return result;
#endif
    }
  case PlaylistMediaType::Pdf:
    {
#if defined(VTRPON_PDF_SUPPORT)
      if (cancellationRequested(cancelRequested)) {
        return failedResult(filePath, mediaType, targetSize,
                            QStringLiteral("Thumbnail generation was canceled."));
      }
      QString errorDetail;
      QImage image = generatePdfThumbnail(filePath, targetSize,
                                          &errorDetail);
      if (image.isNull()) {
        return failedResult(
            filePath, mediaType, targetSize,
            errorDetail.isEmpty()
                ? QStringLiteral("Could not render the first PDF page.")
                : errorDetail);
      }
      MediaThumbnailResult result;
      result.image = std::move(image);
      result.mediaType = PlaylistMediaType::Pdf;
      result.succeeded = true;
      return result;
#else
      MediaThumbnailResult result;
      result.image = createTypeCard(filePath, mediaType, targetSize);
      result.mediaType = PlaylistMediaType::Pdf;
      result.succeeded = true;
      return result;
#endif
    }
  case PlaylistMediaType::Unknown:
    break;
  }
  return failedResult(filePath, mediaType, targetSize,
                      QStringLiteral("Could not determine the media type."));
}

} // namespace

MediaThumbnailResult generateMediaThumbnail(
    const QString &filePath, PlaylistMediaType mediaType,
    std::optional<qint64> durationMs, const QSize &targetSize) {
  return generateMediaThumbnailInternal(filePath, mediaType, durationMs,
                                        targetSize, nullptr);
}

MediaThumbnailResult generateUvcThumbnail(const QString &devicePath,
                                          const QSize &targetSize) {
  return generateUvcThumbnailInternal(devicePath, targetSize, nullptr);
}

MediaThumbnailer::MediaThumbnailer(QObject *parent) : QObject(parent) {
  workerPool_.setMaxThreadCount(2);
  workerPool_.setExpiryTimeout(5000);
}

MediaThumbnailer::~MediaThumbnailer() {
  requestShutdown();
  workerPool_.waitForDone(); // Already complete after the asynchronous close handshake.
}

void MediaThumbnailer::requestShutdown() {
  shuttingDown_.store(true, std::memory_order_release);
  cancelAllThumbnails();
  workerPool_.clear();
}

void MediaThumbnailer::requestThumbnail(
    quint64 itemId, const QString &filePath, PlaylistMediaType mediaType,
    std::optional<qint64> durationMs) {
  if (itemId == 0 || shuttingDown_.load(std::memory_order_acquire)) {
    return;
  }
  const auto pending = pendingThumbnails_.value(itemId);
  if (pending && pending->filePath == filePath &&
      pending->mediaType == mediaType && pending->durationMs == durationMs) {
    return;
  }
  cancelThumbnail(itemId);
  auto request = std::make_shared<PendingThumbnail>();
  request->filePath = filePath;
  request->mediaType = mediaType;
  request->durationMs = durationMs;
  pendingThumbnails_.insert(itemId, request);
  workerPool_.start([this, itemId, request] {
    if (request->canceled.load(std::memory_order_acquire)) {
      return;
    }
    MediaThumbnailResult result = generateMediaThumbnailInternal(
        request->filePath, request->mediaType, request->durationMs,
        QSize(120, 68), &request->canceled);
    if (request->canceled.load(std::memory_order_acquire)) {
      return;
    }
    QMetaObject::invokeMethod(
        this,
        [this, itemId, request, result = std::move(result)] {
          if (pendingThumbnails_.value(itemId) != request) {
            return;
          }
          pendingThumbnails_.remove(itemId);
          emit thumbnailReady(itemId, result);
        },
        Qt::QueuedConnection);
  });
}

void MediaThumbnailer::cancelThumbnail(quint64 itemId) {
  if (const auto request = pendingThumbnails_.take(itemId)) {
    request->canceled.store(true, std::memory_order_release);
  }
}

void MediaThumbnailer::cancelAllThumbnails() {
  for (const auto &request : pendingThumbnails_) {
    request->canceled.store(true, std::memory_order_release);
  }
  pendingThumbnails_.clear();
}

void MediaThumbnailer::requestUvcThumbnail(quint64 requestId,
                                           const QString &devicePath) {
  if (requestId == 0 || devicePath.isEmpty() ||
      shuttingDown_.load(std::memory_order_acquire)) {
    return;
  }
  workerPool_.start([this, requestId, devicePath] {
    MediaThumbnailResult result = generateUvcThumbnailInternal(
        devicePath, QSize(120, 68), &shuttingDown_);
    if (shuttingDown_.load(std::memory_order_acquire)) {
      return;
    }
    QMetaObject::invokeMethod(
        this,
        [this, requestId, result = std::move(result)] {
          if (!shuttingDown_.load(std::memory_order_acquire)) {
            emit thumbnailReady(requestId, result);
          }
        },
        Qt::QueuedConnection);
  });
}

bool MediaThumbnailer::shutdownComplete() { return workerPool_.waitForDone(0); }
