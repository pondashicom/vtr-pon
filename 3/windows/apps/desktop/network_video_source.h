#pragma once
#include <QString>

namespace vtrpon {
// Network feeds share the existing live-capture playlist/session behavior.
// The URI retains the full discovery name, including the sending host.
inline bool isNetworkVideoSource(const QString &path) {
    return path.startsWith(QLatin1StringView("network-video://"));
}
inline QString networkVideoSourcePath(const QString &name) {
    return QStringLiteral("network-video://") + QString::fromLatin1(
        name.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
inline QString networkVideoSourceName(const QString &path) {
    if (!isNetworkVideoSource(path)) return {};
    const auto result = QString::fromUtf8(QByteArray::fromBase64(path.mid(16).toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors));
    return networkVideoSourcePath(result) == path ? result : QString{};
}
inline QString defaultNetworkVideoOutputName() {
    return QStringLiteral("VTR-PON-NDI-OUTPPUT");
}
}
