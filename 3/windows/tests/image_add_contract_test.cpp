#include "image_add_dialog.h"
#include "still_capture.h"
#include "image_add_registration.h"
#include "language_text.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>

namespace {

bool check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

QByteArray fileDigest(const QString &filePath) {
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QCryptographicHash::hash(file.readAll(),
                                  QCryptographicHash::Sha256);
}

bool createFixtures(const QString &pngPath, const QString &jpegPath) {
  QImage png(QSize(8, 8), QImage::Format_ARGB32);
  png.fill(qRgba(20, 120, 220, 0));
  png.setPixelColor(4, 4, QColor(220, 40, 30, 128));

  QImage jpeg(QSize(8, 8), QImage::Format_RGB32);
  jpeg.fill(QColor(20, 80, 180));
  return png.save(pngPath) && jpeg.save(jpegPath);
}

bool verifyExtensionRouting() {
  return check(isPngOrJpegImagePath(QStringLiteral("still.png")),
               "PNG was not routed to the image dialog") &&
         check(isPngOrJpegImagePath(QStringLiteral("still.JPG")),
               "Uppercase JPG was not routed to the image dialog") &&
         check(isPngOrJpegImagePath(QStringLiteral("still.JpEg")),
               "Mixed-case JPEG was not routed to the image dialog") &&
         check(!isPngOrJpegImagePath(QStringLiteral("still.bmp")),
               "BMP was unexpectedly routed to the image dialog") &&
         check(!isPngOrJpegImagePath(QStringLiteral("still.webp")),
               "WebP was unexpectedly routed to the image dialog") &&
         check(!isPngOrJpegImagePath(QStringLiteral("movie.mp4")),
               "Video was unexpectedly routed to the image dialog");
}

bool verifyDialogContract() {
  ImageAddDialog dialog;
  auto *settingsGroup = dialog.findChild<QGroupBox *>(
      QStringLiteral("imageSettingsGroup"));
  auto *duration = dialog.findChild<QDoubleSpinBox *>(
      QStringLiteral("imageDisplayDuration"));
  auto *remember = dialog.findChild<QCheckBox *>(
      QStringLiteral("imageRememberForSession"));
  auto *buttonBox = dialog.findChild<QDialogButtonBox *>();
  bool succeeded =
      check(dialog.objectName() == QStringLiteral("imageAddDialog"),
            "Image add dialog object name changed") &&
      check(settingsGroup && duration && remember && buttonBox,
            "Image add dialog controls were incomplete") &&
      check(dialog.findChildren<QRadioButton *>().isEmpty(),
            "Image add dialog still exposes a conversion method") &&
      check(dialog.windowTitle() == vtrpon::uiText("image.add.title"),
            "Image add dialog title changed");
  if (!succeeded) {
    return false;
  }

  const ImageAddSettings defaults = dialog.settings();
  succeeded =
      check(defaults.displayDurationMs == 5000 &&
                !defaults.rememberForSession,
            "Image add dialog defaults changed") &&
      check(std::abs(duration->minimum() - 0.1) < 0.001 &&
                std::abs(duration->maximum() - 600.0) < 0.001 &&
                std::abs(duration->singleStep() - 0.5) < 0.001,
            "Image display-duration range changed") &&
      check(duration->focusPolicy() != Qt::NoFocus &&
                remember->focusPolicy() != Qt::NoFocus &&
                !duration->accessibleName().isEmpty() &&
                !duration->accessibleDescription().isEmpty(),
            "Image duration keyboard or accessibility contract changed") &&
      succeeded;

  QLabel *durationLabel = nullptr;
  for (QLabel *label : dialog.findChildren<QLabel *>()) {
    if (label->buddy() == duration) {
      durationLabel = label;
      break;
    }
  }
  succeeded = check(durationLabel != nullptr,
                    "Image duration label has no keyboard buddy") &&
              succeeded;

  QPushButton *addButton = buttonBox->button(QDialogButtonBox::Ok);
  QPushButton *cancelButton = buttonBox->button(QDialogButtonBox::Cancel);
  succeeded =
      check(addButton && cancelButton && addButton->isDefault() &&
                addButton->focusPolicy() != Qt::NoFocus &&
                cancelButton->focusPolicy() != Qt::NoFocus,
            "Image dialog buttons are not keyboard operable") &&
      succeeded;

  duration->setValue(3.5);
  remember->setChecked(true);
  const ImageAddSettings selected = dialog.settings();
  succeeded =
      check(selected.displayDurationMs == 3500 &&
                selected.rememberForSession,
            "Image duration settings were not preserved") &&
      succeeded;
  return succeeded;
}

bool verifyRegistrationContract(const QString &pngPath,
                                const QString &jpegPath,
                                const QString &temporaryDirectory) {
  const QByteArray pngBefore = fileDigest(pngPath);
  const auto pngItem =
      makeImagePlaylistRegistration(pngPath, 3500, 4);
  bool succeeded =
      check(pngItem.has_value(), "Transparent PNG was not registered");
  if (pngItem) {
    succeeded =
        check(pngItem->filePath == pngPath &&
                  pngItem->displayName.isEmpty() &&
                  pngItem->sourceType == PlaylistSourceType::File &&
                  pngItem->mediaType == PlaylistMediaType::StillImage &&
                  pngItem->powerPointMode == PlaylistPowerPointMode::None &&
                  pngItem->settings.outPointMs == 3500 &&
                  pngItem->playlistNumber == 4,
              "PNG registration did not preserve the original-file contract") &&
        succeeded;
  }

  const QImage pngAfter(pngPath);
  succeeded =
      check(!pngBefore.isEmpty() && pngBefore == fileDigest(pngPath) &&
                pngAfter.hasAlphaChannel() &&
                pngAfter.pixelColor(0, 0).alpha() == 0 &&
                pngAfter.pixelColor(4, 4).alpha() == 128,
            "PNG registration modified or lost source alpha data") &&
      succeeded;

  const auto jpegItem =
      makeImagePlaylistRegistration(jpegPath, 2000, 5);
  succeeded =
      check(jpegItem &&
                jpegItem->filePath == jpegPath &&
                jpegItem->mediaType == PlaylistMediaType::StillImage &&
                jpegItem->settings.outPointMs == 2000 &&
                jpegItem->playlistNumber == 5,
            "JPEG registration contract changed") &&
      succeeded;

  const QString missingPng =
      QDir(temporaryDirectory).filePath(QStringLiteral("missing.png"));
  succeeded =
      check(!makeImagePlaylistRegistration(missingPng, 1000, 1) &&
                !makeImagePlaylistRegistration(pngPath, 0, 1) &&
                !makeImagePlaylistRegistration(pngPath, 1000, 0) &&
                !makeImagePlaylistRegistration(
                    QDir(temporaryDirectory).filePath(
                        QStringLiteral("unsupported.webp")),
                    1000, 1),
            "Invalid image registration input was accepted") &&
      succeeded;
  return succeeded;
}

} // namespace

int main(int argc, char *argv[]) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef _WIN32
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("windows"));
#else
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
#endif
  }
  QApplication application(argc, argv);

  QTemporaryDir temporaryDirectory;
  if (!temporaryDirectory.isValid()) {
    std::cerr << "Could not create a temporary image directory.\n";
    return 2;
  }
  const QString pngPath = QDir(temporaryDirectory.path())
                              .filePath(QStringLiteral("transparent.png"));
  const QString jpegPath = QDir(temporaryDirectory.path())
                               .filePath(QStringLiteral("still.jpeg"));
  if (!createFixtures(pngPath, jpegPath)) {
    std::cerr << "Could not create PNG/JPEG fixtures.\n";
    return 2;
  }


  const auto capture = [] {
    QImage image(640, 360, QImage::Format_RGB32);
    image.fill(QColor("#123456"));
    return image;
  };
  const auto first = saveStillCapture(capture, temporaryDirectory.filePath("captures"), false, 3750, 3);
  const auto second = saveStillCapture(capture, temporaryDirectory.filePath("captures"), true, 5000, 3);
  const QImage saved(first.path);
  if (!check(first.item && second.item && first.path != second.path &&
             saved.size() == QSize(1920, 1080) && saved.pixelColor(960, 540) == QColor("#123456") &&
             first.item->settings.outPointMs == 3750 && first.item->playlistNumber == 3 &&
             first.item->mediaType == PlaylistMediaType::StillImage,
             "Still capture PNG resolution, content, duration, destination or unique name is wrong"))
    return 1;
  if (!check(!saveStillCapture({}, temporaryDirectory.path(), false, 5000, 1).item &&
             !saveStillCapture(capture, pngPath, false, 5000, 1).item &&
             QImage(pngPath).size() != QSize(1920, 1080),
             "Missing frames or a file used as destination must fail without overwriting files"))
    return 1;

  return verifyExtensionRouting() && verifyDialogContract() &&
                 verifyRegistrationContract(pngPath, jpegPath,
                                            temporaryDirectory.path())
             ? 0
             : 1;
}
