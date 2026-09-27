#pragma once
#include "language_text.h"
#include "settings_style.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace vtrpon {
enum class AddonRuntimeKind { Atem, NetworkVideo };

// Probes run in a disposable process: a missing/broken vendor runtime must not
// block the Controller or touch the active add-on's COM/media session.
class AddonRuntimeStatusWidget final : public QWidget {
public:
    AddonRuntimeStatusWidget(AddonRuntimeKind kind, const QString &addonFile,
                            QWidget *parent = nullptr, const QString &probeProgram = {})
        : QWidget(parent), kind_(kind), addonFile_(addonFile),
          probeProgram_(probeProgram.isEmpty()
              ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("vtr-pon-addon-runtime-probe.exe"))
              : probeProgram) {
        setObjectName(kind == AddonRuntimeKind::Atem
            ? QStringLiteral("atemRuntimeStatus") : QStringLiteral("networkVideoRuntimeStatus"));
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
        layout->setAlignment(Qt::AlignTop);
        message_ = new QLabel(this); message_->setObjectName(QStringLiteral("addonRuntimeMessage"));
        setSettingsRole(message_, "status");
        message_->setWordWrap(true); message_->setTextFormat(Qt::PlainText);
        layout->addWidget(message_);
        details_ = new QLabel(this); details_->setObjectName(QStringLiteral("addonRuntimeDetails"));
        setSettingsRole(details_, "description");
        details_->setWordWrap(true); details_->setTextFormat(Qt::PlainText);
        details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(details_);
        auto *buttons = new QHBoxLayout;
        download_ = new QPushButton(this); download_->setObjectName(QStringLiteral("addonRuntimeDownload"));
        retry_ = new QPushButton(this); retry_->setObjectName(QStringLiteral("addonRuntimeRetry"));
        buttons->addWidget(download_); buttons->addWidget(retry_); buttons->addStretch();
        layout->addLayout(buttons);
        timeout_.setSingleShot(true); timeout_.setInterval(5000);
        connect(&timeout_, &QTimer::timeout, this, [this] {
            if (process_) { setResult(QStringLiteral("timeout")); process_->kill(); }
        });
        connect(retry_, &QPushButton::clicked, this, [this] { check(); });
        connect(download_, &QPushButton::clicked, this, [this] {
            if (!QDesktopServices::openUrl(downloadUrl())) {
                detail_ = uiText("addons.runtime.browser_failed") + QLatin1Char('\n') + downloadUrl().toString(); retranslate();
            }
        });
        retranslate();
        const bool installed = QFileInfo(addonFile_).isFile();
        setVisible(installed);
        if (installed) QTimer::singleShot(0, this, [this] { check(); });
    }
    ~AddonRuntimeStatusWidget() override {
        // QProcess's destructor waits for a running child. Transfer ownership
        // before killing so closing Settings never waits on vendor teardown.
        if (process_) {
            disconnect(process_, nullptr, this, nullptr);
            process_->setParent(QCoreApplication::instance());
            connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process_, &QObject::deleteLater);
            connect(process_, &QProcess::started, process_, &QProcess::kill);
            process_->kill();
            if (process_->state() == QProcess::NotRunning) process_->deleteLater();
        }
    }
    QUrl downloadUrl() const {
        return QUrl(kind_ == AddonRuntimeKind::Atem
            ? QStringLiteral("https://www.blackmagicdesign.com/jp/support/family/atem-live-production-switchers")
            : QStringLiteral("https://ndi.link/NDIRedistV6"));
    }
    void retranslate() {
        const auto software = kind_ == AddonRuntimeKind::Atem
            ? QStringLiteral("ATEM Switchers Software") : QStringLiteral("NDI Runtime 6 (x64)");
        const auto key = QByteArray("addons.runtime.") + state_.toLatin1();
        message_->setText(uiText(key.constData()).arg(software));
        details_->setText(detail_); details_->setVisible(!detail_.isEmpty());
        download_->setText(uiText("addons.runtime.download"));
        download_->setAccessibleDescription(software);
        download_->setToolTip(downloadUrl().toString());
        download_->setVisible(state_ == QLatin1StringView("missing") || state_ == QLatin1StringView("incompatible"));
        retry_->setText(uiText("addons.runtime.retry"));
        retry_->setEnabled(!process_);
        setProperty("runtimeState", state_);
    }
    void check() {
        if (process_) return;
        if (!QFileInfo(addonFile_).isFile()) { setResult(QStringLiteral("addon_missing")); hide(); return; }
        show();
        if (!QFileInfo(probeProgram_).isFile()) { setResult(QStringLiteral("check_unavailable")); return; }
        process_ = new QProcess(this);
        process_->setProgram(probeProgram_);
        process_->setArguments({QStringLiteral("--kind"), kind_ == AddonRuntimeKind::Atem
            ? QStringLiteral("atem") : QStringLiteral("network-video"), QStringLiteral("--addon-file"), addonFile_});
#ifdef _WIN32
        process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= 0x08000000; // CREATE_NO_WINDOW
        });
#endif
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) { setResult(QStringLiteral("check_unavailable")); finish(); }
        });
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            output_ += process_->readAllStandardOutput();
            if (output_.size() > 16384) { setResult(QStringLiteral("error")); process_->kill(); }
        });
        connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus exit) {
                if (state_ == QLatin1StringView("checking")) {
                    output_ += process_->readAllStandardOutput();
                    const auto object = QJsonDocument::fromJson(output_).object();
                    const auto status = object.value(QStringLiteral("status")).toString();
                    const QStringList allowed{QStringLiteral("ready"), QStringLiteral("missing"), QStringLiteral("incompatible"),
                        QStringLiteral("addon_missing"), QStringLiteral("addon_error"), QStringLiteral("check_unavailable"), QStringLiteral("error")};
                    if (exit == QProcess::NormalExit && code == 0 && output_.size() <= 16384 && allowed.contains(status))
                        setResult(status, object.value(QStringLiteral("detail")).toString().left(2048));
                    else setResult(QStringLiteral("error"));
                }
                finish();
            });
        output_.clear(); setResult(QStringLiteral("checking")); timeout_.start(); process_->start();
    }
private:
    void setResult(const QString &state, const QString &detail = {}) { state_ = state; detail_ = detail; retranslate(); }
    void finish() {
        timeout_.stop();
        if (process_) { process_->deleteLater(); process_ = nullptr; }
        retranslate();
    }
    AddonRuntimeKind kind_;
    QString addonFile_, probeProgram_, state_ = QStringLiteral("checking"), detail_;
    QLabel *message_, *details_;
    QPushButton *download_, *retry_;
    QProcess *process_ = nullptr;
    QTimer timeout_;
    QByteArray output_;
};
}
