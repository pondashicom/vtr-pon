#include "gpu_configuration.h"
#include "controller_settings.h"

#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QSettings>
#include <gst/gst.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace vtrpon {
GpuPreferences readGpuPreferences(const QSettings &settings) {
  return {settings.value(QStringLiteral("video/gpuAdapterId")).toString(),
          settings.value(QStringLiteral("video/softwareDecoding"), false).toBool()};
}

bool saveGpuPreferences(QSettings &settings, const GpuPreferences &preferences) {
  settings.sync();
  if (settings.status() != QSettings::NoError || !settings.isWritable()) return false;
  const auto previous = readGpuPreferences(settings);
  settings.setValue(QStringLiteral("video/gpuAdapterId"), preferences.adapterId);
  settings.setValue(QStringLiteral("video/softwareDecoding"), preferences.softwareDecoding);
  settings.sync();
  if (settings.status() == QSettings::NoError) return true;
  settings.setValue(QStringLiteral("video/gpuAdapterId"), previous.adapterId);
  settings.setValue(QStringLiteral("video/softwareDecoding"), previous.softwareDecoding);
  settings.sync();
  return false;
}

void freezeGpuPreferences() {
  auto *app = QCoreApplication::instance();
  if (!app || app->property("gpu.preferencesFrozen").toBool()) return;
  const auto settings = makeControllerSettings();
  const auto preferences = readGpuPreferences(settings);
  app->setProperty("gpu.startupAdapterId", preferences.adapterId);
  app->setProperty("gpu.startupSoftwareDecoding", preferences.softwareDecoding);
  app->setProperty("gpu.preferencesFrozen", true);
}

GpuPreferences startupGpuPreferences() {
  freezeGpuPreferences();
  const auto *app = QCoreApplication::instance();
  if (!app) return {};
  return {app->property("gpu.startupAdapterId").toString(),
          app->property("gpu.startupSoftwareDecoding").toBool()};
}

QVector<GpuAdapter> availableGpuAdapters() {
  QVector<GpuAdapter> result;
#ifdef Q_OS_WIN
  using Microsoft::WRL::ComPtr;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return result;
  QHash<QString, int> occurrences;
  for (UINT index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT hr = factory->EnumAdapters1(index, &adapter);
    if (hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(hr)) break;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
    const QString hardware = QStringLiteral("%1:%2:%3:%4")
        .arg(desc.VendorId, 4, 16, QLatin1Char('0'))
        .arg(desc.DeviceId, 4, 16, QLatin1Char('0'))
        .arg(desc.SubSysId, 8, 16, QLatin1Char('0'))
        .arg(desc.Revision, 2, 16, QLatin1Char('0'));
    const int occurrence = occurrences[hardware]++;
    const quint64 bits = (quint64(quint32(desc.AdapterLuid.HighPart)) << 32)
        | desc.AdapterLuid.LowPart;
    result.push_back({hardware + QStringLiteral(":%1").arg(occurrence),
                      QString::fromWCharArray(desc.Description), qint64(bits)});
  }
#endif
  return result;
}

GpuSelection selectGpuAdapter(const QVector<GpuAdapter> &adapters,
                              const QString &requestedId, qint64 displayLuid) {
  for (const auto &adapter : adapters)
    if (!requestedId.isEmpty() && adapter.id == requestedId) return {adapter, false};
  for (const auto &adapter : adapters)
    if (adapter.luid == displayLuid) return {adapter, !requestedId.isEmpty()};
  // Do not silently render on an unrelated GPU if the display disappeared.
  return {{}, !requestedId.isEmpty()};
}

void configureVideoDecoders(qint64 adapterLuid, bool softwareOnly) {
#ifdef Q_OS_WIN
  GList *features = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
  for (GList *it = features; it; it = it->next) {
    auto *factory = GST_ELEMENT_FACTORY(it->data);
    if (!gst_element_factory_list_is_type(factory,
          GstElementFactoryListType(GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO))) continue;
    const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    const char *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    const QString featureName = QString::fromUtf8(name);
    const bool d3d11 = featureName.startsWith(QLatin1StringView("d3d11"));
    const bool hardware = (klass && QString::fromUtf8(klass).contains(QLatin1StringView("Hardware")))
        || d3d11 || featureName.startsWith(QLatin1StringView("d3d12"))
        || featureName.startsWith(QLatin1StringView("nv"))
        || featureName.startsWith(QLatin1StringView("qsv"))
        || featureName.startsWith(QLatin1StringView("amf"))
        || featureName.startsWith(QLatin1StringView("mf"));
    if (!hardware) continue; // Software codecs remain available as fallback.
    bool compatible = false;
    if (d3d11 && !softwareOnly) {
      GstElement *decoder = gst_element_factory_create(factory, nullptr);
      if (decoder) {
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(decoder), "adapter-luid")) {
          gint64 luid = 0;
          g_object_get(decoder, "adapter-luid", &luid, nullptr);
          compatible = luid == adapterLuid;
        }
        gst_object_unref(decoder);
      }
    }
    const guint rank = compatible ? GST_RANK_PRIMARY + 256 : GST_RANK_NONE;
    gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), rank);
    qInfo().noquote() << "GPU_DECODER" << name << "rank=" << rank;
  }
  gst_plugin_feature_list_free(features);
#else
  Q_UNUSED(adapterLuid);
  Q_UNUSED(softwareOnly);
#endif
}

void publishGpuStatus(const GpuSelection &selection, bool softwareOnly) {
  if (auto *app = QCoreApplication::instance()) {
    app->setProperty("gpu.activeName", selection.adapter.name);
    app->setProperty("gpu.activeAdapterId", selection.adapter.id);
    app->setProperty("gpu.activeSoftwareDecoding", softwareOnly);
    app->setProperty("gpu.requestedAdapterMissing", selection.requestedAdapterMissing);
  }
  qInfo().noquote() << "GPU_SELECTED" << selection.adapter.name
                    << "id=" << selection.adapter.id << "luid=" << selection.adapter.luid
                    << "softwareDecoding=" << softwareOnly
                    << "missingAdapterFallback=" << selection.requestedAdapterMissing;
}
} // namespace vtrpon
