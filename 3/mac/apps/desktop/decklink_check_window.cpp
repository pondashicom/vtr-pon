#include "decklink_check_window.h"
#include "decklink.h"
#include "language_text.h"
#include "background_io_queue.h"
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>
#include <QVBoxLayout>
#include <QEvent>
#include <array>

namespace vtrpon {
QJsonObject deckLinkCheckSnapshot() {
    const auto state = deckLinkState();
    const std::array<const char *, 5> availability{"addon_missing", "checking", "runtime_unavailable", "no_devices", "ready"};
    const auto settings = savedDeckLinkSettings();
    QJsonArray outputs;
    for (unsigned i=0;i<3;++i) outputs.append(QJsonObject{{"route", i==0?"composite":i==1?"fill":"key"},
        {"enabled", bool(settings.outputMask()&(1u<<i))}, {"running",state.outputs[i].running},
        {"errorPresent",!state.outputs[i].error.isEmpty()}, {"sentFrames",QString::number(state.outputs[i].sentFrames)},
        {"modeId",QString::number(settings.mode(i))}});
    // Deliberately omit raw backend errors, library/media paths and persistent
    // device IDs. Driver state is evidence, never an operator's success result.
    return {{"schemaVersion",1}, {"kind","vtrpon-decklink-operation-report"},
        {"createdAt",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"applicationVersion",QCoreApplication::applicationVersion()}, {"os",QSysInfo::productType()},
        {"osVersion",QSysInfo::productVersion()}, {"cpu",QSysInfo::currentCpuArchitecture()},
        {"hardwareVerification","unverified"}, {"availability",availability.at(size_t(state.availability))},
        {"channelCount",state.channels.size()}, {"inputEnabled",settings.inputEnabled},
        {"inputModeId",QString::number(settings.inputMode)}, {"pairSynchronized",state.pairSynchronized},
        {"errorPresent",!state.error.isEmpty()||!state.readbackError.isEmpty()}, {"outputs",outputs}};
}
namespace {
constexpr std::array<const char *,7> resultKeys{"input_video","input_audio","composite","fill_key","av_sync","reconnect","long_run"};
constexpr std::array<const char *,7> labelKeys{"decklink.check.input_video","decklink.check.input_audio","decklink.check.composite","decklink.check.fill_key","decklink.check.av_sync","decklink.check.reconnect","decklink.check.long_run"};
QString subject() { return uiText("decklink.check.mail_subject"); }
QString mailBody(const QJsonObject &report, const QString &path) {
    QStringList lines{uiText("decklink.check.mail_intro"), "VTR-PON: "+report["applicationVersion"].toString(),
        "OS: "+report["os"].toString()+" "+report["osVersion"].toString()+" / "+report["cpu"].toString()};
    const auto results=report["operatorResults"].toObject();
    for(unsigned i=0;i<resultKeys.size();++i) {
        const auto result=results[QLatin1String(resultKeys[i])].toString();
        lines << uiText(labelKeys[i])+": "+uiText(result=="worked"?"switcher.check.worked":result=="failed"?"switcher.check.failed":"switcher.check.not_tested");
    }
    lines << QString() << uiText("switcher.check.mail_attachment").arg(QFileInfo(path).fileName());
    return lines.join(QStringLiteral("\r\n"));
}
class CheckWindow final : public QWidget {
public:
    explicit CheckWindow(QWidget *parent) : QWidget(parent,Qt::Window) {
        setObjectName("deckLinkCheckWindow");setAttribute(Qt::WA_DeleteOnClose);resize(760,800);
        auto *layout=new QVBoxLayout(this);
        help_=new QLabel(this);help_->setObjectName("deckLinkCheckInstructions");help_->setWordWrap(true);layout->addWidget(help_);
        auto *form=new QFormLayout;
        const std::array<const char *,4> names{"deckLinkCheckDevice","deckLinkCheckRuntime","deckLinkCheckConnection","deckLinkCheckFormat"};
        for(unsigned i=0;i<fields_.size();++i) {
            fields_[i]=new QLineEdit(this);fields_[i]->setObjectName(names[i]);fields_[i]->setMaxLength(240);
            fieldLabels_[i]=new QLabel(this);fieldLabels_[i]->setBuddy(fields_[i]);form->addRow(fieldLabels_[i],fields_[i]);
            connect(fields_[i],&QLineEdit::textChanged,this,[this]{updatePreview();});
        }
        for(unsigned i=0;i<answers_.size();++i) {
            answers_[i]=new QComboBox(this);answers_[i]->setObjectName(QStringLiteral("deckLinkCheckResult%1").arg(i));
            answers_[i]->addItem(QString(),"not_tested");answers_[i]->addItem(QString(),"worked");answers_[i]->addItem(QString(),"failed");
            labels_[i]=new QLabel(this);labels_[i]->setBuddy(answers_[i]);form->addRow(labels_[i],answers_[i]);
            connect(answers_[i],&QComboBox::currentIndexChanged,this,[this]{updatePreview();});
        }
        layout->addLayout(form);
        comment_=new QPlainTextEdit(this);comment_->setObjectName("deckLinkCheckComment");comment_->setMaximumHeight(85);layout->addWidget(comment_);
        connect(comment_,&QPlainTextEdit::textChanged,this,[this]{updatePreview();});
        preview_=new QPlainTextEdit(this);preview_->setObjectName("deckLinkCheckPreview");preview_->setReadOnly(true);layout->addWidget(preview_,1);
        status_=new QLabel(this);status_->setObjectName("deckLinkCheckStatus");status_->setWordWrap(true);status_->setTextInteractionFlags(Qt::TextSelectableByMouse);layout->addWidget(status_);
        auto *row=new QHBoxLayout;
        refresh_=button(row,"deckLinkCheckRefresh",[this]{snapshot_=deckLinkCheckSnapshot();updatePreview();});
        save_=button(row,"deckLinkCheckSave",[this]{save(false);});
        email_=button(row,"deckLinkCheckEmail",[this]{save(true);});
        folder_=button(row,"deckLinkCheckFolder",[this]{if(!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path_).absolutePath())))status_->setText(uiText("switcher.check.folder_failed").arg(path_));});
        folder_->setEnabled(false);layout->addLayout(row);io_=new BackgroundIoQueue(this);
        snapshot_=deckLinkCheckSnapshot();retranslate();updatePreview();
    }
protected:
    void changeEvent(QEvent *event) override { QWidget::changeEvent(event);if(event->type()==QEvent::LanguageChange)retranslate(); }
private:
    template<class F> QPushButton *button(QHBoxLayout *layout,const char *name,F action) {
        auto *b=new QPushButton(this);b->setObjectName(name);layout->addWidget(b);connect(b,&QPushButton::clicked,this,action);return b;
    }
    void retranslate() {
        setWindowTitle(uiText("decklink.check.title"));help_->setText(uiText("decklink.trial.notice")+"\n"+uiText("decklink.check.instructions"));
        const std::array<const char *,4> ids{"decklink.check.device","decklink.check.runtime","decklink.check.connection","decklink.check.format"};
        for(unsigned i=0;i<fields_.size();++i)fieldLabels_[i]->setText(uiText(ids[i]));
        for(unsigned i=0;i<answers_.size();++i) {
            labels_[i]->setText(uiText(labelKeys[i]));answers_[i]->setItemText(0,uiText("switcher.check.not_tested"));
            answers_[i]->setItemText(1,uiText("switcher.check.worked"));answers_[i]->setItemText(2,uiText("switcher.check.failed"));
        }
        comment_->setPlaceholderText(uiText("switcher.check.comment"));preview_->setAccessibleName(uiText("decklink.check.preview"));
        refresh_->setText(uiText("decklink.check.refresh"));save_->setText(uiText("switcher.check.save"));
        email_->setText(uiText("switcher.check.email"));email_->setToolTip(uiText("decklink.check.email_help"));folder_->setText(uiText("switcher.check.folder"));
    }
    QJsonObject report() const {
        auto result=snapshot_;QJsonObject observations;
        for(unsigned i=0;i<answers_.size();++i)observations[QLatin1String(resultKeys[i])]=answers_[i]->currentData().toString();
        result["operatorResults"]=observations;
        const std::array<const char *,4> names{"deviceReportedByUser","runtimeReportedByUser","connectionReportedByUser","formatReportedByUser"};
        for(unsigned i=0;i<fields_.size();++i)result[QLatin1String(names[i])]=fields_[i]->text().left(240);
        result["operatorComment"]=comment_->toPlainText().left(4000);return result;
    }
    void updatePreview() { if(preview_)preview_->setPlainText(QString::fromUtf8(QJsonDocument(report()).toJson())); }
    void save(bool email) {
        if(busy_)return;
        const auto data=report();QString path,directory;
        if(email) {
            directory=qEnvironmentVariable("VTRPON_TEST_DECKLINK_REPORT_DIR");
            if(directory.isEmpty())directory=QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
            if(directory.isEmpty())directory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if(directory.isEmpty()){status_->setText(uiText("switcher.check.no_report_folder"));return;}
            directory=QDir(directory).filePath("VTR-PON/DeckLinkReports");
            path=QDir(directory).filePath("vtr-pon-decklink-"+QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss-zzz")+"-"+QUuid::createUuid().toString(QUuid::Id128).left(8)+".json");
        } else {
            path=QFileDialog::getSaveFileName(this,uiText("switcher.check.save"),"vtr-pon-decklink.json","JSON (*.json)");if(path.isEmpty())return;
        }
        busy_=true;email_->setEnabled(false);save_->setEnabled(false);status_->setText(uiText("switcher.check.preparing_email"));
        const auto bytes=QJsonDocument(data).toJson();auto error=std::make_shared<QString>();
        io_->enqueue([directory,path,bytes,error]{
            if(!directory.isEmpty()&&!QDir().mkpath(directory)){*error=directory;return;}
            QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())*error=file.errorString();
        },[this,data,path,email,error](const QString &workerError){
            busy_=false;email_->setEnabled(true);save_->setEnabled(true);
            const auto detail=workerError.isEmpty()?*error:workerError;
            if(!detail.isEmpty()){status_->setText(uiText("switcher.check.save_failed").arg(detail));return;}
            path_=path;folder_->setEnabled(true);
            if(email) {
                const bool opened=QDesktopServices::openUrl(deckLinkCheckReportMailUrl(data,path));
                status_->setText(uiText(opened?"switcher.check.email_manual_attachment":"switcher.check.email_failed").arg(path));
            } else status_->setText(uiText("switcher.check.saved").arg(path));
        });
    }
    QJsonObject snapshot_;QString path_;bool busy_=false;
    BackgroundIoQueue *io_{};QLabel *help_{},*status_{};
    std::array<QLineEdit *,4> fields_{};std::array<QLabel *,4> fieldLabels_{};
    std::array<QComboBox *,7> answers_{};std::array<QLabel *,7> labels_{};
    QPlainTextEdit *comment_{},*preview_{};
    QPushButton *refresh_{},*save_{},*email_{},*folder_{};
};
}
QUrl deckLinkCheckReportMailUrl(const QJsonObject &report,const QString &fileName) {
    return QUrl::fromEncoded("mailto:info@pondashi.com?subject="+QUrl::toPercentEncoding(subject())+"&body="+QUrl::toPercentEncoding(mailBody(report,fileName)));
}
void showDeckLinkCheckWindow(QWidget *parent) {
    auto *window=parent?parent->findChild<QWidget *>("deckLinkCheckWindow"):nullptr;
    if(!window)window=new CheckWindow(parent);window->show();window->raise();window->activateWindow();
}
}
