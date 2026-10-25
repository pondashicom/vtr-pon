#pragma once

#include "video_frame_capture.h"
#include <gst/video/video.h>
#include <memory>

// Automatic sinks are bins on macOS; the selected child owns last-sample.
// Return an owned sample, never a pointer to a sink that may be retired.
inline GstSample *lastSinkSample(GstElement *sink) {
  if (!sink) return nullptr;
  GstSample *sample = nullptr;
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "last-sample"))
    g_object_get(sink, "last-sample", &sample, nullptr);
  if (sample || !GST_IS_BIN(sink)) return sample;
  GstIterator *children = gst_bin_iterate_recurse(GST_BIN(sink));
  GValue child = G_VALUE_INIT;
  while (gst_iterator_next(children, &child) == GST_ITERATOR_OK) {
    auto *element = GST_ELEMENT(g_value_get_object(&child));
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "last-sample"))
      g_object_get(element, "last-sample", &sample, nullptr);
    g_value_reset(&child);
    if (sample) break;
  }
  if (G_VALUE_TYPE(&child)) g_value_unset(&child);
  gst_iterator_free(children);
  return sample;
}

inline VideoFrameCapture captureSinkFrame(GstElement *sink) {
  GstSample *sample = lastSinkSample(sink);
  if (!sample) return {};
  auto owned = std::shared_ptr<GstSample>(sample, gst_sample_unref);
  return [owned] {
    GstVideoInfo info;
    gst_video_info_init(&info);
    auto *caps = gst_sample_get_caps(owned.get());
    auto *buffer = gst_sample_get_buffer(owned.get());
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps)) return QImage{};
    GstVideoFrame frame{};
    // Mapping D3D11 memory without GST_MAP_D3D11 performs its CPU readback.
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) return QImage{};
    QImage image;
    if (GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_BGRA) {
      const QImage view(static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0)),
                        GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                        GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), QImage::Format_ARGB32);
      image = view.convertToFormat(QImage::Format_RGB32);
      // Detach even if Qt could reuse the mapped view's storage.
      image.detach();
    } else {
      // Native sinks can negotiate RGBA/YUV. Convert on the capture worker,
      // after retaining the source frame, without changing the live pipeline.
      GstVideoInfo outputInfo;
      gst_video_info_init(&outputInfo);
      gst_video_info_set_format(&outputInfo, GST_VIDEO_FORMAT_BGRA,
                                GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info));
      outputInfo.fps_n = info.fps_n; outputInfo.fps_d = info.fps_d;
      GstBuffer *converted = gst_buffer_new_allocate(nullptr, outputInfo.size, nullptr);
      GstVideoFrame output{};
      GstVideoConverter *converter = gst_video_converter_new(&info, &outputInfo, nullptr);
      if (converted && converter && gst_video_frame_map(&output, &outputInfo, converted, GST_MAP_WRITE)) {
        gst_video_converter_frame(converter, &frame, &output);
        const QImage view(static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&output, 0)),
                          GST_VIDEO_INFO_WIDTH(&outputInfo), GST_VIDEO_INFO_HEIGHT(&outputInfo),
                          GST_VIDEO_FRAME_PLANE_STRIDE(&output, 0), QImage::Format_ARGB32);
        image = view.convertToFormat(QImage::Format_RGB32);
        image.detach();
        gst_video_frame_unmap(&output);
      }
      if (converter) gst_video_converter_free(converter);
      if (converted) gst_buffer_unref(converted);
    }
    gst_video_frame_unmap(&frame);
    if (image.isNull()) return image;
    return image.scaled(1920, 1080, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  };
}
