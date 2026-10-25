#include "media_duration_probe.h"

#include <QUrl>

#include <gst/pbutils/gstdiscoverer.h>

namespace {

constexpr GstClockTime kDiscoveryTimeout = 5 * GST_SECOND;

QString discovererResultDetail(GstDiscovererResult result) {
  switch (result) {
  case GST_DISCOVERER_URI_INVALID:
    return QStringLiteral("The media URI is invalid.");
  case GST_DISCOVERER_ERROR:
    return QStringLiteral("An error occurred while analyzing the media.");
  case GST_DISCOVERER_TIMEOUT:
    return QStringLiteral("Timed out while determining the media duration.");
  case GST_DISCOVERER_BUSY:
    return QStringLiteral("Could not start media analysis.");
  case GST_DISCOVERER_MISSING_PLUGINS:
    return QStringLiteral("A plugin required for media analysis is missing.");
  case GST_DISCOVERER_OK:
    break;
  }
  return QStringLiteral("Could not determine the media duration.");
}

QString takeErrorDetail(GError *&error) {
  if (!error) {
    return QString();
  }
  const QString detail = QString::fromUtf8(error->message);
  g_clear_error(&error);
  return detail;
}

PlaylistMediaType discoverMediaType(GstDiscovererInfo *info) {
  GList *videoStreams = gst_discoverer_info_get_video_streams(info);
  bool hasMovingVideo = false;
  bool hasImageVideo = false;
  for (GList *node = videoStreams; node; node = node->next) {
    const auto *videoInfo = GST_DISCOVERER_VIDEO_INFO(node->data);
    if (gst_discoverer_video_info_is_image(videoInfo)) {
      hasImageVideo = true;
    } else {
      hasMovingVideo = true;
    }
  }
  gst_discoverer_stream_info_list_free(videoStreams);

  GList *audioStreams = gst_discoverer_info_get_audio_streams(info);
  const bool hasAudio = audioStreams != nullptr;
  gst_discoverer_stream_info_list_free(audioStreams);
  if (hasMovingVideo) {
    return PlaylistMediaType::Video;
  }
  if (hasAudio) {
    return PlaylistMediaType::Audio;
  }
  if (hasImageVideo) {
    return PlaylistMediaType::StillImage;
  }
  return PlaylistMediaType::Unknown;
}

} // namespace

MediaDurationProbeResult probeMediaDuration(const QString &filePath) {
  MediaDurationProbeResult probe;
  GError *error = nullptr;
  GstDiscoverer *discoverer = gst_discoverer_new(kDiscoveryTimeout, &error);
  if (!discoverer) {
    probe.errorDetail = takeErrorDetail(error);
    if (probe.errorDetail.isEmpty()) {
      probe.errorDetail = QStringLiteral("Could not initialize media analysis.");
    }
    return probe;
  }

  const QByteArray uri = QUrl::fromLocalFile(filePath).toEncoded();
  GstDiscovererInfo *info =
      gst_discoverer_discover_uri(discoverer, uri.constData(), &error);
  const QString discoveryError = takeErrorDetail(error);
  if (!info) {
    probe.errorDetail = discoveryError.isEmpty()
                            ? QStringLiteral("Could not analyze the media.")
                            : discoveryError;
    g_object_unref(discoverer);
    return probe;
  }

  const GstDiscovererResult result = gst_discoverer_info_get_result(info);
  if (result != GST_DISCOVERER_OK) {
    probe.errorDetail = discoveryError.isEmpty()
                            ? discovererResultDetail(result)
                            : discoveryError;
    gst_discoverer_info_unref(info);
    g_object_unref(discoverer);
    return probe;
  }

  const GstClockTime duration = gst_discoverer_info_get_duration(info);
  probe.mediaType = discoverMediaType(info);
  probe.isStillImage =
      probe.mediaType == PlaylistMediaType::StillImage;
  if (probe.mediaType != PlaylistMediaType::Unknown &&
      !probe.isStillImage && GST_CLOCK_TIME_IS_VALID(duration) &&
      duration > 0) {
    const qint64 durationMs = static_cast<qint64>(duration / GST_MSECOND);
    if (durationMs > 0) {
      probe.durationMs = durationMs;
      probe.succeeded = true;
    }
  }
  if (!probe.succeeded) {
    probe.succeeded = probe.isStillImage;
    if (!probe.succeeded) {
      probe.errorDetail = QStringLiteral("Could not determine the media duration.");
    }
  }

  gst_discoverer_info_unref(info);
  g_object_unref(discoverer);
  return probe;
}
