#include "vst_editor.h"
#include "language_text.h"
#include "controller_scaling.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>

namespace vtrpon {
namespace {
QString channel(VstSlot slot) {
    switch (slot) {
    case VstSlot::Edit: return QStringLiteral("EDIT");
    case VstSlot::Item: return QStringLiteral("ITEM");
    default: return QStringLiteral("MASTER");
    }
}
class VstWindow final : public QDialog {
public:
    VstWindow(QWidget *parent, VstSlot slot) : QDialog(parent, Qt::Window), slot_(slot) {
        setObjectName(QStringLiteral("vst%1Window").arg(channel(slot)));
        setModal(false);
        auto *layout = new QVBoxLayout(this);
        label_ = new QLabel(this);
        plugins_ = new QComboBox(this);
        plugins_->setObjectName(QStringLiteral("vstPluginComboBox"));
        plugins_->setMinimumWidth(scalePx(360));
        plugins_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        label_->setBuddy(plugins_);
        apply_ = new QPushButton(this); apply_->setObjectName(QStringLiteral("vstApply"));
        auto *selection = new QHBoxLayout;
        selection->addWidget(plugins_, 1); selection->addWidget(apply_);
        layout->addWidget(label_); layout->addLayout(selection);
        current_ = new QLabel(this); current_->setObjectName(QStringLiteral("vstCurrent"));
        current_->setWordWrap(true); layout->addWidget(current_);
        auto *buttons = new QHBoxLayout;
        bypass_ = new QCheckBox(this); bypass_->setObjectName(QStringLiteral("vstBypass"));
        open_ = new QPushButton(QStringLiteral("OPEN"), this); open_->setObjectName(QStringLiteral("vstOpen"));
        scan_ = new QPushButton(this);
        scan_->setObjectName(QStringLiteral("vstScanButton"));
        buttons->addWidget(bypass_); buttons->addStretch(); buttons->addWidget(scan_); buttons->addWidget(open_);
        layout->addLayout(buttons);
        status_ = new QLabel(this); status_->setWordWrap(true); layout->addWidget(status_);
        scanStatus_ = new QLabel(this); scanStatus_->setObjectName(QStringLiteral("vstScanStatus"));
        scanStatus_->setWordWrap(true); layout->addWidget(scanStatus_);
        hint_ = new QLabel(this); hint_->setWordWrap(true); layout->addWidget(hint_);
        auto &service = VstService::instance();
        connect(&service, &VstService::changed, this, [this] { refresh(); });
        connect(&service, &VstService::catalogChanged, this, [this] { catalog(); });
        connect(&service, &VstService::scanProgressChanged, this, [this] { refresh(); });
        connect(scan_, &QPushButton::clicked, &service, [&service] {
            if (service.scanning()) service.cancelScan();
            else service.scan();
        });
        connect(bypass_, &QCheckBox::toggled, this, [this](bool value) { VstService::instance().bypass(slot_, value); });
        connect(plugins_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
        connect(apply_, &QPushButton::clicked, this, [this] {
            const auto item = plugins_->currentData().toJsonObject();
            VstService::instance().select(slot_, item.value("path").toString(), item.value("uid").toString());
        });
        connect(open_, &QPushButton::clicked, this, [this] {
            const auto position = frameGeometry().topLeft();
            VstService::instance().openEditor(slot_, quintptr(parentWidget()->window()->winId()), position.x(), position.y());
        });
        catalog();
        if (!service.catalogRequested()) service.scan();
    }
protected:
    void changeEvent(QEvent *event) override {
        QDialog::changeEvent(event);
        if (event->type() == QEvent::LanguageChange) catalog();
    }
private:
    void catalog() {
        const auto &service = VstService::instance();
        // Scanning and language changes must not discard an unapplied choice.
        const auto draft = initialized_ ? plugins_->currentData().toJsonObject()
            : QJsonObject{{"path", service.selectedPath(slot_)}, {"uid", service.selectedUid(slot_)}};
        const auto draftLabel = initialized_ ? plugins_->currentText() : QString();
        initialized_ = true;
        const QSignalBlocker blocker(plugins_);
        plugins_->clear(); plugins_->addItem(uiText("vst.none"));
        int selected = 0;
        for (const auto &value : service.plugins()) {
            const auto item = value.toObject();
            plugins_->addItem(item.value("name").toString() + QStringLiteral(" — ") + item.value("vendor").toString(), item);
            const auto scannedPath = QDir::cleanPath(QDir::fromNativeSeparators(item.value("path").toString()));
            const auto selectedPath = QDir::cleanPath(QDir::fromNativeSeparators(draft.value("path").toString()));
            if (scannedPath.compare(selectedPath, Qt::CaseInsensitive) == 0 &&
                item.value("uid").toString().compare(draft.value("uid").toString(), Qt::CaseInsensitive) == 0) selected = plugins_->count() - 1;
        }
        if (!selected && !draft.value("path").toString().isEmpty()) {
            plugins_->addItem(draftLabel.isEmpty() ? draft.value("path").toString()
                + QStringLiteral(" / ") + draft.value("uid").toString() : draftLabel, draft);
            selected = plugins_->count() - 1;
        }
        plugins_->setCurrentIndex(selected); refresh();
    }
    void refresh() {
        const auto &service = VstService::instance();
        setWindowTitle(channel(slot_) + QStringLiteral(" — VST3"));
        label_->setText(uiText("vst.plugin"));
        apply_->setText(uiText(service.applying(slot_) ? "vst.applying_button" : "vst.apply"));
        QString currentName = service.selectedPath(slot_).isEmpty() ? uiText("vst.none")
            : QFileInfo(service.selectedPath(slot_)).fileName() + QStringLiteral(" / ") + service.selectedUid(slot_);
        for (const auto &value : service.plugins()) {
            const auto item = value.toObject();
            if (QDir::fromNativeSeparators(item.value("path").toString()).compare(
                    QDir::fromNativeSeparators(service.selectedPath(slot_)), Qt::CaseInsensitive) == 0
                && item.value("uid").toString().compare(service.selectedUid(slot_), Qt::CaseInsensitive) == 0) {
                currentName = item.value("name").toString(); break;
            }
        }
        current_->setText(uiText("vst.current").arg(currentName));
        scan_->setText(uiText(service.scanning() ? "vst.scan_cancel" : "vst.rescan"));
        const auto progress = service.scanProgress();
        if (service.scanning()) {
            scanStatus_->setText(progress.discovering ? uiText("vst.scan_discovering") :
                uiText("vst.scan_progress").arg(std::min(progress.checked + 1, progress.total))
                    .arg(progress.total).arg(QFileInfo(progress.currentPath).fileName()));
        } else if (progress.canceled) {
            scanStatus_->setText(uiText("vst.scan_canceled"));
        } else if (!progress.error.isEmpty()) {
            scanStatus_->setText(uiText("vst.scan_failed").arg(progress.error));
        } else if (service.catalogRequested()) {
            scanStatus_->setText(uiText("vst.scan_complete").arg(service.plugins().size()).arg(progress.skipped));
        } else scanStatus_->clear();
        bypass_->setText(uiText("vst.bypass"));
        const QSignalBlocker blocker(bypass_); bypass_->setChecked(service.bypassed(slot_));
        auto status = uiText(service.statusKey(slot_).toUtf8().constData());
        if (!service.detail(slot_).isEmpty()) status += QStringLiteral("\n") + service.detail(slot_);
        if (service.applying(slot_)) status += QStringLiteral("\n") + uiText("vst.applying");
        else if (!service.applyError(slot_).isEmpty())
            status += QStringLiteral("\n") + uiText("vst.apply_failed").arg(service.applyError(slot_));
        status_->setText(status);
        hint_->setText(uiText("vst.hint").arg(service.latency(slot_)));
        open_->setEnabled(vstModeEnabled() && (service.active(slot_) || service.statusKey(slot_) == QLatin1StringView("vst.bypassed")));
        const auto draft = plugins_->currentData().toJsonObject();
        const bool different = QDir::fromNativeSeparators(draft.value("path").toString()).compare(
                QDir::fromNativeSeparators(service.selectedPath(slot_)), Qt::CaseInsensitive) != 0
            || draft.value("uid").toString().compare(service.selectedUid(slot_), Qt::CaseInsensitive) != 0;
        apply_->setEnabled(vstModeEnabled() && !service.applying(slot_)
            && (different || (!draft.value("path").toString().isEmpty()
                && !service.active(slot_) && !service.bypassed(slot_))));
        plugins_->setEnabled(vstModeEnabled() && !service.applying(slot_)); bypass_->setEnabled(vstModeEnabled());
        layout()->activate();
        adjustSize();
        if (!vstModeEnabled()) hide();
    }
    VstSlot slot_;
    bool initialized_ = false;
    QComboBox *plugins_;
    QCheckBox *bypass_;
    QPushButton *open_, *scan_, *apply_;
    QLabel *label_, *status_, *scanStatus_, *hint_, *current_;
};
class Binding final : public QObject {
public:
    Binding(QPushButton *button, VstSlot slot) : QObject(button), button(button), slot(slot) {
        button->setProperty("vstSlot", int(slot));
        button->setProperty("vstBinding", QVariant::fromValue(static_cast<QObject *>(this)));
        connect(&VstService::instance(), &VstService::changed, this, [this] {
            updateVstButton(this->button);
            if (!vstModeEnabled() && window) window->hide();
        });
        button->installEventFilter(this);
        updateVstButton(button);
    }
    bool eventFilter(QObject *, QEvent *event) override {
        if (!refreshing && (event->type() == QEvent::LanguageChange || event->type() == QEvent::StyleChange)) {
            const QScopedValueRollback guard(refreshing, true);
            updateVstButton(button);
        }
        return false;
    }
    bool refreshing = false;
    QPushButton *button;
    VstSlot slot;
    QPointer<VstWindow> window;
};
}
void bindVstButton(QPushButton *button, VstSlot slot) { new Binding(button, slot); }
bool updateVstButton(QPushButton *button) {
    if (!button->property("vstSlot").isValid()) return false;
    const bool enabled = vstModeEnabled();
    const auto slot = VstSlot(button->property("vstSlot").toInt());
    const auto &service = VstService::instance();
    button->setText(enabled ? QStringLiteral("VST") : QStringLiteral("EQ"));
    const bool active = enabled && service.active(slot);
    const bool stateChanged = button->property("vstMode") != QVariant(enabled)
        || button->property("vstActive") != QVariant(active);
    button->setProperty("vstMode", enabled);
    button->setProperty("vstActive", active);
    // Hardware shares the fader buttons' complete QSS (including hover/focus).
    // Preserve the existing appearance of the other skins.
    const bool hardware = qApp->property("controllerSkinUsesHardwareEffects").toBool();
    const QString style = (!enabled || hardware) ? QString() : active
        ? QStringLiteral("QPushButton { background: #1b743b; color: white; border: 1px solid #5dc582; } QPushButton:focus { border: 1px solid #fff1a6; }")
        : QStringLiteral("QPushButton { background: #353939; color: #eeeeee; border: 1px solid #6c7474; } QPushButton:disabled { color: #888888; } QPushButton:focus { border: 1px solid #fff1a6; }");
    if (button->styleSheet() != style) button->setStyleSheet(style);
    else if (stateChanged) {
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
    if (enabled) {
        button->setToolTip(channel(slot) + QStringLiteral(" VST3 / ") + uiText(service.statusKey(slot).toUtf8().constData()));
        button->setAccessibleName(button->toolTip());
    }
    return enabled;
}
bool showVstEditor(QPushButton *button, VstSlot slot) {
    if (!vstModeEnabled()) return false;
    auto *binding = static_cast<Binding *>(button->property("vstBinding").value<QObject *>());
    if (!binding) return false;
    VstService::instance().activate(slot);
    if (!binding->window) binding->window = new VstWindow(button->window(), slot);
    binding->window->showNormal(); binding->window->raise(); binding->window->activateWindow();
    return true;
}
} // namespace vtrpon
