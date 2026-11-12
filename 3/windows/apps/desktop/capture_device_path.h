#pragma once

#include <QSet>
#include <QString>

namespace vtrpon {

// MF device symbolic links use case-insensitive ASCII matching, including in
// mfvideosrc. Keep other identifiers (NDI, DeckLink, UAC) byte-for-byte intact.
inline QString canonicalCaptureDevicePath(QString path) {
    if (!path.startsWith(QStringLiteral("\\\\?\\"))) return path;
    for (qsizetype index = 0; index < path.size(); ++index) {
        const ushort character = path.at(index).unicode();
        if (character >= 'A' && character <= 'Z')
            path[index] = QChar(character + ('a' - 'A'));
    }
    return path;
}

inline QSet<QString> canonicalCaptureDevicePaths(const QSet<QString> &paths) {
    QSet<QString> canonical;
    canonical.reserve(paths.size());
    for (const auto &path : paths) canonical.insert(canonicalCaptureDevicePath(path));
    return canonical;
}

inline bool sameCaptureDevicePath(const QString &left, const QString &right) {
    return canonicalCaptureDevicePath(left) == canonicalCaptureDevicePath(right);
}

} // namespace vtrpon
