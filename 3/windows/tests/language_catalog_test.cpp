#include "language_text.h"

#include <QCoreApplication>
#include <QTranslator>

#include <iostream>

namespace {

bool verifyCatalog(QCoreApplication &application, const QString &catalogPath,
                   const QString &expectedSettingsTitle,
                   const QString &expectedExportMessage,
                   const QString &expectedNominalVolume,
                   const QString &expectedProgramSection,
                   const QString &expectedRepeatEndLabel,
                   const QString &expectedFiniteRepeat,
                   const QString &expectedCopyState,
                   const QString &expectedPasteState,
                   const QString &expectedImageDescription,
                   const QString &expectedImageRememberDuration) {
  QTranslator translator;
  if (!translator.load(catalogPath)) {
    std::cerr << "Could not load translation catalog: "
              << catalogPath.toStdString() << '\n';
    return false;
  }
  if (!application.installTranslator(&translator)) {
    std::cerr << "Could not install translation catalog: "
              << catalogPath.toStdString() << '\n';
    return false;
  }

  bool succeeded = true;
  if (vtrpon::uiText("settings.title") != expectedSettingsTitle) {
    std::cerr << "Unexpected Settings translation in "
              << catalogPath.toStdString() << '\n';
    succeeded = false;
  }
  const QString exportMessage =
      vtrpon::LocalizedText("playlist.export.succeeded").arg(2).arg(3).resolve();
  if (exportMessage != expectedExportMessage) {
    std::cerr << "Unexpected argument substitution in "
              << catalogPath.toStdString() << ": "
              << exportMessage.toStdString() << '\n';
    succeeded = false;
  }
  if (vtrpon::uiText("edit.nominal_volume") != expectedNominalVolume) {
    std::cerr << "Unexpected nominal volume terminology in "
              << catalogPath.toStdString() << '\n';
    succeeded = false;
  }
  if (vtrpon::uiText("program.section") != expectedProgramSection) {
    std::cerr << "Unexpected Program section terminology in "
              << catalogPath.toStdString() << '\n';
    succeeded = false;
  }
  if (vtrpon::uiText("edit.repeat_end_mode") != expectedRepeatEndLabel) {
    std::cerr << "Unexpected repeat end label in "
              << catalogPath.toStdString() << '\n';
    succeeded = false;
  }
  if (vtrpon::uiText("playlist.item.fade_in") != QStringLiteral(" + FI") ||
      vtrpon::uiText("playlist.item.fade_out") != QStringLiteral(" + FO") ||
      vtrpon::uiText("playlist.item.end_crossfade") != QStringLiteral(" + CF")) {
    std::cerr << "Unexpected abbreviated transition display in "
              << catalogPath.toStdString() << '\n';
    succeeded = false;
  }
  const QString finiteRepeat =
      vtrpon::uiText("common.repeat") +
      vtrpon::uiText("playlist.item.repeat_finite")
          .arg(2)
          .arg(3)
          .arg(vtrpon::uiText("common.goto"));
  if (finiteRepeat != expectedFiniteRepeat) {
    std::cerr << "Unexpected finite repeat display in "
              << catalogPath.toStdString() << ": "
              << finiteRepeat.toStdString() << '\n';
    succeeded = false;
  }
  if (vtrpon::uiText("playlist.context.copy_state") != expectedCopyState ||
      vtrpon::uiText("playlist.context.paste_state") != expectedPasteState) {
    std::cerr << "Unexpected item state action translation in "
              << catalogPath.toStdString() << '\n';
    succeeded = false;
  }
  if (vtrpon::uiText("image.add.description") !=
          expectedImageDescription ||
      vtrpon::uiText("image.add.remember_session") !=
          expectedImageRememberDuration) {
    std::cerr << "Unexpected image duration terminology in "
              << catalogPath.toStdString() << '\n';
    succeeded = false;
  }
  for (const QString &id : vtrpon::knownUiTextIds()) {
    if (vtrpon::uiText(id.toLatin1().constData()) == id) {
      std::cerr << "Missing ID-based translation: " << id.toStdString()
                << " in " << catalogPath.toStdString() << '\n';
      succeeded = false;
    }
  }

  application.removeTranslator(&translator);
  return succeeded;
}

} // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  if (argc != 3) {
    std::cerr << "Expected English and Japanese catalog paths.\n";
    return 2;
  }

  const bool englishOk = verifyCatalog(
      application, QString::fromLocal8Bit(argv[1]), QStringLiteral("Settings"),
      QStringLiteral("Exported 2 playlist(s) and 3 item(s)."),
      QStringLiteral("Nominal Volume"), QStringLiteral("ON AIR"),
      QStringLiteral("After Repeat"), QStringLiteral("Repeat(2/3)-GOTO"),
      QStringLiteral("Copy State"), QStringLiteral("Paste State"),
      QStringLiteral(
          "Add the original PNG/JPEG file without conversion and set how long VTR-PON displays it. Transparent PNG data is preserved."),
      QStringLiteral("Remember this display duration for this session"));
  const bool japaneseOk = verifyCatalog(
      application, QString::fromLocal8Bit(argv[2]), QStringLiteral("設定"),
      QStringLiteral("2個のプレイリストと3個の項目を書き出しました。"),
      QStringLiteral("規定音量"), QStringLiteral("オンエア"),
      QStringLiteral("回数終了後処理"),
      QStringLiteral("リピート(2/3)-GOTO"),
      QStringLiteral("状態をコピー"), QStringLiteral("状態を貼り付け"),
      QStringLiteral(
          "元のPNG/JPEGファイルを変換せずに追加し、VTR-PONでの表示時間を指定します。透過PNGの透過情報も保持されます。"),
      QStringLiteral("このセッションではこの表示時間を記憶"));
  return englishOk && japaneseOk ? 0 : 1;
}
