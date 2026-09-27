#include "media_duration_probe.h"
#include "media_thumbnailer.h"
#include "uvc_input_guard.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QtLogging>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#include <gst/gst.h>

#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>

class MediaThumbnailerLifecycleTest {
public:
  static bool run(const QString &imagePath) {
    struct Gate {
      std::mutex mutex;
      std::condition_variable ready;
      std::array<bool, 2> released{false, false};
      std::atomic_int entered{0}, exited{0};
    };
    auto gate = std::make_shared<Gate>();
    auto thumbnailer = std::make_unique<MediaThumbnailer>();
    struct ReleaseGate {
      std::shared_ptr<Gate> gate;
      ~ReleaseGate() {
        { std::lock_guard lock(gate->mutex); gate->released.fill(true); }
        gate->ready.notify_all();
      }
    } releaseGate{gate};
    const auto waitFor = [](const auto &condition) {
      QElapsedTimer elapsed;
      elapsed.start();
      while (!condition() && elapsed.elapsed() < 4000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
      }
      return condition();
    };
    // Occupy the real owned pool, without relying on a slow filesystem or
    // camera. Real thumbnail requests below remain queued behind these jobs.
    for (int index = 0; index < 2; ++index) {
      thumbnailer->workerPool_.start([gate, index] {
        ++gate->entered;
        std::unique_lock lock(gate->mutex);
        gate->ready.wait(lock, [&] { return gate->released[index]; });
        ++gate->exited;
      });
    }
    if (!waitFor([&] { return gate->entered.load() == 2; })) return false;
    int delivered = 0;
    QObject::connect(thumbnailer.get(), &MediaThumbnailer::thumbnailReady,
                     [&](quint64, const MediaThumbnailResult &) { ++delivered; });
    thumbnailer->requestThumbnail(1, imagePath, PlaylistMediaType::StillImage);
    thumbnailer->requestThumbnail(2, imagePath, PlaylistMediaType::StillImage);
    thumbnailer->requestUvcThumbnail(3, QStringLiteral("shutdown-fixture-must-not-open"));
    const auto first = thumbnailer->pendingThumbnails_.value(1);
    const auto second = thumbnailer->pendingThumbnails_.value(2);
    if (!first || !second) return false;
    auto queuedRan = std::make_shared<std::atomic_bool>(false);
    thumbnailer->workerPool_.start([queuedRan] { queuedRan->store(true); });
    QElapsedTimer elapsed;
    elapsed.start();
    thumbnailer->requestShutdown();
    thumbnailer->requestShutdown();
    if (elapsed.elapsed() >= 500 || thumbnailer->shutdownComplete()
        || !first->canceled.load() || !second->canceled.load()
        || !thumbnailer->pendingThumbnails_.isEmpty()) return false;
    thumbnailer->requestThumbnail(4, imagePath, PlaylistMediaType::StillImage);
    thumbnailer->requestUvcThumbnail(5, QStringLiteral("shutdown-fixture-must-not-open"));
    int heartbeats = 0;
    QTimer heartbeat;
    heartbeat.setInterval(1);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start();
    if (!waitFor([&] { return heartbeats >= 5; }) || thumbnailer->shutdownComplete()) return false;
    { std::lock_guard lock(gate->mutex); gate->released[0] = true; }
    gate->ready.notify_all();
    if (!waitFor([&] { return gate->exited.load() == 1; }) || thumbnailer->shutdownComplete()) return false;
    { std::lock_guard lock(gate->mutex); gate->released[1] = true; }
    gate->ready.notify_all();
    if (!waitFor([&] { return thumbnailer->shutdownComplete(); })) return false;
    QCoreApplication::processEvents();
    if (gate->exited != 2 || delivered != 0 || queuedRan->load()
        || !thumbnailer->pendingThumbnails_.isEmpty()) return false;
    elapsed.restart();
    thumbnailer.reset();
    return elapsed.elapsed() < 500;
  }
};

namespace {

bool createMediaFixture(const QByteArray &description,
                        const QString &outputPath, QString &errorDetail)
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(description.constData(), &error);
    if (!pipeline) {
        errorDetail = error ? QString::fromUtf8(error->message)
                            : QStringLiteral("fixture pipeline creation failed");
        g_clear_error(&error);
        return false;
    }
    GstElement *output = gst_bin_get_by_name(GST_BIN(pipeline), "output");
    if (!output) {
        errorDetail = QStringLiteral("fixture output element was not found");
        gst_object_unref(pipeline);
        return false;
    }
    const QByteArray encodedPath = QFile::encodeName(outputPath);
    g_object_set(output, "location", encodedPath.constData(), nullptr);
    gst_object_unref(output);

    bool succeeded = false;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
        != GST_STATE_CHANGE_FAILURE) {
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 15 * GST_SECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS
                                        | GST_MESSAGE_ERROR));
        if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            succeeded = true;
        } else if (message) {
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            errorDetail = error ? QString::fromUtf8(error->message)
                                : QStringLiteral("fixture pipeline failed");
            g_clear_error(&error);
            g_free(debug);
        } else {
            errorDetail = QStringLiteral("fixture pipeline timed out");
        }
        if (message) {
            gst_message_unref(message);
        }
        gst_object_unref(bus);
    } else {
        errorDetail = QStringLiteral("fixture pipeline did not start");
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    (void)gst_element_get_state(pipeline, nullptr, nullptr, 2 * GST_SECOND);
    gst_object_unref(pipeline);
    return succeeded;
}

bool createPdfFixture(const QString &outputPath)
{
    QPdfWriter writer(outputPath);
    writer.setResolution(72);
    const QPageSize squarePage(
        QSizeF(400.0, 400.0), QPageSize::Point,
        QStringLiteral("VTR-PON Thumbnail Test"), QPageSize::ExactMatch);
    const QPageLayout layout(squarePage, QPageLayout::Portrait,
                             QMarginsF(), QPageLayout::Point);
    if (!writer.setPageLayout(layout)) {
        return false;
    }
    QPainter painter(&writer);
    if (!painter.isActive()) {
        return false;
    }
    painter.fillRect(QRect(0, 0, writer.width(), writer.height()),
                     Qt::white);
    painter.fillRect(QRect(writer.width() / 4, writer.height() / 4,
                           writer.width() / 2, writer.height() / 2),
                     QColor(220, 32, 32));
    return painter.end();
}

int countNonBlackPixels(const QImage &image)
{
    int count = 0;
    for (int row = 0; row < image.height(); ++row) {
        for (int column = 0; column < image.width(); ++column) {
            const QColor color = image.pixelColor(column, row);
            if (color.red() > 12 || color.green() > 12 || color.blue() > 12) {
                ++count;
            }
        }
    }
    return count;
}

int countRedFramePixels(const QImage &image)
{
    int count = 0;
    for (int row = 0; row < image.height(); ++row) {
        for (int column = 0; column < image.width(); ++column) {
            const QColor color = image.pixelColor(column, row);
            if (color.red() > 140 && color.green() < 80
                && color.blue() < 80) {
                ++count;
            }
        }
    }
    return count;
}

int countGreenWaveformPixels(const QImage &image)
{
    int count = 0;
    for (int row = 0; row < image.height(); ++row) {
        for (int column = 0; column < image.width(); ++column) {
            const QColor color = image.pixelColor(column, row);
            if (color.green() > 80 && color.green() > color.red() * 1.5
                && color.green() > color.blue() * 1.3) {
                ++count;
            }
        }
    }
    return count;
}

int countSmoothedWaveformEdgePixels(const QImage &image)
{
    int count = 0;
    const int center = image.height() / 2;
    for (int row = 0; row < image.height(); ++row) {
        if (std::abs(row - center) <= 2) {
            continue;
        }
        for (int column = 0; column < image.width(); ++column) {
            const QColor color = image.pixelColor(column, row);
            if (color.green() >= 90 && color.green() < 180
                && color.green() > color.red() * 1.5
                && color.green() > color.blue() * 1.3) {
                ++count;
            }
        }
    }
    return count;
}

int greenWaveformVerticalSpan(const QImage &image)
{
    int minimumRow = image.height();
    int maximumRow = -1;
    for (int row = 0; row < image.height(); ++row) {
        for (int column = 0; column < image.width(); ++column) {
            const QColor color = image.pixelColor(column, row);
            if (color.green() > 80 && color.green() > color.red() * 1.5
                && color.green() > color.blue() * 1.3) {
                minimumRow = (std::min)(minimumRow, row);
                maximumRow = (std::max)(maximumRow, row);
            }
        }
    }
    return maximumRow >= minimumRow ? maximumRow - minimumRow + 1 : 0;
}

bool saveResult(const QString &outputDirectory, const QString &name,
                const MediaThumbnailResult &result)
{
    return outputDirectory.isEmpty()
        || result.image.save(QDir(outputDirectory).filePath(name));
}

} // namespace

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef _WIN32
        qputenv("QT_QPA_PLATFORM", "windows");
#else
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    }
    QGuiApplication application(argc, argv);
    gst_init(&argc, &argv);
    {
        const QString ownPath=QStringLiteral("opaque-self-camera-thumbnail-test");
        vtrpon::UvcSelfCameraRegistration registration;
        registration.finish(ownPath);
        const auto rejected=generateUvcThumbnail(ownPath.toUpper());
        if(rejected.succeeded || !rejected.errorDetail.contains(QStringLiteral("feedback"))) {
            qCritical()<<"Self-camera thumbnail must be rejected before capture";
            return 1;
        }
    }

    QString outputDirectory;
    const QStringList arguments = application.arguments();
    const int outputOptionIndex = arguments.indexOf(QStringLiteral("--output-dir"));
    if (outputOptionIndex >= 0 && outputOptionIndex + 1 < arguments.size()) {
        outputDirectory = QDir(arguments.at(outputOptionIndex + 1)).absolutePath();
        if (!QDir().mkpath(outputDirectory)) {
            qCritical() << "Could not create output directory" << outputDirectory;
            return 1;
        }
    }

    QTemporaryDir fixtureDirectory;
    if (!fixtureDirectory.isValid()) {
        qCritical() << "Could not create fixture directory";
        return 1;
    }
    const QString videoPath = fixtureDirectory.filePath("video.avi");
    const QString blackHeadVideoPath =
        fixtureDirectory.filePath("black-head-long-gop.mp4");
    const QString audioPath = fixtureDirectory.filePath("audio.wav");
    const QString imagePath = fixtureDirectory.filePath("image.png");
    const QString pdfPath = fixtureDirectory.filePath("document.pdf");

    QString fixtureError;
    if (!createMediaFixture(
            "videotestsrc pattern=ball num-buffers=60 ! "
            "video/x-raw,width=320,height=180,framerate=30/1 ! "
            "videoconvert ! jpegenc ! avimux ! filesink name=output",
            videoPath, fixtureError)) {
        qCritical() << "Video fixture failed:" << fixtureError;
        return 1;
    }
    if (!createMediaFixture(
            "concat name=sequence ! videoconvert ! "
            "openh264enc gop-size=300 scene-change-detection=false "
            "enable-frame-skip=false ! h264parse ! mp4mux ! "
            "filesink name=output "
            "videotestsrc pattern=black num-buffers=10 ! "
            "video/x-raw,width=160,height=90,framerate=30/1 ! sequence. "
            "videotestsrc pattern=red num-buffers=290 ! "
            "video/x-raw,width=160,height=90,framerate=30/1 ! sequence.",
            blackHeadVideoPath, fixtureError)) {
        qCritical() << "Black-head video fixture failed:" << fixtureError;
        return 1;
    }
    if (!createMediaFixture(
            "audiotestsrc wave=sine freq=3000 volume=0.5 num-buffers=100 ! "
            "audio/x-raw,rate=48000 ! audioconvert ! wavenc ! "
            "filesink name=output",
            audioPath, fixtureError)) {
        qCritical() << "Audio fixture failed:" << fixtureError;
        return 1;
    }

    QImage sourceImage(200, 320, QImage::Format_RGBA8888);
    sourceImage.fill(QColor(QStringLiteral("#246bce")));
    QPainter sourcePainter(&sourceImage);
    sourcePainter.fillRect(QRect(20, 40, 160, 100),
                           QColor(QStringLiteral("#f2c84b")));
    sourcePainter.fillRect(QRect(20, 180, 160, 100),
                           QColor(QStringLiteral("#d84a4a")));
    sourcePainter.end();
    if (!sourceImage.save(imagePath)) {
        qCritical() << "Image fixture failed";
        return 1;
    }
    if (!createPdfFixture(pdfPath)) {
        qCritical() << "PDF fixture failed";
        return 1;
    }

    const MediaDurationProbeResult videoProbe = probeMediaDuration(videoPath);
    const MediaDurationProbeResult blackHeadVideoProbe =
        probeMediaDuration(blackHeadVideoPath);
    const MediaDurationProbeResult audioProbe = probeMediaDuration(audioPath);
    const MediaDurationProbeResult imageProbe = probeMediaDuration(imagePath);
    if (!videoProbe.succeeded
        || videoProbe.mediaType != PlaylistMediaType::Video
        || !blackHeadVideoProbe.succeeded
        || blackHeadVideoProbe.mediaType != PlaylistMediaType::Video
        || !audioProbe.succeeded
        || audioProbe.mediaType != PlaylistMediaType::Audio
        || !imageProbe.succeeded
        || imageProbe.mediaType != PlaylistMediaType::StillImage) {
        qCritical() << "Media classification failed"
                    << static_cast<int>(videoProbe.mediaType)
                    << static_cast<int>(blackHeadVideoProbe.mediaType)
                    << static_cast<int>(audioProbe.mediaType)
                    << static_cast<int>(imageProbe.mediaType);
        return 1;
    }

    qInfo() << "Generating video thumbnail";
    const MediaThumbnailResult video = generateMediaThumbnail(
        videoPath, videoProbe.mediaType, videoProbe.durationMs);
    qInfo() << "Generating black-head video thumbnail";
    const MediaThumbnailResult blackHeadVideo = generateMediaThumbnail(
        blackHeadVideoPath, blackHeadVideoProbe.mediaType,
        blackHeadVideoProbe.durationMs);
    qInfo() << "Generating audio thumbnail";
    const MediaThumbnailResult audio = generateMediaThumbnail(
        audioPath, audioProbe.mediaType, audioProbe.durationMs);
    const MediaThumbnailResult trimmedAudio = generateMediaThumbnail(
        audioPath, audioProbe.mediaType, 1000); // Trimmed OUT is not source length.
    qInfo() << "Generating image thumbnail";
    const MediaThumbnailResult image = generateMediaThumbnail(
        imagePath, imageProbe.mediaType, imageProbe.durationMs);
    qInfo() << "Generating PDF thumbnail";
    const MediaThumbnailResult pdf = generateMediaThumbnail(
        pdfPath, PlaylistMediaType::Pdf);

    constexpr QSize expectedSize(120, 68);
    if (video.sourceCodec.isEmpty()
        || blackHeadVideo.sourceCodec != QStringLiteral("H.264")
        || audio.sourceCodec != QStringLiteral("PCM")
        || trimmedAudio.sourceCodec != audio.sourceCodec
        || !image.sourceCodec.isEmpty() || !pdf.sourceCodec.isEmpty()) {
        qCritical() << "Source codec metadata failed" << video.sourceCodec
                    << blackHeadVideo.sourceCodec << audio.sourceCodec;
        return 1;
    }
    if (video.sourceSize != QSize(320, 180)
        || image.sourceSize != sourceImage.size()
        || video.sourceDurationMs != videoProbe.durationMs
        || audio.sourceDurationMs != audioProbe.durationMs
        || trimmedAudio.sourceDurationMs != audioProbe.durationMs
        || image.sourceDurationMs.has_value()) {
        qCritical() << "Source metadata must retain original size and full duration";
        return 1;
    }
    const std::array<MediaThumbnailResult, 5> results = {
        video, blackHeadVideo, audio, image, pdf};
    for (const MediaThumbnailResult &result : results) {
        if (!result.succeeded || result.image.size() != expectedSize) {
            qCritical() << "Thumbnail failed" << result.errorDetail
                        << result.image.size();
            return 1;
        }
    }
    if (countNonBlackPixels(video.image) < 500) {
        qCritical() << "Video thumbnail did not contain a visible frame";
        return 1;
    }
    if (countRedFramePixels(blackHeadVideo.image) < 5000) {
        qCritical() << "Video thumbnail seek returned the black keyframe "
                       "instead of the exact target frame";
        return 1;
    }
    if (countGreenWaveformPixels(audio.image) < 100) {
        qCritical() << "Audio thumbnail did not contain a visible waveform";
        return 1;
    }
    if (countSmoothedWaveformEdgePixels(audio.image) < 20) {
        qCritical() << "Audio thumbnail waveform edges were not smoothed";
        return 1;
    }
    if (greenWaveformVerticalSpan(audio.image) < 24) {
        qCritical() << "High-frequency audio waveform amplitude was too small";
        return 1;
    }
    if (countNonBlackPixels(image.image) < 1000
        || image.image.pixelColor(0, 0) != QColor(Qt::black)) {
        qCritical() << "Image thumbnail did not preserve letterboxing";
        return 1;
    }
    if (pdf.mediaType != PlaylistMediaType::Pdf
        || countRedFramePixels(pdf.image) < 500
        || pdf.image.pixelColor(0, 0) != QColor(Qt::black)) {
        qCritical() << "PDF thumbnail did not render the first page with "
                       "letterboxing";
        return 1;
    }
    if (!saveResult(outputDirectory, QStringLiteral("video.png"), video)
        || !saveResult(outputDirectory,
                       QStringLiteral("black-head-video.png"),
                       blackHeadVideo)
        || !saveResult(outputDirectory, QStringLiteral("audio.png"), audio)
        || !saveResult(outputDirectory, QStringLiteral("image.png"), image)
        || !saveResult(outputDirectory, QStringLiteral("pdf.png"), pdf)
        || (!outputDirectory.isEmpty()
            && !sourceImage.save(
                QDir(outputDirectory).filePath("source-image.png")))) {
        qCritical() << "Could not save visual verification output";
        return 1;
    }

    if (!MediaThumbnailerLifecycleTest::run(imagePath)) {
        qCritical() << "Thumbnail asynchronous shutdown lifecycle failed";
        return 1;
    }
    qInfo() << "MEDIA_THUMBNAILS_OK"
            << "video" << video.image.size()
            << "audio" << audio.image.size()
            << "image" << image.image.size()
            << "pdf" << pdf.image.size();
    return 0;
}
