#include "pdf_document_backend.h"
#include "mac_media_runtime.h"
#include "pdf_boundary_fixtures.h"
#include "pdf_import_service.h"
#include "pdf_source_session_manager.h"
#include "deck_source_session.h"
#include "program_gpu_compositor.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QPageLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <gst/gst.h>
#include <functional>

int runMacRuntimeSelectionChecks();

namespace {
bool check(bool ok, const char *label) {
    qInfo() << (ok ? "PASS" : "FAIL") << label;
    return ok;
}
bool waitFor(const std::function<bool()> &condition, int timeout = 6000) {
    QElapsedTimer timer; timer.start();
    do {
        QApplication::processEvents();
        if (condition()) return true;
        QThread::msleep(10);
    } while (timer.elapsed() < timeout);
    return false;
}
bool fixture(const QString &path) {
    QPdfWriter writer(path);
    writer.setResolution(72);
    writer.setPageLayout(QPageLayout(QPageSize(QSizeF(400,400), QPageSize::Point,
        QStringLiteral("Square"), QPageSize::ExactMatch), QPageLayout::Portrait, QMarginsF()));
    QPainter painter(&writer);
    if (!painter.isActive()) return false;
    int page = 0;
    for (const auto &color : {QColor(230,32,32), QColor(32,80,230), QColor(32,200,70)}) {
        if (page++ && !writer.newPage()) return false;
        painter.fillRect(QRect(0,0,writer.width(),writer.height()),Qt::white);
        painter.fillRect(QRect(writer.width()/4,writer.height()/4,writer.width()/2,writer.height()/2),color);
        painter.setPen(Qt::black);
        painter.drawText(QRect(10,10,writer.width()-20,60), Qt::AlignCenter,
            QStringLiteral("VTR-PON Mac PDF / PAGE %1 / 日本語").arg(page));
    }
    return painter.end();
}
bool near(const QImage &image, QPoint point, QColor color) {
    if (image.isNull() || !image.rect().contains(point)) return false;
    const auto got = image.pixelColor(point);
    return std::abs(got.red()-color.red())<5 && std::abs(got.green()-color.green())<5 &&
        std::abs(got.blue()-color.blue())<5 && got.alpha()==255;
}
bool backendChecks(const QString &path, const QString &directory) {
    PdfDocumentBackend pdf;
    bool ok = check(pdf.load(path) && pdf.pageCount()==3, "native Qt PDF opens 3-page Japanese path");
    auto image=pdf.renderPage(0);
    ok &= check(near(image,{960,540},{230,32,32}) && near(image,{10,540},Qt::black) &&
        near(image,{450,540},Qt::white), "page colors, opaque white paper and black aspect borders");
    ok &= check(pdf.renderPage(3).isNull() && pdf.error()==PdfDocumentError::InvalidPage, "invalid page rejected");
    ok &= check(pdf.renderPage(0,{3840,2160}).isNull(), "oversized PDF canvas rejected");
    QString error;
    auto thumbnail=generatePdfThumbnail(path,{160,90},&error);
    ok &= check(!thumbnail.isNull() && error.isEmpty(), "PDF thumbnail generated");
    PdfAddSettings settings;
    const auto imported=PdfImportService::importFile(path,settings,1,nullptr,nullptr,{},directory);
    ok &= check(imported.succeeded && imported.items.size()==1 && imported.generatedDirectory.isEmpty() &&
        imported.items.first().sourceType==PlaylistSourceType::Pdf, "native import keeps original PDF and creates no conversion files");
    settings.method=static_cast<PdfAddMethod>(-1);
    ok &= check(!PdfImportService::importFile(path,settings,1).succeeded, "unknown PDF add method rejected at service entry");
    std::atomic_bool canceled{true}; settings.method=PdfAddMethod::Native;
    ok &= check(PdfImportService::importFile(path,settings,1,nullptr,&canceled).canceled, "canceled native import does not register item");
    const auto missing=directory+QStringLiteral("/missing.pdf");
    ok &= check(!pdf.load(missing) && pdf.error()==PdfDocumentError::MissingFile, "missing PDF rejected");
    QFile corrupt(directory+QStringLiteral("/corrupt.pdf"));
    if (corrupt.open(QIODevice::WriteOnly)) { corrupt.write("%PDF-1.7\ninvalid\n"); corrupt.close(); }
    ok &= check(!pdf.load(corrupt.fileName()) && pdf.error()==PdfDocumentError::InvalidFile, "corrupt PDF rejected");
    return ok;
}
bool displayChecks(const QString &path, const QString &directory) {
    QWidget output, monitor;
    output.resize(640,360); monitor.resize(640,360);
    output.show(); monitor.show(); QApplication::processEvents();
    ProgramGpuCompositor compositor(output.winId(),monitor.winId());
    PdfSourceSessionManager pdf(1920,1080);
    DeckSourceSession deck, other;
    bool ok=check(compositor.start() && deck.initialize(0,1920,1080) && other.initialize(1,1920,1080), "real Metal and two physical decks start");
    if (!ok) return false;
    deck.attachSourceManagers(nullptr,nullptr,&pdf,&compositor);
    other.attachSourceManagers(nullptr,nullptr,&pdf,&compositor);
    compositor.configureDeckInput(0,deck.pipeline(),deck.appSink());
    compositor.configureDeckInput(1,other.pipeline(),other.appSink());
    const auto ready=[&](DeckSourceSession &value) {
        return waitFor([&]{
            const auto result=value.refreshPreparation();
            QString error;
            if (value.takeSourceError(error)) qWarning()<<error;
            return result.state==DualDeckPlayer::CueStatus::Ready;
        });
    };
    deck.prepare({DualDeckPlayer::DeckSource::Type::Pdf,path});
    ok &= check(ready(deck) && deck.hasVideo() && !deck.hasAudio(), "PDF standby reaches Ready without an audio input");
    compositor.setDeckLayers(0,1,-1,0);
    const auto frameIs=[&](QColor color) {
        return waitFor([&]{auto capture=compositor.captureStillFrame(); return capture && near(capture(),{960,540},color);});
    };
    ok &= check(frameIs({230,32,32}), "PDF page 1 reaches shared final Metal frame");
    auto navigate=[&](bool next, PdfCommandResult expected) {
        PdfCommandResponse response;
        bool done=waitFor([&]{response=next?deck.nextPdfPage():deck.previousPdfPage();return response.result!=PdfCommandResult::Busy;});
        return done && response.result==expected;
    };
    ok &= check(navigate(false,PdfCommandResult::Boundary), "previous at first page remains bounded");
    ok &= check(navigate(true,PdfCommandResult::Completed) && frameIs({32,80,230}) &&
        deck.pdfNavigationState().currentPage==2, "asynchronous next-page command publishes page 2");
    monitor.hide();
    ok &= check(navigate(true,PdfCommandResult::Completed) && frameIs({32,200,70}), "hiding monitor does not stop page 3 or final output");
    ok &= check(navigate(true,PdfCommandResult::Boundary), "next at last page remains bounded");
    monitor.show();
    ok &= check(navigate(false,PdfCommandResult::Completed) && frameIs({32,80,230}), "previous-page command publishes page 2");
    // A failed standby on the other deck cannot disturb the current PDF.
    other.prepare({DualDeckPlayer::DeckSource::Type::Pdf,directory+QStringLiteral("/missing.pdf")});
    QString error;
    ok &= check(waitFor([&]{return other.takeSourceError(error);}) && !error.isEmpty() && frameIs({32,80,230}), "missing standby PDF preserves on-air frame and reports failure");
    ok &= check(other.clearForReplacement().succeeded, "failed standby ownership can be released");
    const auto overLimit = QDir(directory).filePath("display-1001.pdf");
    ok &= check(PdfBoundaryFixtures::writePdf(overLimit,1001), "over-limit standby fixture created");
    for (const auto &rejected : {PdfBoundaryFixtures::encryptedFixturePath(),overLimit}) {
        other.prepare({DualDeckPlayer::DeckSource::Type::Pdf,rejected});
        error.clear();
        ok &= check(waitFor([&]{return other.takeSourceError(error);}) && !error.isEmpty() &&
            frameIs({32,80,230}), "encrypted or over-limit standby preserves current on-air Metal frame");
        ok &= check(other.clearForReplacement().succeeded, "rejected PDF standby ownership releases");
    }
    other.prepare({DualDeckPlayer::DeckSource::Type::Pdf,path});
    ok &= check(ready(other), "PDF can prepare on second deck after failure");
    compositor.setDeckLayers(1,1,-1,0);
    ok &= check(deck.standbyAfterTake().succeeded && frameIs({230,32,32}), "TAKE detaches outgoing PDF before worker release");
    compositor.setDeckLayers(-1,0,-1,0);
    ok &= check(other.clearAfterOffAir().succeeded && frameIs(Qt::black), "OFF AIR clears final frame and releases input");
    // Cancel while the worker is still opening the PDF, then reuse that deck.
    deck.prepare({DualDeckPlayer::DeckSource::Type::Pdf,path});
    ok &= check(deck.clearForReplacement().succeeded, "preparation can be canceled while document worker opens PDF");
    deck.prepare({DualDeckPlayer::DeckSource::Type::Pdf,path});
    ok &= check(ready(deck), "canceled deck can prepare PDF again");
    compositor.setDeckLayers(0,1,-1,0);
    ok &= check(frameIs({230,32,32}), "reloaded PDF starts at page 1");
    compositor.setDeckLayers(-1,0,-1,0);
    ok &= check(deck.clearAfterOffAir().succeeded, "native PDF detaches before testing converted files");
    PdfAddSettings pngSettings; pngSettings.method=PdfAddMethod::PngSequence;
    const auto pages=PdfImportService::importFile(path,pngSettings,1,nullptr,nullptr,{},directory);
    ok &= check(pages.succeeded && pages.items.size()==3, "converted PNG fixture creates three pages");
    if (pages.items.size()==3) {
        deck.prepare({DualDeckPlayer::DeckSource::Type::File,pages.items.first().filePath});
        ok &= check(ready(deck), "generated PNG reaches Ready through the normal file deck");
        compositor.setDeckLayers(0,1,-1,0);
        ok &= check(frameIs({230,32,32}), "generated PNG reaches the shared Metal final frame");
        compositor.setDeckLayers(-1,0,-1,0);
        ok &= check(deck.clearAfterOffAir().succeeded, "generated PNG releases through normal OFF AIR");
    }
    auto videoSettings=pngSettings;videoSettings.method=PdfAddMethod::VideoConversion;
    videoSettings.pageDurationSeconds=0.4;videoSettings.crossDissolveEnabled=false;
    const auto video=PdfImportService::importFile(path,videoSettings,1,nullptr,nullptr,{},directory);
    ok &= check(video.succeeded && video.items.size()==1, "converted MP4 fixture creates one movie");
    if (video.items.size()==1) {
        other.prepare({DualDeckPlayer::DeckSource::Type::File,video.items.first().filePath});
        ok &= check(ready(other), "generated MP4 reaches Ready through the normal file deck");
        compositor.setDeckLayers(1,1,-1,0);
        ok &= check(waitFor([&]{auto capture=compositor.captureStillFrame();if (!capture) return false;
            const auto image=capture();return !image.isNull() && image.pixelColor(960,540).red()>180 && image.pixelColor(960,540).blue()<70;
        }), "generated MP4 first page reaches shared Metal frame");
        ok &= check(other.setFileState(GST_STATE_PLAYING), "generated MP4 starts normal file playback");
        // Lossy H.264 may shift channel values slightly; compare broad page colors.
        const auto movieColor=[&](int channel) {
            return waitFor([&]{auto capture=compositor.captureStillFrame();if (!capture) return false;
                const auto image=capture();if (image.isNull()) return false;
                const auto color=image.pixelColor(960,540);
                return channel==2 ? color.blue()>180 && color.red()<70 : color.green()>150 && color.red()<70;
            });
        };
        ok &= check(movieColor(2) && movieColor(1), "generated MP4 advances through pages 2 and 3 on the normal file deck");
        compositor.setDeckLayers(-1,0,-1,0);
        ok &= check(other.clearAfterOffAir().succeeded, "generated MP4 releases through normal OFF AIR");
    }
    compositor.prepareShutdown();
    pdf.requestShutdown();
    ok &= check(waitFor([&]{return pdf.shutdownComplete();}), "PDF worker shutdown confirms producer stop");
    ok &= check(deck.stopPipelinesForShutdown() && other.stopPipelinesForShutdown() && compositor.stop(), "deck and compositor shutdown confirms stop");
    deck.releaseResourcesAfterConfirmedShutdown(); other.releaseResourcesAfterConfirmedShutdown();
    return ok;
}
}
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    if (!configureMacBundledMediaRuntime()) return 2;
    gst_init(&argc,&argv);
    if (app.arguments().size()==3 && app.arguments().at(1)==QStringLiteral("--create-fixture"))
        return fixture(app.arguments().at(2)) ? 0 : 1;
    if (app.arguments().contains(QStringLiteral("--runtime-validation"))) return runMacRuntimeSelectionChecks();
    QTemporaryDir directory;
    app.setProperty("vtrpon.settingsFile",directory.filePath("settings.ini"));
    const auto path=directory.filePath(QStringLiteral("日本語 PDF 確認.pdf"));
    if (!fixture(path)) return 1;
    bool ok=backendChecks(path,directory.path());
    if (!app.arguments().contains(QStringLiteral("--backend"))) ok=displayChecks(path,directory.path()) && ok;
    return ok ? 0 : 1;
}
