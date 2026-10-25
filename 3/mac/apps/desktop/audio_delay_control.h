#pragma once
#include "controller_scaling.h"
#include "language_text.h"
#include <QEvent>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace vtrpon {
class AudioDelayControl final : public QWidget {
public:
    AudioDelayControl(QWidget *parent, const QString &name) : QWidget(parent) {
        setObjectName(name + QStringLiteral("Control"));
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(scalePx(1));
        spin_ = new QSpinBox(this);
        spin_->setObjectName(name);
        spin_->setProperty("audioDelayControl", true);
        spin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin_->setRange(0, 6);
        spin_->setSingleStep(1);
        spin_->setKeyboardTracking(false);
        spin_->setSuffix(QStringLiteral(" F"));
        spin_->setSpecialValueText(QStringLiteral("DELAY"));
        spin_->setAlignment(Qt::AlignCenter);
        // Match the manual fade duration field and its separate arrow column.
        spin_->setFixedSize(scaleSize(41, 24));
        row->addWidget(spin_);

        auto *column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        const auto createStepButton = [&](Qt::ArrowType arrow, const QString &suffix) {
            auto *button = new QToolButton(this);
            button->setObjectName(name + suffix);
            button->setProperty("fadeDurationStepButton", true);
            button->setArrowType(arrow);
            button->setAutoRepeat(true);
            button->setAutoRepeatDelay(400);
            button->setAutoRepeatInterval(80);
            button->setFixedSize(scaleSize(18, 12));
            button->setFocusPolicy(Qt::StrongFocus);
            column->addWidget(button);
            return button;
        };
        increase_ = createStepButton(Qt::UpArrow, QStringLiteral("IncreaseButton"));
        decrease_ = createStepButton(Qt::DownArrow, QStringLiteral("DecreaseButton"));
        row->addLayout(column);
        setFixedSize(spin_->width() + row->spacing() + increase_->width(), scalePx(24));
        setFocusProxy(spin_);
        connect(increase_, &QToolButton::clicked, spin_, &QAbstractSpinBox::stepUp);
        connect(decrease_, &QToolButton::clicked, spin_, &QAbstractSpinBox::stepDown);
        spin_->installEventFilter(this);
        updateStepButtons();
    }

    QSpinBox *spinBox() const { return spin_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == spin_ && event->type() == QEvent::EnabledChange)
            updateStepButtons();
        return QWidget::eventFilter(watched, event);
    }

private:
    void updateStepButtons() {
        increase_->setEnabled(spin_->isEnabled());
        decrease_->setEnabled(spin_->isEnabled());
    }
    QSpinBox *spin_ = nullptr;
    QToolButton *increase_ = nullptr;
    QToolButton *decrease_ = nullptr;
};

inline void retranslateAudioDelayControl(QSpinBox *spin, const char *title) {
    spin->setAccessibleName(uiText(title));
    spin->setToolTip(uiText(title) + QStringLiteral("\n") + uiText("audio.delay.tooltip"));
    for (bool up : {true, false}) {
        auto *button = spin->parentWidget()->findChild<QToolButton *>(spin->objectName()
            + (up ? QStringLiteral("IncreaseButton") : QStringLiteral("DecreaseButton")));
        const QString text = uiText(up ? "operator_timer.increase"
                                      : "operator_timer.decrease").arg(uiText(title));
        button->setAccessibleName(text);
        button->setToolTip(text + QStringLiteral("\n") + uiText("audio.delay.tooltip"));
    }
}
inline AudioDelayControl *createAudioDelayControl(QWidget *parent, const QString &name, const char *title) {
    auto *control = new AudioDelayControl(parent, name);
    retranslateAudioDelayControl(control->spinBox(), title);
    return control;
}
}
