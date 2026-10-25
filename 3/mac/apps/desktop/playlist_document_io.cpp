#include "playlist_document_io.h"
#include "playlist_media_path.h"
#include <QFileInfo>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr auto kFormatName = "vtr-pon-playlist";
constexpr int kFormatVersion = 1;
constexpr qint64 kMaximumExactJsonInteger = 9007199254740991LL;
constexpr qsizetype kMaximumJsonValues = 500000;

void setError(QString *errorDetail, const QString &message) {
  if (errorDetail) {
    *errorDetail = message;
  }
}

bool checkCancellation(const std::atomic_bool *cancel, QString *errorDetail) {
  if (!cancel || !cancel->load(std::memory_order_acquire)) return false;
  setError(errorDetail, QStringLiteral("Playlist import was cancelled."));
  return true;
}

bool checkJsonBudget(const QJsonObject &root, QString *errorDetail,
                     const std::atomic_bool *cancel) {
  std::vector<QJsonValue> pending{root};
  qsizetype values = 0;
  qsizetype stringLength = 0;
  const auto checkString = [&](const QString &value) {
    if (value.size() > PlaylistDocumentIo::kMaximumStringLength) {
      setError(errorDetail, QStringLiteral("Playlist JSON string exceeds %1 characters.")
                                .arg(PlaylistDocumentIo::kMaximumStringLength));
      return false;
    }
    stringLength += value.size();
    if (stringLength > PlaylistDocumentIo::kMaximumTotalStringLength) {
      setError(errorDetail, QStringLiteral("Playlist JSON strings exceed %1 characters in total.")
                                .arg(PlaylistDocumentIo::kMaximumTotalStringLength));
      return false;
    }
    return true;
  };
  while (!pending.empty()) {
    if (checkCancellation(cancel, errorDetail)) return false;
    if (++values > kMaximumJsonValues) {
      setError(errorDetail, QStringLiteral("Playlist JSON exceeds %1 values.")
                                .arg(kMaximumJsonValues));
      return false;
    }
    QJsonValue value = std::move(pending.back());
    pending.pop_back();
    if (value.isString() && !checkString(value.toString())) return false;
    if (value.isArray()) {
      const QJsonArray array = value.toArray();
      if (array.size() > kMaximumJsonValues - values - static_cast<qsizetype>(pending.size())) {
        setError(errorDetail, QStringLiteral("Playlist JSON exceeds %1 values.")
                                  .arg(kMaximumJsonValues));
        return false;
      }
      for (const auto &entry : array) pending.push_back(entry);
    } else if (value.isObject()) {
      const QJsonObject object = value.toObject();
      if (object.size() > kMaximumJsonValues - values - static_cast<qsizetype>(pending.size())) {
        setError(errorDetail, QStringLiteral("Playlist JSON exceeds %1 values.")
                                  .arg(kMaximumJsonValues));
        return false;
      }
      for (auto it = object.begin(); it != object.end(); ++it) {
        if (!checkString(it.key())) return false;
        pending.push_back(it.value());
      }
    }
  }
  return true;
}

bool readString(const QJsonObject &object, const QString &key, QString *value,
                QString *errorDetail, const QString &path) {
  const QJsonValue jsonValue = object.value(key);
  if (!jsonValue.isString()) {
    setError(errorDetail,
             QStringLiteral("%1.%2 must be a string.").arg(path, key));
    return false;
  }
  *value = jsonValue.toString();
  return true;
}

bool readBool(const QJsonObject &object, const QString &key, bool *value,
              QString *errorDetail, const QString &path) {
  const QJsonValue jsonValue = object.value(key);
  if (!jsonValue.isBool()) {
    setError(errorDetail,
             QStringLiteral("%1.%2 must be true or false.").arg(path, key));
    return false;
  }
  *value = jsonValue.toBool();
  return true;
}

bool readInteger(const QJsonObject &object, const QString &key,
                 qint64 minimum, qint64 maximum, qint64 *value,
                 QString *errorDetail, const QString &path) {
  const QJsonValue jsonValue = object.value(key);
  if (!jsonValue.isDouble()) {
    setError(errorDetail,
             QStringLiteral("%1.%2 must be an integer.").arg(path, key));
    return false;
  }
  const double number = jsonValue.toDouble();
  if (!std::isfinite(number) || std::trunc(number) != number ||
      number < static_cast<double>(minimum) ||
      number > static_cast<double>(maximum)) {
    setError(errorDetail,
             QStringLiteral("%1.%2 is outside the supported integer range.")
                 .arg(path, key));
    return false;
  }
  *value = static_cast<qint64>(number);
  return true;
}

bool readInt(const QJsonObject &object, const QString &key, int minimum,
             int maximum, int *value, QString *errorDetail,
             const QString &path) {
  qint64 parsedValue = 0;
  if (!readInteger(object, key, minimum, maximum, &parsedValue, errorDetail,
                   path)) {
    return false;
  }
  *value = static_cast<int>(parsedValue);
  return true;
}

QString startModeName(PlaylistStartMode mode) {
  switch (mode) {
  case PlaylistStartMode::Pause:
    return QStringLiteral("pause");
  case PlaylistStartMode::Play:
    return QStringLiteral("play");
  }
  return QString();
}

bool parseStartMode(const QString &name, PlaylistStartMode *mode) {
  if (name == QStringLiteral("pause")) {
    *mode = PlaylistStartMode::Pause;
    return true;
  }
  if (name == QStringLiteral("play")) {
    *mode = PlaylistStartMode::Play;
    return true;
  }
  return false;
}

QString playlistModeName(PlaylistMode mode) {
  switch (mode) {
  case PlaylistMode::Standard:
    return QStringLiteral("standard");
  case PlaylistMode::Bgm:
    return QStringLiteral("bgm");
  }
  return QStringLiteral("standard");
}

bool parsePlaylistMode(const QString &name, PlaylistMode *mode) {
  if (name == QStringLiteral("standard")) {
    *mode = PlaylistMode::Standard;
    return true;
  }
  if (name == QStringLiteral("bgm")) {
    *mode = PlaylistMode::Bgm;
    return true;
  }
  return false;
}

QString endModeName(PlaylistEndMode mode) {
  switch (mode) {
  case PlaylistEndMode::Stop:
    return QStringLiteral("stop");
  case PlaylistEndMode::Pause:
    return QStringLiteral("pause");
  case PlaylistEndMode::Repeat:
    return QStringLiteral("repeat");
  case PlaylistEndMode::Next:
    return QStringLiteral("next");
  case PlaylistEndMode::Goto:
    return QStringLiteral("goto");
  }
  return QString();
}

bool parseEndMode(const QString &name, PlaylistEndMode *mode) {
  if (name == QStringLiteral("stop")) {
    *mode = PlaylistEndMode::Stop;
    return true;
  }
  if (name == QStringLiteral("pause")) {
    *mode = PlaylistEndMode::Pause;
    return true;
  }
  if (name == QStringLiteral("repeat")) {
    *mode = PlaylistEndMode::Repeat;
    return true;
  }
  if (name == QStringLiteral("next")) {
    *mode = PlaylistEndMode::Next;
    return true;
  }
  if (name == QStringLiteral("goto")) {
    *mode = PlaylistEndMode::Goto;
    return true;
  }
  return false;
}

QString repeatEndModeName(PlaylistRepeatEndMode mode) {
  switch (mode) {
  case PlaylistRepeatEndMode::Stop:
    return QStringLiteral("stop");
  case PlaylistRepeatEndMode::Pause:
    return QStringLiteral("pause");
  case PlaylistRepeatEndMode::Next:
    return QStringLiteral("next");
  case PlaylistRepeatEndMode::Goto:
    return QStringLiteral("goto");
  }
  return QStringLiteral("pause");
}

bool parseRepeatEndMode(const QString &name, PlaylistRepeatEndMode *mode) {
  if (name == QStringLiteral("stop")) {
    *mode = PlaylistRepeatEndMode::Stop;
    return true;
  }
  if (name == QStringLiteral("pause")) {
    *mode = PlaylistRepeatEndMode::Pause;
    return true;
  }
  if (name == QStringLiteral("next")) {
    *mode = PlaylistRepeatEndMode::Next;
    return true;
  }
  if (name == QStringLiteral("goto")) {
    *mode = PlaylistRepeatEndMode::Goto;
    return true;
  }
  return false;
}

QString sourceTypeName(PlaylistSourceType sourceType) {
  switch (sourceType) {
  case PlaylistSourceType::File:
    return QStringLiteral("file");
  case PlaylistSourceType::Uvc:
    return QStringLiteral("uvc");
  case PlaylistSourceType::PowerPoint:
    return QStringLiteral("powerpoint");
  case PlaylistSourceType::Pdf:
    return QStringLiteral("pdf");
  }
  return QString();
}

bool parseSourceType(const QString &name, PlaylistSourceType *sourceType) {
  if (name == QStringLiteral("file")) {
    *sourceType = PlaylistSourceType::File;
    return true;
  }
  if (name == QStringLiteral("uvc")) {
    *sourceType = PlaylistSourceType::Uvc;
    return true;
  }
  if (name == QStringLiteral("powerpoint")) {
    *sourceType = PlaylistSourceType::PowerPoint;
    return true;
  }
  if (name == QStringLiteral("pdf")) {
    *sourceType = PlaylistSourceType::Pdf;
    return true;
  }
  return false;
}

QString mediaTypeName(PlaylistMediaType mediaType) {
  switch (mediaType) {
  case PlaylistMediaType::Unknown:
    return QStringLiteral("unknown");
  case PlaylistMediaType::Video:
    return QStringLiteral("video");
  case PlaylistMediaType::Audio:
    return QStringLiteral("audio");
  case PlaylistMediaType::StillImage:
    return QStringLiteral("stillImage");
  case PlaylistMediaType::Presentation:
    return QStringLiteral("presentation");
  case PlaylistMediaType::Pdf:
    return QStringLiteral("pdf");
  }
  return QString();
}

bool parseMediaType(const QString &name, PlaylistMediaType *mediaType) {
  if (name == QStringLiteral("unknown")) {
    *mediaType = PlaylistMediaType::Unknown;
    return true;
  }
  if (name == QStringLiteral("video")) {
    *mediaType = PlaylistMediaType::Video;
    return true;
  }
  if (name == QStringLiteral("audio")) {
    *mediaType = PlaylistMediaType::Audio;
    return true;
  }
  if (name == QStringLiteral("stillImage")) {
    *mediaType = PlaylistMediaType::StillImage;
    return true;
  }
  if (name == QStringLiteral("presentation")) {
    *mediaType = PlaylistMediaType::Presentation;
    return true;
  }
  if (name == QStringLiteral("pdf")) {
    *mediaType = PlaylistMediaType::Pdf;
    return true;
  }
  return false;
}

QString powerPointModeName(PlaylistPowerPointMode mode) {
  switch (mode) {
  case PlaylistPowerPointMode::None:
    return QStringLiteral("none");
  case PlaylistPowerPointMode::NativeSlideShow:
    return QStringLiteral("native");
  case PlaylistPowerPointMode::VideoConversion:
    return QStringLiteral("video");
  case PlaylistPowerPointMode::PngSequence:
    return QStringLiteral("pngSequence");
  }
  return QString();
}

bool parsePowerPointMode(const QString &name, PlaylistPowerPointMode *mode) {
  if (name == QStringLiteral("none")) {
    *mode = PlaylistPowerPointMode::None;
    return true;
  }
  if (name == QStringLiteral("native")) {
    *mode = PlaylistPowerPointMode::NativeSlideShow;
    return true;
  }
  if (name == QStringLiteral("video")) {
    *mode = PlaylistPowerPointMode::VideoConversion;
    return true;
  }
  if (name == QStringLiteral("pngSequence")) {
    *mode = PlaylistPowerPointMode::PngSequence;
    return true;
  }
  return false;
}

bool powerPointModeMatchesItem(PlaylistPowerPointMode mode,
                               PlaylistSourceType sourceType,
                               PlaylistMediaType mediaType) {
  switch (mode) {
  case PlaylistPowerPointMode::None:
    if (sourceType == PlaylistSourceType::Pdf) {
      return mediaType == PlaylistMediaType::Pdf;
    }
    return mediaType != PlaylistMediaType::Pdf;
  case PlaylistPowerPointMode::NativeSlideShow:
    return sourceType == PlaylistSourceType::PowerPoint &&
           mediaType == PlaylistMediaType::Presentation;
  case PlaylistPowerPointMode::VideoConversion:
    return sourceType == PlaylistSourceType::File &&
           mediaType == PlaylistMediaType::Video;
  case PlaylistPowerPointMode::PngSequence:
    return sourceType == PlaylistSourceType::File &&
           mediaType == PlaylistMediaType::StillImage;
  }
  return false;
}

QString itemColorName(PlaylistItemColor color) {
  switch (color) {
  case PlaylistItemColor::Default:
    return QStringLiteral("default");
  case PlaylistItemColor::Red:
    return QStringLiteral("red");
  case PlaylistItemColor::Yellow:
    return QStringLiteral("yellow");
  case PlaylistItemColor::Blue:
    return QStringLiteral("blue");
  case PlaylistItemColor::Green:
    return QStringLiteral("green");
  }
  return QString();
}

bool parseItemColor(const QString &name, PlaylistItemColor *color) {
  if (name == QStringLiteral("default")) {
    *color = PlaylistItemColor::Default;
    return true;
  }
  if (name == QStringLiteral("red")) {
    *color = PlaylistItemColor::Red;
    return true;
  }
  if (name == QStringLiteral("yellow")) {
    *color = PlaylistItemColor::Yellow;
    return true;
  }
  if (name == QStringLiteral("blue")) {
    *color = PlaylistItemColor::Blue;
    return true;
  }
  if (name == QStringLiteral("green")) {
    *color = PlaylistItemColor::Green;
    return true;
  }
  return false;
}

QJsonObject fadeObject(bool enabled, qint64 durationMs) {
  return QJsonObject{{QStringLiteral("enabled"), enabled},
                     {QStringLiteral("durationMs"), durationMs}};
}

QJsonObject settingsObject(const PlaylistItemSettings &settings) {
  QJsonObject result{
      {QStringLiteral("inPointMs"), settings.inPointMs},
      {QStringLiteral("startMode"), startModeName(settings.startMode)},
      {QStringLiteral("endMode"), endModeName(settings.endMode)},
      {QStringLiteral("fadeIn"),
       fadeObject(settings.fadeInEnabled, settings.fadeInDurationMs)},
      {QStringLiteral("fadeOut"),
       fadeObject(settings.fadeOutEnabled, settings.fadeOutDurationMs)},
      {QStringLiteral("endCrossfade"),
       fadeObject(settings.endCrossfadeEnabled,
                  settings.endCrossfadeDurationMs)},
      {QStringLiteral("nominalVolumeTenthsDb"),
       settings.nominalVolumeTenthsDb},
      {QStringLiteral("nominalAudioDelayFrames"), settings.nominalAudioDelayFrames},
      {QStringLiteral("nominalEqLowTenthsDb"), settings.nominalEqTenthsDb[0]},
      {QStringLiteral("nominalEqMidTenthsDb"), settings.nominalEqTenthsDb[1]},
      {QStringLiteral("nominalEqHighTenthsDb"), settings.nominalEqTenthsDb[2]},
  };
  result.insert(QStringLiteral("outPointMs"),
                settings.outPointMs
                    ? QJsonValue(*settings.outPointMs)
                    : QJsonValue(QJsonValue::Null));
  result.insert(QStringLiteral("gotoTargetItemId"),
                settings.gotoTargetItemId != 0
                    ? QJsonValue(QString::number(settings.gotoTargetItemId))
                    : QJsonValue(QJsonValue::Null));
  result.insert(QStringLiteral("repeatCount"),
                settings.repeatCount == kInfiniteItemRepeatCount
                    ? QJsonValue(QJsonValue::Null)
                    : QJsonValue(settings.repeatCount));
  result.insert(QStringLiteral("repeatEndMode"),
                repeatEndModeName(settings.repeatEndMode));
  return result;
}

bool parseFade(const QJsonObject &settingsObject, const QString &key,
               bool *enabled, qint64 *durationMs, QString *errorDetail,
               const QString &path) {
  const QJsonValue value = settingsObject.value(key);
  if (!value.isObject()) {
    setError(errorDetail,
             QStringLiteral("%1.%2 must be an object.").arg(path, key));
    return false;
  }
  const QJsonObject fade = value.toObject();
  const QString fadePath = path + QLatin1Char('.') + key;
  return readBool(fade, QStringLiteral("enabled"), enabled, errorDetail,
                  fadePath) &&
         readInteger(fade, QStringLiteral("durationMs"), 0,
                     kMaximumExactJsonInteger, durationMs, errorDetail,
                     fadePath);
}

bool parseOptionalFade(const QJsonObject &settingsObject, const QString &key,
                       bool *enabled, qint64 *durationMs,
                       QString *errorDetail, const QString &path) {
  if (settingsObject.value(key).isUndefined()) {
    return true;
  }
  return parseFade(settingsObject, key, enabled, durationMs, errorDetail,
                   path);
}

bool parseSettings(const QJsonValue &value, PlaylistItemSettings *settings,
                   QString *errorDetail, const QString &path) {
  if (!value.isObject()) {
    setError(errorDetail, QStringLiteral("%1 must be an object.").arg(path));
    return false;
  }
  const QJsonObject object = value.toObject();
  if (!readInteger(object, QStringLiteral("inPointMs"), 0,
                   kMaximumExactJsonInteger, &settings->inPointMs,
                   errorDetail, path)) {
    return false;
  }
  const QJsonValue outPoint = object.value(QStringLiteral("outPointMs"));
  if (outPoint.isNull()) {
    settings->outPointMs = std::nullopt;
  } else {
    qint64 parsedOutPoint = 0;
    if (!readInteger(object, QStringLiteral("outPointMs"), 0,
                     kMaximumExactJsonInteger, &parsedOutPoint, errorDetail,
                     path)) {
      return false;
    }
    settings->outPointMs = parsedOutPoint;
  }

  QString startMode;
  if (!readString(object, QStringLiteral("startMode"), &startMode,
                  errorDetail, path)) {
    return false;
  }
  if (!parseStartMode(startMode, &settings->startMode)) {
    setError(errorDetail,
             QStringLiteral("%1.startMode is not supported.").arg(path));
    return false;
  }
  QString endMode;
  if (!readString(object, QStringLiteral("endMode"), &endMode, errorDetail,
                  path)) {
    return false;
  }
  if (!parseEndMode(endMode, &settings->endMode)) {
    setError(errorDetail,
             QStringLiteral("%1.endMode is not supported.").arg(path));
    return false;
  }
  if (!parseFade(object, QStringLiteral("fadeIn"),
                 &settings->fadeInEnabled, &settings->fadeInDurationMs,
                 errorDetail, path) ||
      !parseFade(object, QStringLiteral("fadeOut"),
                 &settings->fadeOutEnabled, &settings->fadeOutDurationMs,
                 errorDetail, path)) {
    return false;
  }
  if (!parseOptionalFade(object, QStringLiteral("endCrossfade"),
                         &settings->endCrossfadeEnabled,
                         &settings->endCrossfadeDurationMs, errorDetail,
                         path)) {
    return false;
  }
  const QJsonValue gotoTarget =
      object.value(QStringLiteral("gotoTargetItemId"));
  settings->gotoTargetItemId = 0;
  if (!gotoTarget.isUndefined() && !gotoTarget.isNull()) {
    if (!gotoTarget.isString()) {
      setError(errorDetail,
               QStringLiteral("%1.gotoTargetItemId must be a positive integer string or null.")
                   .arg(path));
      return false;
    }
    bool validTargetId = false;
    settings->gotoTargetItemId = gotoTarget.toString().toULongLong(
        &validTargetId);
    if (!validTargetId || settings->gotoTargetItemId == 0) {
      setError(errorDetail,
               QStringLiteral("%1.gotoTargetItemId must be a positive integer string or null.")
                   .arg(path));
      return false;
    }
  }
  const QJsonValue repeatCount = object.value(QStringLiteral("repeatCount"));
  const QJsonValue repeatEndMode =
      object.value(QStringLiteral("repeatEndMode"));
  settings->repeatCount = kInfiniteItemRepeatCount;
  settings->repeatEndMode = PlaylistRepeatEndMode::Pause;
  if (repeatCount.isUndefined() != repeatEndMode.isUndefined()) {
    setError(errorDetail,
             QStringLiteral("%1.repeatCount and %1.repeatEndMode must be provided together.")
                 .arg(path));
    return false;
  }
  if (!repeatCount.isUndefined()) {
    if (!repeatCount.isNull()) {
      if (!readInt(object, QStringLiteral("repeatCount"), 1,
                   kMaximumItemRepeatCount, &settings->repeatCount,
                   errorDetail, path)) {
        return false;
      }
    }
    if (!repeatEndMode.isNull()) {
      if (!repeatEndMode.isString() ||
          !parseRepeatEndMode(repeatEndMode.toString(),
                              &settings->repeatEndMode)) {
        setError(errorDetail,
                 QStringLiteral("%1.repeatEndMode is not supported.").arg(path));
        return false;
      }
    }
  }
  for (int band = 0; band < 3; ++band) {
    const char *keys[] = {"nominalEqLowTenthsDb", "nominalEqMidTenthsDb", "nominalEqHighTenthsDb"};
    if (object.contains(QString::fromLatin1(keys[band]))
        && !readInt(object, QString::fromLatin1(keys[band]),
                    -vtrpon::kEqMaximumTenthsDb, vtrpon::kEqMaximumTenthsDb,
                    &settings->nominalEqTenthsDb[band], errorDetail, path)) return false;
  }
  if (object.contains(QStringLiteral("nominalAudioDelayFrames")) &&
      !readInt(object, QStringLiteral("nominalAudioDelayFrames"), 0, 6,
               &settings->nominalAudioDelayFrames, errorDetail, path)) return false;
  return readInt(object, QStringLiteral("nominalVolumeTenthsDb"),
                 (std::numeric_limits<int>::min)(),
                 (std::numeric_limits<int>::max)(),
                 &settings->nominalVolumeTenthsDb, errorDetail, path);
}

} // namespace

namespace PlaylistDocumentIo {

QByteArray toJson(const PlaylistModelSnapshot &snapshot) {
  QJsonArray playlists;
  for (const PlaylistState &playlist : snapshot.playlists) {
    QJsonArray items;
    for (const PlaylistItemState &item : snapshot.items) {
      if (item.playlistNumber != playlist.number) {
        continue;
      }
      QJsonObject itemObject{
          {QStringLiteral("id"), QString::number(item.id)},
          {QStringLiteral("filePath"), item.filePath},
          {QStringLiteral("displayName"), item.displayName},
          {QStringLiteral("sourceType"), sourceTypeName(item.sourceType)},
          {QStringLiteral("mediaType"), mediaTypeName(item.mediaType)},
          {QStringLiteral("color"), itemColorName(item.color)},
          {QStringLiteral("separatorBelow"), item.separatorBelow},
          {QStringLiteral("settings"), settingsObject(item.settings)},
      };
      if (item.powerPointMode != PlaylistPowerPointMode::None) {
        itemObject.insert(QStringLiteral("powerPointMode"),
                          powerPointModeName(item.powerPointMode));
      }
      items.append(itemObject);
    }
    playlists.append(QJsonObject{
        {QStringLiteral("number"), playlist.number},
        {QStringLiteral("name"), playlist.name},
        {QStringLiteral("mode"), playlistModeName(playlist.mode)},
        {QStringLiteral("manualCrossfadeEnabled"), playlist.manualCrossfadeEnabled},
        {QStringLiteral("items"), items},
    });
  }
  const QJsonObject root{
      {QStringLiteral("format"), QString::fromLatin1(kFormatName)},
      {QStringLiteral("version"), kFormatVersion},
      {QStringLiteral("documentId"), snapshot.documentId},
      {QStringLiteral("nextItemId"), QString::number(snapshot.nextItemId)},
      {QStringLiteral("macros"), MacroDocumentIo::toJson(snapshot.macros)},
      {QStringLiteral("activePlaylistNumber"),
       snapshot.activePlaylistNumber},
      {QStringLiteral("playlists"), playlists},
      {QStringLiteral("manualCrossfadeDurationMs"), snapshot.manualCrossfadeDurationMs},
  };
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::optional<PlaylistModelSnapshot> fromJson(const QByteArray &json,
                                              QString *errorDetail,
                                              const std::atomic_bool *cancel) {
  if (checkCancellation(cancel, errorDetail)) return std::nullopt;
  if (json.size() > kMaximumJsonBytes) {
    setError(errorDetail, QStringLiteral("Playlist JSON exceeds %1 bytes.").arg(kMaximumJsonBytes));
    return std::nullopt;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (checkCancellation(cancel, errorDetail)) return std::nullopt;
  if (parseError.error != QJsonParseError::NoError) {
    setError(errorDetail,
             QStringLiteral("Invalid JSON at offset %1: %2")
                 .arg(parseError.offset)
                 .arg(parseError.errorString()));
    return std::nullopt;
  }
  if (!document.isObject()) {
    setError(errorDetail,
             QStringLiteral("The playlist document root must be an object."));
    return std::nullopt;
  }

  const QJsonObject root = document.object();
  if (!checkJsonBudget(root, errorDetail, cancel)) return std::nullopt;
  if (root.value(QStringLiteral("format")).toString() !=
      QString::fromLatin1(kFormatName)) {
    setError(errorDetail,
             QStringLiteral("This is not a VTR-PON playlist document."));
    return std::nullopt;
  }
  int version = 0;
  if (!readInt(root, QStringLiteral("version"), 1,
               (std::numeric_limits<int>::max)(), &version, errorDetail,
               QStringLiteral("root")) ||
      version != kFormatVersion) {
    if (version != 0 && version != kFormatVersion) {
      setError(errorDetail,
               QStringLiteral("Playlist document version %1 is not supported.")
                   .arg(version));
    }
    return std::nullopt;
  }

  PlaylistModelSnapshot snapshot;
  if (root.contains(QStringLiteral("nextItemId"))) {
    QString nextId;
    if (!readString(root, QStringLiteral("nextItemId"), &nextId,
                    errorDetail, QStringLiteral("root"))) return std::nullopt;
    bool validId = false;
    snapshot.nextItemId = nextId.toULongLong(&validId);
    if (!validId || snapshot.nextItemId == 0 || QString::number(snapshot.nextItemId) != nextId) {
      setError(errorDetail, QStringLiteral("root.nextItemId must be a positive integer ID string."));
      return std::nullopt;
    }
  }
  if (root.contains(QStringLiteral("documentId"))) {
    if (!readString(root, QStringLiteral("documentId"), &snapshot.documentId,
                    errorDetail, QStringLiteral("root")) ||
        QUuid(snapshot.documentId).isNull()) {
      setError(errorDetail, QStringLiteral("root.documentId must be a UUID."));
      return std::nullopt;
    }
  }
  const auto macros = MacroDocumentIo::fromJson(root.value(QStringLiteral("macros")), errorDetail);
  if (!macros) return std::nullopt;
  snapshot.macros = *macros;
  if (root.contains(QStringLiteral("manualCrossfadeDurationMs")) &&
      !readInteger(root, QStringLiteral("manualCrossfadeDurationMs"),
                   kMinimumItemFadeDurationMs, kMaximumItemFadeDurationMs,
                   &snapshot.manualCrossfadeDurationMs, errorDetail,
                   QStringLiteral("root"))) {
    return std::nullopt;
  }
  if (!readInt(root, QStringLiteral("activePlaylistNumber"), 1,
               (std::numeric_limits<int>::max)(),
               &snapshot.activePlaylistNumber, errorDetail,
               QStringLiteral("root"))) {
    return std::nullopt;
  }
  const QJsonValue playlistsValue = root.value(QStringLiteral("playlists"));
  if (!playlistsValue.isArray()) {
    setError(errorDetail, QStringLiteral("root.playlists must be an array."));
    return std::nullopt;
  }

  QSet<int> playlistNumbers;
  QSet<quint64> itemIds;
  const QJsonArray playlists = playlistsValue.toArray();
  if (playlists.size() > kMaximumPlaylists) {
    setError(errorDetail, QStringLiteral("Playlist document exceeds %1 playlists.").arg(kMaximumPlaylists));
    return std::nullopt;
  }
  qsizetype totalItems = 0;
  for (qsizetype playlistIndex = 0; playlistIndex < playlists.size();
       ++playlistIndex) {
    if (checkCancellation(cancel, errorDetail)) return std::nullopt;
    const QString playlistPath =
        QStringLiteral("root.playlists[%1]").arg(playlistIndex);
    const QJsonValue playlistValue = playlists.at(playlistIndex);
    if (!playlistValue.isObject()) {
      setError(errorDetail,
               QStringLiteral("%1 must be an object.").arg(playlistPath));
      return std::nullopt;
    }
    const QJsonObject playlistObject = playlistValue.toObject();
    PlaylistState playlist;
    if (playlistObject.contains(QStringLiteral("manualCrossfadeEnabled")) &&
        !readBool(playlistObject, QStringLiteral("manualCrossfadeEnabled"),
                  &playlist.manualCrossfadeEnabled, errorDetail, playlistPath)) {
      return std::nullopt;
    }
    if (!readInt(playlistObject, QStringLiteral("number"), 1,
                 (std::numeric_limits<int>::max)(), &playlist.number,
                 errorDetail, playlistPath) ||
        !readString(playlistObject, QStringLiteral("name"), &playlist.name,
                    errorDetail, playlistPath)) {
      return std::nullopt;
    }
    const QJsonValue modeValue = playlistObject.value(QStringLiteral("mode"));
    if (!modeValue.isUndefined() &&
        (!modeValue.isString() ||
         !parsePlaylistMode(modeValue.toString(), &playlist.mode))) {
      setError(errorDetail,
               QStringLiteral("%1.mode is not supported.").arg(playlistPath));
      return std::nullopt;
    }
    if (playlistNumbers.contains(playlist.number)) {
      setError(errorDetail,
               QStringLiteral("Playlist number %1 is duplicated.")
                   .arg(playlist.number));
      return std::nullopt;
    }
    playlistNumbers.insert(playlist.number);
    snapshot.playlists.append(playlist);

    const QJsonValue itemsValue =
        playlistObject.value(QStringLiteral("items"));
    if (!itemsValue.isArray()) {
      setError(errorDetail,
               QStringLiteral("%1.items must be an array.")
                   .arg(playlistPath));
      return std::nullopt;
    }
    const QJsonArray items = itemsValue.toArray();
    if (items.size() > kMaximumItems - totalItems) {
      setError(errorDetail, QStringLiteral("Playlist document exceeds %1 items.").arg(kMaximumItems));
      return std::nullopt;
    }
    totalItems += items.size();
    for (qsizetype itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
      if (checkCancellation(cancel, errorDetail)) return std::nullopt;
      const QString itemPath =
          QStringLiteral("%1.items[%2]").arg(playlistPath).arg(itemIndex);
      const QJsonValue itemValue = items.at(itemIndex);
      if (!itemValue.isObject()) {
        setError(errorDetail,
                 QStringLiteral("%1 must be an object.").arg(itemPath));
        return std::nullopt;
      }
      const QJsonObject itemObject = itemValue.toObject();
      PlaylistItemState item;
      QString itemId;
      if (!readString(itemObject, QStringLiteral("id"), &itemId,
                      errorDetail, itemPath)) {
        return std::nullopt;
      }
      bool validItemId = false;
      item.id = itemId.toULongLong(&validItemId);
      if (!validItemId || item.id == 0 || itemIds.contains(item.id)) {
        setError(errorDetail,
                 !validItemId || item.id == 0
                     ? QStringLiteral("%1.id must be a positive integer string.")
                           .arg(itemPath)
                     : QStringLiteral("Playlist item ID %1 is duplicated.")
                           .arg(item.id));
        return std::nullopt;
      }
      itemIds.insert(item.id);
      item.itemNumber = static_cast<int>(itemIndex) + 1;
      item.playlistNumber = playlist.number;
      if (!readString(itemObject, QStringLiteral("filePath"), &item.filePath,
                      errorDetail, itemPath) ||
          !readString(itemObject, QStringLiteral("displayName"),
                      &item.displayName, errorDetail, itemPath)) {
        return std::nullopt;
      }
      QString sourceType;
      if (!readString(itemObject, QStringLiteral("sourceType"), &sourceType,
                      errorDetail, itemPath)) {
        return std::nullopt;
      }
      if (!parseSourceType(sourceType, &item.sourceType)) {
        setError(errorDetail,
                 QStringLiteral("%1.sourceType is not supported.")
                     .arg(itemPath));
        return std::nullopt;
      }
      QString mediaType;
      if (!readString(itemObject, QStringLiteral("mediaType"), &mediaType,
                      errorDetail, itemPath)) {
        return std::nullopt;
      }
      if (!parseMediaType(mediaType, &item.mediaType)) {
        setError(errorDetail,
                 QStringLiteral("%1.mediaType is not supported.")
                     .arg(itemPath));
        return std::nullopt;
      }
      const QJsonValue powerPointModeValue =
          itemObject.value(QStringLiteral("powerPointMode"));
      if (powerPointModeValue.isUndefined()) {
        if (item.sourceType == PlaylistSourceType::PowerPoint &&
            item.mediaType == PlaylistMediaType::Presentation) {
          item.powerPointMode = PlaylistPowerPointMode::NativeSlideShow;
        }
      } else if (!powerPointModeValue.isString() ||
                 !parsePowerPointMode(powerPointModeValue.toString(),
                                      &item.powerPointMode)) {
        setError(errorDetail,
                 QStringLiteral("%1.powerPointMode is not supported.")
                     .arg(itemPath));
        return std::nullopt;
      }
      if (!powerPointModeMatchesItem(item.powerPointMode, item.sourceType,
                                     item.mediaType)) {
        setError(errorDetail,
                 QStringLiteral("%1.powerPointMode does not match its source and media types.")
                     .arg(itemPath));
        return std::nullopt;
      }
      const QJsonValue colorValue =
          itemObject.value(QStringLiteral("color"));
      if (!colorValue.isUndefined()) {
        if (!colorValue.isString() ||
            !parseItemColor(colorValue.toString(), &item.color)) {
          setError(errorDetail,
                   QStringLiteral("%1.color is not supported.")
                       .arg(itemPath));
          return std::nullopt;
        }
      }
      const QJsonValue separatorBelowValue =
          itemObject.value(QStringLiteral("separatorBelow"));
      if (!separatorBelowValue.isUndefined() &&
          !readBool(itemObject, QStringLiteral("separatorBelow"),
                    &item.separatorBelow, errorDetail, itemPath)) {
        return std::nullopt;
      }
      if (!parseSettings(itemObject.value(QStringLiteral("settings")),
                         &item.settings, errorDetail,
                         itemPath + QStringLiteral(".settings"))) {
        return std::nullopt;
      }
      snapshot.items.append(item);
    }
  }
  if (snapshot.playlists.isEmpty()) {
    setError(errorDetail,
             QStringLiteral("The playlist document contains no playlists."));
    return std::nullopt;
  }
  if (!playlistNumbers.contains(snapshot.activePlaylistNumber)) {
    setError(errorDetail,
             QStringLiteral("The active playlist number does not exist."));
    return std::nullopt;
  }
  const auto nextId = PlaylistModel::nextItemIdForSnapshot(snapshot, errorDetail);
  if (!nextId) return std::nullopt;
  snapshot.nextItemId = *nextId;
  if (errorDetail) {
    errorDetail->clear();
  }
  return snapshot;
}

bool writeFile(const QString &filePath, const PlaylistModelSnapshot &snapshot,
               QString *errorDetail) {
  QSaveFile file(filePath);
  if (!file.open(QIODevice::WriteOnly)) {
    setError(errorDetail,
             QStringLiteral("Could not open the playlist file for writing:\n%1")
                 .arg(file.errorString()));
    return false;
  }
  const QByteArray json = toJson(snapshot);
  if (file.write(json) != json.size()) {
    setError(errorDetail,
             QStringLiteral("Could not write the complete playlist file:\n%1")
                 .arg(file.errorString()));
    file.cancelWriting();
    return false;
  }
  if (!file.commit()) {
    setError(errorDetail,
             QStringLiteral("Could not finish writing the playlist file:\n%1")
                 .arg(file.errorString()));
    return false;
  }
  if (errorDetail) {
    errorDetail->clear();
  }
  return true;
}

std::optional<PlaylistModelSnapshot> readFile(const QString &filePath,
                                              QString *errorDetail,
                                              const std::atomic_bool *cancel) {
  if (checkCancellation(cancel, errorDetail)) return std::nullopt;
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    setError(errorDetail,
             QStringLiteral("Could not open the playlist file:\n%1")
                 .arg(file.errorString()));
    return std::nullopt;
  }
  if (file.size() > kMaximumJsonBytes) {
    setError(errorDetail, QStringLiteral("Playlist file exceeds %1 bytes.").arg(kMaximumJsonBytes));
    return std::nullopt;
  }
  QByteArray json;
  while (!file.atEnd()) {
    if (checkCancellation(cancel, errorDetail)) return std::nullopt;
    const QByteArray chunk = file.read(64 * 1024);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError) {
      setError(errorDetail, QStringLiteral("Could not read the playlist file:\n%1").arg(file.errorString()));
      return std::nullopt;
    }
    if (chunk.size() > kMaximumJsonBytes - json.size()) {
      setError(errorDetail, QStringLiteral("Playlist file exceeds %1 bytes.").arg(kMaximumJsonBytes));
      return std::nullopt;
    }
    json.append(chunk);
  }
  return fromJson(json, errorDetail, cancel);
}

std::optional<PreparedRestore> readForRestore(const QString &filePath, QString *errorDetail, const std::atomic_bool *cancel) {
  if (checkCancellation(cancel, errorDetail)) return std::nullopt;
  auto snapshot = readFile(filePath, errorDetail, cancel);
  if (!snapshot || !PlaylistModel::canRestoreSnapshot(*snapshot, errorDetail)) return std::nullopt;
  if (checkCancellation(cancel, errorDetail)) return std::nullopt;
  PreparedRestore prepared{std::move(*snapshot), {}};
  for (auto &item : prepared.snapshot.items) {
    if (checkCancellation(cancel, errorDetail)) return std::nullopt;
    if (item.sourceType == PlaylistSourceType::Uvc) continue;
    item.filePath = vtrpon::restoredPlaylistMediaPath(item.filePath, filePath);
    if (item.displayName.isEmpty()) item.displayName = vtrpon::playlistMediaFileName(item.filePath);
    if (!vtrpon::playlistMediaFileExists(item.filePath)) prepared.offlineItems.insert(item.id);
  }
  return prepared;
}

} // namespace PlaylistDocumentIo
