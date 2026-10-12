#pragma once
#include "decklink.h"
#include "language_text.h"
#include "settings_style.h"
#include "addon_runtime_status_widget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QSet>
#include <cstring>

class DeckLinkSettingsPage final : public QWidget {
public:
    explicit DeckLinkSettingsPage(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("deckLinkSettingsPage")); vtrpon::setSettingsRole(this, "page");
        auto *outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
        title_ = new QLabel(this); vtrpon::setSettingsRole(title_, "pageTitle"); outer->addWidget(title_);
        description_ = new QLabel(this); description_->setWordWrap(true); vtrpon::setSettingsRole(description_, "description"); outer->addWidget(description_);
        auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget(scroll); auto *layout = new QVBoxLayout(content); layout->setContentsMargins(0, 0, 0, 0);
        scroll->setWidget(content); content->setAutoFillBackground(false); scroll->viewport()->setAutoFillBackground(false); outer->addWidget(scroll, 1);
        runtime_ = new vtrpon::AddonRuntimeStatusWidget(vtrpon::AddonRuntimeKind::DeckLink, vtrpon::deckLinkAddonPath(), this); layout->addWidget(runtime_);
        deviceStatus_ = new QLabel(this); deviceStatus_->setWordWrap(true); deviceStatus_->setTextFormat(Qt::PlainText);
        vtrpon::setSettingsRole(deviceStatus_, "status"); layout->addWidget(deviceStatus_);
        output_ = new QCheckBox(this); output_->setObjectName(QStringLiteral("deckLinkOutputEnabled")); layout->addWidget(output_);
        const std::array<const char *, 3> enableNames{"deckLinkCompositeEnabled", "deckLinkFillEnabled", "deckLinkKeyEnabled"};
        const std::array<const char *, 3> channelNames{"deckLinkOutputChannel", "deckLinkFillChannel", "deckLinkKeyChannel"};
        const std::array<const char *, 3> modeNames{"deckLinkOutputMode", "deckLinkFillMode", "deckLinkKeyMode"};
        for (unsigned i = 0; i < 3; ++i) {
            kinds_[i] = new QCheckBox(this); kinds_[i]->setObjectName(QString::fromLatin1(enableNames[i])); layout->addWidget(kinds_[i]);
            auto *form = new QFormLayout;
            channelLabels_[i] = new QLabel(this); channels_[i] = new QComboBox(this); channels_[i]->setObjectName(QString::fromLatin1(channelNames[i]));
            channels_[i]->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); channels_[i]->setMinimumContentsLength(24);
            channelLabels_[i]->setBuddy(channels_[i]); form->addRow(channelLabels_[i], channels_[i]);
            modeLabels_[i] = new QLabel(this); modes_[i] = new QComboBox(this); modes_[i]->setObjectName(QString::fromLatin1(modeNames[i]));
            modeLabels_[i]->setBuddy(modes_[i]); form->addRow(modeLabels_[i], modes_[i]); layout->addLayout(form);
            connect(kinds_[i], &QCheckBox::toggled, this, [this] { save(); });
            connect(channels_[i], &QComboBox::activated, this, [this, i] { refreshModes(i); save(); });
            connect(modes_[i], &QComboBox::activated, this, [this] { save(); });
        }
        auto *fillForm = new QFormLayout;
        fillLabel_ = new QLabel(this); fillFormat_ = new QComboBox(this); fillFormat_->setObjectName(QStringLiteral("deckLinkFillFormat"));
        fillFormat_->addItems({QStringLiteral("Straight"), QStringLiteral("Premultiplied")}); fillLabel_->setBuddy(fillFormat_); fillForm->addRow(fillLabel_, fillFormat_); layout->addLayout(fillForm);
        help_ = new QLabel(this); help_->setWordWrap(true); vtrpon::setSettingsRole(help_, "description"); layout->addWidget(help_);
        input_ = new QCheckBox(this); input_->setObjectName(QStringLiteral("deckLinkInputEnabled")); layout->addWidget(input_);
        auto *inputForm = new QFormLayout;
        inputModeLabel_ = new QLabel(this); inputMode_ = new QComboBox(this); inputMode_->setObjectName(QStringLiteral("deckLinkInputMode"));
        inputModeLabel_->setBuddy(inputMode_); inputForm->addRow(inputModeLabel_, inputMode_); layout->addLayout(inputForm);
        status_ = new QLabel(this); status_->setWordWrap(true); status_->setTextInteractionFlags(Qt::TextSelectableByMouse); status_->setTextFormat(Qt::PlainText);
        vtrpon::setSettingsRole(status_, "status"); layout->addWidget(status_); layout->addStretch();
        reload(); retranslate();
        connect(input_, &QCheckBox::toggled, this, [this] { save(); });
        connect(output_, &QCheckBox::toggled, this, [this] { save(); });
        connect(fillFormat_, &QComboBox::currentIndexChanged, this, [this] { save(); });
        connect(inputMode_, &QComboBox::activated, this, [this] { save(); });
        auto *timer = new QTimer(this); connect(timer, &QTimer::timeout, this, [this] { refresh(); }); timer->start(500);
    }
    void reload() {
        settings_ = vtrpon::savedDeckLinkSettings();
        const QSignalBlocker a(input_), b(output_), c(fillFormat_);
        input_->setChecked(settings_.inputEnabled); output_->setChecked(settings_.outputEnabled); fillFormat_->setCurrentIndex(settings_.premultipliedFill ? 1 : 0);
        const std::array<bool, 3> enabled{settings_.compositeEnabled, settings_.fillEnabled, settings_.keyEnabled};
        for (unsigned i = 0; i < 3; ++i) { const QSignalBlocker block(kinds_[i]); kinds_[i]->setChecked(enabled[i]); }
        channelSnapshot_.clear(); channels_[0]->clear(); refresh(); updateControls();
    }
    void retranslate() {
        runtime_->retranslate(); title_->setText(vtrpon::uiText("decklink.title")); description_->setText(vtrpon::uiText("decklink.description"));
        input_->setText(vtrpon::uiText("decklink.enable_input")); output_->setText(vtrpon::uiText("decklink.enable_output"));
        const std::array<const char *, 3> labels{"network_video.composite", "network_video.fill", "network_video.key"};
        for (unsigned i = 0; i < 3; ++i) {
            kinds_[i]->setText(vtrpon::uiText(labels[i])); channelLabels_[i]->setText(vtrpon::uiText("decklink.output_channel")); modeLabels_[i]->setText(vtrpon::uiText("decklink.output_mode"));
            channels_[i]->setAccessibleName(kinds_[i]->text() + QStringLiteral(" — ") + channelLabels_[i]->text());
            modes_[i]->setAccessibleName(kinds_[i]->text() + QStringLiteral(" — ") + modeLabels_[i]->text());
        }
        fillLabel_->setText(vtrpon::uiText("network_video.fill_format")); fillFormat_->setAccessibleName(fillLabel_->text());
        help_->setText(vtrpon::uiText("decklink.fill_key_help")); inputModeLabel_->setText(vtrpon::uiText("decklink.input_mode")); inputMode_->setAccessibleName(inputModeLabel_->text());
        channelSnapshot_.clear(); channels_[0]->clear(); refresh();
    }
private:
    void updateControls() {
        for (unsigned i = 0; i < 3; ++i) {
            kinds_[i]->setEnabled(output_->isChecked()); const bool enabled = output_->isChecked() && kinds_[i]->isChecked();
            channels_[i]->setEnabled(enabled); modes_[i]->setEnabled(enabled); channelLabels_[i]->setEnabled(enabled); modeLabels_[i]->setEnabled(enabled);
        }
        fillFormat_->setEnabled(output_->isChecked() && kinds_[1]->isChecked()); fillLabel_->setEnabled(fillFormat_->isEnabled());
    }
    void save() {
        settings_.inputEnabled = input_->isChecked(); settings_.outputEnabled = output_->isChecked();
        settings_.compositeEnabled = kinds_[0]->isChecked(); settings_.fillEnabled = kinds_[1]->isChecked(); settings_.keyEnabled = kinds_[2]->isChecked(); settings_.premultipliedFill = fillFormat_->currentIndex() == 1;
        settings_.outputChannel = channels_[0]->currentData().toString(); settings_.fillChannel = channels_[1]->currentData().toString(); settings_.keyChannel = channels_[2]->currentData().toString();
        settings_.outputMode = modes_[0]->currentData().toUInt(); settings_.fillMode = modes_[1]->currentData().toUInt(); settings_.keyMode = modes_[2]->currentData().toUInt();
        if (inputMode_->currentData().isValid()) settings_.inputMode = inputMode_->currentData().toUInt();
        vtrpon::saveDeckLinkSettings(settings_); updateControls();
    }
    void refreshModes(unsigned i) {
        const QSignalBlocker blocker(modes_[i]); modes_[i]->clear(); modes_[i]->addItem(vtrpon::uiText("decklink.select_format"), 0u);
        for (const auto &channel : vtrpon::deckLinkState().channels) if (!channel.input && channel.path == channels_[i]->currentData().toString())
            for (const auto &mode : channel.modes) modes_[i]->addItem(QString::fromUtf8(mode.nameUtf8, int(strnlen(mode.nameUtf8, 128))), mode.modeId);
        const auto saved = modes_[i]->findData(settings_.mode(i)); if (saved >= 0) modes_[i]->setCurrentIndex(saved);
        else if (settings_.mode(i)) { modes_[i]->addItem(vtrpon::uiText("decklink.saved_unavailable"), settings_.mode(i)); modes_[i]->setCurrentIndex(modes_[i]->count() - 1); }
    }
    void refresh() {
        const auto state = vtrpon::deckLinkState(); QStringList snapshot;
        for (const auto &channel : state.channels) { snapshot.append(channel.path + channel.name + QString::number(channel.input)); for (const auto &mode : channel.modes) snapshot.append(QString::number(mode.modeId)); }
        if (snapshot != channelSnapshot_ || !channels_[0]->count()) {
            channelSnapshot_ = snapshot;
            for (unsigned i = 0; i < 3; ++i) {
                const QSignalBlocker block(channels_[i]); channels_[i]->clear(); channels_[i]->addItem(vtrpon::uiText("decklink.select_channel"), QString{});
                for (const auto &channel : state.channels) if (!channel.input) channels_[i]->addItem(channel.name, channel.path);
                const int index = channels_[i]->findData(settings_.channel(i));
                if (index >= 0) channels_[i]->setCurrentIndex(index);
                else if (!settings_.channel(i).isEmpty()) { channels_[i]->addItem(vtrpon::uiText("decklink.saved_unavailable"), settings_.channel(i)); channels_[i]->setCurrentIndex(channels_[i]->count() - 1); }
                refreshModes(i);
            }
            const QSignalBlocker block(inputMode_); inputMode_->clear(); inputMode_->addItem(vtrpon::uiText("decklink.auto_mode"), 0u);
            QSet<uint32_t> inputModes;
            for (const auto &channel : state.channels) if (channel.input) for (const auto &mode : channel.modes) if (!inputModes.contains(mode.modeId)) {
                inputModes.insert(mode.modeId); inputMode_->addItem(QString::fromUtf8(mode.nameUtf8, int(strnlen(mode.nameUtf8, 128))), mode.modeId);
            }
            const auto index = inputMode_->findData(settings_.inputMode); if (index >= 0) inputMode_->setCurrentIndex(index);
            else if (settings_.inputMode) { inputMode_->addItem(vtrpon::uiText("decklink.saved_unavailable"), settings_.inputMode); inputMode_->setCurrentIndex(inputMode_->count() - 1); }
        }
        QStringList text;
        deviceStatus_->setText(vtrpon::uiText("decklink.no_devices"));
        deviceStatus_->setVisible(state.availability == vtrpon::DeckLinkState::Availability::NoDevices);
        if (!state.error.isEmpty()) text.append(state.error);
        if (!state.readbackError.isEmpty()) text.append(state.readbackError);
        const std::array<QString, 3> names{QStringLiteral("Composite"), QStringLiteral("Fill"), QStringLiteral("Key")};
        if (settings_.outputEnabled) for (unsigned i = 0; i < 3; ++i) if (settings_.outputMask() & (1u << i)) {
            const auto &out = state.outputs[i]; text.append(names[i] + QStringLiteral(": ") + (out.error.isEmpty() ?
                vtrpon::uiText(out.running ? "decklink.output_running" : "decklink.output_starting") : out.error));
        }
        if (settings_.outputEnabled && settings_.fillEnabled && settings_.keyEnabled && state.outputs[1].running && state.outputs[2].running) {
            if (state.pairSynchronized) text.append(vtrpon::uiText("decklink.pair_synchronized"));
            else if (!state.syncWarning.isEmpty()) text.append(vtrpon::uiText("decklink.pair_unsynchronized") + QStringLiteral(" ") + state.syncWarning);
        }
        if (text.isEmpty()) text.append(vtrpon::uiText(state.channels.isEmpty() ? "decklink.no_devices" : "decklink.ready"));
        status_->setText(text.join(QLatin1Char('\n')));
    }
    vtrpon::AddonRuntimeStatusWidget *runtime_;
    vtrpon::DeckLinkSettings settings_;
    QStringList channelSnapshot_;
    QLabel *title_, *description_, *fillLabel_, *help_, *inputModeLabel_, *status_, *deviceStatus_;
    QCheckBox *input_, *output_;
    QComboBox *inputMode_, *fillFormat_;
    std::array<QCheckBox *, 3> kinds_{};
    std::array<QComboBox *, 3> channels_{}, modes_{};
    std::array<QLabel *, 3> channelLabels_{}, modeLabels_{};
};
