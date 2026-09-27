#include "gpu_configuration.h"
#include "controller_settings.h"
#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <gst/gst.h>
#include <iostream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir directory;
  if (!directory.isValid()) return 1;
  app.setProperty("vtrpon.settingsFile", directory.filePath(QStringLiteral("gpu.ini")));
  auto settings = vtrpon::makeControllerSettings();
  const auto check = [](bool ok, const char *message) {
    if (!ok) std::cerr << message << '\n';
    return ok;
  };
  bool ok = check(vtrpon::readGpuPreferences(settings) == vtrpon::GpuPreferences{}, "Defaults must use auto hardware decoding");
  const QVector<vtrpon::GpuAdapter> adapters{{"amd", "AMD", 10}, {"nvidia", "NVIDIA", 20}};
  ok &= check(vtrpon::selectGpuAdapter(adapters, {}, 20).adapter.id == "nvidia", "Auto must follow display GPU, not adapter zero");
  ok &= check(vtrpon::selectGpuAdapter(adapters, "amd", 20).adapter.luid == 10, "Manual GPU must override display GPU");
  const auto missing = vtrpon::selectGpuAdapter(adapters, "removed", 20);
  ok &= check(missing.requestedAdapterMissing && missing.adapter.id == "nvidia", "Missing GPU must fall back explicitly");
  ok &= check(vtrpon::selectGpuAdapter({}, {}, 10).adapter.id.isEmpty(), "No GPU must not manufacture a selection");
  ok &= check(vtrpon::selectGpuAdapter(adapters, {}, 30).adapter.id.isEmpty(), "Missing display GPU must not choose unrelated adapter");
  ok &= check(vtrpon::saveGpuPreferences(settings, {"amd", false}), "Save hardware GPU");
  vtrpon::freezeGpuPreferences();
  ok &= check(vtrpon::saveGpuPreferences(settings, {"nvidia", true}), "Save software preference");
  QSettings reopened(settings.fileName(), QSettings::IniFormat);
  ok &= check(vtrpon::readGpuPreferences(reopened) == vtrpon::GpuPreferences{"nvidia", true}, "Settings must persist across QSettings instances");
  ok &= check(vtrpon::startupGpuPreferences() == vtrpon::GpuPreferences{"amd", false}, "Changing settings must not alter active startup selection");
  auto nextBoot = adapters;
  nextBoot[0].luid = 99;
  ok &= check(vtrpon::selectGpuAdapter(nextBoot, "amd", 20).adapter.luid == 99, "Saved identity must survive LUID changes");

#ifdef Q_OS_WIN
  gst_init(&argc, &argv);
  const auto actual = vtrpon::availableGpuAdapters();
  if (!actual.isEmpty()) {
    // Deliberately simulate an inherited environment override of a hardware decoder.
    auto *feature = gst_registry_find_feature(gst_registry_get(), "d3d12h264dec", GST_TYPE_ELEMENT_FACTORY);
    if (feature) gst_plugin_feature_set_rank(feature, 1000);
    vtrpon::configureVideoDecoders(actual.front().luid, true);
    if (feature) {
      ok &= check(gst_plugin_feature_get_rank(feature) == 0, "Software mode must override inherited hardware ranks");
      gst_object_unref(feature);
    }
    vtrpon::configureVideoDecoders(actual.front().luid, false);
    GList *factories = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
    for (GList *it = factories; it; it = it->next) {
      auto *factory = GST_ELEMENT_FACTORY(it->data);
      if (!gst_element_factory_list_is_type(factory, GstElementFactoryListType(GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO))) continue;
      const QString name = QString::fromUtf8(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)));
      if (!name.startsWith("d3d11")) continue;
      GstElement *decoder = gst_element_factory_create(factory, nullptr);
      if (!decoder) continue;
      gint64 luid = 0;
      g_object_get(decoder, "adapter-luid", &luid, nullptr);
      ok &= check(gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory)) == (luid == actual.front().luid ? 512u : 0u), "Only the selected GPU's D3D11 decoders may have hardware priority");
      gst_object_unref(decoder);
    }
    gst_plugin_feature_list_free(factories);
    vtrpon::configureVideoDecoders(actual.front().luid, true);
    feature = gst_registry_find_feature(gst_registry_get(), "avdec_h264", GST_TYPE_ELEMENT_FACTORY);
    ok &= check(feature && gst_plugin_feature_get_rank(feature) > 0, "Software decoder must remain available");
    if (feature) gst_object_unref(feature);
  }
#endif
  return ok ? 0 : 1;
}
