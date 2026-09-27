#include "audio_output_coordinator.h"

#include "audio_output_settings.h"
#include "edit_session_controller.h"
#include "language_text.h"
#include "playlist_model.h"
#include "program_playout_controller.h"
#include "user_info.h"

#include <QDebug>

AudioOutputCoordinator::AudioOutputCoordinator(
    PlaylistModel *playlistModel, EditSessionController *editController,
    ProgramPlayoutController *programController, UserInfoCenter *userInfoCenter,
    QObject *parent)
    : QObject(parent), editController_(editController),
      programController_(programController), userInfoCenter_(userInfoCenter),
      audioOutputMonitor_(new AudioOutputDeviceMonitor(this)) {
  audioOutputMonitor_->setRefreshInterval(0);
  connect(playlistModel, &PlaylistModel::mediaAvailabilityCheckRequested,
          audioOutputMonitor_, &AudioOutputDeviceMonitor::refresh);
  connect(programController_,
          &ProgramPlayoutController::programAudioOutputError, this,
          [this](const QString &detail) {
            if (shuttingDown_)
              return;
            pendingProgramAudioError_ = detail;
            pendingProgramAudioDevice_ =
                programController_->appliedProgramOutputDevice();
            audioOutputMonitor_->refresh();
          });
  connect(audioOutputMonitor_, &AudioOutputDeviceMonitor::refreshed, this,
          &AudioOutputCoordinator::audioOutputsRefreshed);
  connect(editController_, &EditSessionController::pflRequested, this,
          &AudioOutputCoordinator::requestPfl);
  connect(editController_, &EditSessionController::pflOutputError, this,
          [this](const QString &message) {
            if (shuttingDown_)
              return;
            pendingPflError_ = message;
            audioOutputMonitor_->refresh();
          });
  connect(userInfoCenter_, &UserInfoCenter::pflSameDeviceDecision, this,
          [this](bool accepted) {
            const auto route = pendingPflConfirmation_;
            pendingPflConfirmation_.reset();
            if (!accepted || shuttingDown_ || !route ||
                vtrpon::savedEditPflOutputDeviceId() != route->pflId) {
              editController_->setPflEnabled(false);
              return;
            }
            approvedPflRoute_ = route;
            pendingPflDevice_ = route->pflId;
            audioOutputMonitor_->refresh();
          });
}

void AudioOutputCoordinator::applySavedSettings() {
  cancelPflRequest();
  programController_->requestProgramOutputDevice(
      vtrpon::savedProgramOutputDeviceId());
  editController_->setPflOutputDeviceId(vtrpon::savedEditPflOutputDeviceId());
  audioOutputMonitor_->refresh();
}

void AudioOutputCoordinator::programOutputSelectionChanged(const QString &id) {
  cancelPflRequest();
  programController_->requestProgramOutputDevice(id);
  showAudioOutputSelectionChanged(true, id);
}

void AudioOutputCoordinator::pflOutputSelectionChanged(const QString &id) {
  cancelPflRequest();
  editController_->setPflOutputDeviceId(id);
  showAudioOutputSelectionChanged(false, id);
  audioOutputMonitor_->refresh();
}

void AudioOutputCoordinator::requestShutdown() {
  shuttingDown_ = true;
  cancelPflRequest();
  audioOutputMonitor_->requestShutdown();
}

bool AudioOutputCoordinator::shutdownComplete() const {
  return audioOutputMonitor_->shutdownComplete();
}

void AudioOutputCoordinator::cancelPflRequest() {
  pendingPflDevice_.reset();
  pendingPflConfirmation_.reset();
  approvedPflRoute_.reset();
  userInfoCenter_->cancelPflSameDeviceConfirmation();
  editController_->setPflEnabled(false);
}

void AudioOutputCoordinator::requestPfl(bool enabled) {
  if (shuttingDown_)
    return;
  qInfo() << "OP PFL source=UI enabled=" << enabled;
  if (!enabled) {
    cancelPflRequest();
    return;
  }
  editController_->setPflEnabled(false);
  if (pendingPflConfirmation_) return;
  pendingPflDevice_.reset();
  approvedPflRoute_.reset();
  const auto id = vtrpon::savedEditPflOutputDeviceId();
  if (id.isEmpty()) {
    editController_->setPflEnabled(false);
    userInfoCenter_->showAudioOutputError(
        vtrpon::LocalizedText("edit.pfl.device_not_configured"));
    return;
  }
  pendingPflDevice_ = id;
  audioOutputMonitor_->refresh();
}

void AudioOutputCoordinator::audioOutputsRefreshed(bool success) {
  if (shuttingDown_)
    return;
  const auto pflError = pendingPflError_;
  pendingPflError_.reset();
  const auto audioError = pendingProgramAudioError_;
  pendingProgramAudioError_.reset();
  const auto requestedPfl = pendingPflDevice_;
  pendingPflDevice_.reset();
  const auto approvedRoute = approvedPflRoute_;
  approvedPflRoute_.reset();
  if (!success) {
    if (pflError)
      userInfoCenter_->showAudioOutputWarning(
          vtrpon::LocalizedText::fromRaw(*pflError));
    if (audioError)
      userInfoCenter_->showAudioOutputWarning(
          vtrpon::LocalizedText("audio.output.unavailable"), *audioError);
    if (requestedPfl) {
      editController_->setPflEnabled(false);
      userInfoCenter_->showAudioOutputError(
          vtrpon::LocalizedText("audio.outputs.check_failed"));
    }
    return; // Enumeration failures are not proof that a device was removed.
  }
  auto programId = vtrpon::savedProgramOutputDeviceId();
  const auto pflId = vtrpon::savedEditPflOutputDeviceId();
  const auto missing = vtrpon::missingAudioOutputs(
      audioOutputMonitor_->devices(), audioOutputMonitor_->defaultDeviceId(),
      programId, pflId);
  bool programMissing = missing.program;
  const bool pflMissing = missing.pfl || (requestedPfl && pflId.isEmpty());
  bool failedProgramDeviceMissing =
      audioError &&
      vtrpon::missingAudioOutputs(audioOutputMonitor_->devices(),
                                  audioOutputMonitor_->defaultDeviceId(),
                                  pendingProgramAudioDevice_, {})
          .program;
  bool programFallback = false;
  const bool defaultAvailable =
      !vtrpon::missingAudioOutputs(audioOutputMonitor_->devices(),
                                   audioOutputMonitor_->defaultDeviceId(), {},
                                   {})
           .program;
  if (vtrpon::savedUseDefaultAudioOutputWhenMissing() && defaultAvailable) {
    if ((programMissing || failedProgramDeviceMissing) &&
        vtrpon::saveProgramOutputDeviceId({})) {
      programId.clear();
      programController_->requestProgramOutputDevice({});
      programMissing = false;
      failedProgramDeviceMissing = false;
      programFallback = true;
      userInfoCenter_->showSuccess(
          QStringLiteral("PROGRAM"),
          vtrpon::LocalizedText("audio.output.fallback_applied")
              .arg(vtrpon::uiText("info.source.program")));
    }
    // PFL must keep its selected endpoint: default output may be on air.
    if (programFallback)
      emit savedAudioOutputsChanged();
  }
  const PflRoute route{pflId, programId,
                       programController_->appliedProgramOutputDevice(),
                       audioOutputMonitor_->defaultDeviceId()};
  const bool sameDevice =
      vtrpon::sameAudioOutputDevice(route.programId, route.pflId, route.defaultId) ||
      vtrpon::sameAudioOutputDevice(route.appliedProgramId, route.pflId,
                                    route.defaultId);
  if (programFallback && sameDevice) editController_->setPflEnabled(false);
  const auto programKey = QStringLiteral("PROGRAM:") + programId.toLower();
  const auto pflKey = QStringLiteral("PFL:") + pflId.toLower();
  if (!programMissing && missingAudioOutputs_.contains(programKey)) {
    // Retry the same configured endpoint on reconnection, without requiring
    // OFF AIR. The failed audio graph is independent of video playback.
    programController_->requestProgramOutputDevice(programId);
  }
  const bool warnProgram =
      failedProgramDeviceMissing ||
      (programMissing && !missingAudioOutputs_.contains(programKey));
  const bool warnPfl = pflMissing && (!missingAudioOutputs_.contains(pflKey) ||
                                      (requestedPfl && *requestedPfl == pflId));
  missingAudioOutputs_.clear();
  if (programMissing)
    missingAudioOutputs_.insert(programKey);
  if (pflMissing)
    missingAudioOutputs_.insert(pflKey);
  if (warnProgram || warnPfl) {
    const vtrpon::LocalizedText message(
        warnProgram && warnPfl ? "startup.audio_outputs_missing"
        : warnProgram          ? "startup.program_output_missing"
        : pflId.isEmpty()      ? "edit.pfl.device_not_configured"
                               : "startup.pfl_output_missing");
    if (warnPfl && (requestedPfl || pflError))
      userInfoCenter_->showAudioOutputError(message);
    else
      userInfoCenter_->showAudioOutputWarning(message);
  }
  if (requestedPfl && *requestedPfl == pflId) {
    if (pflMissing) {
      editController_->setPflEnabled(false);
    } else if (sameDevice && (!approvedRoute || *approvedRoute != route)) {
      editController_->setPflEnabled(false);
      pendingPflConfirmation_ = route;
      userInfoCenter_->showPflSameDeviceConfirmation();
    } else {
      editController_->setPflEnabled(true);
    }
  }
  if (pflError && !pflMissing)
    userInfoCenter_->showAudioOutputWarning(
        vtrpon::LocalizedText::fromRaw(*pflError));
  const bool outputFailed =
      !programController_->programAudioOutputReady() &&
      programController_->programOutputDeviceStatusKey() ==
          QStringLiteral("settings.output.failed");
  const QString failedKey =
      outputFailed ? programId.toLower() + QStringLiteral(":failed")
                   : QString();
  if (!programMissing && !failedProgramDeviceMissing && !programFallback &&
      (audioError || (outputFailed && failedKey != lastAudioOutputFailure_))) {
    userInfoCenter_->showAudioOutputWarning(
        vtrpon::LocalizedText("audio.output.unavailable"),
        audioError.value_or(QString()));
  }
  lastAudioOutputFailure_ = failedKey;
}

void AudioOutputCoordinator::showAudioOutputSelectionChanged(
    bool program, const QString &id) {
  QString name = id;
  if ((program && id.isEmpty()) ||
      id == QLatin1StringView(vtrpon::kSystemDefaultPflDeviceId))
    name = vtrpon::uiText("settings.output.default");
  else if (!program && id.isEmpty())
    name = vtrpon::uiText("settings.output.none");
  else {
    for (const auto &device : audioOutputMonitor_->devices()) {
      if (device.id.compare(id, Qt::CaseInsensitive) == 0) {
        name = device.name;
        break;
      }
    }
  }
  userInfoCenter_->showSuccess(
      program ? QStringLiteral("PROGRAM") : QStringLiteral("EDIT"),
      vtrpon::LocalizedText("audio.output.selection_changed")
          .arg(program ? QStringLiteral("Program") : QStringLiteral("PFL"))
          .arg(name));
}
