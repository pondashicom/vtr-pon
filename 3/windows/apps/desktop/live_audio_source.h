#pragma once

#include <QString>

namespace vtrpon {
// Audio-only inputs share the live-capture playlist contract. Preserve the
// endpoint ID in a URI so it cannot be mistaken for a file or camera path.
inline bool isLiveAudioSource(const QString &path) {
    return path.startsWith(QLatin1StringView("live-audio://"));
}

inline QString liveAudioSourcePath(const QString &deviceId) {
    return QStringLiteral("live-audio://") + QString::fromLatin1(
        deviceId.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

inline QString liveAudioDeviceId(const QString &path) {
    if (!isLiveAudioSource(path)) return {};
    const auto result = QString::fromUtf8(QByteArray::fromBase64(path.mid(13).toLatin1(),
        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors));
    return !result.isEmpty() && liveAudioSourcePath(result) == path ? result : QString{};
}
}
