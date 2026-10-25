#pragma once
#include "network_video.h"
#include "language_text.h"
#include "addon_runtime_status_widget.h"
#include "settings_style.h"
#include <QCheckBox>
#include <QComboBox>
#include <QToolButton>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

class NetworkVideoSettingsPage final : public QWidget {
public:
    explicit NetworkVideoSettingsPage(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("networkVideoSettingsPage"));
        vtrpon::setSettingsRole(this, "page");
        auto *layout = new QVBoxLayout(this);
        title_ = new QLabel(QStringLiteral("NDI"), this);
        title_->setObjectName(QStringLiteral("settingsPageTitle"));
        vtrpon::setSettingsRole(title_, "pageTitle");
        layout->addWidget(title_);
        description_ = new QLabel(this); description_->setWordWrap(true);
        vtrpon::setSettingsRole(description_, "description");
        layout->addWidget(description_);
        auto *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto *content = new QWidget(scroll);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        scroll->setWidget(content);
        // setWidget enables background filling; keep the settings page visible underneath.
        content->setAutoFillBackground(false);
        scroll->viewport()->setAutoFillBackground(false);
        layout->addWidget(scroll, 1);
        layout = vtrpon::addSettingsGroup(contentLayout, "settings.group.ndi", false);
        runtime_ = new vtrpon::AddonRuntimeStatusWidget(vtrpon::AddonRuntimeKind::NetworkVideo,
            vtrpon::networkVideoAddonPath(), this);
        layout->addWidget(runtime_);
        output_ = new QCheckBox(this); output_->setObjectName(QStringLiteral("networkVideoOutputEnabled"));
        layout->addWidget(output_);
        auto *form = new QFormLayout;
        nameLabel_ = new QLabel(this);
        vtrpon::setSettingsRole(nameLabel_, "fieldLabel");
        name_ = new QLineEdit(this); name_->setObjectName(QStringLiteral("networkVideoOutputName"));
        name_->setMaxLength(200); name_->setPlaceholderText(vtrpon::defaultNetworkVideoOutputName());
        nameLabel_->setBuddy(name_); form->addRow(nameLabel_, name_); layout->addLayout(form);
        outputsLabel_ = new QLabel(this);
        vtrpon::setSettingsRole(outputsLabel_, "fieldLabel"); layout->addWidget(outputsLabel_);
        const std::array<const char *, 4> objects{"networkVideoCompositeEnabled", "networkVideoFillEnabled", "networkVideoKeyEnabled", "networkVideoAlphaEnabled"};
        for (unsigned i = 0; i < kinds_.size(); ++i) {
            kinds_[i] = new QCheckBox(this); kinds_[i]->setObjectName(QString::fromLatin1(objects[i]));
            layout->addWidget(kinds_[i]);
        }
        details_ = new QToolButton(this); details_->setCheckable(true);
        details_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); details_->setArrowType(Qt::RightArrow);
        details_->setObjectName(QStringLiteral("networkVideoDetails")); layout->addWidget(details_);
        detailsBody_ = new QWidget(this);
        auto *detailsLayout = new QFormLayout(detailsBody_); detailsLayout->setContentsMargins(0, 0, 0, 0);
        fillLabel_ = new QLabel(detailsBody_); vtrpon::setSettingsRole(fillLabel_, "fieldLabel");
        fillFormat_ = new QComboBox(detailsBody_); fillFormat_->setObjectName(QStringLiteral("networkVideoFillFormat"));
        fillFormat_->addItems({QStringLiteral("Straight"), QStringLiteral("Premultiplied")});
        fillLabel_->setBuddy(fillFormat_); detailsLayout->addRow(fillLabel_, fillFormat_);
        layout->addWidget(detailsBody_); detailsBody_->hide();
        alphaHelp_ = new QLabel(this); alphaHelp_->setWordWrap(true);
        vtrpon::setSettingsRole(alphaHelp_, "description"); layout->addWidget(alphaHelp_);
        connect(details_, &QToolButton::toggled, this, [this](bool expanded) {
            detailsBody_->setVisible(expanded); details_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        });
        input_ = new QCheckBox(this); input_->setObjectName(QStringLiteral("networkVideoInputEnabled"));
        layout->addWidget(input_);
        status_ = new QLabel(this); status_->setWordWrap(true);
        vtrpon::setSettingsRole(status_, "status");
        status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(status_); layout->addStretch();
        reload(); retranslate();
        connect(output_, &QCheckBox::toggled, this, [this] { save(); });
        connect(input_, &QCheckBox::toggled, this, [this] { save(); });
        connect(name_, &QLineEdit::editingFinished, this, [this] { save(); });
        for (auto *kind : kinds_) connect(kind, &QCheckBox::toggled, this, [this] { save(); });
        connect(fillFormat_, &QComboBox::currentIndexChanged, this, [this] { save(); });
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refreshStatus(); });
        timer->start(500);
    }
    void reload() {
        const auto settings = vtrpon::savedNetworkVideoSettings();
        const QSignalBlocker output(output_), input(input_), name(name_);
        output_->setChecked(settings.outputEnabled); input_->setChecked(settings.inputEnabled);
        name_->setText(settings.outputName);
        const std::array<bool, 4> enabled{settings.compositeEnabled, settings.fillEnabled, settings.keyEnabled, settings.alphaEnabled};
        for (unsigned i = 0; i < kinds_.size(); ++i) {
            const QSignalBlocker block(kinds_[i]); kinds_[i]->setChecked(enabled[i]);
        }
        const QSignalBlocker format(fillFormat_);
        fillFormat_->setCurrentIndex(settings.premultipliedFill ? 1 : 0);
        updateControls();
    }
    void retranslate() {
        vtrpon::retranslateSettingsGroups(this);
        runtime_->retranslate();
        description_->setText(vtrpon::uiText("network_video.description"));
        output_->setText(vtrpon::uiText("network_video.enable_output"));
        input_->setText(vtrpon::uiText("network_video.enable_input"));
        nameLabel_->setText(vtrpon::uiText("network_video.source_name"));
        name_->setAccessibleName(nameLabel_->text());
        outputsLabel_->setText(vtrpon::uiText("network_video.outputs"));
        const std::array<const char *, 4> labels{"network_video.composite", "network_video.fill", "network_video.key", "network_video.alpha"};
        for (unsigned i = 0; i < kinds_.size(); ++i) kinds_[i]->setText(vtrpon::uiText(labels[i]));
        details_->setText(vtrpon::uiText("network_video.details"));
        fillLabel_->setText(vtrpon::uiText("network_video.fill_format"));
        fillFormat_->setAccessibleName(fillLabel_->text());
        alphaHelp_->setText(vtrpon::uiText("network_video.alpha_help"));
        refreshStatus();
    }
private:
    void save() {
        vtrpon::saveNetworkVideoSettings({output_->isChecked(), input_->isChecked(), name_->text(),
            kinds_[0]->isChecked(), kinds_[1]->isChecked(), kinds_[2]->isChecked(), kinds_[3]->isChecked(),
            fillFormat_->currentIndex() == 1});
        updateControls(); refreshStatus();
    }
    void refreshStatus() {
        const auto state = vtrpon::networkVideoState();
        QString text;
        const std::array<QString, 4> names{QStringLiteral("Composite"), QStringLiteral("Fill"), QStringLiteral("Key"), QStringLiteral("Alpha")};
        if (!output_->isChecked()) text = vtrpon::uiText("network_video.output_disabled");
        else for (unsigned i = 0; i < kinds_.size(); ++i) if (kinds_[i]->isChecked()) {
            if (!text.isEmpty()) text += QLatin1Char('\n');
            const auto &output = state.outputs[i];
            text += names[i] + QStringLiteral(": ") + (output.error.isEmpty()
                ? vtrpon::uiText(output.running ? "network_video.output_running" : "network_video.output_starting")
                : vtrpon::uiText("network_video.output_error") + QStringLiteral(" ") + output.error);
            if (!output.name.isEmpty()) text += QStringLiteral(" — ") + output.name;
        }
        if (text.isEmpty()) text = vtrpon::uiText("network_video.no_outputs");
        if (input_->isChecked()) text += QLatin1Char('\n') + vtrpon::uiText("network_video.sources_found").arg(state.sources.size());
        if (!state.error.isEmpty()) text += QLatin1Char('\n') + state.error;
        status_->setText(text);
    }
    void updateControls() {
        for (auto *kind : kinds_) kind->setEnabled(output_->isChecked());
        fillFormat_->setEnabled(output_->isChecked() && kinds_[1]->isChecked());
        fillLabel_->setEnabled(fillFormat_->isEnabled());
        alphaHelp_->setVisible(kinds_[1]->isChecked() || kinds_[2]->isChecked() || kinds_[3]->isChecked());
    }
    std::array<QCheckBox *, 4> kinds_{};
    QLabel *outputsLabel_, *fillLabel_, *alphaHelp_;
    QToolButton *details_;
    QWidget *detailsBody_;
    QComboBox *fillFormat_;
    QLabel *title_, *description_, *nameLabel_, *status_;
    QCheckBox *output_, *input_;
    QLineEdit *name_;
    vtrpon::AddonRuntimeStatusWidget *runtime_;
};
