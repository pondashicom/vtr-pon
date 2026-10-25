#include "mac_media_runtime.h"
#include "pdf_import_service.h"
#include "document_import_queue.h"
#include "playlist_model.h"
#include "controller_settings.h"
#include "background_io_queue.h"
#include "user_info.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QPageLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QThread>
#include <QTimer>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <functional>

bool runDocumentImportQueueRegression(ProgramPlayoutController *program);
bool runPdfBackendBoundaryChecks(const QString &directory);
bool runPdfImportBoundaryChecks(const QString &directory);
bool runPdfQueueBoundaryChecks(const QString &directory);
namespace {
bool check(bool condition, const char *label) {
    qInfo() << (condition ? "PASS" : "FAIL") << label;
    return condition;
}
bool waitFor(const std::function<bool()> &completed, int timeout = 10000) {
    QElapsedTimer elapsed; elapsed.start();
    do {
        QApplication::processEvents();
        if (completed()) return true;
        QThread::msleep(2);
    } while (elapsed.elapsed() < timeout);
    return false;
}
QByteArray fileHash(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);
}
bool writePdf(const QString &path) {
    QPdfWriter writer(path); writer.setResolution(72);
    writer.setPageLayout(QPageLayout(QPageSize(QSizeF(640,360),QPageSize::Point,
        QStringLiteral("Video"),QPageSize::ExactMatch),QPageLayout::Portrait,QMarginsF()));
    QPainter painter(&writer);
    if (!painter.isActive()) return false;
    painter.fillRect(QRect(0,0,writer.width(),writer.height()),QColor(230,32,32));
    if (!writer.newPage()) return false;
    painter.fillRect(QRect(0,0,writer.width(),writer.height()),QColor(32,80,230));
    return painter.end();
}
PdfAddSettings videoSettings() {
    PdfAddSettings settings;
    settings.method=PdfAddMethod::VideoConversion;
    settings.pageDurationSeconds=0.4;
    settings.crossDissolveEnabled=true;
    settings.crossDissolveDurationSeconds=0.2;
    return settings;
}
bool decodeChecks(const PdfImportResult &result) {
    if (!result.succeeded || result.items.size()!=1) {
        qWarning()<<result.errorDetail; return false;
    }
    // Explicit native decoder: this readback test needs no FFmpeg or GPL codec.
    GError *error=nullptr;
    auto *pipeline=gst_parse_launch(
        "filesrc name=input ! qtdemux ! h264parse ! vtdec ! videoconvert ! "
        "video/x-raw,format=BGRA ! appsink name=frames sync=false max-buffers=2 wait-on-eos=false",&error);
    if (!pipeline || error) {
        if (error) {qWarning()<<error->message;g_error_free(error);}
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    auto *source=gst_bin_get_by_name(GST_BIN(pipeline),"input");
    auto *sink=gst_bin_get_by_name(GST_BIN(pipeline),"frames");
    auto *bus=gst_element_get_bus(pipeline);
    g_object_set(source,"location",result.items.first().filePath.toUtf8().constData(),nullptr);
    gst_element_set_state(pipeline,GST_STATE_PLAYING);
    int count=0; bool dimensions=true, timestamps=true; QList<QColor> centers;
    QElapsedTimer elapsed;elapsed.start();
    GstClockTime previous=GST_CLOCK_TIME_NONE;
    while (elapsed.elapsed()<10000) {
        auto *sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),100*GST_MSECOND);
        if (!sample) {
            if (gst_app_sink_is_eos(GST_APP_SINK(sink))) break;
            continue;
        }
        GstVideoInfo info{}; GstVideoFrame frame{};
        auto *buffer=gst_sample_get_buffer(sample);
        if (!gst_video_info_from_caps(&info,gst_sample_get_caps(sample)) ||
            !gst_video_frame_map(&frame,&info,buffer,GST_MAP_READ)) dimensions=false;
        else {
            dimensions &= info.width==1920 && info.height==1080 && info.fps_n==30 && info.fps_d==1;
            const auto *data=static_cast<const guint8*>(GST_VIDEO_FRAME_PLANE_DATA(&frame,0));
            const auto *pixel=data+540*GST_VIDEO_FRAME_PLANE_STRIDE(&frame,0)+960*4;
            centers.append(QColor(pixel[2],pixel[1],pixel[0],pixel[3]));
            gst_video_frame_unmap(&frame);
        }
        const auto pts=GST_BUFFER_PTS(buffer);
        const auto expected=gst_util_uint64_scale(count,GST_SECOND,30);
        timestamps &= GST_CLOCK_TIME_IS_VALID(pts) &&
            (!GST_CLOCK_TIME_IS_VALID(previous) || pts>previous) &&
            std::abs(static_cast<gint64>(pts)-static_cast<gint64>(expected))<GST_MSECOND;
        previous=pts; ++count;gst_sample_unref(sample);
    }
    bool noError=true;
    if (auto *message=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR)) {
        GError *detail=nullptr;gchar *debug=nullptr;gst_message_parse_error(message,&detail,&debug);
        qWarning()<<(detail?detail->message:"")<<(debug?debug:"");
        g_clear_error(&detail);g_free(debug);gst_message_unref(message);noError=false;
    }
    gst_element_set_state(pipeline,GST_STATE_NULL);
    gst_object_unref(bus);gst_object_unref(source);gst_object_unref(sink);gst_object_unref(pipeline);
    bool ok=check(noError && dimensions && count==24,"MP4 decodes all 24 frames at 1920x1080/30fps with native VideoToolbox");
    ok &= check(timestamps && result.items.first().settings.outPointMs &&
        std::abs(*result.items.first().settings.outPointMs-800)<=34,"MP4 timestamps advance and duration matches page timing");
    bool colors=false;
    if (centers.size()==24) {
        const auto first=centers.first(), blend=centers[8], last=centers.last();
        qInfo()<<"MP4 readback center colors"<<first<<blend<<last;
        colors=first.red()>200 && first.blue()<55 && last.blue()>200 && last.red()<55 &&
            blend.red()>70 && blend.blue()>70 && blend.red()<200 && blend.blue()<200;
    }
    ok &= check(colors,"MP4 preserves first/last page colors and the cross-dissolve midpoint");
    return ok;
}
bool readback(const QString &path,const QString &directory) {
    const auto original=fileHash(path);
    PdfAddSettings settings;settings.method=PdfAddMethod::PngSequence;settings.pageDurationSeconds=0.4;
    auto png=PdfImportService::importFile(path,settings,1,nullptr,nullptr,{},directory);
    bool ok=check(png.succeeded && png.items.size()==2,"PNG output commits one file per PDF page");
    if (png.items.size()==2) {
        const QImage first(png.items[0].filePath),second(png.items[1].filePath);
        ok &= check(first.size()==QSize(1920,1080) && second.size()==first.size() &&
            first.pixelColor(960,540).red()>220 && second.pixelColor(960,540).blue()>220,
            "PNG page order and lossless colors survive saved-file reload");
    }
    auto movie=PdfImportService::importFile(path,videoSettings(),1,nullptr,nullptr,{},directory);
    ok=decodeChecks(movie) && ok;
    ok &= check(QDir(movie.generatedDirectory).entryList({QStringLiteral("*.png")},QDir::Files).isEmpty(),
                "successful MP4 commit removes temporary rendered pages");
    ok &= check(fileHash(path)==original,"conversion leaves the source PDF byte-identical");
    return ok;
}
struct QueueFixture {
    PlaylistModel model;
    UserInfoCenter info;
    BackgroundIoQueue io;
    DocumentImportQueue queue{&model,nullptr,&info,&io};
    ~QueueFixture() {
        queue.requestShutdown();waitFor([this]{return queue.shutdownComplete();});
        io.requestShutdown();waitFor([this]{return io.shutdownComplete();});
    }
};
bool queueChecks(const QString &path, const QString &directory) {
    auto settings=vtrpon::makeControllerSettings();
    settings.setValue("files/documentConversionDirectory",directory);settings.sync();
    bool ok=runDocumentImportQueueRegression(nullptr);
    QueueFixture fixture;
    bool wrongThread=false;int ticks=0;QList<PlaylistItemConversionStage> stages;
    QObject::connect(&fixture.model,&PlaylistModel::itemConversionStateChanged,&fixture.model,[&](quint64 id){
        wrongThread |= QThread::currentThread()!=QApplication::instance()->thread();
        if (auto state=fixture.model.itemConversionState(id)) stages.append(state->stage);
    },Qt::DirectConnection);
    QTimer heartbeat;heartbeat.setInterval(1);QObject::connect(&heartbeat,&QTimer::timeout,[&]{++ticks;});heartbeat.start();
    PdfAddSettings png;png.method=PdfAddMethod::PngSequence;png.pageDurationSeconds=0.4;
    QString error;
    ok &= check(fixture.queue.enqueuePdf(path,png,1,&error) &&
        fixture.queue.enqueuePdf(path,videoSettings(),1,&error),"PNG and MP4 jobs queue together in the selected order");
    const int other=fixture.model.createPlaylist();fixture.model.setActivePlaylistNumber(other);
    fixture.queue.startNext();
    ok &= check(waitFor([&]{return !fixture.queue.hasPendingImports();}),"conversion jobs finish without blocking the GUI event loop");
    ok &= check(ticks>0 && !wrongThread && stages.contains(PlaylistItemConversionStage::Rendering) &&
        stages.contains(PlaylistItemConversionStage::Encoding),"GUI heartbeat and per-item rendering/encoding progress continue");
    ok &= check(fixture.model.itemCount(1)==3 && fixture.model.itemCount(other)==0 &&
        fixture.model.itemState(fixture.model.itemIdAt(1,0))->mediaType==PlaylistMediaType::StillImage &&
        fixture.model.itemState(fixture.model.itemIdAt(1,2))->mediaType==PlaylistMediaType::Video,
        "converted pages and movie replace placeholders in original playlist order");
    const QString root=QDir(directory).filePath("PDF");
    const auto before=QDir(root).entryList(QDir::AllEntries|QDir::NoDotAndDotDot,QDir::Name);
    QueueFixture canceled;
    bool cancelSent=false;
    QObject::connect(&canceled.model,&PlaylistModel::itemConversionStateChanged,&canceled.model,[&](quint64 id){
        const auto state=canceled.model.itemConversionState(id);
        if (state && state->stage==PlaylistItemConversionStage::Encoding && !cancelSent) {
            cancelSent=true;canceled.queue.cancel(id,true);
        }
    });
    auto longVideo=videoSettings();longVideo.pageDurationSeconds=30;
    ok &= check(canceled.queue.enqueuePdf(path,longVideo,1,&error),"long conversion can be queued for active cancellation");
    canceled.queue.startNext();
    ok &= check(waitFor([&]{return !canceled.queue.hasPendingImports();}) && cancelSent && canceled.model.itemCount()==0,
                "removing an encoding item cancels it without restoring a stale placeholder");
    canceled.io.requestShutdown();
    ok &= check(waitFor([&]{return canceled.io.shutdownComplete();}) &&
        QDir(root).entryList(QDir::AllEntries|QDir::NoDotAndDotDot,QDir::Name)==before,
        "active cancellation cleans only that job's output and keeps earlier results");
    return ok;
}
}
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    if (!configureMacBundledMediaRuntime()) return 2;
    gst_init(&argc,&argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    app.setProperty("vtrpon.settingsFile",directory.filePath("user.ini"));
    if (app.arguments().contains("--boundary-backend")) return runPdfBackendBoundaryChecks(directory.path()) ? 0 : 1;
    if (app.arguments().contains("--boundary-import")) return runPdfImportBoundaryChecks(directory.path()) ? 0 : 1;
    if (app.arguments().contains("--boundary-queue")) return runPdfQueueBoundaryChecks(directory.path()) ? 0 : 1;
    const auto path=directory.filePath(QStringLiteral("日本語 PDF 変換.pdf"));
    if (!writePdf(path)) return 1;
    if (app.arguments().contains("--queue")) return queueChecks(path,directory.path()) ? 0 : 1;
    if (app.arguments().size()==3 && app.arguments().at(1)=="--export-fixture") {
        const auto output=app.arguments().at(2);QDir().mkpath(output);
        const auto pdf=QDir(output).filePath(QStringLiteral("日本語 PDF 変換.pdf"));
        if (!QFile::copy(path,pdf)) return 1;
        return readback(pdf,output) ? 0 : 1;
    }
    return readback(path,directory.path()) ? 0 : 1;
}
