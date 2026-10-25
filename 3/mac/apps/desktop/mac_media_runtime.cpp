#include "mac_media_runtime.h"
#include <gst/gst.h>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

void configureMacAudioDecoders() {
    // applemedia's atdec can negotiate raw ADTS AAC but deliver silent PCM.
    // Prefer FFmpeg's AAC decoder when available; keep native decoding available
    // for other formats. The distribution candidate supplies an LGPL libav build.
    auto *factory = gst_element_factory_find("avdec_aac");
    if (!factory) return;
    auto *feature = GST_PLUGIN_FEATURE(factory);
    if (gst_plugin_feature_get_rank(feature) < GST_RANK_PRIMARY + 64)
        gst_plugin_feature_set_rank(feature, GST_RANK_PRIMARY + 64);
    gst_object_unref(factory);
}

bool configureMacBundledMediaRuntime() {
    const QDir contents(QDir::cleanPath(QCoreApplication::applicationDirPath()+QStringLiteral("/..")));
    QFile marker(contents.filePath(QStringLiteral("Resources/vtrpon-runtime.json")));
    if (!marker.exists()) return true;
    if (!marker.open(QIODevice::ReadOnly) ||
        QJsonDocument::fromJson(marker.readAll()).object().value(QStringLiteral("selfContained")).toBool()==false) {
        qCritical()<<"BUNDLED_RUNTIME invalid marker";return false;
    }
    const auto plugins=contents.filePath(QStringLiteral("PlugIns/gstreamer"));
    const auto scanner=contents.filePath(QStringLiteral("Helpers/gst-plugin-scanner"));
    if (!QFileInfo(plugins).isDir() || !QFileInfo(scanner).isExecutable()) {
        qCritical()<<"BUNDLED_RUNTIME missing plugins or scanner";return false;
    }
    for (const auto *key : {"GST_PLUGIN_SYSTEM_PATH_1_0","GST_PLUGIN_SYSTEM_PATH"}) qputenv(key,"");
    for (const auto *key : {"GST_PLUGIN_PATH_1_0","GST_PLUGIN_PATH"}) qputenv(key,QFile::encodeName(plugins));
    for (const auto *key : {"GST_PLUGIN_SCANNER_1_0","GST_PLUGIN_SCANNER"}) qputenv(key,QFile::encodeName(scanner));
    qInfo().noquote()<<"BUNDLED_RUNTIME"<<contents.absolutePath()<<"scanner="<<scanner;
    return true;
}
