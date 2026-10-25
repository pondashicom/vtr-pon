#pragma once

#include <QString>
#include <QUrl>

namespace vtrpon {

struct UpdateTarget {
  QString platform;
  QUrl catalogUrl;
  QUrl downloadPage;
};

inline bool officialUpdateUrl(const QUrl &url) {
  return url.isValid() && url.scheme() == QStringLiteral("https") &&
      url.host() == QStringLiteral("pondashi.com") &&
      (url.port() == -1 || url.port() == 443) && url.userInfo().isEmpty() &&
      !url.hasQuery() && !url.hasFragment() &&
      url.path().startsWith(QStringLiteral("/vtr-pon/")) &&
      !url.path().contains(QStringLiteral("..")) &&
      !url.path().contains(QLatin1Char('%')) && !url.path().contains(QLatin1Char('\\'));
}

inline bool validUpdateDownload(const QString &platform, const QUrl &url) {
  if (!officialUpdateUrl(url)) return false;
  if (platform == QStringLiteral("windows-x64"))
    return url.path() == QStringLiteral("/vtr-pon/download/windows/");
  if (platform == QStringLiteral("macos-arm64") || platform == QStringLiteral("macos-x64"))
    return url.path() == QStringLiteral("/vtr-pon/download/mac/index.html");
  return false;
}

inline bool updateTargetConfigured(const UpdateTarget &target) {
  // Explicitly supplied loopback catalogs are used only by local network tests.
  return target.catalogUrl.isValid() && !target.catalogUrl.isEmpty() &&
      validUpdateDownload(target.platform, target.downloadPage);
}

inline UpdateTarget defaultUpdateTarget() {
#ifdef Q_OS_WIN
  return {QStringLiteral("windows-x64"),
      QUrl(QStringLiteral("https://pondashi.com/vtr-pon/updates.json")),
      QUrl(QStringLiteral("https://pondashi.com/vtr-pon/download/windows/"))};
#elif defined(Q_OS_MACOS)
#if defined(Q_PROCESSOR_ARM_64)
  const auto platform = QStringLiteral("macos-arm64");
#elif defined(Q_PROCESSOR_X86_64)
  const auto platform = QStringLiteral("macos-x64");
#else
  const auto platform = QStringLiteral("macos-unsupported");
#endif
  return {platform,
      QUrl(QStringLiteral("https://pondashi.com/vtr-pon/updates.json")),
      QUrl(QStringLiteral("https://pondashi.com/vtr-pon/download/mac/index.html"))};
#else
  return {QStringLiteral("unsupported"), {}, {}};
#endif
}

inline QString updateTargetLabel(const QString &platform, bool japanese) {
  if (platform == QStringLiteral("macos-arm64")) return QStringLiteral("Mac (Apple Silicon / arm64)");
  if (platform == QStringLiteral("macos-x64")) return QStringLiteral("Mac (Intel / x64)");
  if (platform == QStringLiteral("windows-x64")) return QStringLiteral("Windows (x64)");
  return japanese ? QStringLiteral("このOS・CPU") : QStringLiteral("this OS and CPU");
}

} // namespace vtrpon
