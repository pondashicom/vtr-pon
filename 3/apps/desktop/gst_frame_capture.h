#pragma once

#include "video_frame_capture.h"
#include <gst/video/video.h>
#include <memory>

inline VideoFrameCapture captureSinkFrame(GstElement *sink) {
  if (!sink || !g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "last-sample"))
    return {};
  GstSample *sample = nullptr;
  g_object_get(sink, "last-sample", &sample, nullptr);
  if (!sample) return {};
  auto owned = std::shared_ptr<GstSample>(sample, gst_sample_unref);
  return [owned] {
    GstVideoInfo info;
    gst_video_info_init(&info);
    auto *caps = gst_sample_get_caps(owned.get());
    auto *buffer = gst_sample_get_buffer(owned.get());
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA)
      return QImage{};
    GstVideoFrame frame{};
    // Mapping D3D11 memory without GST_MAP_D3D11 performs its CPU readback.
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return QImage{};
    const QImage view(static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
                      GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                      GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), QImage::Format_ARGB32);
    QImage image = view.convertToFormat(QImage::Format_RGB32);
    // Detach even if Qt could reuse the mapped view's storage.
    image.detach();
    gst_video_frame_unmap(&frame);
    return image.scaled(1920, 1080, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  };
}
