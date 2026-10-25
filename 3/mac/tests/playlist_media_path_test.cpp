#include "playlist_document_io.h"
#include "playlist_media_path.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
static bool makeFile(const QString &path) {
  QFile file(path); return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temporary;
  CHECK(temporary.isValid());
  const QString directory = temporary.filePath(QStringLiteral("日本語 素材 #100%"));
  CHECK(QDir().mkpath(directory));
  const QString first = QDir(directory).filePath(QStringLiteral("映像 ①.mov"));
  CHECK(makeFile(first));
  PlaylistModel model;
  PlaylistItemRegistration registration;
  registration.filePath = first;
  registration.mediaType = PlaylistMediaType::Video;
  registration.displayName = QStringLiteral("番組の名前");
  registration.color = PlaylistItemColor::Blue;
  registration.separatorBelow = true;
  registration.settings.inPointMs = 100;
  registration.settings.outPointMs = 2000;
  const auto ids = model.addItems({registration, registration});
  CHECK(ids.size() == 2);
  auto snapshot = model.snapshot();
  const QString windowsPath = QStringLiteral(R"(D:\収録素材\映像 ①.mov)");
  snapshot.items[0].filePath = windowsPath;
  snapshot.items[0].settings.endMode = PlaylistEndMode::Goto;
  snapshot.items[0].settings.gotoTargetItemId = ids[1];
  MacroDefinition macro;
  MacroStep step;
  step.type = MacroStepType::Cue; step.itemId = ids[0]; step.documentId = snapshot.documentId;
  macro.steps = {step}; snapshot.macros = {macro};
  const QString document = QDir(directory).filePath(QStringLiteral("日本語 プレイリスト.vtrpon"));
  CHECK(PlaylistDocumentIo::writeFile(document, snapshot));
  auto prepared = PlaylistDocumentIo::readForRestore(document);
  CHECK(prepared && prepared->snapshot.items[0].filePath == windowsPath);
  CHECK(prepared->offlineItems.contains(ids[0]) && !prepared->offlineItems.contains(ids[1]));
  CHECK(model.restoreSnapshot(prepared->snapshot, nullptr, &prepared->offlineItems));
  CHECK(model.itemState(ids[0])->filePath == windowsPath && model.itemMediaOffline(ids[0]));
  CHECK(model.macros() == snapshot.macros);
  const auto stable = model.snapshot();
  CHECK(model.macros() == snapshot.macros && model.itemCount() == 2);
  CHECK(PlaylistDocumentIo::writeFile(document, stable));
  prepared = PlaylistDocumentIo::readForRestore(document);
  CHECK(prepared && prepared->snapshot == stable);
  CHECK(prepared->snapshot.items[0].filePath == windowsPath);
  CHECK(prepared->offlineItems.contains(ids[0]) && !prepared->offlineItems.contains(ids[1]));

  // Missing Windows drive and UNC paths retain their spelling; native names
  // containing a backslash are not globally rewritten.
  for (const auto &path : {windowsPath, QStringLiteral("Z:/素材/映像.mov"), QStringLiteral("//server/共有/映像.mov"),
       QStringLiteral(R"(\\server\共有\映像.mov)")}) {
    snapshot.items[0].filePath = path; snapshot.items[0].displayName.clear();
    CHECK(PlaylistDocumentIo::writeFile(document, snapshot));
    prepared = PlaylistDocumentIo::readForRestore(document);
    CHECK(prepared && prepared->snapshot.items[0].filePath == path);
    CHECK(prepared->offlineItems.contains(ids[0]));
    CHECK(prepared->snapshot.items[0].displayName ==
          (path.contains(QStringLiteral("①")) ? QStringLiteral("映像 ①.mov") : QStringLiteral("映像.mov")));
    CHECK(model.restoreSnapshot(prepared->snapshot));
    CHECK(model.itemState(ids[0])->filePath == path && model.itemMediaOffline(ids[0]));
  }
  // Relative references are resolved from the playlist, independently of cwd.
  snapshot.items[0].filePath = QStringLiteral("映像 ①.mov");
  CHECK(PlaylistDocumentIo::writeFile(document, snapshot));
  const auto oldCwd = QDir::currentPath(); CHECK(QDir::setCurrent(temporary.path()));
  prepared = PlaylistDocumentIo::readForRestore(document);
  CHECK(QDir::setCurrent(oldCwd));
  CHECK(prepared && prepared->snapshot.items[0].filePath == first && prepared->offlineItems.isEmpty());
  // Literal backslash is legal in a native Mac filename.
  const QString native = QDir(directory).filePath(QStringLiteral(R"(素材\別名.mov)"));
  CHECK(makeFile(native)); CHECK(vtrpon::playlistMediaFileExists(native));
  CHECK(vtrpon::playlistMediaFileName(native) == QStringLiteral(R"(素材\別名.mov)"));
  std::atomic_bool canceled{true};
  CHECK(!PlaylistDocumentIo::readForRestore(document, nullptr, &canceled));
  return 0;
}
