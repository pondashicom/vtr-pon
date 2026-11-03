#pragma once

#include "addon_runtime_status_widget.h"
#include "atem_addon_switcher.h"
#include "controller_scaling.h"
#include "decklink.h"
#include "network_video.h"
#include <QDialog>
#include <QEvent>
#include <QGroupBox>
#include <QScrollArea>
#include <QVector>

namespace vtrpon {

struct AddonRuntimeTarget {
    QString id;
    QString title;
    AddonRuntimeKind kind;
    QString file;
};

inline QVector<AddonRuntimeTarget> addonRuntimeTargets() {
    // Use the host's paths, including development overrides and macOS bundles.
    return {
        {QStringLiteral("vtr-pon-atem-addon"), QStringLiteral("ATEM"),
         AddonRuntimeKind::Atem, AtemAddonSwitcher::addonFilePath()},
        {QStringLiteral("vtr-pon-network-video-addon"), QStringLiteral("NDI"),
         AddonRuntimeKind::NetworkVideo, networkVideoAddonPath()},
        {QStringLiteral("vtr-pon-decklink-addon"), QStringLiteral("DeckLink"),
         AddonRuntimeKind::DeckLink, deckLinkAddonPath()}};
}

inline QString addonOutputOperationalStatus(unsigned running, unsigned enabled) {
    if (!running) return uiText("addons.runtime.operation_starting");
    if (running == enabled) return uiText("addons.runtime.operation_running");
    return uiText("addons.runtime.operation_partial").arg(running).arg(enabled);
}

inline QString networkVideoOperationalStatus(const NetworkVideoSettings &settings,
                                             const NetworkVideoState &state) {
    if (!settings.inputEnabled && !settings.outputEnabled) return uiText("addons.runtime.operation_off");
    if (!state.error.isEmpty()) return uiText("addons.runtime.operation_error").arg(state.error);
    unsigned running = 0, enabled = 0;
    for (unsigned i = 0; i < state.outputs.size(); ++i) {
        if (!settings.outputEnabled || !(settings.outputMask() & (1u << i))) continue;
        if (!state.outputs[i].error.isEmpty())
            return uiText("addons.runtime.operation_error").arg(state.outputs[i].error);
        ++enabled;
        if (state.outputs[i].running) ++running;
    }
    QStringList text;
    if (enabled) text << addonOutputOperationalStatus(running, enabled);
    if (settings.inputEnabled)
        text << (state.inputRunning ? uiText("addons.runtime.operation_input").arg(state.sources.size())
                                   : uiText("addons.runtime.operation_input_starting"));
    return text.isEmpty() ? uiText("addons.runtime.operation_off") : text.join(QLatin1Char('\n'));
}

inline QString deckLinkOperationalStatus(const DeckLinkSettings &settings, const DeckLinkState &state) {
    if (!settings.inputEnabled && !settings.outputEnabled) return uiText("addons.runtime.operation_off");
    if (!state.error.isEmpty()) return uiText("addons.runtime.operation_error").arg(state.error);
    if (state.availability == DeckLinkState::Availability::AddonMissing)
        return uiText("addons.runtime.operation_unavailable");
    if (state.availability == DeckLinkState::Availability::Checking)
        return uiText("addons.runtime.operation_checking");
    if (state.availability == DeckLinkState::Availability::NoDevices)
        return uiText("addons.runtime.operation_no_devices");
    if (state.availability == DeckLinkState::Availability::RuntimeUnavailable)
        return uiText("addons.runtime.operation_unavailable");
    if (settings.outputEnabled && !state.outputError.isEmpty())
        return uiText("addons.runtime.operation_error").arg(state.outputError);
    if (settings.inputEnabled && !state.readbackError.isEmpty())
        return uiText("addons.runtime.operation_error").arg(state.readbackError);
    unsigned running = 0, enabled = 0;
    for (unsigned i = 0; i < state.outputs.size(); ++i) {
        if (!(settings.outputMask() & (1u << i))) continue;
        if (!state.outputs[i].error.isEmpty())
            return uiText("addons.runtime.operation_error").arg(state.outputs[i].error);
        ++enabled;
        if (state.outputs[i].running) ++running;
    }
    if (enabled) return addonOutputOperationalStatus(running, enabled);
    return uiText("addons.runtime.operation_devices").arg(state.channels.size());
}

inline QString addonOperationalStatus(const QString &id) {
    if (id == QLatin1StringView("vtr-pon-network-video-addon"))
        return networkVideoOperationalStatus(savedNetworkVideoSettings(), networkVideoState());
    if (id == QLatin1StringView("vtr-pon-decklink-addon"))
        return deckLinkOperationalStatus(savedDeckLinkSettings(), deckLinkState());
    return {};
}

// Remains hidden while the disposable probes run. Show one modeless notice
// only when an installed add-on needs attention; never start a media session.
class AddonRuntimeStartupWarning final : public QDialog {
public:
    explicit AddonRuntimeStartupWarning(QWidget *parent,
            const QVector<AddonRuntimeTarget> &targets = addonRuntimeTargets(),
            const QString &probeProgram = {}) : QDialog(parent) {
        setObjectName(QStringLiteral("addonRuntimeStartupWarning"));
        setAttribute(Qt::WA_DeleteOnClose);
        setModal(false);
        setStyleSheet(settingsStyleSheet(objectName()));
        auto *layout = new QVBoxLayout(this);
        description_ = new QLabel(this);
        description_->setWordWrap(true);
        description_->setTextFormat(Qt::PlainText);
        setSettingsRole(description_, "description");
        layout->addWidget(description_);
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget(scroll);
        auto *rows = new QVBoxLayout(content);
        rows->setAlignment(Qt::AlignTop);
        for (const auto &target : targets) {
            if (!QFileInfo(target.file).isFile()) continue;
            auto *group = new QGroupBox(target.title, content);
            auto *groupLayout = new QVBoxLayout(group);
            auto *runtime = new AddonRuntimeStatusWidget(
                target.kind, target.file, group, probeProgram);
            groupLayout->addWidget(runtime);
            rows->addWidget(group);
            rows_.push_back({group, runtime});
            runtime->setStateChangedHandler([this] { refresh(); });
        }
        scroll->setWidget(content);
        layout->addWidget(scroll, 1);
        auto *actions = new QHBoxLayout;
        actions->addStretch();
        close_ = new QPushButton(this);
        close_->setObjectName(QStringLiteral("addonRuntimeClose"));
        connect(close_, &QPushButton::clicked, this, &QDialog::close);
        actions->addWidget(close_);
        layout->addLayout(actions);
        resize(scalePx(640), scalePx(420));
        retranslate();
        QTimer::singleShot(0, this, [this] { refresh(); });
    }

protected:
    void changeEvent(QEvent *event) override {
        QDialog::changeEvent(event);
        if (event->type() == QEvent::LanguageChange) retranslate();
    }

private:
    struct Row { QGroupBox *group; AddonRuntimeStatusWidget *runtime; };
    void refresh() {
        for (const auto &row : rows_)
            if (row.runtime->runtimeState() == QLatin1StringView("checking")) return;
        bool needsAttention = false;
        for (const auto &row : rows_) {
            const auto state = row.runtime->runtimeState();
            const bool problem = state != QLatin1StringView("ready")
                && state != QLatin1StringView("addon_missing")
                && state != QLatin1StringView("no_devices");
            row.group->setVisible(problem);
            needsAttention |= problem;
        }
        if (!needsAttention) {
            deleteLater();
        } else if (!isVisible()) {
            show();
        }
    }
    void retranslate() {
        setWindowTitle(uiText("addons.runtime.startup_title"));
        description_->setText(uiText("addons.runtime.startup_description"));
        close_->setText(uiText("common.close"));
        for (const auto &row : rows_) row.runtime->retranslate();
    }
    QVector<Row> rows_;
    QLabel *description_;
    QPushButton *close_;
};
}
