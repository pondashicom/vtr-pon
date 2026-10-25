#include "application_log.h"
#ifdef Q_OS_MACOS
#include "mac_media_runtime.h"
#endif
#include "addon_package.h"
#include "virtual_camera/output.h"
#include "controller_settings.h"
#include "video_output_settings.h"
#include "gpu_configuration.h"
#include "controller_scaling.h"
#include "controller_window_scaling.h"
#include "language_manager.h"
#include "language_text.h"
#include "main_controller.h"
#include "product_identity.h"
#include "skins/controller_skin.h"
#include "settings_style.h"
#include "mac_widget_style.h"
#include "user_info.h"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QProcess>
#include <QDir>
#include <QStandardPaths>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <gst/gst.h>

#include <string>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
// Keep this on the GUI thread: execution state belongs to the calling thread.
// This guard neither changes Windows settings nor overrides lock policies.
class DisplayActivityGuard final : public QAbstractNativeEventFilter
{
public:
    explicit DisplayActivityGuard(QApplication &application)
        : application_(application)
    {
        previousState_ = SetThreadExecutionState(
            ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        if (previousState_ == 0) {
            qWarning() << "DISPLAY_ACTIVITY failed to request display/system wakefulness";
        }
        application_.installNativeEventFilter(this);
    }

    ~DisplayActivityGuard() override
    {
        application_.removeNativeEventFilter(this);
        if (previousState_ != 0) {
            SetThreadExecutionState(previousState_ | ES_CONTINUOUS);
        }
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override
    {
        if (eventType != "windows_generic_MSG" || !message) {
            return false;
        }
        const auto *nativeMessage = static_cast<const MSG *>(message);
        // Windows sends this to the foreground application's window. Other
        // foreground apps and policy-enforced screen savers are outside our
        // control; sleep inhibition alone does not inhibit screen savers.
        if (nativeMessage->message == WM_SYSCOMMAND
            && (nativeMessage->wParam & 0xfff0) == SC_SCREENSAVE) {
            if (result) {
                *result = 0;
            }
            return true;
        }
        return false;
    }

private:
    QApplication &application_;
    EXECUTION_STATE previousState_ = 0;
};
#endif

bool configureGStreamerDllSearchPath()
{
#ifdef _WIN32
    const auto hasRuntime = [](const std::wstring &binDirectory) {
        const std::wstring libraryPath = binDirectory + L"/gstreamer-1.0-0.dll";
        return GetFileAttributesW(libraryPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    };

    std::wstring binDirectory;
    const QDir executable(QCoreApplication::applicationDirPath());
    const bool distributed = vtrpon::isDistributionDirectory(executable.absolutePath());
    if (distributed) {
        const QString binPath = executable.filePath(QStringLiteral("gstreamer/bin"));
        binDirectory = binPath.toStdWString();
        const QString plugins = executable.filePath(
            QStringLiteral("gstreamer/lib/gstreamer-1.0"));
        const QString scanner = executable.filePath(
            QStringLiteral("gstreamer/libexec/gstreamer-1.0/gst-plugin-scanner.exe"));
        SetEnvironmentVariableW(L"GST_PLUGIN_SYSTEM_PATH_1_0",
                                plugins.toStdWString().c_str());
        SetEnvironmentVariableW(L"GST_PLUGIN_PATH_1_0", L"");
        SetEnvironmentVariableW(L"GST_PLUGIN_SCANNER",
                                scanner.toStdWString().c_str());
        const QString originalPath = qEnvironmentVariable("PATH");
        const QString localPath = binPath + QLatin1Char(';') + originalPath;
        SetEnvironmentVariableW(L"PATH", localPath.toStdWString().c_str());
    }
    wchar_t environmentRoot[32768] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"GSTREAMER_1_0_ROOT_MSVC_X86_64", environmentRoot,
        static_cast<DWORD>(std::size(environmentRoot)));
    if (!distributed && length > 0 && length < std::size(environmentRoot)) {
        binDirectory = std::wstring(environmentRoot, length) + L"/bin";
    }

    if (!distributed && !hasRuntime(binDirectory)) {
        binDirectory = VTRPON_GSTREAMER_BIN_DIR;
    }
    return hasRuntime(binDirectory) && SetDllDirectoryW(binDirectory.c_str()) != FALSE;
#elif defined(Q_OS_MACOS)
    return configureMacBundledMediaRuntime();
#else
    return true;
#endif
}

QScreen *fullscreenScreen()
{
    QScreen *mainScreen = QGuiApplication::primaryScreen();
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen != mainScreen) {
            return screen;
        }
    }
    return mainScreen;
}

void placeWindow(QWidget *window, QScreen *screen, const QRect &geometry)
{
    if (!screen) {
        return;
    }

    window->winId();
    if (QWindow *windowHandle = window->windowHandle()) {
        windowHandle->setScreen(screen);
        windowHandle->setGeometry(geometry);
    }
    window->setGeometry(geometry);
}

void showFullscreenOnScreen(QWidget *window, QScreen *screen)
{
    if (!screen) {
        window->showFullScreen();
        return;
    }

    const QRect geometry = screen->geometry();
    placeWindow(window, screen, geometry);
#ifdef __APPLE__
    // A borderless normal window does not hide macOS's menu bar or Dock.
    window->showFullScreen();
#else
    window->show();
    placeWindow(window, screen, geometry);
#endif
    window->raise();
}

void showControllerOnMainScreen(QWidget *window)
{
    QScreen *mainScreen = QGuiApplication::primaryScreen();
    window->adjustSize();
    window->resize(window->sizeHint().expandedTo(window->minimumSizeHint()));
    if (mainScreen) {
        const QRect available = mainScreen->availableGeometry();
        QRect geometry(QPoint(0, 0), window->size());
        geometry.moveCenter(available.center());
        placeWindow(window, mainScreen, geometry);
    }
    window->showMaximized();
}

qreal controllerScaleForScreen(QScreen *screen)
{
    if (!screen) {
        return 1.0;
    }
    return vtrpon::controllerScaleForGeometry(
        screen->geometry().size(), screen->availableGeometry().size());
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
#ifdef __APPLE__
    vtrpon::installMacComboPopupStyle(app);
#endif
    QIcon appIcon;
    for (const int size : {16, 32, 48, 64, 128, 256}) {
        appIcon.addFile(QStringLiteral(":/vtrpon/assets/icons/vtr-pon3/windows/png/VTR-PON3_%1x%1.png")
                            .arg(size));
    }
    app.setWindowIcon(appIcon);
    vtrpon::initializeProductIdentity();
    app.setApplicationDisplayName(QStringLiteral("VTR-PON"));
    app.setApplicationVersion(QStringLiteral(VTRPON_APP_VERSION));
    vtrpon::initializeControllerSettingsLocation();
    const QString cacheDirectory = vtrpon::productCacheDirectory();
    if (QDir().mkpath(cacheDirectory)) {
        QString registryName = QStringLiteral("gstreamer-registry.bin");
#ifdef Q_OS_MACOS
        if (QFileInfo(QCoreApplication::applicationDirPath()+QStringLiteral("/../Resources/vtrpon-runtime.json")).isFile())
            registryName = QStringLiteral("gstreamer-bundled-registry.bin");
#endif
        const QString registry = QDir(cacheDirectory).filePath(registryName);
#ifdef _WIN32
        SetEnvironmentVariableW(L"GST_REGISTRY_1_0", registry.toStdWString().c_str());
#else
        qputenv("GST_REGISTRY_1_0", QFile::encodeName(registry));
#endif
    }
    vtrpon::freezeGpuPreferences();
    vtrpon::startApplicationLog();
    struct LogLifetime {
      ~LogLifetime() { vtrpon::stopApplicationLog(); }
    } logLifetime;
    vtrpon::UiLanguageManager languageManager(&app);
    QString languageError;
    if (!languageManager.initialize(&languageError)) {
        qWarning().noquote() << languageError;
    }
#ifdef Q_OS_MACOS
    // Development activation uses the same production request without loading
    // a playback session or changing add-on selections in the /Applications copy.
    if(app.arguments().contains(QStringLiteral("--activate-virtual-camera"))) {
        vtrpon::activateVirtualCameraExtension();
        QTimer::singleShot(60000,&app,&QApplication::quit);
        return app.exec();
    }
#endif
    vtrpon::setUiTextSize(vtrpon::makeControllerSettings().value("accessibility/textSize", 0).toInt());
    vtrpon::setUiScale(controllerScaleForScreen(app.primaryScreen()));
    // Menus and other controls without an explicit QSS font use the app font.
    QFont controllerFont = app.font();
    if (controllerFont.pointSizeF() > 0) {
        controllerFont.setPointSizeF(controllerFont.pointSizeF() * vtrpon::uiScale() * vtrpon::uiTextScale());
    } else {
        controllerFont.setPixelSize(vtrpon::scaleTextPx(controllerFont.pixelSize()));
    }
    app.setFont(controllerFont);
    if (const QScreen *screen = app.primaryScreen()) {
        qInfo() << "CONTROLLER_SCALE logicalScreen=" << screen->geometry().size()
                << "available=" << screen->availableGeometry().size()
                << "devicePixelRatio=" << screen->devicePixelRatio()
                << "uiScale=" << vtrpon::uiScale();
    }
    QString skinError;
    if (!vtrpon::applyControllerSkin(
            app, vtrpon::configuredControllerSkinKey(), &skinError)) {
        qWarning().noquote() << skinError;
    }
    // Windows supplies separate class fonts for menus.
    app.setFont(controllerFont, "QMenuBar");
    app.setFont(controllerFont, "QMenu");

    const QString addonState = vtrpon::addons::stateDirectory(
        QCoreApplication::applicationDirPath());
    const bool addonJobPending = !vtrpon::addons::pendingJobs(
        QCoreApplication::applicationDirPath()).isEmpty();
    const auto showAddonStartupMessage = [&app](QMessageBox::Icon icon, const QString &text) {
        QMessageBox message(icon, vtrpon::uiText("settings.addons.title"), text, QMessageBox::Ok);
        message.setObjectName(QStringLiteral("addonStartupDialog"));
        message.setPalette(vtrpon::controllerSkinPalette(
            app, vtrpon::ControllerSkinPalette::VtrPonClasic));
        message.setStyleSheet(vtrpon::settingsStyleSheet(message.objectName()));
        message.exec();
    };
    QString addonRestartError;
    const QString errorPrefix = QStringLiteral("--addon-restart-error=");
    for (const auto &argument : app.arguments()) {
        if (argument.startsWith(errorPrefix)) addonRestartError = argument.sliced(errorPrefix.size());
    }
    if (!addonRestartError.isEmpty()) {
        showAddonStartupMessage(QMessageBox::Warning,
            vtrpon::uiText("settings.addons.restart_failed").arg(addonRestartError));
        if (addonJobPending) return 1;
    } else if (addonJobPending ||
               (!app.arguments().contains(QStringLiteral("--addon-restart-complete")) &&
                vtrpon::addons::restartInProgress(QCoreApplication::applicationDirPath()))) {
        QString error;
        const bool launched = vtrpon::addons::requestRestart(
            QCoreApplication::applicationDirPath(), app.arguments().mid(1), QDir::currentPath(), &error);
        showAddonStartupMessage(launched ? QMessageBox::Information : QMessageBox::Warning,
            launched ? vtrpon::uiText("settings.addons.startup_resuming") :
                vtrpon::uiText("settings.addons.restart_failed").arg(error + QStringLiteral("\n") + addonState));
        return launched ? 0 : 1;
    }

    if (!configureGStreamerDllSearchPath()) {
        QWidget startupErrorWindow;
        startupErrorWindow.setObjectName(
            QStringLiteral("controllerWindow"));
        startupErrorWindow.setAttribute(Qt::WA_StyledBackground, true);
        startupErrorWindow.setWindowTitle(
            vtrpon::uiText("window.controller.title"));
        startupErrorWindow.setMinimumWidth(vtrpon::scalePx(760));

        UserInfoCenter userInfoCenter(&startupErrorWindow);
        auto *layout = new QVBoxLayout(&startupErrorWindow);
        layout->setContentsMargins(vtrpon::scalePx(10),
                                   vtrpon::scalePx(10),
                                   vtrpon::scalePx(10),
                                   vtrpon::scalePx(10));
        layout->addWidget(new UserInfoBar(&userInfoCenter,
                                          &startupErrorWindow));
        userInfoCenter.showError(
            QStringLiteral("SYSTEM"),
            vtrpon::LocalizedText("startup.gstreamer_runtime_missing"));
        showControllerOnMainScreen(&startupErrorWindow);
        return app.exec();
    }

    gst_init(&argc, &argv);
#ifdef Q_OS_MACOS
    configureMacAudioDecoders();
#endif
    vtrpon::attachGStreamerLog();
    QCommandLineParser commandLine;
    commandLine.setApplicationDescription(
        vtrpon::uiText("application.description"));
    commandLine.addHelpOption();
    QCommandLineOption addonRestartComplete(QStringLiteral("addon-restart-complete"));
    addonRestartComplete.setFlags(QCommandLineOption::HiddenFromHelp);
    commandLine.addOption(addonRestartComplete);
    QCommandLineOption addonRestartFailure(QStringLiteral("addon-restart-error"), QString(), QStringLiteral("detail"));
    addonRestartFailure.setFlags(QCommandLineOption::HiddenFromHelp);
    commandLine.addOption(addonRestartFailure);
    commandLine.addOption(QCommandLineOption(QStringLiteral("portable"),
        QStringLiteral("Use config/<product profile>/user.ini beside the executable (Windows).")));
    const QCommandLineOption output4kOption(
        QStringLiteral("program-output-4k"),
        vtrpon::uiText("application.option.program_output_4k"));
    commandLine.addOption(output4kOption);
    commandLine.process(app);
    const bool use4kProgramOutput = vtrpon::initializeProgramOutputResolution(
        commandLine.isSet(output4kOption));

#ifdef _WIN32
    DisplayActivityGuard displayActivityGuard(app);
#endif

    vtrpon::VirtualCameraRuntime virtualCameraRuntime;

    QWidget videoWindow;
    videoWindow.setWindowTitle(vtrpon::uiText("window.player.title"));
    videoWindow.setWindowFlag(Qt::FramelessWindowHint, true);
    videoWindow.setObjectName(QStringLiteral("programVideoWindow"));
    videoWindow.setCursor(Qt::BlankCursor);
    videoWindow.setAttribute(Qt::WA_NativeWindow, true);
    videoWindow.winId();
#ifdef __APPLE__
    // Resolve the startup output display before selecting its rendering GPU.
    // Enter its fullscreen Space only after the Controller/menu is ready.
    if (auto *screen = fullscreenScreen()) placeWindow(&videoWindow, screen, screen->geometry());
#endif

    auto *controlWindow = new QWidget;
    controlWindow->setObjectName(QStringLiteral("controllerWindow"));
    controlWindow->setAttribute(Qt::WA_StyledBackground, true);
    controlWindow->setAttribute(Qt::WA_DeleteOnClose, true);
    controlWindow->setWindowTitle(vtrpon::uiText("window.controller.title"));
    controlWindow->setMinimumWidth(vtrpon::scalePx(664));

#ifndef __APPLE__
    showFullscreenOnScreen(&videoWindow, fullscreenScreen());
#endif
    MainController controller(&videoWindow, controlWindow,
                              use4kProgramOutput, &languageManager);
    if (!controller.isAvailable()) {
        videoWindow.hide();
    }

    QObject::connect(controlWindow, &QObject::destroyed, &app, &QApplication::quit);
    QObject::connect(
        &languageManager, &vtrpon::UiLanguageManager::languageChanged,
        controlWindow, [&videoWindow, controlWindow] {
            videoWindow.setWindowTitle(vtrpon::uiText("window.player.title"));
            controlWindow->setWindowTitle(
                vtrpon::uiText("window.controller.title"));
        });

    showControllerOnMainScreen(controlWindow);
#ifdef __APPLE__
    // Create the Controller and its application menus before Cocoa enters the
    // output window's fullscreen Space. Keep the operator window accessible.
    if (controller.isAvailable())
        showFullscreenOnScreen(&videoWindow, fullscreenScreen());
    controlWindow->raise();
    controlWindow->activateWindow();
#endif
    // Windows delivers the maximized client geometry after showMaximized().
    QTimer::singleShot(0, controlWindow, [controlWindow] {
        new vtrpon::ControllerWindowScaling(controlWindow);
    });
    vtrpon::logStartupSnapshot(QStringLiteral("controller-ready"));
    return app.exec();
}
