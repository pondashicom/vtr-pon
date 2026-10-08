#include "decklink.h"
#include "controller_settings.h"
#include "test_pattern_store.h"
#include "virtual_camera/output.h"
#include "main_controller.h"
#include "live_audio_source.h"
#include "macro_editor_window.h"
#include "macro_runner.h"
#include "vst_service.h"
#include "application_log.h"
#include "addon_package.h"
#include "settings_style.h"
#include "about_window.h"
#include "application_shutdown_deadline.h"
#include "background_io_queue.h"
#include "media_thumbnailer.h"
#include "audio_output_coordinator.h"
#include "document_import_queue.h"
#include <QJsonDocument>

#include "controller_shortcuts.h"
#include "controller_operations.h"
#include "controller_settings_dialog.h"
#include "controller_settings_io.h"
#include "skins/controller_skin.h"
#include <QApplication>
#include <QMessageBox>
#include <QKeySequence>
#include "edit_panel.h"
#include "edit_session_controller.h"
#include "input_control_panel.h"
#include "file_management_settings.h"
#include "still_capture.h"
#include "image_add_dialog.h"
#include "image_add_registration.h"
#include "media_import_queue.h"
#include "network_time_clock.h"
#include "system_load_widget.h"
#include "playlist_document_io.h"
#include "playlist_item.h"
#include "playlist_model.h"
#include "playlist_panel.h"
#include "program_panel.h"
#include "program_playout_controller.h"
#include "switcher_control.h"
#include "user_info.h"
#include "uvc_audio_mapping.h"
#include "uvc_device_monitor.h"
#include "uvc_source_session_manager.h"
#include "network_video.h"
#include "view_menu_controller.h"
#include "controller_scaling.h"
#include "language_manager.h"
#include "language_text.h"
#include "update_notification.h"

#ifdef _WIN32
#include "document_add_method_dialog.h"
#include "pdf_import_service.h"
#include "powerpoint_add_method_dialog.h"
#include "powerpoint_source_session_manager.h"
#endif

#include <QAction>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHash>
#include <QMenu>
#include <QMenuBar>
#include <QLabel>
#include <QObject>
#include <QPointer>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyleOptionButton>
#include <QSettings>
#include <QSet>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <optional>

namespace {

class SettingsButton final : public QPushButton {
public:
  explicit SettingsButton(QWidget *parent) : QPushButton(parent) {
    setObjectName(QStringLiteral("controllerSettingsButton"));
    setFocusPolicy(Qt::TabFocus);
    setFixedSize(vtrpon::scalePx(32), vtrpon::scalePx(24));
    setCursor(Qt::PointingHandCursor);
  }

protected:
  void paintEvent(QPaintEvent *event) override {
    QPushButton::paintEvent(event);
    QStyleOptionButton option;
    initStyleOption(&option);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(rect().center() + QPointF(0.5, 0.5));
    const qreal scale = vtrpon::scalePx(18) / 20.0;
    painter.scale(scale, scale);
    QPainterPath gear;
    constexpr qreal pi = 3.14159265358979323846;
    for (int point = 0; point < 32; ++point) {
      const qreal angle = (point - 0.5) * pi / 16.0;
      const qreal radius = point % 4 < 2 ? 9.5 : 7.0;
      const QPointF vertex(radius * std::cos(angle), radius * std::sin(angle));
      if (point == 0) gear.moveTo(vertex);
      else gear.lineTo(vertex);
    }
    gear.closeSubpath();
    gear.addEllipse(QPointF(0, 0), 3.2, 3.2);
    gear.setFillRule(Qt::OddEvenFill);
    painter.fillPath(gear, option.palette.buttonText());
  }
};

enum class PlaylistRestoreSource {
  ManualImport,
  PreviousSession,
};

QString previousSessionFilePath() {
  const QString dataDirectory =
      QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  return dataDirectory.isEmpty()
             ? QString()
             : QDir(dataDirectory).filePath(
                   QStringLiteral("previous-session.json"));
}

} // namespace

class MainController::Impl final : public QObject {
public:
  Impl(QWidget *videoWindow, QWidget *controlWindow, bool use4kProgramOutput,
       vtrpon::UiLanguageManager *languageManager)
      : videoWindow_(videoWindow), controlWindow_(controlWindow),
        languageManager_(languageManager),
        operations_(new ControllerOperations(this)),
        userInfoCenter_(new UserInfoCenter(this)),
        playlistModel_(new PlaylistModel(this)),
        mediaImports_(new MediaImportQueue(playlistModel_, this)),
        uvcDeviceMonitor_(new UvcDeviceMonitor(this)),
        audioInputDeviceMonitor_(new AudioInputDeviceMonitor(this)),
        uvcAudioMappingStore_(new UvcAudioMappingStore(this)),
        uvcSourceSessions_(new UvcSourceSessionManager(
            use4kProgramOutput, this)),
        editPanel_(new EditPanel(userInfoCenter_, controlWindow, operations_)),
        playlistPanel_(new PlaylistPanel(playlistModel_, controlWindow, true,
                                         userInfoCenter_, true, operations_)),
        programPanel_(new ProgramPanel(controlWindow, operations_)),
        inputControlPanel_(new InputControlPanel(controlWindow, operations_)),
        editController_(new EditSessionController(editPanel_, playlistPanel_,
                                                   playlistModel_, uvcSourceSessions_,
                                                   uvcAudioMappingStore_, this)),
        programController_(new ProgramPlayoutController(
            videoWindow, programPanel_, playlistPanel_, playlistModel_,
            userInfoCenter_, use4kProgramOutput, uvcSourceSessions_,
            uvcAudioMappingStore_, this)),
        switcherController_(new vtrpon::SwitcherController(
            programController_, this)) {

    fileIo_ = new BackgroundIoQueue(this);
    documentImports_ = new DocumentImportQueue(
        playlistModel_, programController_, userInfoCenter_, fileIo_, this);
#ifdef _WIN32
    powerPointComRegistered_ = powerPointComRegistered();
    connect(documentImports_, &DocumentImportQueue::powerPointSettingsRemembered,
            this, [this](const PowerPointAddSettings &settings) {
      rememberedPowerPointAddSettings_ = settings;
    });
#endif
    // Only content changes invalidate a pending import. Playback references are
    // rechecked separately immediately before the model commit.
    const auto edited = [this] { ++playlistRevision_; };
    connect(playlistModel_, &PlaylistModel::itemAdded, this, edited);
    connect(playlistModel_, &PlaylistModel::itemRemoved, this, edited);
    connect(playlistModel_, &PlaylistModel::itemMoved, this, edited);
    connect(playlistModel_, &PlaylistModel::itemMovedToPlaylist, this, edited);
    connect(playlistModel_, &PlaylistModel::itemChanged, this, edited);
    connect(playlistModel_, &PlaylistModel::playlistAdded, this, edited);
    connect(playlistModel_, &PlaylistModel::playlistRemoved, this, edited);
    connect(playlistModel_, &PlaylistModel::playlistRemoved, this, [this](int number) {
      if (stillCapturePlaylist_ == number) stillCapturePlaylist_ = 0;
    });
    connect(playlistModel_, &PlaylistModel::playlistNameChanged, this, edited);
    connect(playlistModel_, &PlaylistModel::playlistModeChanged, this, edited);
    connect(playlistModel_, &PlaylistModel::activePlaylistChanged, this, edited);
    connect(playlistModel_, &PlaylistModel::modelReset, this, edited);
    connect(playlistModel_, &PlaylistModel::macrosChanged, this, edited);
    connect(mediaImports_, &MediaImportQueue::importsFailed, this,
            [this](const QStringList &failures) {
      userInfoCenter_->showWarning(QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.add_media.some_failed"),
          failures.join(QLatin1Char('\n')));
    });
    videoWindow_->installEventFilter(this);
    controlWindow_->installEventFilter(this);

    auto *windowLayout = new QVBoxLayout(controlWindow_);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->setSpacing(0);

    auto *menuBar = new QMenuBar(controlWindow_);
    menuBar->setObjectName(QStringLiteral("applicationMenuBar"));
    fileMenu_ = menuBar->addMenu(QString());
    fileMenu_->setObjectName(QStringLiteral("fileMenu"));

    addFileAction_ = fileMenu_->addAction(QString());
    fileMenu_->addSeparator();
    exportPlaylistAction_ = fileMenu_->addAction(QString());
    exportPlaylistAction_->setObjectName(
        QStringLiteral("exportPlaylistAction"));
    importPlaylistAction_ = fileMenu_->addAction(QString());
    importPlaylistAction_->setObjectName(
        QStringLiteral("importPlaylistAction"));
    restorePreviousSessionAction_ = fileMenu_->addAction(QString());
    restorePreviousSessionAction_->setObjectName(
        QStringLiteral("restorePreviousSessionAction"));
    fileMenu_->addSeparator();
    exportSettingsAction_ = fileMenu_->addAction(QString());
    exportSettingsAction_->setObjectName(QStringLiteral("exportSettingsAction"));
    importSettingsAction_ = fileMenu_->addAction(QString());
    importSettingsAction_->setObjectName(QStringLiteral("importSettingsAction"));
    connect(exportSettingsAction_, &QAction::triggered, this, [this] { exportSettings(); });
    connect(importSettingsAction_, &QAction::triggered, this, [this] { importSettings(); });
    fileMenu_->addSeparator();
    settingsAction_ = fileMenu_->addAction(QString());
    settingsAction_->setObjectName(QStringLiteral("settingsAction"));
    settingsAction_->setAutoRepeat(false);
    fileMenu_->addSeparator();
    exitAction_ = fileMenu_->addAction(QString());
    operations_->bindMember(ControllerOperations::Action::AddFilesDialog, this,
                            &Impl::registerPlaylistItems);
    operations_->bindMember(ControllerOperations::Action::Exit, controlWindow,
                            &QWidget::close);
    operations_->setGuard<>(ControllerOperations::Action::OpenSettings, this,
        [this] { return controlWindow_ && !closing_ && !activeSettingsDialog_; });
    operations_->bind<>(ControllerOperations::Action::OpenSettings, this,
        [this] { showSettings(); });
    connect(addFileAction_, &QAction::triggered, this, [this] {
      (void)operations_->executeFrom("Menu", ControllerOperations::Action::AddFilesDialog);
    });
    connect(exportPlaylistAction_, &QAction::triggered, this,
            &Impl::exportPlaylist);
    connect(importPlaylistAction_, &QAction::triggered, this,
            &Impl::importPlaylist);
    connect(restorePreviousSessionAction_, &QAction::triggered, this,
            &Impl::restorePreviousSession);
    connect(settingsAction_, &QAction::triggered, this,
            [this] { (void)operations_->executeFrom("Menu", ControllerOperations::Action::OpenSettings); });
    connect(exitAction_, &QAction::triggered, this, [this] {
      (void)operations_->executeFrom("Menu", ControllerOperations::Action::Exit);
    });
    createEditMenu(menuBar);
    menuBar->addMenu(playlistPanel_->bulkOperationsMenu());
    createOnAirMenu(menuBar);
    viewMenuController_ = new vtrpon::ViewMenuController(
        menuBar, videoWindow_, controlWindow_, this);
    viewMenuController_->bindOperations(operations_);
    viewMenuController_->operatorMonitorStatusProvider = [this] {
      return programPanel_->operatorMonitorStatus();
    };
    connect(viewMenuController_, &vtrpon::ViewMenuController::operatorTimerChanged,
            programPanel_, &ProgramPanel::setOperatorTimerPresentation);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::operatorTimerStartRequested,
                              ControllerOperations::Action::OperatorTimerStart);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::operatorTimerPauseRequested,
                              ControllerOperations::Action::OperatorTimerPause);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::operatorTimerResetRequested,
                              ControllerOperations::Action::OperatorTimerReset);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::operatorTimerSettingsRequested,
                              ControllerOperations::Action::OperatorTimerSettings);
    programPanel_->setOperatorTimerPresentation(viewMenuController_->operatorTimerPresentation());
    connect(viewMenuController_, &vtrpon::ViewMenuController::operatorMonitorUnavailable,
            this, [this] {
      userInfoCenter_->showWarning(QStringLiteral("RETURN MONITOR"),
          vtrpon::LocalizedText{"operator_monitor.unavailable"});
    });
    retranslateUi();
    toolsMenu_ = menuBar->addMenu(QString());
    toolsMenu_->setObjectName(QStringLiteral("toolsMenu"));
    auto *toolsMenu = toolsMenu_;
    initializeMacros();
    operatorTimerSettingsAction_ = toolsMenu->addAction(vtrpon::uiText("operator_timer.settings"));
    operatorTimerSettingsAction_->setObjectName(QStringLiteral("operatorTimerSettingsAction"));
    auto *operatorTimerSeparator = toolsMenu->addSeparator();
    const auto updateTimerSettingsVisibility = [this, operatorTimerSeparator](const QJsonObject &state) {
      const bool visible = state.value(QStringLiteral("enabled")).toBool();
      operatorTimerSettingsAction_->setVisible(visible);
      operatorTimerSeparator->setVisible(visible);
    };
    connect(viewMenuController_, &vtrpon::ViewMenuController::operatorTimerChanged,
            this, updateTimerSettingsVisibility);
    updateTimerSettingsVisibility(viewMenuController_->operatorTimerPresentation());
    connect(operatorTimerSettingsAction_, &QAction::triggered,
            this, [this] { (void)operations_->executeFrom("Menu", ControllerOperations::Action::OperatorTimerSettings); });
    testPatternMenu_ = toolsMenu->addMenu(vtrpon::uiText("menu.tools.test_pattern"));
    testPatternMenu_->setObjectName(QStringLiteral("testPatternMenu"));
    rebuildTestPatternMenu();
    toolsMenu->addSeparator();
    clockSyncAction_ = toolsMenu->addAction(QString());
    clockSyncAction_->setObjectName(QStringLiteral("clockSyncAction"));
    auto *networkClock = playlistPanel_->networkTimeClock();
    connect(clockSyncAction_, &QAction::triggered, this, [this, networkClock] {
      if (networkClock->isSynchronizationInProgress()) {
        return;
      }
      manualClockSyncPending_ = true;
      networkClock->synchronize();
    });
    connect(networkClock, &vtrpon::NetworkTimeClock::synchronizationStarted,
            this, &Impl::updateClockSyncAction);
    connect(networkClock, &vtrpon::NetworkTimeClock::synchronizationFinished,
            this, [this, networkClock](bool succeeded) {
      updateClockSyncAction();
      if (!manualClockSyncPending_) {
        return;
      }
      manualClockSyncPending_ = false;
      if (succeeded) {
        userInfoCenter_->showSuccess(
            QStringLiteral("SYSTEM"),
            vtrpon::LocalizedText("clock.sync.succeeded"));
      } else {
        userInfoCenter_->showWarning(
            QStringLiteral("SYSTEM"),
            vtrpon::LocalizedText(networkClock->isNetworkSynchronized()
                                     ? "clock.sync.failed_keep_time"
                                     : "clock.sync.failed_local_time"));
      }
    });
    updateClockSyncAction();

    helpMenu_ = menuBar->addMenu(QString());
    helpMenu_->setObjectName(QStringLiteral("helpMenu"));
    vtrpon::installLogViewer(helpMenu_, controlWindow, languageManager_, fileIo_);
    checkUpdatesAction_ = helpMenu_->addAction(QString());
    checkUpdatesAction_->setObjectName(QStringLiteral("checkUpdatesAction"));
    connect(checkUpdatesAction_, &QAction::triggered, this, [this] {
      checkUpdatesAction_->setEnabled(false);
      checkUpdatesAction_->setText(vtrpon::uiText("menu.help.checking"));
      updateChecker_->check(true);
    });
    aboutAction_ = helpMenu_->addAction(QString());
    aboutAction_->setObjectName(QStringLiteral("aboutAction"));
    connect(aboutAction_, &QAction::triggered, this, [this] {
      if (!aboutWindow_) aboutWindow_ = new vtrpon::AboutWindow(controlWindow_);
      aboutWindow_->retranslate();
      aboutWindow_->show();
      aboutWindow_->raise();
      aboutWindow_->activateWindow();
    });
    updateChecker_ = new vtrpon::UpdateChecker(
        QCoreApplication::applicationVersion(),
        [this](bool manual, vtrpon::UpdateCheckStatus status,
               std::optional<vtrpon::UpdateRelease> release) {
          if (closing_) return;
          checkUpdatesAction_->setEnabled(true);
          checkUpdatesAction_->setText(vtrpon::uiText("menu.help.check_updates"));
          if (status == vtrpon::UpdateCheckStatus::Available || manual) {
            pendingUpdate_ = std::make_pair(status, std::move(release));
            pendingUpdateManual_ = manual;
            showPendingUpdate();
          }
        }, this);
    const auto retryPending = [this] {
      QTimer::singleShot(0, this, [this] { showPendingUpdate(); });
    };
    connect(playlistModel_, &PlaylistModel::onAirStateChanged, this, retryPending);
    connect(playlistModel_, &PlaylistModel::dskStateChanged, this, retryPending);
    connect(playlistModel_, &PlaylistModel::bgmStateChanged, this, retryPending);
    QTimer::singleShot(0, controlWindow_, [this] {
      if (!closing_) updateChecker_->check(false);
    });
    retranslateUi();

    operations_->setLogContextProvider([this] {
      return QStringLiteral(
                 "playlist=%1 selected=%2 onAir=%3 edit=%4 dsk=%5 bgm=%6")
          .arg(playlistModel_->activePlaylistNumber())
          .arg(playlistPanel_ ? playlistPanel_->selectedItemId() : 0)
          .arg(playlistModel_->onAirItemId())
          .arg(playlistModel_->editItemId())
          .arg(playlistModel_->dskItemId())
          .arg(playlistModel_->bgmItemId());
    });
    const auto logItem = [this](quint64 id) {
      const auto item = playlistModel_->itemState(id);
      if (!item)
        return;
      const auto &v = item->settings;
      qInfo() << "ITEM" << id << item->filePath << item->displayName
              << "playlist=" << item->playlistNumber << "inMs=" << v.inPointMs
              << "outMs=" << v.outPointMs.value_or(-1)
              << "startMode=" << static_cast<int>(v.startMode)
              << "endMode=" << static_cast<int>(v.endMode)
              << "repeat=" << v.repeatCount
              << "repeatEnd=" << static_cast<int>(v.repeatEndMode)
              << "goto=" << v.gotoTargetItemId
              << "volumeTenthsDb=" << v.nominalVolumeTenthsDb
              << "fadeIn=" << v.fadeInEnabled << v.fadeInDurationMs
              << "fadeOut=" << v.fadeOutEnabled << v.fadeOutDurationMs
              << "crossfade=" << v.endCrossfadeEnabled
              << v.endCrossfadeDurationMs;
    };
    connect(playlistModel_, &PlaylistModel::itemAdded, this, logItem);
    connect(playlistModel_, &PlaylistModel::itemReplaced, this, logItem);
    connect(playlistModel_, &PlaylistModel::itemSettingsChanged, this,
            [this, logItem](quint64 id, const PlaylistItemSettings &) {
              // Collapse slider updates and preserve the final settings for
              // each item.
              auto *&timer = itemLogTimers_[id];
              if (!timer) {
                timer = new QTimer(this);
                timer->setSingleShot(true);
                connect(timer, &QTimer::timeout, this,
                        [logItem, id] { logItem(id); });
              }
              timer->start(250);
            });
    connect(playlistModel_, &PlaylistModel::itemRemoved, this,
            [this](quint64 id, int) { delete itemLogTimers_.take(id); });
    const auto logInventory = [this, logItem] {
      const auto snapshot = playlistModel_->snapshot();
      for (const auto &item : snapshot.items)
        logItem(item.id);
    };
    connect(playlistModel_, &PlaylistModel::modelReset, this, logInventory);
    connect(menuBar->findChild<QAction *>("debugModeAction"), &QAction::toggled,
            this, [this, logInventory](bool enabled) {
              if (!enabled)
                return;
              qInfo().noquote()
                  << "DEBUG snapshot"
                  << QStringLiteral("onAir=%1 cue=%2 edit=%3 dsk=%4 bgm=%5")
                         .arg(playlistModel_->onAirItemId())
                         .arg(playlistModel_->cueItemId())
                         .arg(playlistModel_->editItemId())
                         .arg(playlistModel_->dskItemId())
                         .arg(playlistModel_->bgmItemId());
              logInventory();
              qInfo() << "OUTPUT device="
                      << programController_->appliedProgramOutputDevice()
                      << "status="
                      << programController_->programOutputDeviceStatusKey();
            });
    const auto logState = [this](const char *name, quint64 previous,
                                 quint64 current) {
      qInfo() << "STATE" << name << "previous=" << previous
              << "current=" << current
              << "cueState=" << static_cast<int>(playlistModel_->cueState());
    };
    connect(playlistModel_, &PlaylistModel::onAirStateChanged, this,
            [logState](quint64 before, quint64 after) {
              logState("ON_AIR", before, after);
            });
    connect(playlistModel_, &PlaylistModel::cueStateChanged, this,
            [logState](quint64 before, quint64 after) {
              logState("CUE", before, after);
            });
    connect(playlistModel_, &PlaylistModel::editStateChanged, this,
            [logState](quint64 before, quint64 after) {
              logState("EDIT", before, after);
            });
    connect(playlistModel_, &PlaylistModel::dskStateChanged, this,
            [logState](quint64 before, quint64 after) {
              logState("DSK", before, after);
            });
    connect(playlistModel_, &PlaylistModel::bgmStateChanged, this,
            [logState](quint64 before, quint64 after) {
              logState("BGM", before, after);
            });
    connect(playlistModel_, &PlaylistModel::itemAdded, this,
            [this](quint64 id) {
              const auto item = playlistModel_->itemState(id);
              if (item)
                qInfo() << "ITEM added" << id << item->filePath
                        << "playlist=" << item->playlistNumber;
            });
    connect(playlistModel_, &PlaylistModel::itemRemoved, this,
            [](quint64 id, int row) {
              qInfo() << "ITEM removed" << id << "row=" << row;
            });
    connect(playlistModel_, &PlaylistModel::itemReplaced, this,
            [](quint64 id) { qInfo() << "ITEM replaced" << id; });
    for (auto *action : menuBar->findChildren<QAction *>()) {
      connect(action, &QAction::triggered, this, [action](bool checked) {
        qInfo() << "MENU" << action->objectName() << action->text()
                << "checked=" << checked;
      });
    }

    auto *settingsLogTimer = new QTimer(this);
    settingsLogTimer->setInterval(500);
    const auto logSettings = [last = QJsonObject{}]() mutable {
      QSettings settings = vtrpon::makeControllerSettings();
      const auto current = vtrpon::controllerSettingsSnapshot(settings);
      if (current != last) {
        qInfo().noquote() << "SETTINGS"
                          << QJsonDocument(current).toJson(
                                 QJsonDocument::Compact);
        last = current;
      }
    };
    connect(settingsLogTimer, &QTimer::timeout, this, logSettings);
    connect(qApp, &QCoreApplication::aboutToQuit, this, logSettings);
    connect(settingsLogTimer, &QTimer::timeout, this,
            [this, lastError = QString()]() mutable {
              const auto error = vtrpon::applicationLogError();
              if (!error.isEmpty() && error != lastError) {
                lastError = error;
                userInfoCenter_->showWarning(
                    QStringLiteral("SYSTEM"),
                    vtrpon::uiText("logs.failed").arg(error));
              }
            });
    settingsLogTimer->start();
    windowLayout->setMenuBar(menuBar);

    auto *layout = new QHBoxLayout;
    layout->setAlignment(Qt::AlignHCenter);
    layout->setContentsMargins(vtrpon::scalePx(12), vtrpon::scalePx(12),
                               vtrpon::scalePx(12), vtrpon::scalePx(10));
    layout->setSpacing(vtrpon::scalePx(12));
    auto *editColumn = new QVBoxLayout;
    editColumn->setContentsMargins(0, 0, 0, 0);
    editColumn->setSpacing(vtrpon::scalePx(8));
    editColumn->addWidget(editPanel_);
    editColumn->addWidget(inputControlPanel_);
    layout->addLayout(editColumn);
    layout->addWidget(playlistPanel_);
    layout->addWidget(programPanel_);
    windowLayout->addLayout(layout);
    createMacroStatus(windowLayout);

    auto *woodTrim = new QWidget(controlWindow_);
    woodTrim->setObjectName(QStringLiteral("controllerWoodTrim"));
    woodTrim->setAttribute(Qt::WA_StyledBackground, true);
    woodTrim->setFixedHeight(vtrpon::scalePx(24));
    woodTrim->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *footerLayout = new QHBoxLayout(woodTrim);
    footerLayout->setContentsMargins(0, 0, vtrpon::scalePx(12), 0);
    footerLayout->setSpacing(0);
    footerLayout->addStretch();
    auto *systemLoad = new SystemLoadWidget(woodTrim);
    footerLayout->addWidget(systemLoad);
    auto *fullscreenStatsTimer = new QTimer(systemLoad);
    fullscreenStatsTimer->setInterval(1000);
    const auto refreshFullscreenStats =
        [controller = QPointer(programController_), systemLoad] {
      const auto stats = controller ? controller->fullscreenOutputStats()
                                    : DualDeckPlayer::FullscreenOutputStats{};
      systemLoad->setFullscreenOutput(stats.presentedFrames, stats.resolution);
    };
    connect(fullscreenStatsTimer, &QTimer::timeout, systemLoad, refreshFullscreenStats);
    refreshFullscreenStats();
    fullscreenStatsTimer->start();
    auto *settingsButton = new SettingsButton(woodTrim);
    const auto updateSettingsLabel = [this, settingsButton] {
      settingsButton->setToolTip(settingsAction_->text());
      settingsButton->setAccessibleName(settingsAction_->text());
      settingsButton->setEnabled(settingsAction_->isEnabled());
    };
    connect(settingsAction_, &QAction::changed, settingsButton, updateSettingsLabel);
    updateSettingsLabel();
    connect(settingsButton, &QPushButton::clicked, settingsAction_, &QAction::trigger);
    footerLayout->addWidget(settingsButton);
    windowLayout->addWidget(woodTrim);

    applyPlaylistSettings(
        ControllerSettingsDialog::savedPlaylistEditActivationMode(),
        ControllerSettingsDialog::savedAutoSelectNextItemAfterOnAir(),
        ControllerSettingsDialog::savedPreferAudioAlbumArt());
    applyOnAirSettings(
        ControllerSettingsDialog::savedAllowSameItemOnAirRetake(),
        ControllerSettingsDialog::savedShowPlaybackRateControls(),
        ControllerSettingsDialog::savedDisableFtbButton(),
        ControllerSettingsDialog::savedOnAirToggleMode());
    applyAudioSettings(
        ControllerSettingsDialog::savedShowBgmPlayerControls());
    applyPlayerBackgroundSettings();
    connect(programPanel_, &ProgramPanel::playerBackgroundRequested, this,
        [this](const QColor &color) {
      QSettings settings = vtrpon::makeControllerSettings();
      auto values = vtrpon::controllerSettingsSnapshot(settings);
      values.insert(QStringLiteral("player/backgroundColor"), color.name());
      if (!vtrpon::replaceControllerSettings(settings, values)) {
        settingsResult(false, "settings.manage.failed");
        return;
      }
      applyPlayerBackgroundSettings();
      if (activeSettingsDialog_) activeSettingsDialog_->reloadSavedSettings();
    });
    connectPlaylistControls();
    connectUvcControls();
    connectEditControls();
    connectProgramControls();
    shortcuts_ = std::make_unique<ControllerShortcuts>(
        controlWindow_, playlistPanel_, programPanel_, operations_,
        addFileAction_, exitAction_, viewMenuController_);
    switcherController_->setOperations(operations_);
    connect(controlWindow_, &QObject::destroyed, this,
            [this] { shutdownPlayers(); });
    connect(videoWindow_, &QObject::destroyed, this,
            [this] { shutdownPlayers(); });

    editController_->refreshWindowHandle();
    programController_->refreshWindowHandles();
    if (!programController_->isAvailable()) {
      userInfoCenter_->showError(
          QStringLiteral("SYSTEM"),
          vtrpon::LocalizedText("startup.playbin_creation_failed"));
    }
    connect(userInfoCenter_, &UserInfoCenter::audioSettingsRequested,
            this, [this] { showSettings(true); });
    audioOutputs_ = new AudioOutputCoordinator(
        playlistModel_, editController_, programController_, userInfoCenter_, this);
    connect(audioOutputs_, &AudioOutputCoordinator::savedAudioOutputsChanged,
            this, [this] {
      if (activeSettingsDialog_) activeSettingsDialog_->refreshSavedAudioOutputs();
    });
    connect(&vtrpon::VstService::instance(), &vtrpon::VstService::failure,
        this, [this](const QString &channel, const QString &reason) {
          userInfoCenter_->showWarning(QStringLiteral("VST3"),
              vtrpon::LocalizedText::fromRaw(vtrpon::uiText("vst.failure_notice").arg(channel) + QStringLiteral("\n") + reason));
        });
    playlistPanel_->focusAddFilesButton();
  }

  ~Impl() override {
    vtrpon::armApplicationShutdownDeadline();
    if (macroRunner_) macroRunner_->stop();
    delete macroEditor_.data();
    mediaImports_->cancelAll();
    documentImports_->requestShutdown();
    savePreviousSession();
    documentImports_->waitForActiveImport();
    // The queue's completion handlers use the model, I/O queue and players.
    // Destroy it before any of those dependencies are torn down.
    delete documentImports_;
    documentImports_ = nullptr;
    shortcuts_.reset();
    shutdownPlayers();
    delete editPanel_.data();
    delete playlistPanel_.data();
    delete programPanel_.data();
    delete inputControlPanel_.data();
  }

  ControllerOperations *operations() const { return operations_; }

  bool isAvailable() const {
    return programController_ && programController_->isAvailable();
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::LanguageChange && watched == controlWindow_) {
      retranslateUi();
      if (userInfoCenter_) {
        userInfoCenter_->retranslate();
      }
    }
    if (event->type() == QEvent::Close && (watched == videoWindow_ || watched == controlWindow_)) {
      if (!closeReady_) {
        event->ignore();
        beginClose();
        return true;
      }
    }
    return QObject::eventFilter(watched, event);
  }

private:
  void captureStill(bool program, qint64 durationMs) {
    if (closing_ || stillCapturePending_) return;
    auto frame = program ? programController_->captureStillFrame()
                         : editController_->captureStillFrame();
    if (!frame) {
      userInfoCenter_->showWarning(QStringLiteral("CAPTURE"),
                                    vtrpon::uiText("input.still.no_frame"));
      return;
    }
    const int playlistNumber = playlistModel_->activePlaylistNumber();
    stillCapturePlaylist_ = playlistNumber;
    const auto generation = restoreGeneration_;
    const QString directory = vtrpon::stillCaptureDirectory();
    auto result = std::make_shared<StillCaptureResult>();
    stillCapturePending_ = true;
    inputControlPanel_->setStillCaptureBusy(true);
    const bool queued = fileIo_->enqueue(
        [frame = std::move(frame), directory, program, durationMs, playlistNumber, result] {
          *result = saveStillCapture(frame, directory, program, durationMs, playlistNumber);
        },
        [this, result, generation, playlistNumber](const QString &error) {
          stillCapturePending_ = false;
          inputControlPanel_->setStillCaptureBusy(false);
          const auto playlists = playlistModel_->playlists();
          const bool destinationExists = stillCapturePlaylist_ == playlistNumber &&
              generation == restoreGeneration_ &&
              std::any_of(playlists.cbegin(), playlists.cend(), [playlistNumber](const auto &playlist) {
                return playlist.number == playlistNumber;
              });
          if (!error.isEmpty() || !result->errorKey.isEmpty()) {
            userInfoCenter_->showError(QStringLiteral("CAPTURE"),
                vtrpon::uiText(result->errorKey.isEmpty() ? "input.still.save_failed"
                                                         : result->errorKey.toUtf8().constData()),
                error.isEmpty() ? result->detail : error);
          } else if (!destinationExists || !result->item ||
                     playlistModel_->addItemsAtomically({*result->item}).isEmpty()) {
            userInfoCenter_->showWarning(QStringLiteral("CAPTURE"),
                vtrpon::uiText("input.still.add_failed"), result->path);
          } else {
            userInfoCenter_->showSuccess(QStringLiteral("CAPTURE"),
                vtrpon::uiText("input.still.saved"), result->path);
          }
          if (closeAfterStillCapture_) beginClose();
        });
    if (!queued) {
      stillCapturePending_ = false;
      inputControlPanel_->setStillCaptureBusy(false);
      userInfoCenter_->showWarning(QStringLiteral("CAPTURE"),
                                  vtrpon::uiText("input.still.save_failed"));
    }
  }

  void beginClose() {
    if (closing_) return;
    if (stillCapturePending_) {
      closeAfterStillCapture_ = true;
      if (controlWindow_) controlWindow_->setEnabled(false);
      return;
    }
    // Arm before any cancellation/save/teardown can enter an external call.
    vtrpon::armApplicationShutdownDeadline();
    closing_ = true;
    if (macroRunner_) macroRunner_->stop();
    if (viewMenuController_) viewMenuController_->requestShutdown();
    audioOutputs_->requestShutdown();
    ++restoreGeneration_;
    if (restoreCancellation_) restoreCancellation_->store(true);
    operations_->disable();
    if (controlWindow_) controlWindow_->setEnabled(false);
    mediaImports_->requestShutdown();
    documentImports_->requestShutdown();
    playlistModel_->requestShutdown();
    if (controlWindow_) {
      for (auto *thumbnailer : controlWindow_->findChildren<MediaThumbnailer *>()) {
        thumbnailersClosing_.append(thumbnailer);
        thumbnailer->requestShutdown();
      }
    }
    uvcDeviceMonitor_->requestShutdown();
    networkVideoRuntime_.requestShutdown();
    deckLinkRuntime_.requestShutdown();
    audioInputDeviceMonitor_->requestShutdown();
    switcherController_->requestShutdown();
    savePreviousSession();
    closePoll_.setInterval(25);
    connect(&closePoll_, &QTimer::timeout, this, [this] { advanceClose(); });
    closePoll_.start();
  }

  void advanceClose() {
    // Conversion completion may still call finishPowerPointConversion on the
    // GUI thread. Drain that completion before touching native player ownership.
    if (!documentImports_->shutdownComplete()) return;
    fileIo_->requestShutdown();
    if (!playersClosing_) {
      playersClosing_ = true;
      programController_->requestShutdown();
      editController_->requestShutdown();
    }
    if (!programController_->shutdownComplete() || !editController_->shutdownComplete()) return;
    uvcSourceSessions_->requestShutdown();
    if (!uvcSourceSessions_->shutdownComplete() || !uvcDeviceMonitor_->shutdownComplete()
        || !networkVideoRuntime_.shutdownComplete()
        || !deckLinkRuntime_.shutdownComplete()
        || !audioInputDeviceMonitor_->shutdownComplete() || !audioOutputs_->shutdownComplete()
        || !switcherController_->isShutdownComplete() || !mediaImports_->shutdownComplete()
        || !playlistModel_->shutdownComplete() || !fileIo_->shutdownComplete()
        || (viewMenuController_ && !viewMenuController_->shutdownComplete())) return;
    for (const auto &thumbnailer : std::as_const(thumbnailersClosing_))
      if (thumbnailer && !thumbnailer->shutdownComplete()) return;
    vtrpon::requestApplicationLogShutdown();
    if (!vtrpon::applicationLogShutdownComplete()) return;
    closePoll_.stop();
    closeReady_ = true;
    if (videoWindow_) videoWindow_->hide();
    if (controlWindow_) controlWindow_->close();
  }

  void updateClockSyncAction() {
    if (!clockSyncAction_) {
      return;
    }
    const bool inProgress =
        playlistPanel_->networkTimeClock()->isSynchronizationInProgress();
    clockSyncAction_->setEnabled(!inProgress);
    clockSyncAction_->setText(vtrpon::uiText(
        inProgress ? "clock.sync.in_progress" : "clock.sync.action"));
  }

  void createOnAirMenu(QMenuBar *menuBar) {
    using A = ControllerOperations::Action;
    onAirMenu_ = menuBar->addMenu(vtrpon::uiText("menu.on_air"));
    onAirMenu_->setObjectName(QStringLiteral("onAirMenu"));
    const auto add = [this](const char *id, A operation) {
      auto *action = onAirMenu_->addAction(vtrpon::uiText(
          (QByteArray("menu.on_air.") + id).constData()));
      action->setObjectName(QStringLiteral("onAirMenu") + QString::fromLatin1(id));
      action->setProperty("onAirTextId", QByteArray("menu.on_air.") + id);
      connect(action, &QAction::triggered, this, [this, operation] {
        (void)operations_->executeFrom("Menu", operation);
      });
    };
    add("TakeOnAir", A::TakeSelectedOnAir);
    add("OffAir", A::OffAir);
    add("ToggleFtb", A::ToggleFtb);
    add("ToggleOnAirPlayback", A::ProgramTogglePlayback);
    onAirMenu_->addSeparator();
    add("MasterFadeIn", A::MasterFadeIn);
    add("MasterFadeOut", A::MasterFadeOut);
    add("ItemFadeIn", A::ItemFadeIn);
    add("ItemFadeOut", A::ItemFadeOut);
    add("BgmFadeIn", A::BgmFadeInConfigured);
    add("BgmFadeOut", A::BgmFadeOutConfigured);
    onAirMenu_->addSeparator();
    add("ToggleDsk", A::ToggleDsk);
  }

  void createEditMenu(QMenuBar *menuBar) {
    using A = ControllerOperations::Action;
    editMenu_ = menuBar->addMenu(vtrpon::uiText("menu.edit"));
    editMenu_->setObjectName(QStringLiteral("editMenu"));
    const auto add = [this](const char *id, A operation, const QVariantList &arguments = {}) {
      auto *action = editMenu_->addAction(vtrpon::uiText(
          (QByteArray("settings.shortcuts.") + id).constData()));
      action->setObjectName(QStringLiteral("editMenu") + QString::fromLatin1(id));
      action->setProperty("editTextId", QByteArray("settings.shortcuts.") + id);
      connect(action, &QAction::triggered, this, [this, operation, arguments] {
        ControllerOperations::InputScope inputScope("Menu");
        (void)operations_->dispatch(operation, arguments);
      });
      return action;
    };
    add("EditToggleStartMode", A::EditToggleStartMode);
    editMenu_->addSeparator();
    const auto end = [&add](const char *id, PlaylistEndMode mode) {
      auto *action = add(id, A::EditEndMode, {QVariant::fromValue(mode)});
      action->setCheckable(true);
      action->setProperty("editEndMode", static_cast<int>(mode));
    };
    end("EditEndStop", PlaylistEndMode::Stop);
    end("EditEndPause", PlaylistEndMode::Pause);
    end("EditEndRepeat", PlaylistEndMode::Repeat);
    end("EditEndNext", PlaylistEndMode::Next);
    auto *fade = add("EditToggleFadeOut", A::EditToggleFadeOut);
    fade->setCheckable(true);
    editMenu_->addSeparator();
    add("EditReset", A::EditReset);
    editMenu_->addSeparator();
    add("EditSetIn", A::EditSetIn);
    add("EditSetOut", A::EditSetOut);
    connect(editMenu_, &QMenu::aboutToShow, this, [this, fade] {
      const auto state = playlistModel_->itemState(playlistModel_->editItemId());
      fade->setChecked(state && state->settings.fadeOutEnabled);
      for (auto *action : editMenu_->actions()) {
        const auto mode = action->property("editEndMode");
        if (mode.isValid()) action->setChecked(state &&
            static_cast<int>(state->settings.endMode) == mode.toInt());
      }
    });
  }

  QString macroText(const char *english, const char *japanese) const {
    return QString::fromUtf8(languageManager_ &&
        languageManager_->currentLanguage() == vtrpon::UiLanguage::Japanese ? japanese : english);
  }

  void initializeMacros() {
    auto *clock = playlistPanel_->networkTimeClock();
    macroRunner_ = new MacroRunner(playlistModel_, operations_,
        {[clock = QPointer<vtrpon::NetworkTimeClock>(clock)] {
          return clock ? clock->currentLocalDateTime() : QDateTime::currentDateTime();
        }, {}}, this);
    using A = ControllerOperations::Action;
    operations_->bind<quint64, quint64>(A::MacroCue, this, [this](quint64 request, quint64 id) {
      return programController_->requestMacroCue(request, id);
    });
    operations_->bind<quint64, quint64, bool, bool, qint64>(A::MacroTake, this,
        [this](quint64 request, quint64 id, bool ready, bool crossfade, qint64 duration) {
      return programController_->requestMacroTake(request, id, ready, crossfade, duration);
    });
    operations_->bind<>(A::CancelMacroRequests, this,
        [this] { programController_->cancelMacroRequests(); });
    operations_->bind<quint64, QJsonObject>(A::MacroItemSettings, this,
        [this](quint64 id, const QJsonObject &patch) {
      const auto item = playlistModel_->itemState(id);
      PlaylistItemSettings settings;
      if (!item || !MacroDocumentIo::applySettingsPatch(item->settings, patch, &settings)) return false;
      return playlistModel_->updateItemSettings(id, settings);
    });
    operations_->bind<QString, double>(A::MacroRuntimeSetting, this,
        [this](const QString &operation, double value) {
      if (!std::isfinite(value)) return false;
      using A = ControllerOperations::Action;
      if (operation == QLatin1String("playbackRate"))
        return programController_->setMacroPlaybackRate(value);
      if (operation == QLatin1String("mute"))
        return (value == 0.0 || value == 1.0) &&
            operations_->executeFrom("Macro", A::MasterMute, value == 1.0).invoked();
      if (value != std::floor(value) || value < -960 || value > 60) return false;
      if (operation == QLatin1String("masterVolume"))
        return operations_->executeFrom("Macro", A::SetMasterVolume, int(value)).invoked();
      if (operation == QLatin1String("itemVolume"))
        return operations_->executeFrom("Macro", A::SetItemVolume, int(value)).invoked();
      return false;
    });
    operations_->bind<quint64, QString>(A::MacroTransport, this, [this](quint64 request, const QString &operation) {
      return programController_->requestMacroTransport(request, operation);
    });
    connect(programController_, &ProgramPlayoutController::macroRequestFinished,
            macroRunner_, &MacroRunner::finishRequest);
    connect(programController_, &ProgramPlayoutController::macroIntervention,
            macroRunner_, &MacroRunner::interrupt);
    connect(programController_, &ProgramPlayoutController::macroReservationFailed,
            macroRunner_, &MacroRunner::fail);
    connect(macroRunner_, &MacroRunner::stateChanged, this, [this] {
      updateMacroStatus();
      if (macroRunner_->state() == MacroRunner::State::Error)
        userInfoCenter_->showWarning(QStringLiteral("MACRO"),
            vtrpon::LocalizedText::fromRaw(macroRunner_->statusText()));
    });
    connect(macroRunner_, &MacroRunner::progressChanged, this, &Impl::updateMacroStatus);
  }

  void showMacroEditor() {
    if (closing_) return;
    if (!macroEditor_) {
      macroEditor_ = new MacroEditorWindow(playlistModel_, macroRunner_, controlWindow_);
      macroEditor_->setLanguageManager(languageManager_);
      macroEditor_->setClock(playlistPanel_->networkTimeClock());
      connect(macroEditor_, &MacroEditorWindow::saveRequested, this, &Impl::exportPlaylist);
      connect(macroEditor_, &MacroEditorWindow::addFilesRequested, this, [this](int playlist) {
        const auto playlists = playlistModel_->playlists();
        if (macroRunner_->isActive() || std::none_of(playlists.cbegin(), playlists.cend(),
            [playlist](const PlaylistState &state) { return state.number == playlist; })) return;
        const auto paths = QFileDialog::getOpenFileNames(macroEditor_,
            vtrpon::uiText("file_dialog.add_files.title"),
            vtrpon::makeControllerSettings().value("fileDialog/lastMediaDirectory").toString(),
            playlistMediaFileFilter());
        if (!paths.isEmpty() && !macroRunner_->isActive()) registerPlaylistPathsImpl(paths, true, playlist);
      });
    }
    macroEditor_->show();
    macroEditor_->raise();
    macroEditor_->activateWindow();
  }

  void createMacroStatus(QVBoxLayout *layout) {
    macroStatus_ = new QWidget(controlWindow_);
    macroStatus_->setObjectName(QStringLiteral("macroStatusBar"));
    auto *row = new QHBoxLayout(macroStatus_);
    row->setContentsMargins(vtrpon::scalePx(12), 2, vtrpon::scalePx(12), 2);
    macroStatusLabel_ = new QLabel(macroStatus_);
    macroStatusLabel_->setWordWrap(true);
    macroStopButton_ = new QPushButton(macroStatus_);
    macroStopButton_->setObjectName(QStringLiteral("macroStopButton"));
    row->addWidget(macroStatusLabel_, 1);
    row->addWidget(macroStopButton_);
    connect(macroStopButton_, &QPushButton::clicked, macroRunner_, &MacroRunner::stop);
    layout->addWidget(macroStatus_);
    updateMacroStatus();
  }

  void updateMacroStatus() {
    if (!macroStatus_) return;
    macroStopButton_->setText(macroText("Stop Macro", "マクロ停止"));
    macroStatusLabel_->setText(QStringLiteral("%1 — %2 (#%3)  %4 s")
        .arg(macroRunner_->activeMacroName(), macroRunner_->statusText())
        .arg(macroRunner_->currentStepIndex() + 1)
        .arg(macroRunner_->remainingMs() / 1000.0, 0, 'f', 1));
    macroStopButton_->setEnabled(macroRunner_->isActive());
    macroStatus_->setVisible(macroRunner_->isActive() || macroRunner_->state() == MacroRunner::State::Error);
  }

  bool macroBlocksRestore() {
    if (!macroRunner_ || !macroRunner_->isActive()) return false;
    userInfoCenter_->showWarning(QStringLiteral("MACRO"), vtrpon::LocalizedText::fromRaw(
        macroText("Stop the macro before importing or restoring a playlist.",
                  "プレイリストの読込・復元前にマクロを停止してください。")));
    return true;
  }

  void retranslateUi() {
    if (!fileMenu_) {
      return;
    }
    fileMenu_->setTitle(vtrpon::uiText("menu.file"));
    if (toolsMenu_) toolsMenu_->setTitle(vtrpon::uiText("menu.tools"));
    if (helpMenu_) helpMenu_->setTitle(vtrpon::uiText("menu.help"));
    if (aboutAction_) aboutAction_->setText(vtrpon::uiText("menu.help.about"));
    if (aboutWindow_) aboutWindow_->retranslate();
    if (checkUpdatesAction_)
      checkUpdatesAction_->setText(vtrpon::uiText(
          checkUpdatesAction_->isEnabled() ? "menu.help.check_updates"
                                           : "menu.help.checking"));
    if (updateWindow_) updateWindow_->retranslate();
    updateMacroStatus();
    if (operatorTimerSettingsAction_)
      operatorTimerSettingsAction_->setText(vtrpon::uiText("operator_timer.settings"));
    if (testPatternMenu_) {
      testPatternMenu_->setTitle(vtrpon::uiText("menu.tools.test_pattern"));
      rebuildTestPatternMenu();
    }
    if (editMenu_) {
      editMenu_->setTitle(vtrpon::uiText("menu.edit"));
      for (auto *action : editMenu_->actions()) {
        const auto id = action->property("editTextId").toByteArray();
        if (!id.isEmpty()) action->setText(vtrpon::uiText(id.constData()));
      }
    }
    if (onAirMenu_) {
      onAirMenu_->setTitle(vtrpon::uiText("menu.on_air"));
      for (auto *action : onAirMenu_->actions()) {
        const auto id = action->property("onAirTextId").toByteArray();
        if (!id.isEmpty()) action->setText(vtrpon::uiText(id.constData()));
      }
    }
    addFileAction_->setText(vtrpon::uiText("menu.file.add"));
    exportPlaylistAction_->setText(
        vtrpon::uiText("menu.file.export_playlist"));
    importPlaylistAction_->setText(
        vtrpon::uiText("menu.file.import_playlist"));
    restorePreviousSessionAction_->setText(
        vtrpon::uiText("menu.file.restore_previous_session"));
    settingsAction_->setText(vtrpon::uiText("menu.file.settings"));
    exportSettingsAction_->setText(vtrpon::uiText("menu.file.export_settings"));
    importSettingsAction_->setText(vtrpon::uiText("menu.file.import_settings"));
    exitAction_->setText(vtrpon::uiText("menu.file.exit"));
    viewMenuController_->retranslate();
    updateClockSyncAction();
  }

  void showPendingUpdate() {
    if (!pendingUpdate_ || closing_ || !controlWindow_) return;
    if (playlistModel_->onAirItemId() != 0 ||
        playlistModel_->dskItemId() != 0 || programController_->bgmIsActive())
      return;
    if (!updateWindow_)
      updateWindow_ = new vtrpon::UpdateWindow(
          QCoreApplication::applicationVersion(),
          [this] { return languageManager_ &&
                            languageManager_->currentLanguage() == vtrpon::UiLanguage::Japanese; },
          controlWindow_);
    updateWindow_->setResult(pendingUpdate_->first, pendingUpdate_->second);
    updateWindow_->setAttribute(Qt::WA_ShowWithoutActivating,
                                !pendingUpdateManual_);
    updateWindow_->show();
    if (pendingUpdateManual_) {
      updateWindow_->raise();
      updateWindow_->activateWindow();
    }
    pendingUpdate_.reset();
    pendingUpdateManual_ = false;
  }

  void settingsResult(bool success, const char *message) {
    if (activeSettingsDialog_)
      activeSettingsDialog_->setSettingsFileStatus(
          vtrpon::uiText(success ? message : "settings.manage.failed"));
    if (success) userInfoCenter_->showSuccess(QStringLiteral("SETTINGS"), vtrpon::LocalizedText(message));
    else userInfoCenter_->showError(QStringLiteral("SETTINGS"), vtrpon::LocalizedText("settings.manage.failed"));
  }

  void exportSettings() {
    QWidget *parent = activeSettingsDialog_ ? activeSettingsDialog_.data() : controlWindow_.data();
    QFileDialog dialog(parent, vtrpon::uiText("menu.file.export_settings"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(vtrpon::uiText("settings.manage.filter"));
    dialog.setDefaultSuffix(QStringLiteral("json"));
    dialog.selectFile(QStringLiteral("vtr-pon-settings-%1.json")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    const QString path = dialog.selectedFiles().constFirst();
    QSettings settings = vtrpon::makeControllerSettings();
    settings.sync();
    settingsResult(vtrpon::exportControllerSettings(settings, path), "settings.export.succeeded");
  }

  void importSettings() {
    QWidget *parent = activeSettingsDialog_ ? activeSettingsDialog_.data() : controlWindow_.data();
    const QString path = QFileDialog::getOpenFileName(parent,
        vtrpon::uiText("menu.file.import_settings"), QString(), vtrpon::uiText("settings.manage.filter"));
    if (path.isEmpty()) return;
    QSettings settings = vtrpon::makeControllerSettings();
    if (!vtrpon::importControllerSettings(settings, path)) {
      settingsResult(false, "settings.import.succeeded");
      return;
    }
    const bool applied = applySavedSettings();
    if (activeSettingsDialog_) activeSettingsDialog_->reloadSavedSettings();
    if (applied) settingsResult(true, "settings.import.succeeded");
  }

  void applyPlayerBackgroundSettings() {
    const QColor color = ControllerSettingsDialog::savedPlayerBackgroundColor();
    programController_->setBackgroundColor(color);
    programPanel_->setPlayerBackground(color);
  }

  bool applySavedSettings() {
    vtrpon::reloadVirtualCameraEnabled();
    rebuildTestPatternMenu();
    vtrpon::applyNetworkVideoSettings(vtrpon::savedNetworkVideoSettings());
    viewMenuController_->reloadOperatorMonitorSettings();
    playlistPanel_->applyTimeSettings();
    programPanel_->applyTimeSettings();
    if (shortcuts_) shortcuts_->reloadSavedSettings();
    applyPlayerBackgroundSettings();
    QString error;
    const bool skinApplied = vtrpon::applyControllerSkin(*qApp, vtrpon::configuredControllerSkinKey(), &error);
    const bool languageApplied = !languageManager_ || languageManager_->initialize(&error);
    applyPlaylistSettings(ControllerSettingsDialog::savedPlaylistEditActivationMode(),
                           ControllerSettingsDialog::savedAutoSelectNextItemAfterOnAir(),
                           ControllerSettingsDialog::savedPreferAudioAlbumArt());
    applyOnAirSettings(ControllerSettingsDialog::savedAllowSameItemOnAirRetake(),
                       ControllerSettingsDialog::savedShowPlaybackRateControls(),
                       ControllerSettingsDialog::savedDisableFtbButton(),
        ControllerSettingsDialog::savedOnAirToggleMode());
    applyAudioSettings(ControllerSettingsDialog::savedShowBgmPlayerControls());
    applySwitcherSettings(ControllerSettingsDialog::savedSwitcherControlSettings());
    audioOutputs_->applySavedSettings();
    for (const auto &device : uvcDeviceMonitor_->devices())
      emit uvcAudioMappingStore_->mappingChanged(device.devicePath,
          uvcAudioMappingStore_->audioDeviceIdFor(device.devicePath));
    if (!skinApplied || !languageApplied) settingsResult(false, "settings.import.succeeded");
    return skinApplied && languageApplied;
  }

  void showSettings(bool audioOutputPage = false) {
    if (!controlWindow_ || activeSettingsDialog_) {
      return;
    }
    ControllerSettingsDialog dialog(
        uvcDeviceMonitor_, audioInputDeviceMonitor_, uvcAudioMappingStore_,
        switcherController_, languageManager_, controlWindow_);
    connect(&dialog, &ControllerSettingsDialog::addonRestartRequested, &dialog,
        [this, &dialog] {
          if (closing_) return;
          QString error;
          if (!vtrpon::addons::requestRestart(QCoreApplication::applicationDirPath(),
                  QCoreApplication::arguments().mid(1), QDir::currentPath(), &error)) {
            QMessageBox message(QMessageBox::Warning,
                vtrpon::uiText("settings.addons.restart_title"),
                vtrpon::uiText("settings.addons.restart_failed").arg(error),
                QMessageBox::Ok, &dialog);
            message.setObjectName(QStringLiteral("addonRestartErrorDialog"));
            message.setPalette(vtrpon::controllerSkinPalette(
                *qApp, vtrpon::ControllerSkinPalette::VtrPonClasic));
            message.setStyleSheet(vtrpon::settingsStyleSheet(message.objectName()));
            message.exec();
            return;
          }
          dialog.accept();
          QTimer::singleShot(0, this, [this] {
            if (controlWindow_) controlWindow_->close();
          });
        });
    bool openTimerSettings = false;
    connect(&dialog, &ControllerSettingsDialog::operatorTimerSettingsRequested,
            &dialog, [&openTimerSettings] { openTimerSettings = true; });
    connect(&dialog, &ControllerSettingsDialog::operatorMonitorSettingsChanged,
            viewMenuController_, &vtrpon::ViewMenuController::reloadOperatorMonitorSettings);
    connect(&dialog, &ControllerSettingsDialog::timeSettingsChanged, this, [this](bool remainingChanged) {
      playlistPanel_->applyTimeSettings();
      if (remainingChanged) programPanel_->applyTimeSettings();
    });
    auto *clock = playlistPanel_->networkTimeClock();
    dialog.setTimeSynchronizationState(clock->isSynchronizationInProgress());
    connect(&dialog, &ControllerSettingsDialog::timeSynchronizationRequested, clock, &vtrpon::NetworkTimeClock::synchronize);
    connect(clock, &vtrpon::NetworkTimeClock::synchronizationStarted, &dialog, [&dialog] {
      dialog.setTimeSynchronizationState(true);
    });
    connect(clock, &vtrpon::NetworkTimeClock::synchronizationFinished, &dialog, [&dialog, clock](bool ok) {
      dialog.setTimeSynchronizationState(false, vtrpon::uiText(ok ? "clock.sync.succeeded" :
          clock->isNetworkSynchronized() ? "clock.sync.failed_keep_time" : "clock.sync.failed_local_time"));
    });
    connect(&dialog, &ControllerSettingsDialog::testPatternsChanged,
            this, [this] { rebuildTestPatternMenu(); });
    dialog.addVideoOutputControls(viewMenuController_->createSettingsWidget(dialog.videoOutputPage()));
    if (audioOutputPage) dialog.showAudioOutputPage();
    connect(&dialog, &ControllerSettingsDialog::shortcutsChanged,
            this, [this] { if (shortcuts_) shortcuts_->reloadSavedSettings(); });
    connect(&dialog, &ControllerSettingsDialog::exportSettingsRequested,
            this, [this] { exportSettings(); });
    connect(&dialog, &ControllerSettingsDialog::importSettingsRequested,
            this, [this] { importSettings(); });
    connect(&dialog, &ControllerSettingsDialog::settingsReplaced,
            this, [this] { applySavedSettings(); });
    connect(&dialog, &ControllerSettingsDialog::programOutputDeviceChanged,
            audioOutputs_, &AudioOutputCoordinator::programOutputSelectionChanged);
    connect(&dialog, &ControllerSettingsDialog::editPflOutputDeviceChanged,
            audioOutputs_, &AudioOutputCoordinator::pflOutputSelectionChanged);
    QTimer outputStateTimer;
    outputStateTimer.setInterval(100);
    const auto refreshOutputState = [this, &dialog] {
      dialog.setProgramOutputState(programController_->appliedProgramOutputDevice(),
                                   programController_->programOutputDeviceStatusKey());
    };
    connect(&outputStateTimer, &QTimer::timeout, &dialog, refreshOutputState);
    refreshOutputState();
    outputStateTimer.start();
    connect(&dialog,
            &ControllerSettingsDialog::showBgmPlayerControlsChanged,
            this, [this](bool visible) { applyAudioSettings(visible); });
    connect(&dialog,
            &ControllerSettingsDialog::audioThumbnailPreferenceChanged,
            playlistPanel_, &PlaylistPanel::setPreferAudioAlbumArt);
    connect(&dialog,
            &ControllerSettingsDialog::switcherControlSettingsChanged,
            this, [this](const vtrpon::SwitcherControlSettings &settings) {
              applySwitcherSettings(settings);
            });
    activeSettingsDialog_ = &dialog;
    (void)dialog.exec();
    activeSettingsDialog_.clear();
    applyPlaylistSettings(dialog.playlistEditActivationMode(),
                           dialog.autoSelectNextItemAfterOnAir(),
                           dialog.preferAudioAlbumArt());
    applyOnAirSettings(dialog.allowSameItemOnAirRetake(),
                       dialog.showPlaybackRateControls(),
                       dialog.disableFtbButton(), dialog.onAirToggleMode());
    applyAudioSettings(dialog.showBgmPlayerControls());
    applySwitcherSettings(dialog.switcherControlSettings());
    if (openTimerSettings) viewMenuController_->showOperatorTimerSettings();
  }

  void applyPlaylistSettings(PlaylistEditActivationMode editActivationMode,
                              bool autoSelectNextItemAfterOnAir,
                              bool preferAudioAlbumArt) {
    if (playlistPanel_) {
      playlistPanel_->setEditActivationMode(editActivationMode);
      playlistPanel_->setPreferAudioAlbumArt(preferAudioAlbumArt);
    }
    if (programController_) {
      programController_->setAutoSelectNextItemAfterOnAir(
          autoSelectNextItemAfterOnAir);
    }
  }

  void applyOnAirSettings(bool allowSameItemOnAirRetake,
                          bool showPlaybackRateControls,
                          bool disableFtbButton, bool toggleMode) {
    if (playlistPanel_) playlistPanel_->setOnAirToggleMode(toggleMode);
    if (programPanel_) programPanel_->setOffAirButtonVisible(!toggleMode);
    if (programController_) {
      programController_->setAllowSameItemOnAirRetake(
          allowSameItemOnAirRetake);
    }
    if (programPanel_) {
      programPanel_->setPlaybackRateControlsVisible(
          showPlaybackRateControls);
      programPanel_->setFtbControlsVisible(!disableFtbButton);
    }
  }

  void applyAudioSettings(bool showBgmPlayerControls) {
    if (programController_) {
      programController_->setBgmControlsVisible(showBgmPlayerControls);
    }
  }

  void applySwitcherSettings(
      const vtrpon::SwitcherControlSettings &settings) {
    if (switcherController_) {
      switcherController_->applySettings(settings);
    }
  }

  void connectPlaylistControls() {
    using A = ControllerOperations::Action;
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::addRequested, A::AddFilesDialog);
    operations_->bind<quint64, int>(A::MoveItem, this, [this](quint64 id, int offset) {
      return playlistModel_->moveItem(id, offset);
    });
    operations_->bind<quint64, int>(A::MoveItemToRow, this, [this](quint64 id, int row) {
      return playlistModel_->moveItemTo(id, row);
    });
    operations_->bind<quint64, int, int, bool>(A::TransferItem, this,
        [this](quint64 id, int playlist, int row, bool copy) {
      if (copy) {
        if (!playlistPanel_->allowItemAction(id, PlaylistItemAction::CopyItem)) return false;
        return playlistModel_->copyItemToPlaylist(id, playlist, row) != 0;
      }
      return playlistModel_->moveItemToPlaylist(id, playlist, row);
    });
    connect(playlistPanel_, &PlaylistPanel::moveUpRequested, this,
            [this](quint64 id) { (void)operations_->execute(A::MoveItem, id, -1); });
    connect(playlistPanel_, &PlaylistPanel::moveDownRequested, this,
            [this](quint64 id) { (void)operations_->execute(A::MoveItem, id, 1); });
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::moveRequested, A::MoveItemToRow);
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::itemDropRequested, A::TransferItem);
    operations_->bindMember(A::AddFiles, this, &Impl::registerPlaylistPaths);
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::filesDropped, A::AddFiles);
    operations_->bindMember(A::RemoveItem, this, &Impl::removePlaylistItem);
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::removeRequested, A::RemoveItem);
    operations_->bindMember(A::PlaylistBulk, this, &Impl::applyPlaylistBulkOperation);
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::bulkOperationRequested, A::PlaylistBulk);
    connect(playlistModel_, &PlaylistModel::bulkUndoAvailableChanged,
            playlistPanel_, &PlaylistPanel::setBulkUndoAvailable);
  }

  bool clearPlaylist(int playlistNumber) {
    QList<quint64> itemIds;
    itemIds.reserve(playlistModel_->itemCount(playlistNumber));
    for (int row = 0; row < playlistModel_->itemCount(playlistNumber); ++row) {
      itemIds.append(playlistModel_->itemIdAt(playlistNumber, row));
    }

    for (const quint64 itemId : itemIds) {
      if (!programController_->canRemoveItem(itemId)) {
        return false;
      }
    }
    for (const quint64 itemId : itemIds) {
      if (!editController_->prepareForItemRemoval(itemId)) {
        return false;
      }
    }
    for (const quint64 itemId : itemIds) {
      if (!programController_->prepareForItemRemoval(itemId)) {
        return false;
      }
    }
    for (const quint64 itemId : itemIds) {
      if (!playlistModel_->removeItem(itemId)) {
        return false;
      }
    }
    return true;
  }

  void undoPlaylistBulkOperation() {
    if (!playlistModel_->hasBulkUndo()) {
      playlistPanel_->setBulkUndoAvailable(false);
      return;
    }
    if (hasPendingImports()) {
      userInfoCenter_->showWarning(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.bulk.pending_imports"));
      return;
    }

    if (playlistModel_->onAirItemId() != 0 ||
        playlistModel_->dskItemId() != 0 ||
        programController_->bgmIsActive()) {
      userInfoCenter_->showWarning(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.bulk.undo.playout_active"));
      return;
    }
    QString errorDetail;
    bool keptLaterChanges = false;
    if (!playlistModel_->undoBulkOperation(&errorDetail, &keptLaterChanges)) {
      userInfoCenter_->showError(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.bulk.undo.failed"), errorDetail);
      return;
    }
    if (keptLaterChanges) {
      userInfoCenter_->showSuccess(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.bulk.undo.kept_later_changes"));
    }
  }

  void applyPlaylistBulkOperation(PlaylistBulkOperation operation) {
    if (!playlistModel_ || !playlistPanel_ || !editController_ ||
        !programController_) {
      return;
    }
    if (operation == PlaylistBulkOperation::Undo) {
      undoPlaylistBulkOperation();
      return;
    }
    if (hasPendingImports()) {
      userInfoCenter_->showWarning(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.bulk.pending_imports"));
      return;
    }

    const int playlistNumber = playlistModel_->activePlaylistNumber();
    const PlaylistModelSnapshot before = playlistModel_->snapshot();
    const bool deletePlaylist = operation == PlaylistBulkOperation::DeletePlaylist;
    if (deletePlaylist && playlistModel_->playlists().size() <= 1) return;
    if (programController_->bgmIsActive() &&
        programController_->bgmPlaylistNumber() == playlistNumber &&
        (operation == PlaylistBulkOperation::BgmMode ||
         operation == PlaylistBulkOperation::StandardMode ||
         operation == PlaylistBulkOperation::Clear || deletePlaylist)) {
      userInfoCenter_->showWarning(
          QStringLiteral("BGM"),
          vtrpon::LocalizedText("playlist.bulk.bgm_active"));
      return;
    }
    if (operation == PlaylistBulkOperation::Clear || deletePlaylist) {
      for (int row = 0; row < playlistModel_->itemCount(playlistNumber); ++row) {
        const auto id = playlistModel_->itemIdAt(playlistNumber, row);
        if (!programController_->canRemoveItem(id)) return;
      }
      for (int row = 0; row < playlistModel_->itemCount(playlistNumber); ++row) {
        const auto id = playlistModel_->itemIdAt(playlistNumber, row);
        if (editController_->deferItemRemoval(id, [this, playlistNumber, deletePlaylist] {
              if (hasPendingImports() || (programController_->bgmIsActive()
                  && programController_->bgmPlaylistNumber() == playlistNumber)) return;
              if (deletePlaylist && playlistModel_->playlists().size() <= 1) return;
              const auto before = playlistModel_->snapshot();
              if (clearPlaylist(playlistNumber) &&
                  (!deletePlaylist || playlistModel_->removeEmptyPlaylist(playlistNumber)))
                playlistModel_->rememberBulkOperation(before, playlistNumber);
            })) return;
      }
    }
    bool succeeded = false;
    switch (operation) {
    case PlaylistBulkOperation::PlayOnce:
      succeeded = playlistModel_->applyBulkPlaybackOperation(
          playlistNumber, PlaylistBulkPlaybackOperation::PlayOnce);
      break;
    case PlaylistBulkOperation::RepeatAll:
      succeeded = playlistModel_->applyBulkPlaybackOperation(
          playlistNumber, PlaylistBulkPlaybackOperation::RepeatAll);
      break;
    case PlaylistBulkOperation::Shuffle:
      succeeded = playlistModel_->applyBulkPlaybackOperation(
          playlistNumber, PlaylistBulkPlaybackOperation::Shuffle);
      break;
    case PlaylistBulkOperation::BgmMode:
      succeeded = playlistModel_->applyBulkPlaybackOperation(
          playlistNumber, PlaylistBulkPlaybackOperation::BgmMode);
      break;
    case PlaylistBulkOperation::StandardMode:
      succeeded = playlistModel_->applyBulkPlaybackOperation(
          playlistNumber, PlaylistBulkPlaybackOperation::StandardMode);
      break;
    case PlaylistBulkOperation::Clear:
      succeeded = clearPlaylist(playlistNumber);
      break;
    case PlaylistBulkOperation::DeletePlaylist:
      succeeded = clearPlaylist(playlistNumber) &&
                  playlistModel_->removeEmptyPlaylist(playlistNumber);
      break;
    case PlaylistBulkOperation::Undo:
      break;
    }
    if (!succeeded) {
      if (operation != PlaylistBulkOperation::Clear && !deletePlaylist) {
        userInfoCenter_->showWarning(
            QStringLiteral("PLAYLIST"),
            vtrpon::LocalizedText("playlist.bulk.failed"));
      }
      return;
    }
    playlistModel_->rememberBulkOperation(before, playlistNumber);
  }

  void connectEditControls() {
    using A = ControllerOperations::Action;
    // EDIT transport owns its separate player. Only explicit item-setting
    // operations write to the shared playlist model; no EDIT action calls the
    // Program controller.
    operations_->bindMember(A::EditLoadSelected, editController_, &EditSessionController::loadSelectedItem);
    operations_->bindMember(A::EditToggleStartMode, editController_, &EditSessionController::toggleStartMode);
    operations_->bindMember(A::EditToggleFadeOut, editController_, &EditSessionController::toggleFadeOut);
    operations_->bindMember(A::EditReset, editController_, &EditSessionController::resetEditor);
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::editRequested, A::EditLoadSelected);
    operations_->bindMember(A::EditPlay, editController_, &EditSessionController::play);
    operations_->connectInput(editPanel_.data(), &EditPanel::playRequested, A::EditPlay);
    operations_->bindMember(A::EditPause, editController_, &EditSessionController::pause);
    operations_->connectInput(editPanel_.data(), &EditPanel::pauseRequested, A::EditPause);
    operations_->bindMember(A::EditStop, editController_, &EditSessionController::stopPlayback);
    operations_->connectInput(editPanel_.data(), &EditPanel::stopRequested, A::EditStop);
    operations_->bindMember(A::EditJumpStart, editController_, &EditSessionController::jumpToStart);
    operations_->connectInput(editPanel_.data(), &EditPanel::jumpToStartRequested, A::EditJumpStart);
    operations_->bindMember(A::EditJumpIn, editController_, &EditSessionController::jumpToIn);
    operations_->connectInput(editPanel_.data(), &EditPanel::jumpToInRequested, A::EditJumpIn);
    operations_->bindMember(A::EditScanBackward, editController_, &EditSessionController::startScanBackward);
    operations_->connectInput(editPanel_.data(), &EditPanel::scanBackwardRequested, A::EditScanBackward);
    operations_->bindMember(A::EditScanForward, editController_, &EditSessionController::startScanForward);
    operations_->connectInput(editPanel_.data(), &EditPanel::scanForwardRequested, A::EditScanForward);
    operations_->bindMember(A::EditScanStop, editController_, &EditSessionController::stopScan);
    operations_->connectInput(editPanel_.data(), &EditPanel::scanStopRequested, A::EditScanStop);
    operations_->bindMember(A::EditJumpOut, editController_, &EditSessionController::jumpToOut);
    operations_->connectInput(editPanel_.data(), &EditPanel::jumpToOutRequested, A::EditJumpOut);
    operations_->bindMember(A::EditJumpEnd, editController_, &EditSessionController::jumpToEnd);
    operations_->connectInput(editPanel_.data(), &EditPanel::jumpToEndRequested, A::EditJumpEnd);
    operations_->bindMember(A::EditSetIn, editController_, &EditSessionController::setInPoint);
    operations_->connectInput(editPanel_.data(), &EditPanel::setInRequested, A::EditSetIn);
    operations_->bindMember(A::EditSetOut, editController_, &EditSessionController::setOutPoint);
    operations_->connectInput(editPanel_.data(), &EditPanel::setOutRequested, A::EditSetOut);
    operations_->bindMember(A::EditInTime, editController_, &EditSessionController::setInPointTime);
    operations_->connectInput(editPanel_.data(), &EditPanel::inPointTimeRequested, A::EditInTime);
    operations_->bindMember(A::EditOutTime, editController_, &EditSessionController::setOutPointTime);
    operations_->connectInput(editPanel_.data(), &EditPanel::outPointTimeRequested, A::EditOutTime);
    operations_->bindMember(A::EditStartMode, editController_, &EditSessionController::setStartMode);
    operations_->connectInput(editPanel_.data(), &EditPanel::startModeRequested, A::EditStartMode);
    operations_->bindMember(A::EditEndMode, editController_, &EditSessionController::setEndMode);
    operations_->connectInput(editPanel_.data(), &EditPanel::endModeRequested, A::EditEndMode);
    operations_->bindMember(A::EditGoto, editController_, &EditSessionController::setGotoTarget);
    operations_->connectInput(editPanel_.data(), &EditPanel::gotoTargetRequested, A::EditGoto);
    operations_->bindMember(A::EditRepeat, editController_, &EditSessionController::setRepeatOptions);
    operations_->connectInput(editPanel_.data(), &EditPanel::repeatOptionsRequested, A::EditRepeat);
    operations_->bindMember(A::EditFadeIn, editController_, &EditSessionController::setFadeIn);
    operations_->connectInput(editPanel_.data(), &EditPanel::fadeInRequested, A::EditFadeIn);
    operations_->bindMember(A::EditFadeOut, editController_, &EditSessionController::setFadeOut);
    operations_->connectInput(editPanel_.data(), &EditPanel::fadeOutRequested, A::EditFadeOut);
    operations_->bindMember(A::EditEndCrossfade, editController_, &EditSessionController::setEndCrossfade);
    operations_->connectInput(editPanel_.data(), &EditPanel::endCrossfadeRequested, A::EditEndCrossfade);
    connect(editPanel_, &EditPanel::nominalVolumeRequested, editController_,
            &EditSessionController::setNominalVolume);
    connect(editPanel_, &EditPanel::nominalDelayRequested, editController_,
            &EditSessionController::setNominalDelayFrames);
    connect(editPanel_, &EditPanel::nominalEqRequested, editController_,
            &EditSessionController::setNominalEq);
    operations_->bindMember(A::EditSeek, editController_, &EditSessionController::seek);
    operations_->connectInput(editPanel_.data(), &EditPanel::seekRequested, A::EditSeek);
    operations_->bindMember(A::EditStepFrames, editController_, &EditSessionController::stepFrames);
    operations_->connectInput(editPanel_.data(), &EditPanel::frameStepRequested, A::EditStepFrames);
  }

  void connectUvcControls() {
    connect(audioInputDeviceMonitor_, &AudioInputDeviceMonitor::devicesChanged, this,
            [this] {
              if (closing_) return;
              inputControlPanel_->setAudioInputDevices(audioInputDeviceMonitor_->devices());
              refreshUvcPreviewSources();
            });
    connect(uvcDeviceMonitor_, &UvcDeviceMonitor::devicesChanged, this,
            [this] {
              if (inputControlPanel_) inputControlPanel_->setUvcDevices(uvcDeviceMonitor_->devices());
              // Retry stored physical-camera previews after virtual-camera
              // identity registration completes and discovery is unblocked.
              captureDiscoveryReady_ = true;
              refreshCaptureDevices(true);
            });
    connect(inputControlPanel_, &InputControlPanel::stillCaptureRequested,
            this, [this](bool program, qint64 durationMs) {
              captureStill(program, durationMs);
            });
    connect(inputControlPanel_, &InputControlPanel::uvcAddRequested, this,
            &Impl::registerUvcDevice);
    connect(uvcSourceSessions_,
            &UvcSourceSessionManager::thumbnailReady, this,
            [this](const QString &devicePath, const QImage &image) {
              applyUvcThumbnail(devicePath, image);
            });
    connect(uvcSourceSessions_, &UvcSourceSessionManager::sourceError, this,
            [this](const QString &devicePath, const QString &detail) {
              const QImage image =
                  uvcSourceSessions_->latestThumbnail(devicePath);
              applyUvcThumbnail(devicePath, image, detail);
              userInfoCenter_->showError(
                  vtrpon::isDeckLinkSource(devicePath) ? QStringLiteral("DeckLink") : vtrpon::isNetworkVideoSource(devicePath) ? QStringLiteral("NDI") : QStringLiteral("UVC"),
                  vtrpon::LocalizedText(vtrpon::isDeckLinkSource(devicePath) ? "decklink.input_error" : vtrpon::isNetworkVideoSource(devicePath) ? "network_video.input_error" : "uvc.input_error"), detail);
            });
    connect(uvcSourceSessions_, &UvcSourceSessionManager::sourceAvailabilityChanged,
            playlistModel_, &PlaylistModel::setUvcDeviceAvailable);
    connect(playlistModel_, &PlaylistModel::itemAdded, this,
            [this] { refreshUvcPreviewSources(); });
    connect(playlistModel_, &PlaylistModel::itemRemoved, this,
            [this] { refreshUvcPreviewSources(); });
    connect(playlistModel_, &PlaylistModel::itemChanged, this,
            [this] { refreshUvcPreviewSources(); });
    connect(playlistModel_, &PlaylistModel::activePlaylistChanged, this,
            [this] { refreshUvcPreviewSources(); });
    connect(playlistModel_, &PlaylistModel::modelReset, this,
            [this] { refreshUvcPreviewSources(); });
    inputControlPanel_->setUvcDevices(uvcDeviceMonitor_->devices());
    inputControlPanel_->setAudioInputDevices(audioInputDeviceMonitor_->devices());
    if (vtrpon::networkVideoAddonInstalled() || vtrpon::deckLinkAddonInstalled()) {
      auto *networkTimer = new QTimer(this);
      connect(networkTimer, &QTimer::timeout, this, [this] { if (!closing_) refreshCaptureDevices(); });
      networkTimer->start(500);
    }
    refreshCaptureDevices();
    refreshUvcPreviewSources();
  }

  QList<UvcDevice> availableCaptureDevices() const {
    auto devices = uvcDeviceMonitor_->devices();
    for (const auto &device : audioInputDeviceMonitor_->devices())
      devices.append({device.displayName, vtrpon::liveAudioSourcePath(device.deviceId)});
    if (vtrpon::networkVideoAddonInstalled() && vtrpon::savedNetworkVideoSettings().inputEnabled)
      for (const auto &source : vtrpon::networkVideoState().sources)
        devices.append({source.name, source.path});
    if (vtrpon::deckLinkAddonInstalled() && vtrpon::savedDeckLinkSettings().inputEnabled)
      for (const auto &channel : vtrpon::deckLinkState().channels) if (channel.input) {
        VtrPonDeckLinkStreamOptions options{};
        if (vtrpon::deckLinkSourceOptions(channel.path, &options)) {
          options.modeId = vtrpon::savedDeckLinkSettings().inputMode;
          devices.append({channel.name, vtrpon::deckLinkSourcePath(options)});
        }
      }
    return devices;
  }

  void refreshCaptureDevices(bool force = false) {
    if (vtrpon::networkVideoAddonInstalled()) {
      const auto state = vtrpon::networkVideoState();
      inputControlPanel_->setNetworkVideoSources(state.sources, vtrpon::savedNetworkVideoSettings().inputEnabled);
    }
    QList<vtrpon::NetworkVideoSource> deckLinkSources;
    for (const auto &device : availableCaptureDevices())
      if (vtrpon::isDeckLinkSource(device.devicePath)) deckLinkSources.append({device.displayName, device.devicePath});
    inputControlPanel_->setDeckLinkSources(deckLinkSources, vtrpon::deckLinkAddonInstalled() && vtrpon::savedDeckLinkSettings().inputEnabled);
    QSet<QString> paths;
    for (const auto &device : availableCaptureDevices())
      if (!vtrpon::isLiveAudioSource(device.devicePath)) paths.insert(device.devicePath);
    // Saved input items retain their selected format. A changed default format
    // does not make a still-connected channel unavailable.
    if (vtrpon::savedDeckLinkSettings().inputEnabled) {
      const auto channels = vtrpon::deckLinkState().channels;
      for (const auto &item : playlistModel_->snapshot().items) {
        VtrPonDeckLinkStreamOptions options{};
        if (!vtrpon::deckLinkSourceOptions(item.filePath, &options)) continue;
        options.modeId = 0;
        const auto channelPath = vtrpon::deckLinkSourcePath(options);
        if (std::any_of(channels.cbegin(), channels.cend(), [&](const auto &c) { return c.input && c.path == channelPath; })) paths.insert(item.filePath);
      }
    }
    // An empty list before the physical-device worker's first result is not
    // evidence that saved cameras were disconnected.
    if (captureDiscoveryReady_ && (force || paths != availableCapturePaths_)) {
      availableCapturePaths_ = paths;
      uvcSourceSessions_->setAvailableDevicePaths(paths);
      refreshUvcPreviewSources();
    }
  }

  void connectProgramControls() {
    using A = ControllerOperations::Action;
    operations_->bindMember(A::TakeSelectedOnAir, programController_, &ProgramPlayoutController::takeSelectedPlaylistItemOnAir);
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::onAirRequested, A::TakeSelectedOnAir);
    operations_->connectInput(playlistPanel_.data(), &PlaylistPanel::offAirRequested, A::OffAir);
    operations_->bindMember(A::OffAir, programController_, &ProgramPlayoutController::clearOnAir);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::offAirRequested, A::OffAir);
    operations_->bindMember(A::ToggleFtb, programController_, &ProgramPlayoutController::toggleFtb);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::ftbRequested, A::ToggleFtb);
    operations_->bindMember(A::ToggleDsk, programController_, &ProgramPlayoutController::toggleDsk);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::dskToggleRequested, A::ToggleDsk);
    operations_->bindMember(A::DskPlay, programController_, &ProgramPlayoutController::playDsk);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::dskPlayRequested, A::DskPlay);
    operations_->bindMember(A::DskPause, programController_, &ProgramPlayoutController::pauseDsk);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::dskPauseRequested, A::DskPause);
    operations_->bindMember(A::ProgramPause, programController_, &ProgramPlayoutController::pauseOnAir);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::pauseRequested, A::ProgramPause);
    operations_->bindMember(A::ProgramPlay, programController_, &ProgramPlayoutController::playOnAir);
    operations_->connectInput(programPanel_.data(), &ProgramPanel::playRequested, A::ProgramPlay);
    operations_->bindMember(A::ProgramTogglePlayback, programController_, &ProgramPlayoutController::togglePlayback);
  }

  void shutdownPlayers() {
    // Window destruction can bypass beginClose; keep the same total deadline.
    vtrpon::armApplicationShutdownDeadline();
    if (macroRunner_) macroRunner_->stop();
    operations_->disable();
    if (viewMenuController_) viewMenuController_->requestShutdown();
    if (audioOutputs_) audioOutputs_->requestShutdown();
    programController_->shutdown();
    uvcSourceSessions_->shutdown();
    editController_->shutdown();
  }

  void registerPlaylistItems() {
    QSettings settings = vtrpon::makeControllerSettings();
    const QString initialDirectory =
        settings.value(QStringLiteral("fileDialog/lastMediaDirectory"))
            .toString();
    const QStringList paths = QFileDialog::getOpenFileNames(
        controlWindow_, vtrpon::uiText("file_dialog.add_files.title"),
        initialDirectory,
        playlistMediaFileFilter());
    if (!paths.isEmpty()) {
      registerPlaylistPaths(paths);
    }
  }

  QString playlistMediaFileFilter() const {
#ifdef _WIN32
    if (powerPointComRegistered_)
      return vtrpon::uiText("file_dialog.media.filter");
#endif
    return vtrpon::uiText("file_dialog.media.filter_without_powerpoint");
  }

  void exportPlaylist() {
    if (!playlistModel_) {
      return;
    }
    if (hasPendingImports()) {
      showImportOperationUnavailable("menu.file.export_playlist");
      return;
    }
    QSettings settings = vtrpon::makeControllerSettings();
    const QString initialDirectory =
        settings.value(QStringLiteral("fileDialog/lastPlaylistDirectory"))
            .toString();
    QString suggestedPath =
        QStringLiteral("vtr-pon-export-%1.json")
            .arg(QDateTime::currentDateTime().toString(
                QStringLiteral("yyyyMMddHHmm")));
    if (!initialDirectory.isEmpty()) {
      suggestedPath = QDir(initialDirectory).filePath(suggestedPath);
    }
    QString filePath = QFileDialog::getSaveFileName(
        controlWindow_, vtrpon::uiText("file_dialog.export_playlist.title"),
        suggestedPath, vtrpon::uiText("file_dialog.playlist.filter"));
    if (filePath.isEmpty()) {
      return;
    }
    if (QFileInfo(filePath).suffix().isEmpty()) {
      filePath += QStringLiteral(".json");
    }

    const PlaylistModelSnapshot snapshot = playlistModel_->snapshot();
    const auto error = std::make_shared<QString>();
    fileIo_->enqueue([filePath, snapshot, error] {
      (void)PlaylistDocumentIo::writeFile(filePath, snapshot, error.get());
    }, [this, filePath, snapshot, error](const QString &workerError) {
      const auto detail = workerError.isEmpty() ? *error : workerError;
      if (!detail.isEmpty()) {
        userInfoCenter_->showError(QStringLiteral("PLAYLIST"),
            vtrpon::LocalizedText("playlist.export.failed"), detail);
        return;
      }
      auto settings = vtrpon::makeControllerSettings();
      settings.setValue(QStringLiteral("fileDialog/lastPlaylistDirectory"), QFileInfo(filePath).absolutePath());
      userInfoCenter_->showSuccess(QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.export.succeeded").arg(snapshot.playlists.size()).arg(snapshot.items.size()));
    });
  }

  void importPlaylist() {
    if (macroBlocksRestore()) return;
    if (!playlistModel_ || !editController_ || !programController_) {
      return;
    }
    if (hasPendingImports()) {
      showImportOperationUnavailable("menu.file.import_playlist");
      return;
    }
    QSettings settings = vtrpon::makeControllerSettings();
    const QString initialDirectory =
        settings.value(QStringLiteral("fileDialog/lastPlaylistDirectory"))
            .toString();
    const QString filePath = QFileDialog::getOpenFileName(
        controlWindow_, vtrpon::uiText("file_dialog.import_playlist.title"),
        initialDirectory, vtrpon::uiText("file_dialog.playlist.filter"));
    if (filePath.isEmpty()) {
      return;
    }

    if (!restorePlaylistFile(filePath, PlaylistRestoreSource::ManualImport)) {
      return;
    }
    settings.setValue(QStringLiteral("fileDialog/lastPlaylistDirectory"),
                      QFileInfo(filePath).absolutePath());
  }

  void restorePreviousSession() {
    if (macroBlocksRestore()) return;
    if (hasPendingImports()) {
      showImportOperationUnavailable(
          "menu.file.restore_previous_session");
      return;
    }
    const QString filePath = previousSessionFilePath();
    if (filePath.isEmpty()) {
      userInfoCenter_->showError(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.restore_previous.failed"),
          QStringLiteral(
              "The local application data directory is unavailable."));
      return;
    }
    (void)restorePlaylistFile(filePath,
                              PlaylistRestoreSource::PreviousSession);
  }

  struct PendingPlaylistRestore {
    QString path;
    PlaylistRestoreSource source;
    quint64 generation;
    quint64 revision;
    std::optional<PlaylistDocumentIo::PreparedRestore> result;
    QString error;
  };

  bool restorePlaylistFile(const QString &filePath, PlaylistRestoreSource source) {
    if (macroBlocksRestore()) return false;
    if (closing_ || hasPendingImports()) return false;
    if (restoreCancellation_) restoreCancellation_->store(true);
    restoreCancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = restoreCancellation_;
    auto request = std::make_shared<PendingPlaylistRestore>();
    request->path = filePath;
    request->source = source;
    request->generation = ++restoreGeneration_;
    request->revision = playlistRevision_;
    return fileIo_->enqueue([request, cancellation] {
      request->result = PlaylistDocumentIo::readForRestore(request->path, &request->error, cancellation.get());
    }, [this, request](const QString &error) {
      if (!error.isEmpty()) request->error = error;
      finishPlaylistRestore(request);
    });
  }

  void finishPlaylistRestore(const std::shared_ptr<PendingPlaylistRestore> &request) {
    if (closing_ || request->generation != restoreGeneration_) return;
    if (macroBlocksRestore()) return;
    const bool previousSession = request->source == PlaylistRestoreSource::PreviousSession;
    const char *failedTextId = previousSession ? "playlist.restore_previous.failed" : "playlist.import.failed";
    if (!request->result || !request->error.isEmpty()) {
      userInfoCenter_->showError(QStringLiteral("PLAYLIST"), vtrpon::LocalizedText(failedTextId), request->error);
      return;
    }
    if (hasPendingImports() || request->revision != playlistRevision_) {
      userInfoCenter_->showError(QStringLiteral("PLAYLIST"), vtrpon::LocalizedText(failedTextId),
          QStringLiteral("The playlist changed while the file was being read. Please import it again."));
      return;
    }
    if (playlistModel_->onAirItemId() != 0 || playlistModel_->dskItemId() != 0 || programController_->bgmIsActive()) {
      userInfoCenter_->showWarning(QStringLiteral("PLAYLIST"), vtrpon::LocalizedText(previousSession
          ? "playlist.restore_previous.playout_active" : "playlist.import.playout_active"));
      return;
    }
    const auto cueId = playlistModel_->cueItemId();
    if (cueId && !programController_->prepareForItemRemoval(cueId)) return;
    const auto editId = playlistModel_->editItemId();
    if (editController_->deferItemRemoval(editId, [] {})) {
      // Main retains the latest restore independently of EDIT's coalesced
      // removal continuation. A newer restore must not be lost behind an old one.
      QTimer::singleShot(25, this, [this, request] { finishPlaylistRestore(request); });
      return;
    }
    if (editId && !editController_->prepareForItemRemoval(editId)) return;
    QString error;
    const auto &prepared = *request->result;
    if (!playlistModel_->restoreSnapshot(prepared.snapshot, &error, &prepared.offlineItems)) {
      userInfoCenter_->showError(QStringLiteral("PLAYLIST"), vtrpon::LocalizedText(failedTextId), error);
      return;
    }
    userInfoCenter_->showSuccess(QStringLiteral("PLAYLIST"), vtrpon::LocalizedText(previousSession
        ? "playlist.restore_previous.succeeded" : "playlist.import.succeeded")
        .arg(prepared.snapshot.playlists.size()).arg(prepared.snapshot.items.size()));
  }

  void savePreviousSession() {
    if (previousSessionSaveAttempted_ || !playlistModel_) return;
    previousSessionSaveAttempted_ = true;
    const QString path = previousSessionFilePath();
    const auto snapshot = playlistModel_->snapshot();
    const auto error = std::make_shared<QString>();
    fileIo_->enqueue([path, snapshot, error] {
      if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("Could not create the previous-session directory.");
        return;
      }
      (void)PlaylistDocumentIo::writeFile(path, snapshot, error.get());
    }, [error](const QString &workerError) {
      const auto detail = workerError.isEmpty() ? *error : workerError;
      if (!detail.isEmpty()) qWarning().noquote() << "Could not save the previous session:" << detail;
    });
  }

  bool hasPendingImports() const {
    return mediaImports_->hasPendingImports() ||
           documentImports_->hasPendingImports();
  }

  void showImportOperationUnavailable(const char *actionTextId) {
    if (!userInfoCenter_) {
      return;
    }
    userInfoCenter_->showWarning(
        QStringLiteral("PLAYLIST"),
        vtrpon::LocalizedText(
            "playlist.conversion.playlist_action_unavailable")
            .arg(vtrpon::uiText(actionTextId)));
  }

  void rebuildTestPatternMenu() {
    if (!testPatternMenu_) return;
    testPatternMenu_->clear();
    for (const auto &entry : vtrpon::testPatterns::load()) {
      const QString path = vtrpon::testPatterns::path(entry);
      auto *action = testPatternMenu_->addAction(
          QFileInfo(path).fileName().replace(QLatin1Char('&'), QStringLiteral("&&")));
      action->setToolTip(QDir::toNativeSeparators(path));
      connect(action, &QAction::triggered, this, [this, path] {
        registerPlaylistPathsImpl({path}, false);
      });
    }
    if (testPatternMenu_->isEmpty()) {
      auto *empty = testPatternMenu_->addAction(vtrpon::uiText("test_patterns.empty"));
      empty->setEnabled(false);
    }
  }
  void registerPlaylistPaths(const QStringList &paths) {
    registerPlaylistPathsImpl(paths, true);
  }

  void registerPlaylistPathsImpl(const QStringList &paths,
                                 bool rememberMediaDirectory, int explicitPlaylist = 0) {
    if (!playlistModel_ || paths.isEmpty()) {
      return;
    }

    const int targetPlaylistNumber = explicitPlaylist > 0
        ? explicitPlaylist : playlistModel_->activePlaylistNumber();
    QSettings settings = vtrpon::makeControllerSettings();
    if (rememberMediaDirectory) {
      settings.setValue(QStringLiteral("fileDialog/lastMediaDirectory"),
                        QFileInfo(paths.constFirst()).absolutePath());
    }

#ifdef _WIN32
    auto batchPowerPointSettings = rememberedPowerPointAddSettings_;
#endif
    QStringList failures;
    for (const QString &path : paths) {
      const QFileInfo fileInfo(path);
      std::optional<ImageAddSettings> imageAddSettings = std::nullopt;
      if (fileInfo.suffix().compare(QStringLiteral("pptx"),
                                    Qt::CaseInsensitive) == 0) {
#ifdef _WIN32
        PowerPointAddSettings addSettings;
        if (batchPowerPointSettings) {
          addSettings = *batchPowerPointSettings;
        } else {
          PowerPointAddMethodDialog dialog(controlWindow_);
          if (dialog.exec() != QDialog::Accepted) {
            continue;
          }
          addSettings = dialog.settings();
          if (addSettings.rememberForSession) batchPowerPointSettings = addSettings;
        }

        QString enqueueError;
        if (!documentImports_->enqueuePowerPoint(path, addSettings, targetPlaylistNumber, &enqueueError)) {
          failures.append(QStringLiteral("%1: %2").arg(fileInfo.fileName(), enqueueError));
        }
#else
        failures.append(
            QStringLiteral("%1: PowerPoint import is available only on Windows.")
                .arg(fileInfo.fileName()));
#endif
        continue;
      }
      if (fileInfo.suffix().compare(QStringLiteral("pdf"),
                                    Qt::CaseInsensitive) == 0) {
#ifdef _WIN32
        PdfAddSettings addSettings;
        if (rememberedPdfAddSettings_) {
          addSettings = *rememberedPdfAddSettings_;
        } else {
          DocumentAddMethodDialog dialog(
              QStringLiteral("pdf.add"), QStringLiteral("pdf"),
              QStringLiteral("page"), controlWindow_);
          if (dialog.exec() != QDialog::Accepted) {
            continue;
          }
          const DocumentAddSettings documentSettings =
              dialog.documentSettings();
          switch (documentSettings.method) {
          case DocumentAddMethod::Native:
            addSettings.method = PdfAddMethod::Native;
            break;
          case DocumentAddMethod::VideoConversion:
            addSettings.method = PdfAddMethod::VideoConversion;
            break;
          case DocumentAddMethod::PngSequence:
            addSettings.method = PdfAddMethod::PngSequence;
            break;
          }
          addSettings.pageDurationSeconds =
              documentSettings.pageDurationSeconds;
          addSettings.crossDissolveEnabled =
              documentSettings.crossDissolveEnabled;
          addSettings.crossDissolveDurationSeconds =
              documentSettings.crossDissolveDurationSeconds;
          addSettings.rememberForSession =
              documentSettings.rememberForSession;
          if (addSettings.rememberForSession) {
            rememberedPdfAddSettings_ = addSettings;
          }
        }

        QString enqueueError;
        if (!documentImports_->enqueuePdf(path, addSettings, targetPlaylistNumber, &enqueueError)) {
          failures.append(QStringLiteral("%1: %2").arg(fileInfo.fileName(), enqueueError));
        }
#else
        failures.append(
            QStringLiteral("%1: PDF import is available only on Windows.")
                .arg(fileInfo.fileName()));
#endif
        continue;
      }
      if (isPngOrJpegImagePath(path)) {
        ImageAddSettings addSettings;
        if (rememberedImageAddSettings_) {
          addSettings = *rememberedImageAddSettings_;
        } else {
          ImageAddDialog dialog;
          if (QWidget *owner = controlWindow_.data()) {
            QWidget *const ownerWindow = owner->window();
            (void)dialog.winId();
            if (ownerWindow && ownerWindow->windowHandle() &&
                dialog.windowHandle()) {
              dialog.windowHandle()->setTransientParent(
                  ownerWindow->windowHandle());
            }
            dialog.adjustSize();
            dialog.move(ownerWindow->frameGeometry().center() -
                        dialog.rect().center());
          }
          if (dialog.exec() != QDialog::Accepted) {
            continue;
          }
          addSettings = dialog.settings();
          if (addSettings.rememberForSession) {
            rememberedImageAddSettings_ = addSettings;
          }
        }
        imageAddSettings = addSettings;
      }
      if (!mediaImports_->enqueue(path, targetPlaylistNumber,
              imageAddSettings
                  ? std::optional<qint64>(imageAddSettings->displayDurationMs)
                  : std::nullopt)) {
        failures.append(QStringLiteral("%1: The media item could not be queued.")
                            .arg(fileInfo.fileName()));
      }
    }
    documentImports_->startNext();
    if (!failures.isEmpty()) {
      userInfoCenter_->showWarning(
          QStringLiteral("PLAYLIST"),
          vtrpon::LocalizedText("playlist.add_media.some_failed"),
          failures.join(QLatin1Char('\n')));
    }
  }

  void registerUvcDevice(const QString &displayName,
                         const QString &devicePath) {
    const bool audioOnly = vtrpon::isLiveAudioSource(devicePath);
    if (!playlistModel_ || !inputControlPanel_ || !uvcSourceSessions_ ||
        displayName.isEmpty() || devicePath.isEmpty()) {
      return;
    }
    const QList<UvcDevice> availableDevices = availableCaptureDevices();
    const bool stillAvailable =
        std::any_of(availableDevices.cbegin(), availableDevices.cend(),
                    [&](const UvcDevice &device) {
                      return device.devicePath == devicePath &&
                             device.displayName == displayName;
                    });
    if (!stillAvailable) {
      userInfoCenter_->showWarning(
          vtrpon::isDeckLinkSource(devicePath) ? QStringLiteral("DeckLink") : vtrpon::isNetworkVideoSource(devicePath) ? QStringLiteral("NDI") : QStringLiteral("UVC"),
          vtrpon::LocalizedText(vtrpon::isDeckLinkSource(devicePath) ? "decklink.selected_unavailable" : vtrpon::isNetworkVideoSource(devicePath) ? "network_video.selected_unavailable" : "uvc.selected_unavailable"));
      refreshCaptureDevices();
      return;
    }

    inputControlPanel_->setUvcCaptureBusy(true);
    QString errorDetail;
    const bool sourceStarted = audioOnly ||
        uvcSourceSessions_->ensureSource(devicePath, &errorDetail);
    inputControlPanel_->setUvcCaptureBusy(false);
    if (!sourceStarted) {
      userInfoCenter_->showError(
          vtrpon::isDeckLinkSource(devicePath) ? QStringLiteral("DeckLink") : vtrpon::isNetworkVideoSource(devicePath) ? QStringLiteral("NDI") : QStringLiteral("UVC"),
          vtrpon::LocalizedText(vtrpon::isDeckLinkSource(devicePath) ? "decklink.start_failed" : vtrpon::isNetworkVideoSource(devicePath) ? "network_video.start_failed" : "uvc.start_failed"), errorDetail);
      return;
    }

    PlaylistItemRegistration item;
    item.filePath = devicePath;
    item.displayName = displayName;
    item.sourceType = PlaylistSourceType::Uvc;
    item.mediaType = audioOnly ? PlaylistMediaType::Audio : PlaylistMediaType::Video;
    item.settings.startMode = PlaylistStartMode::Play;
    item.settings.endMode = PlaylistEndMode::Pause;
    item.settings.outPointMs = std::nullopt;
    item.playlistNumber = playlistModel_->activePlaylistNumber();
    const QList<quint64> addedItemIds = playlistModel_->addItems({item});
    if (addedItemIds.size() != 1) {
      refreshUvcPreviewSources();
      userInfoCenter_->showError(
          vtrpon::isDeckLinkSource(devicePath) ? QStringLiteral("DeckLink") : vtrpon::isNetworkVideoSource(devicePath) ? QStringLiteral("NDI") : QStringLiteral("UVC"),
          vtrpon::LocalizedText(vtrpon::isDeckLinkSource(devicePath) ? "decklink.add_failed" : vtrpon::isNetworkVideoSource(devicePath) ? "network_video.add_failed" : "uvc.add_to_playlist_failed"));
      return;
    }
    const QImage thumbnail = audioOnly ? generateLiveAudioThumbnail()
        : uvcSourceSessions_->latestThumbnail(devicePath);
    if (!thumbnail.isNull()) {
      playlistPanel_->setItemThumbnail(addedItemIds.constFirst(), thumbnail);
    }
    refreshUvcPreviewSources();
  }

  void refreshUvcPreviewSources() {
    if (closing_) return;
    if (!playlistModel_ || !uvcSourceSessions_) {
      return;
    }
    QSet<QString> devicePaths;
    QSet<QString> registeredPaths;
    for (const auto &item : playlistModel_->snapshot().items) {
      if (item.sourceType != PlaylistSourceType::Uvc) continue;
      if (vtrpon::isLiveAudioSource(item.filePath)) {
        const QString deviceId = vtrpon::liveAudioDeviceId(item.filePath);
        const auto devices = audioInputDeviceMonitor_->devices();
        const bool available = audioInputDeviceMonitor_->isAvailable() &&
            std::any_of(devices.cbegin(), devices.cend(), [&](const auto &device) {
              return device.deviceId == deviceId;
            });
        playlistModel_->setUvcDeviceAvailable(item.filePath, available);
        continue;
      }
      if ((vtrpon::isNetworkVideoSource(item.filePath) && !vtrpon::networkVideoAddonInstalled()) || (vtrpon::isDeckLinkSource(item.filePath) && !vtrpon::deckLinkAddonInstalled())) {
        playlistModel_->setUvcDeviceAvailable(item.filePath, false);
        continue;
      }
      registeredPaths.insert(item.filePath);
    }
    uvcSourceSessions_->setRegisteredDevicePaths(registeredPaths);
    for (const auto &path : registeredPaths)
      playlistModel_->setUvcDeviceAvailable(path, !uvcSourceSessions_->sourceOffline(path));
    for (int row = 0; row < playlistModel_->itemCount(); ++row) {
      const auto item = playlistModel_->itemState(
          playlistModel_->itemIdAt(row));
      if (item && item->sourceType == PlaylistSourceType::Uvc &&
          !item->filePath.isEmpty() && registeredPaths.contains(item->filePath)) {
        devicePaths.insert(item->filePath);
      }
    }
    uvcSourceSessions_->setPreviewDevicePaths(devicePaths);
  }

  void applyUvcThumbnail(const QString &devicePath, const QImage &image,
                         const QString &errorDetail = QString()) {
    if (closing_ || !playlistModel_ || !playlistPanel_ || image.isNull()) {
      return;
    }
    for (int row = 0; row < playlistModel_->itemCount(); ++row) {
      const quint64 itemId = playlistModel_->itemIdAt(row);
      const auto item = playlistModel_->itemState(itemId);
      if (item && item->sourceType == PlaylistSourceType::Uvc &&
          item->filePath == devicePath) {
        playlistPanel_->setItemThumbnail(itemId, image, errorDetail);
      }
    }
  }

  void removePlaylistItem(quint64 itemId) {
    if (!playlistModel_ || !editController_ || !programController_) {
      return;
    }
    if (documentImports_->contains(itemId)) {
      documentImports_->cancel(itemId, true);
      documentImports_->startNext();
      return;
    }
    // Check PROGRAM first without mutating it. An EDIT unload failure must not
    // clear or otherwise alter the current PROGRAM CUE/DSK state.
    if (!programController_->canRemoveItem(itemId)) {
      return;
    }
    if (editController_->deferItemRemoval(itemId, [this, itemId] {
          removePlaylistItem(itemId);
        })) return;
    if (!editController_->prepareForItemRemoval(itemId)) {
      return;
    }
    if (!programController_->prepareForItemRemoval(itemId)) {
      return;
    }
    (void)playlistModel_->removeItem(itemId);
  }

  QPointer<QWidget> videoWindow_;
  QPointer<QWidget> controlWindow_;
  int stillCapturePlaylist_ = 0;
  bool stillCapturePending_ = false;
  bool closeAfterStillCapture_ = false;
  BackgroundIoQueue *fileIo_ = nullptr;
  QTimer closePoll_;
  QList<QPointer<MediaThumbnailer>> thumbnailersClosing_;
  quint64 playlistRevision_ = 0;
  quint64 restoreGeneration_ = 0;
  std::shared_ptr<std::atomic_bool> restoreCancellation_;
  bool closing_ = false;
  bool playersClosing_ = false;
  bool closeReady_ = false;
  vtrpon::UiLanguageManager *languageManager_ = nullptr;
  QMenu *fileMenu_ = nullptr;
  QMenu *toolsMenu_ = nullptr;
  QMenu *helpMenu_ = nullptr;
  QAction *checkUpdatesAction_ = nullptr;
  QAction *aboutAction_ = nullptr;
  QPointer<vtrpon::AboutWindow> aboutWindow_;
  vtrpon::UpdateChecker *updateChecker_ = nullptr;
  QPointer<vtrpon::UpdateWindow> updateWindow_;
  std::optional<std::pair<vtrpon::UpdateCheckStatus,
                          std::optional<vtrpon::UpdateRelease>>> pendingUpdate_;
  bool pendingUpdateManual_ = false;
  QMenu *testPatternMenu_ = nullptr;
  QMenu *editMenu_ = nullptr;
  QMenu *onAirMenu_ = nullptr;
  QAction *addFileAction_ = nullptr;
  QAction *exportPlaylistAction_ = nullptr;
  QAction *importPlaylistAction_ = nullptr;
  QAction *restorePreviousSessionAction_ = nullptr;
  QAction *settingsAction_ = nullptr;
  QAction *exportSettingsAction_ = nullptr;
  QAction *importSettingsAction_ = nullptr;
  QAction *exitAction_ = nullptr;
  QAction *clockSyncAction_ = nullptr;
  MacroRunner *macroRunner_ = nullptr;
  QPointer<MacroEditorWindow> macroEditor_;
  QPointer<QWidget> macroStatus_;
  QPointer<QLabel> macroStatusLabel_;
  QPointer<QPushButton> macroStopButton_;
  QAction *operatorTimerSettingsAction_ = nullptr;
  bool manualClockSyncPending_ = false;
  vtrpon::ViewMenuController *viewMenuController_ = nullptr;
  ControllerOperations *operations_ = nullptr;
  UserInfoCenter *userInfoCenter_ = nullptr;
  PlaylistModel *playlistModel_ = nullptr;
  MediaImportQueue *mediaImports_ = nullptr;
  DocumentImportQueue *documentImports_ = nullptr;
  UvcDeviceMonitor *uvcDeviceMonitor_ = nullptr;
  vtrpon::NetworkVideoRuntime networkVideoRuntime_;
  vtrpon::DeckLinkRuntime deckLinkRuntime_;
  QSet<QString> availableCapturePaths_;
  bool captureDiscoveryReady_ = false;
  AudioInputDeviceMonitor *audioInputDeviceMonitor_ = nullptr;
  AudioOutputCoordinator *audioOutputs_ = nullptr;
  QPointer<ControllerSettingsDialog> activeSettingsDialog_;
  UvcAudioMappingStore *uvcAudioMappingStore_ = nullptr;
  UvcSourceSessionManager *uvcSourceSessions_ = nullptr;
  QPointer<EditPanel> editPanel_;
  QPointer<PlaylistPanel> playlistPanel_;
  QPointer<ProgramPanel> programPanel_;
  QPointer<InputControlPanel> inputControlPanel_;
  EditSessionController *editController_ = nullptr;
  ProgramPlayoutController *programController_ = nullptr;
  vtrpon::SwitcherController *switcherController_ = nullptr;
  std::unique_ptr<ControllerShortcuts> shortcuts_;
  std::optional<ImageAddSettings> rememberedImageAddSettings_ = std::nullopt;
#ifdef _WIN32
  bool powerPointComRegistered_ = false;
  std::optional<PowerPointAddSettings> rememberedPowerPointAddSettings_;
  std::optional<PdfAddSettings> rememberedPdfAddSettings_;
#endif
  QHash<quint64, QTimer *> itemLogTimers_;
  bool previousSessionSaveAttempted_ = false;
};

MainController::MainController(QWidget *videoWindow, QWidget *controlWindow,
                               bool use4kProgramOutput,
                               vtrpon::UiLanguageManager *languageManager)
    : impl_(std::make_unique<Impl>(videoWindow, controlWindow,
                                  use4kProgramOutput, languageManager)) {}

MainController::~MainController() = default;

bool MainController::isAvailable() const { return impl_->isAvailable(); }

ControllerOperations *MainController::operations() const { return impl_->operations(); }
