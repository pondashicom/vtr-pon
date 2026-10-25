#pragma once
// Optional state exchange, separate from the protocol-2 real-time audio mapping.
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
namespace vtrpon::vststate {
constexpr int maxStreamBytes = 4 * 1024 * 1024;
constexpr int maxFileBytes = 12 * 1024 * 1024;
inline QString moduleKey(const QString &path) {
    const auto canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QFileInfo(path).absoluteFilePath() : canonical;
}
inline QJsonObject blob(const QByteArray &bytes) {
    return {{"data", QString::fromLatin1(bytes.toBase64())},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}
inline bool decode(const QJsonValue &value, QByteArray &bytes) {
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    const auto data = object.value("data");
    if (!data.isString() || data.toString().size() > ((maxStreamBytes + 2) / 3) * 4) return false;
    const auto encoded = data.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > maxStreamBytes || decoded.decoded.toBase64() != encoded) return false;
    bytes = decoded.decoded;
    return object.value("sha256").toString() == QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
inline bool valid(const QJsonObject &state, const QString &path, const QString &uid) {
    QByteArray bytes;
    return state.value("format") == QJsonValue("vtr-pon-vst3-state") && state.value("version") == QJsonValue(1)
        && state.value("module").toString() == moduleKey(path) && state.value("uid").toString() == uid
        && decode(state.value("component"), bytes)
        && (state.value("controller").isNull() || decode(state.value("controller"), bytes));
}
inline QJsonObject read(const QString &path, int maximum = maxFileBytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximum) return {};
    const auto bytes = file.read(maximum + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > maximum) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject{};
}
inline bool write(const QString &path, const QJsonObject &object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > maxFileBytes) return false;
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        && file.write(bytes) == bytes.size() && file.commit();
}
inline QString savedFile(const QString &settingsFile, const QString &slot, const QString &path, const QString &uid) {
    const auto identity = moduleKey(path).toUtf8() + '\0' + uid.toUtf8();
    return QFileInfo(settingsFile).absolutePath() + QStringLiteral("/vst3-state/") + slot + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()) + QStringLiteral(".json");
}
} // namespace vtrpon::vststate
