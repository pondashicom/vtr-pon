#include "decklink_check_window.h"
#include "decklink_settings_page.h"
#include "controller_settings.h"
#include <QApplication>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTranslator>
#include <QUrlQuery>
#include <cstdio>
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(false)
template<class F> bool until(F f) { QElapsedTimer t;t.start();while(t.elapsed()<5000){QApplication::processEvents();if(f())return true;QThread::msleep(5);}return f(); }
class Capture final : public QObject {
    Q_OBJECT
public: QList<QUrl> urls;
public slots: void handle(const QUrl &url){urls.append(url);}
};
int main(int argc,char **argv) {
    QApplication app(argc,argv);app.setApplicationVersion("3.0.0-beta.1");
    QTemporaryDir temporary;CHECK(temporary.isValid());app.setProperty("vtrpon.settingsFile",temporary.filePath("user.ini"));
    qputenv("VTRPON_TEST_DECKLINK_REPORT_DIR",temporary.path().toUtf8());
    QTranslator translator;const bool japanese=app.arguments().contains("--japanese");
    CHECK(translator.load(QCoreApplication::applicationDirPath()+ (japanese?"/vtrpon_ja.qm":"/vtrpon_en.qm")));app.installTranslator(&translator);
    auto settings=vtrpon::savedDeckLinkSettings();settings.outputChannel="decklink://987654321/0/1/0";vtrpon::saveDeckLinkSettings(settings);
    auto snapshot=vtrpon::deckLinkCheckSnapshot();const auto bytes=QJsonDocument(snapshot).toJson();
    CHECK(!bytes.contains("987654321")&&!bytes.contains("decklink://")&&!bytes.contains(temporary.path().toUtf8()));
    CHECK(snapshot["hardwareVerification"]=="unverified"&&!snapshot.contains("operatorResults"));
    Capture capture;QDesktopServices::setUrlHandler("mailto",&capture,"handle");QDesktopServices::setUrlHandler("file",&capture,"handle");
    DeckLinkSettingsPage page;page.resize(820,800);page.show();
    auto *notice=page.findChild<QLabel *>("deckLinkTrialNotice");CHECK(notice&&notice->text().contains(japanese?"協力者募集中":"volunteers wanted"));
    auto *button=page.findChild<QPushButton *>("deckLinkCheckButton");CHECK(button);button->click();
    auto *window=page.findChild<QWidget *>("deckLinkCheckWindow");CHECK(window);
    auto *preview=window->findChild<QPlainTextEdit *>("deckLinkCheckPreview");CHECK(preview);
    auto report=QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object();
    for(const auto &v:report["operatorResults"].toObject())CHECK(v=="not_tested");
    window->findChild<QLineEdit *>("deckLinkCheckDevice")->setText("Test Device");
    window->findChild<QPlainTextEdit *>("deckLinkCheckComment")->setPlainText("Manual note private marker /Users/test/secret.mov");
    window->findChild<QComboBox *>("deckLinkCheckResult0")->setCurrentIndex(1);
    const auto url=vtrpon::deckLinkCheckReportMailUrl(QJsonDocument::fromJson(preview->toPlainText().toUtf8()).object(),temporary.filePath("report.json"));
    CHECK(url.scheme()=="mailto"&&url.path()=="info@pondashi.com");
    CHECK(!url.toEncoded().contains("secret.mov")&&!QUrlQuery(url).queryItemValue("body").contains(temporary.path()));
    const auto shots=qEnvironmentVariable("VTRPON_TEST_DECKLINK_SCREENSHOTS");
    if(!shots.isEmpty()){QDir().mkpath(shots);QApplication::processEvents();CHECK(page.grab().save(shots+(japanese?"/decklink-settings-ja.png":"/decklink-settings-en.png")));CHECK(window->grab().save(shots+(japanese?"/decklink-report-ja.png":"/decklink-report-en.png")));}
    auto *email=window->findChild<QPushButton *>("deckLinkCheckEmail");CHECK(email);email->click();email->click();
    CHECK(until([&]{return capture.urls.size()==1;}));CHECK(capture.urls.front().scheme()=="mailto");
    QDir directory(temporary.filePath("VTR-PON/DeckLinkReports"));const auto files=directory.entryList({"*.json"},QDir::Files);CHECK(files.size()==1);
    QFile file(directory.filePath(files.front()));CHECK(file.open(QIODevice::ReadOnly));report=QJsonDocument::fromJson(file.readAll()).object();
    CHECK(report["operatorResults"].toObject()["input_video"]=="worked");CHECK(report["operatorComment"].toString().contains("Manual note"));
    CHECK(!report.contains("outputChannel")&&report["hardwareVerification"]=="unverified");
    window->findChild<QPushButton *>("deckLinkCheckFolder")->click();CHECK(capture.urls.size()==2&&capture.urls.back().scheme()=="file");
    qputenv("VTRPON_TEST_DECKLINK_REPORT_DIR","/dev/null");email->click();CHECK(until([&]{return email->isEnabled();}));CHECK(capture.urls.size()==2);
    CHECK(!window->findChild<QLabel *>("deckLinkCheckStatus")->text().isEmpty());
    qputenv("VTRPON_TEST_DECKLINK_REPORT_DIR",temporary.path().toUtf8());email->click();QPointer<QWidget> destroyed=window;window->close();
    CHECK(until([&]{return destroyed.isNull();}));CHECK(capture.urls.size()==2);
    QDesktopServices::unsetUrlHandler("mailto");QDesktopServices::unsetUrlHandler("file");
    std::puts("DeckLink unverified UI, reviewed/bounded JSON, mail draft and manual attachment, duplicate/error/close isolation: PASS");return 0;
}
#include "decklink_report_test.moc"
