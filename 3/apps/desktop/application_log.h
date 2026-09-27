#pragma once

#include <QString>
#include <QStringList>

class BackgroundIoQueue;
class QMenu;
class QMenuBar;
class QWidget;
namespace vtrpon {
class UiLanguageManager;

// Start after QApplication; stop after all media/controller objects are
// destroyed.
void startApplicationLog(const QString &directory = {});
void logStartupSnapshot(const QString &phase);
void stopApplicationLog();
void requestApplicationLogShutdown();
bool applicationLogShutdownComplete();
void attachGStreamerLog();
void setDiagnosticLogging(bool enabled);
bool diagnosticLogging();
void writeApplicationLog(const QString &level, const QString &source,
                         const QString &message);
QStringList applicationLogSnapshot();
QString applicationLogDirectory();
QString applicationLogError();
QMenu *installToolsMenu(QMenuBar *menuBar, QWidget *window,
                        UiLanguageManager *languageManager, BackgroundIoQueue *fileIo = nullptr);
} // namespace vtrpon
