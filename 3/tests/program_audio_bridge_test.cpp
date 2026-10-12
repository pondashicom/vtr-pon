#include "bgm_playback.h"
#include "program_audio_bridge.h"
#include "program_audio_output.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <gst/gst.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr int kRate = 48000;
constexpr float kSampleTolerance = 0.0000001F;

bool require(bool condition, const char *message)
{
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

void pump(int milliseconds)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
}

template<typename Predicate>
bool await(Predicate predicate, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (predicate()) return true;
        QThread::msleep(2);
    } while (timer.elapsed() < timeoutMs);
    return false;
}

QString createSequence(const QString &directory, bool constant = false)
{
    const QString path = directory + (constant ? QStringLiteral("/constant.wav")
                                               : QStringLiteral("/sequence.wav"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = kRate * 8;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + frames * 4);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(2) << quint32(kRate)
           << quint32(kRate * 4) << quint16(4) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(frames * 4);
    for (quint32 index = 0; index < frames; ++index) {
        // Every stereo pair identifies its source frame. A periodic tone can
        // conceal a dropped/repeated whole cycle, including 25 ms at 1 kHz.
        stream << qint16(constant ? 4096 : 4096 + index % 4096)
               << qint16(constant ? 4096 : -4096 - static_cast<int>(index / 4096));
    }
    return stream.status() == QDataStream::Ok ? path : QString{};
}

struct Chunk {
    std::vector<float> samples;
    GstClockTime pts = GST_CLOCK_TIME_NONE;
    GstClockTime duration = GST_CLOCK_TIME_NONE;
    GstClockTime pipelineBaseTime = GST_CLOCK_TIME_NONE;
    GstSegment segment = {};
    QByteArray rawBytes;
    QString caps;
    QString format;
    int rate = 0;
    int channels = 0;
    guint64 arrivalMonotonicNs = 0;
    GstClockTime arrivalClockTime = GST_CLOCK_TIME_NONE;
    guint flags = 0;
};

class PcmTrace final {
public:
    explicit PcmTrace(GstElement *element, GstElement *pipeline = nullptr,
                      const char *padName = "src", bool diagnostics = false)
        : pad_(element ? gst_element_get_static_pad(element, padName) : nullptr)
        , pipeline_(pipeline ? GST_ELEMENT(gst_object_ref(pipeline)) : nullptr)
        , diagnostics_(diagnostics)
    {
        gst_segment_init(&segment_, GST_FORMAT_TIME);
        if (pad_) probe_ = gst_pad_add_probe(pad_,
            static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                        GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
            &PcmTrace::capture, this, nullptr);
    }
    ~PcmTrace()
    {
        if (pad_) {
            if (probe_) gst_pad_remove_probe(pad_, probe_);
            gst_object_unref(pad_);
        }
        if (pipeline_) gst_object_unref(pipeline_);
    }
    bool valid() const { return pad_ && probe_; }
    GstClockTime currentBaseTime() const
    {
        return pipeline_ ? gst_element_get_base_time(pipeline_) : GST_CLOCK_TIME_NONE;
    }
    std::size_t mark() const
    {
        std::lock_guard lock(mutex_);
        return chunks_.size();
    }
    std::vector<Chunk> since(std::size_t mark) const
    {
        std::lock_guard lock(mutex_);
        mark = (std::min)(mark, chunks_.size());
        return {chunks_.begin() + static_cast<std::ptrdiff_t>(mark), chunks_.end()};
    }
    std::vector<Chunk> currentSegment() const
    {
        std::lock_guard lock(mutex_);
        return {chunks_.begin() + static_cast<std::ptrdiff_t>(segmentStart_), chunks_.end()};
    }
    bool formatValid() const
    {
        std::lock_guard lock(mutex_);
        return !invalidFormat_;
    }

private:
    static GstPadProbeReturn capture(GstPad *pad, GstPadProbeInfo *info, gpointer data)
    {
        auto *self = static_cast<PcmTrace *>(data);
        if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
            GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
            if (event && GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
                const GstSegment *segment = nullptr;
                gst_event_parse_segment(event, &segment);
                std::lock_guard lock(self->mutex_);
                self->segmentStart_ = self->chunks_.size();
                if (segment) self->segment_ = *segment;
            }
            return GST_PAD_PROBE_OK;
        }
        GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buffer) return GST_PAD_PROBE_OK;
        const guint64 arrival = self->diagnostics_ ? static_cast<guint64>(g_get_monotonic_time()) * 1000 : 0;
        GstCaps *caps = gst_pad_get_current_caps(pad);
        const GstStructure *structure = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        const char *format = structure ? gst_structure_get_string(structure, "format") : nullptr;
        gint channels = 0;
        gint rate = 0;
        const bool supported = format && std::strcmp(format, "F32LE") == 0
            && gst_structure_get_int(structure, "channels", &channels) && channels == 2
            && gst_structure_get_int(structure, "rate", &rate) && rate == kRate;
        Chunk chunk;
        if (self->diagnostics_) {
            gchar *description = caps ? gst_caps_to_string(caps) : nullptr;
            chunk.caps = QString::fromUtf8(description ? description : "unknown");
            g_free(description);
            chunk.format = QString::fromUtf8(format ? format : "unknown");
            if (structure) {
                gst_structure_get_int(structure, "channels", &channels);
                gst_structure_get_int(structure, "rate", &rate);
            }
            chunk.channels = channels;
            chunk.rate = rate;
        }
        if (caps) gst_caps_unref(caps);
        GstMapInfo map = {};
        if ((!supported && !self->diagnostics_) || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            std::lock_guard lock(self->mutex_);
            self->invalidFormat_ = true;
            return GST_PAD_PROBE_OK;
        }
        chunk.pts = GST_BUFFER_PTS(buffer);
        chunk.duration = GST_BUFFER_DURATION(buffer);
        chunk.pipelineBaseTime = self->pipeline_ ? gst_element_get_base_time(self->pipeline_) : GST_CLOCK_TIME_NONE;
        if (self->diagnostics_) {
            chunk.rawBytes = QByteArray(reinterpret_cast<const char *>(map.data), static_cast<qsizetype>(map.size));
            chunk.arrivalMonotonicNs = arrival;
            chunk.flags = GST_BUFFER_FLAGS(buffer);
            GstClock *clock = self->pipeline_ ? gst_element_get_clock(self->pipeline_) : nullptr;
            if (clock) {
                chunk.arrivalClockTime = gst_clock_get_time(clock);
                gst_object_unref(clock);
            }
        }
        const bool aligned = map.size % (2 * sizeof(float)) == 0;
        if (supported && aligned) {
            chunk.samples.resize(map.size / sizeof(float));
            if (!chunk.samples.empty()) std::memcpy(chunk.samples.data(), map.data, map.size);
        }
        gst_buffer_unmap(buffer, &map);
        std::lock_guard lock(self->mutex_);
        self->invalidFormat_ = self->invalidFormat_ || !supported || !aligned;
        chunk.segment = self->segment_;
        self->chunks_.push_back(std::move(chunk));
        return GST_PAD_PROBE_OK;
    }
    GstPad *pad_ = nullptr;
    GstElement *pipeline_ = nullptr;
    bool diagnostics_ = false;
    gulong probe_ = 0;
    mutable std::mutex mutex_;
    std::vector<Chunk> chunks_;
    std::size_t segmentStart_ = 0;
    GstSegment segment_ = {};
    bool invalidFormat_ = false;
};

bool saveTrace(const PcmTrace &trace, std::size_t mark, const QString &directory, const QString &name,
               std::size_t end = (std::numeric_limits<std::size_t>::max)())
{
    auto chunks = trace.since(mark);
    if (end != (std::numeric_limits<std::size_t>::max)())
        chunks.resize((std::min)(chunks.size(), end >= mark ? end - mark : 0));
    QFile pcm(QDir(directory).filePath(name + QStringLiteral(".pcm")));
    QFile metadata(QDir(directory).filePath(name + QStringLiteral(".jsonl")));
    if (!pcm.open(QIODevice::WriteOnly) || !metadata.open(QIODevice::WriteOnly)) return false;
    const auto timeValue = [](GstClockTime value) -> QJsonValue {
        return GST_CLOCK_TIME_IS_VALID(value) ? QJsonValue(QString::number(value)) : QJsonValue(QJsonValue::Null);
    };
    qint64 byteOffset = 0;
    guint64 previousArrival = 0;
    GstClockTime previousEnd = GST_CLOCK_TIME_NONE;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const auto &chunk = chunks[index];
        QJsonObject row;
        row.insert(QStringLiteral("capture_index"), static_cast<qint64>(mark + index));
        row.insert(QStringLiteral("byte_offset"), byteOffset);
        row.insert(QStringLiteral("byte_count"), chunk.rawBytes.size());
        row.insert(QStringLiteral("caps"), chunk.caps);
        row.insert(QStringLiteral("format"), chunk.format);
        row.insert(QStringLiteral("rate"), chunk.rate);
        row.insert(QStringLiteral("channels"), chunk.channels);
        row.insert(QStringLiteral("pts_ns"), timeValue(chunk.pts));
        row.insert(QStringLiteral("duration_ns"), timeValue(chunk.duration));
        row.insert(QStringLiteral("pipeline_base_ns"), timeValue(chunk.pipelineBaseTime));
        row.insert(QStringLiteral("arrival_monotonic_ns"), QString::number(chunk.arrivalMonotonicNs));
        row.insert(QStringLiteral("arrival_clock_ns"), timeValue(chunk.arrivalClockTime));
        row.insert(QStringLiteral("arrival_running_ns"),
            GST_CLOCK_TIME_IS_VALID(chunk.arrivalClockTime) && GST_CLOCK_TIME_IS_VALID(chunk.pipelineBaseTime)
                && chunk.arrivalClockTime >= chunk.pipelineBaseTime
            ? timeValue(chunk.arrivalClockTime - chunk.pipelineBaseTime) : QJsonValue(QJsonValue::Null));
        row.insert(QStringLiteral("flags"), static_cast<qint64>(chunk.flags));
        row.insert(QStringLiteral("segment_rate"), chunk.segment.rate);
        row.insert(QStringLiteral("segment_start_ns"), timeValue(chunk.segment.start));
        row.insert(QStringLiteral("segment_base_ns"), timeValue(chunk.segment.base));
        if (previousArrival)
            row.insert(QStringLiteral("arrival_delta_ns"), QString::number(chunk.arrivalMonotonicNs - previousArrival));
        if (GST_CLOCK_TIME_IS_VALID(previousEnd) && GST_CLOCK_TIME_IS_VALID(chunk.pts))
            row.insert(QStringLiteral("pts_gap_ns"), QString::number(
                static_cast<gint64>(chunk.pts) - static_cast<gint64>(previousEnd)));
        double squares = 0;
        double peak = 0;
        std::size_t zeroRun = 0;
        std::size_t maximumZeroRun = 0;
        for (std::size_t sample = 0; sample < chunk.samples.size(); ++sample) {
            const double value = chunk.samples[sample];
            squares += value * value;
            peak = (std::max)(peak, std::abs(value));
            if (sample % 2 == 1) {
                zeroRun = chunk.samples[sample] == 0 && chunk.samples[sample - 1] == 0 ? zeroRun + 1 : 0;
                maximumZeroRun = (std::max)(maximumZeroRun, zeroRun);
            }
        }
        row.insert(QStringLiteral("f32_stereo_frames"), static_cast<qint64>(chunk.samples.size() / 2));
        if (!chunk.samples.empty()) {
            row.insert(QStringLiteral("rms"), std::sqrt(squares / chunk.samples.size()));
            row.insert(QStringLiteral("peak"), peak);
            row.insert(QStringLiteral("max_zero_frames_in_buffer"), static_cast<qint64>(maximumZeroRun));
        }
        const QByteArray line = QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
        if (pcm.write(chunk.rawBytes) != chunk.rawBytes.size() || metadata.write(line) != line.size()) return false;
        byteOffset += chunk.rawBytes.size();
        previousArrival = chunk.arrivalMonotonicNs;
        previousEnd = GST_CLOCK_TIME_IS_VALID(chunk.pts) && GST_CLOCK_TIME_IS_VALID(chunk.duration)
            ? chunk.pts + chunk.duration : GST_CLOCK_TIME_NONE;
    }
    const bool written = pcm.flush() && metadata.flush();
    std::cout << "PCM diagnostic " << name.toStdString() << ": " << chunks.size()
              << " buffers, " << byteOffset << " bytes; " << pcm.fileName().toStdString() << '\n';
    return written;
}

std::vector<float> flatten(const std::vector<Chunk> &chunks)
{
    std::vector<float> samples;
    for (const auto &chunk : chunks)
        samples.insert(samples.end(), chunk.samples.begin(), chunk.samples.end());
    return samples;
}

bool sameSample(float left, float right)
{
    return std::isfinite(left) && std::isfinite(right)
        && std::abs(left - right) <= kSampleTolerance;
}

void reportAlignment(const std::vector<float> &expected, const std::vector<float> &actual,
                     std::size_t searchEnd, std::size_t needed, const char *label,
                     const std::vector<Chunk> &sourceChunks, const std::vector<Chunk> &outputChunks,
                     GstClockTime currentSourceBase, GstClockTime currentProgramBase)
{
    const auto printFrames = [](const char *name, const std::vector<float> &samples,
                                std::size_t offset) {
        std::cerr << name << " frame=" << offset / 2 << " [";
        for (std::size_t index = offset; index + 1 < samples.size() && index < offset + 8; index += 2)
            std::cerr << '(' << samples[index] << ',' << samples[index + 1] << ") ";
        std::cerr << "]\n";
    };
    std::cerr << std::setprecision(9) << label << ": alignment diagnostics\n";
    printFrames("expected first four", expected, 0);
    printFrames("actual first four", actual, 0);
    if (expected.size() < 8 || actual.size() < 8) return;
    struct FullMatch {
        std::size_t candidate;
        std::size_t matching;
        GstClockTime pts;
        gint64 predicted;
        gint64 delta;
        bool timeValid;
        gint64 settledPredicted;
        gint64 settledDelta;
        bool settledTimeValid;
    };
    const auto firstPcm = std::find_if(sourceChunks.begin(), sourceChunks.end(),
        [](const Chunk &chunk) { return !chunk.samples.empty(); });
    std::vector<FullMatch> bestMatches;
    std::size_t fullMatches = 0;
    const auto distance = [](const FullMatch &match) {
        if (match.settledTimeValid)
            return static_cast<guint64>(match.settledDelta >= 0 ? match.settledDelta : -match.settledDelta);
        return match.timeValid ? static_cast<guint64>(match.delta >= 0 ? match.delta : -match.delta)
                               : (std::numeric_limits<guint64>::max)();
    };
    for (std::size_t candidate = 0; candidate + 256 <= actual.size() && expected.size() >= 256; candidate += 2) {
        if (!sameSample(actual[candidate], expected[0])
            || !sameSample(actual[candidate + 1], expected[1])) continue;
        std::size_t matching = 2;
        while (matching < expected.size() && candidate + matching < actual.size()
               && sameSample(actual[candidate + matching], expected[matching])) ++matching;
        if (matching < 256) continue;
        ++fullMatches;
        FullMatch result = {candidate, matching, GST_CLOCK_TIME_NONE, 0, 0, false, 0, 0, false};
        std::size_t chunkOffset = 0;
        for (const auto &chunk : outputChunks) {
            if (candidate < chunkOffset + chunk.samples.size()) {
                result.pts = chunk.pts + gst_util_uint64_scale((candidate - chunkOffset) / 2, GST_SECOND, kRate);
                if (firstPcm != sourceChunks.end()) {
                    const GstClockTime sourceRunning = gst_segment_to_running_time(
                        &firstPcm->segment, GST_FORMAT_TIME, firstPcm->pts);
                    result.timeValid = GST_CLOCK_TIME_IS_VALID(chunk.pts)
                        && GST_CLOCK_TIME_IS_VALID(firstPcm->pipelineBaseTime)
                        && GST_CLOCK_TIME_IS_VALID(sourceRunning)
                        && GST_CLOCK_TIME_IS_VALID(chunk.pipelineBaseTime);
                    if (result.timeValid) {
                        result.predicted = static_cast<gint64>(firstPcm->pipelineBaseTime)
                            + static_cast<gint64>(sourceRunning) - static_cast<gint64>(chunk.pipelineBaseTime);
                        result.delta = static_cast<gint64>(result.pts) - result.predicted;
                    }
                    result.settledTimeValid = GST_CLOCK_TIME_IS_VALID(chunk.pts)
                        && GST_CLOCK_TIME_IS_VALID(sourceRunning)
                        && GST_CLOCK_TIME_IS_VALID(currentSourceBase)
                        && GST_CLOCK_TIME_IS_VALID(currentProgramBase);
                    if (result.settledTimeValid) {
                        result.settledPredicted = static_cast<gint64>(currentSourceBase)
                            + static_cast<gint64>(sourceRunning) - static_cast<gint64>(currentProgramBase);
                        result.settledDelta = static_cast<gint64>(result.pts) - result.settledPredicted;
                    }
                }
                break;
            }
            chunkOffset += chunk.samples.size();
        }
        bestMatches.push_back(result);
        std::sort(bestMatches.begin(), bestMatches.end(), [&distance](const FullMatch &a, const FullMatch &b) {
            return distance(a) != distance(b) ? distance(a) < distance(b) : a.candidate < b.candidate;
        });
        if (bestMatches.size() > 3) bestMatches.resize(3);
    }
    if (fullMatches == 0) {
        std::cerr << "FULL-TRACE diagnostic: source frame zero has no 128-frame anchor anywhere in captured output\n";
    } else {
        std::cerr << "FULL-TRACE source-frame-zero anchors=" << fullMatches
                  << "; reporting closest candidates to settled-current predicted PROGRAM time\n";
        if (firstPcm != sourceChunks.end())
            std::cerr << "sourceBaseTime capturedAtSourceProbe=" << firstPcm->pipelineBaseTime
                      << " settledCurrent=" << currentSourceBase
                      << " PROGRAMbaseTime settledCurrent=" << currentProgramBase << '\n';
        for (const auto &match : bestMatches) {
            std::cerr << "FULL-TRACE diagnostic sourceOffset=0 masterCandidate=" << match.candidate / 2
                      << " onsetMs=" << static_cast<double>(match.candidate / 2) * 1000.0 / kRate
                      << " beyondAssertionPrefix=" << (match.candidate > searchEnd)
                      << " exactConsecutiveFrames=" << match.matching / 2
                      << " requestedFrames=" << needed / 2
                      << " completeRequestedSequence=" << (match.matching >= needed)
                      << " actualPTS=" << match.pts << " capturedBasePredictedPROGRAMpts=" << match.predicted
                      << " capturedBaseDeltaNs=" << match.delta << " capturedBaseValid=" << match.timeValid
                      << " settledCurrentPredictedPROGRAMpts=" << match.settledPredicted
                      << " settledCurrentDeltaNs=" << match.settledDelta
                      << " settledCurrentValid=" << match.settledTimeValid << '\n';
        }
    }
    std::size_t nearestCandidate = 0;
    std::size_t nearestSource = 0;
    double bestCandidateError = (std::numeric_limits<double>::infinity)();
    double bestSourceError = (std::numeric_limits<double>::infinity)();
    const auto error = [](const std::vector<float> &left, std::size_t leftOffset,
                          const std::vector<float> &right, std::size_t rightOffset) {
        double total = 0;
        for (std::size_t sample = 0; sample < 8; ++sample)
            total += std::abs(static_cast<double>(left[leftOffset + sample]) - right[rightOffset + sample]);
        return total;
    };
    for (std::size_t index = 0; index <= searchEnd && index + 8 <= actual.size(); index += 2) {
        const double difference = error(expected, 0, actual, index);
        if (difference < bestCandidateError) { bestCandidateError = difference; nearestCandidate = index; }
    }
    for (std::size_t index = 0; index + 8 <= expected.size(); index += 2) {
        const double difference = error(expected, index, actual, 0);
        if (difference < bestSourceError) { bestSourceError = difference; nearestSource = index; }
    }
    std::cerr << "nearest four-frame sourceOffset=" << nearestSource / 2
              << " masterCandidate=" << nearestCandidate / 2
              << " errors=" << bestSourceError << ',' << bestCandidateError << '\n';
    printFrames("nearest master to source frame zero", actual, nearestCandidate);
    const auto key = [](const std::vector<float> &samples, std::size_t index) {
        std::uint32_t left = 0;
        std::uint32_t right = 0;
        std::memcpy(&left, &samples[index], sizeof(left));
        std::memcpy(&right, &samples[index + 1], sizeof(right));
        return (static_cast<std::uint64_t>(left) << 32) | right;
    };
    std::unordered_multimap<std::uint64_t, std::size_t> sourcePairs;
    constexpr std::size_t anchorSamples = 128 * 2;
    for (std::size_t index = 0; index + anchorSamples <= expected.size(); index += 2)
        sourcePairs.emplace(key(expected, index), index);
    for (std::size_t candidate = 0;
         candidate <= searchEnd && candidate + anchorSamples <= actual.size(); candidate += 2) {
        const auto matches = sourcePairs.equal_range(key(actual, candidate));
        for (auto match = matches.first; match != matches.second; ++match) {
            bool equal = true;
            for (std::size_t sample = 2; sample < anchorSamples; ++sample) {
                if (!sameSample(expected[match->second + sample], actual[candidate + sample])) {
                    equal = false;
                    break;
                }
            }
            if (equal) {
                std::cerr << "diagnostic-only 128-frame match: sourceOffset=" << match->second / 2
                          << " masterCandidate=" << candidate / 2 << '\n';
                return;
            }
        }
    }
    std::cerr << "no diagnostic-only 128-frame match within the permitted prefix\n";
}

void reportClockBoundary(const PcmTrace &producer, const PcmTrace &receiver,
                         std::size_t receiverMark, const char *label)
{
    const auto sourceChunks = producer.currentSegment();
    const auto outputChunks = receiver.since(receiverMark > 0 ? receiverMark - 1 : 0);
    const auto expected = flatten(sourceChunks);
    const auto actual = flatten(outputChunks);
    const GstClockTime currentSourceBase = producer.currentBaseTime();
    const GstClockTime currentProgramBase = receiver.currentBaseTime();
    if (sourceChunks.empty() || expected.size() < 256) return;
    std::cout << label << ": source segment first PTS=" << sourceChunks.front().pts
              << " duration=" << sourceChunks.front().duration << '\n';
    const auto firstPcm = std::find_if(sourceChunks.begin(), sourceChunks.end(),
        [](const Chunk &chunk) { return !chunk.samples.empty(); });
    for (std::size_t candidate = 0; candidate + 256 <= actual.size(); candidate += 2) {
        if (!sameSample(actual[candidate], expected[0])
            || !sameSample(actual[candidate + 1], expected[1])) continue;
        bool equal = true;
        for (std::size_t index = 2; index < 256; ++index) {
            if (!sameSample(actual[candidate + index], expected[index])) { equal = false; break; }
        }
        if (!equal) continue;
        std::size_t offset = 0;
        for (std::size_t index = 0; index < outputChunks.size(); ++index) {
            const auto &chunk = outputChunks[index];
            if (candidate < offset + chunk.samples.size()) {
                std::cout << "receiver source-frame-zero chunk PTS=" << chunk.pts
                          << " duration=" << chunk.duration
                          << " frameOffset=" << (candidate - offset) / 2
                          << " capturedPROGRAMbaseTime=" << chunk.pipelineBaseTime
                          << " currentPROGRAMbaseTime=" << currentProgramBase;
                if (firstPcm != sourceChunks.end()) {
                    const GstClockTime sourceRunning = gst_segment_to_running_time(
                        &firstPcm->segment, GST_FORMAT_TIME, firstPcm->pts);
                    std::cout << " sourcePCMpts=" << firstPcm->pts
                              << " capturedSourceBaseTime=" << firstPcm->pipelineBaseTime
                              << " settledCurrentSourceBaseTime=" << currentSourceBase
                              << " sourceRunningTime=" << sourceRunning;
                    if (GST_CLOCK_TIME_IS_VALID(firstPcm->pipelineBaseTime)
                        && GST_CLOCK_TIME_IS_VALID(sourceRunning)
                        && GST_CLOCK_TIME_IS_VALID(chunk.pipelineBaseTime)) {
                        const gint64 predicted = static_cast<gint64>(firstPcm->pipelineBaseTime)
                            + static_cast<gint64>(sourceRunning) - static_cast<gint64>(chunk.pipelineBaseTime);
                        const GstClockTime actualTime = chunk.pts + gst_util_uint64_scale(
                            (candidate - offset) / 2, GST_SECOND, kRate);
                        std::cout << " capturedBasePredictedPROGRAMpts=" << predicted
                                  << " capturedBaseDeltaNs=" << static_cast<gint64>(actualTime) - predicted;
                    }
                    if (GST_CLOCK_TIME_IS_VALID(currentSourceBase)
                        && GST_CLOCK_TIME_IS_VALID(sourceRunning)
                        && GST_CLOCK_TIME_IS_VALID(currentProgramBase)) {
                        const gint64 predicted = static_cast<gint64>(currentSourceBase)
                            + static_cast<gint64>(sourceRunning) - static_cast<gint64>(currentProgramBase);
                        const GstClockTime actualTime = chunk.pts + gst_util_uint64_scale(
                            (candidate - offset) / 2, GST_SECOND, kRate);
                        std::cout << " settledCurrentPredictedPROGRAMpts=" << predicted
                                  << " settledCurrentDeltaNs=" << static_cast<gint64>(actualTime) - predicted;
                    }
                }
                if (index > 0) {
                    const auto &previous = outputChunks[index - 1];
                    std::cout << " previous PTS=" << previous.pts
                              << " duration=" << previous.duration
                              << " end=" << previous.pts + previous.duration;
                }
                std::cout << '\n';
                return;
            }
            offset += chunk.samples.size();
        }
    }
    std::cout << label << ": receiver has no exact source-frame-zero anchor\n";
}

bool checkSequence(const PcmTrace &producer, const PcmTrace &master,
                   std::size_t mark, int milliseconds, const char *label,
                   bool flushingTransition = false, bool requireFirstFrame = false,
                   bool traceIsBridge = false)
{
    const auto sourceChunks = producer.currentSegment();
    const auto expected = flatten(sourceChunks);
    const auto chunks = master.since(mark);
    const auto actual = flatten(chunks);
    bool ok = require(producer.formatValid() && master.formatValid(),
                      "PCM remains F32LE stereo at 48 kHz");
    const std::size_t needed = static_cast<std::size_t>(milliseconds) * kRate / 1000 * 2;
    std::size_t first = 0;
    while (first + 1 < actual.size() && actual[first] == 0.0F && actual[first + 1] == 0.0F)
        first += 2;
    if (requireFirstFrame) {
        std::size_t expectedSilence = 0;
        while (expectedSilence + 1 < expected.size()
               && expected[expectedSilence] == 0.0F && expected[expectedSilence + 1] == 0.0F)
            expectedSilence += 2;
        // scaletempo can legitimately start a new segment with a zero frame.
        // Retain that reference prefix instead of treating it as output padding.
        first = first >= expectedSilence ? first - expectedSilence : 0;
    }
    if (!flushingTransition && first > static_cast<std::size_t>(kRate / 10 * 2)) {
        std::cerr << label << ": audio onset exceeded 100 ms; silentFrames=" << first / 2 << '\n';
        ok = false;
    }
    constexpr std::size_t anchorSamples = 128 * 2;
    const std::size_t prefixAllowance = flushingTransition ? kRate / 5 * 2 : 0;
    bool matched = false;
    std::size_t referenceOffset = 0;
    for (std::size_t candidate = first;
         candidate <= first + prefixAllowance && candidate + needed <= actual.size();
         candidate += 2) {
        for (std::size_t index = 0; index + needed <= expected.size(); index += 2) {
            if (requireFirstFrame && index != 0) break;
            if (!sameSample(actual[candidate], expected[index])
                || !sameSample(actual[candidate + 1], expected[index + 1])) continue;
            bool anchor = true;
            for (std::size_t sample = 2; sample < anchorSamples; ++sample) {
                if (!sameSample(actual[candidate + sample], expected[index + sample])) {
                    anchor = false;
                    break;
                }
            }
            if (!anchor) continue;
            first = candidate;
            referenceOffset = index;
            matched = true;
            break;
        }
        if (matched || !flushingTransition) break;
    }
    if (!matched) {
        std::cerr << label << ": no unique PCM anchor; sourceFrames=" << expected.size() / 2
                  << " masterFrames=" << actual.size() / 2 << " first=" << first / 2 << '\n';
        reportAlignment(expected, actual, first + prefixAllowance, needed, label, sourceChunks, chunks,
                        producer.currentBaseTime(), master.currentBaseTime());
        return false;
    }
    const auto sourceRunningTimeAt = [&sourceChunks](std::size_t sampleOffset) {
        std::size_t offset = 0;
        for (const auto &chunk : sourceChunks) {
            if (sampleOffset < offset + chunk.samples.size()) {
                if (!GST_CLOCK_TIME_IS_VALID(chunk.pts)) return GST_CLOCK_TIME_NONE;
                const GstClockTime position = chunk.pts + gst_util_uint64_scale(
                    (sampleOffset - offset) / 2, GST_SECOND, kRate);
                return gst_segment_to_running_time(&chunk.segment, GST_FORMAT_TIME, position);
            }
            offset += chunk.samples.size();
        }
        return GST_CLOCK_TIME_NONE;
    };
    GstClockTime previousEnd = GST_CLOCK_TIME_NONE;
    GstClockTime firstOutputTime = GST_CLOCK_TIME_NONE;
    GstClockTime firstSourceTime = GST_CLOCK_TIME_NONE;
    bool reportedInheritedDiscontinuity = false;
    std::size_t chunkOffset = 0;
    for (const auto &chunk : chunks) {
        const std::size_t chunkEnd = chunkOffset + chunk.samples.size();
        if (chunkEnd <= first) { chunkOffset = chunkEnd; continue; }
        if (chunkOffset >= first + needed) break;
        const GstClockTime duration = gst_util_uint64_scale(
            chunk.samples.size() / 2, GST_SECOND, kRate);
        const auto differs = [](GstClockTime a, GstClockTime b) {
            return a > b ? a - b > 1 : b - a > 1;
        };
        bool timestampMatches = true;
        GstClockTime expectedSourceTime = GST_CLOCK_TIME_NONE;
        if (traceIsBridge) {
            const std::size_t checkedSample = (std::max)(chunkOffset, first);
            expectedSourceTime = sourceRunningTimeAt(referenceOffset + checkedSample - first);
            const GstClockTime outputTime = GST_CLOCK_TIME_IS_VALID(chunk.pts)
                ? chunk.pts + gst_util_uint64_scale((checkedSample - chunkOffset) / 2, GST_SECOND, kRate)
                : GST_CLOCK_TIME_NONE;
            timestampMatches = GST_CLOCK_TIME_IS_VALID(expectedSourceTime)
                && GST_CLOCK_TIME_IS_VALID(outputTime);
            if (timestampMatches && !GST_CLOCK_TIME_IS_VALID(firstOutputTime)) {
                firstOutputTime = outputTime;
                firstSourceTime = expectedSourceTime;
            } else if (timestampMatches) {
                const gint64 outputDelta = static_cast<gint64>(outputTime) - static_cast<gint64>(firstOutputTime);
                const gint64 sourceDelta = static_cast<gint64>(expectedSourceTime) - static_cast<gint64>(firstSourceTime);
                timestampMatches = std::abs(outputDelta - sourceDelta) <= 1;
            }
        } else {
            timestampMatches = !GST_CLOCK_TIME_IS_VALID(previousEnd) || !differs(chunk.pts, previousEnd);
        }
        if (!GST_CLOCK_TIME_IS_VALID(chunk.pts)
            || !GST_CLOCK_TIME_IS_VALID(chunk.duration)
            || differs(chunk.duration, duration)
            || !timestampMatches) {
            std::cerr << label << ": discontinuous output timestamp/duration"
                      << " chunkFrame=" << chunkOffset / 2
                      << " sampleFrames=" << chunk.samples.size() / 2
                      << " PTS=" << chunk.pts << " duration=" << chunk.duration
                      << " bytesDuration=" << duration << " previousEnd=" << previousEnd
                      << " bridgeMapping=" << traceIsBridge << " expectedSourceTime=" << expectedSourceTime;
            if (GST_CLOCK_TIME_IS_VALID(chunk.pts) && GST_CLOCK_TIME_IS_VALID(previousEnd))
                std::cerr << " ptsDeltaNs=" << static_cast<gint64>(chunk.pts) - static_cast<gint64>(previousEnd);
            if (GST_CLOCK_TIME_IS_VALID(chunk.duration))
                std::cerr << " durationDeltaNs=" << static_cast<gint64>(chunk.duration) - static_cast<gint64>(duration);
            std::cerr << '\n';
            const std::size_t correspondingSample = referenceOffset
                + (chunkOffset >= first ? chunkOffset - first : 0);
            std::size_t sourceOffset = 0;
            GstClockTime sourcePreviousEnd = GST_CLOCK_TIME_NONE;
            for (const auto &sourceChunk : sourceChunks) {
                if (correspondingSample < sourceOffset + sourceChunk.samples.size()) {
                    std::cerr << "corresponding source chunkFrame=" << sourceOffset / 2
                              << " PTS=" << sourceChunk.pts << " duration=" << sourceChunk.duration
                              << " previousEnd=" << sourcePreviousEnd
                              << " sampleFrames=" << sourceChunk.samples.size() / 2
                              << " segmentRate=" << sourceChunk.segment.rate
                              << " segmentStart=" << sourceChunk.segment.start
                              << " segmentTime=" << sourceChunk.segment.time
                              << " segmentBase=" << sourceChunk.segment.base << '\n';
                    break;
                }
                sourceOffset += sourceChunk.samples.size();
                sourcePreviousEnd = sourceChunk.pts + sourceChunk.duration;
            }
            ok = false;
            break;
        }
        if (traceIsBridge && !reportedInheritedDiscontinuity
            && GST_CLOCK_TIME_IS_VALID(previousEnd) && differs(chunk.pts, previousEnd)) {
            // At 1.5x, scaletempo itself can emit a 30 ms buffer followed only
            // 6.666666 ms later. Preserve that source mapping at this boundary;
            // only the mixed MASTER stream must have continuous sample time.
            std::cout << label << ": inherited source timestamp gap/overlap="
                      << static_cast<gint64>(chunk.pts) - static_cast<gint64>(previousEnd)
                      << " ns; source-relative mapping preserved\n";
            reportedInheritedDiscontinuity = true;
        }
        previousEnd = chunk.pts + chunk.duration;
        chunkOffset = chunkEnd;
    }
    for (std::size_t sample = 0; sample < needed; ++sample) {
        if (!sameSample(actual[first + sample], expected[referenceOffset + sample])) {
            std::cerr << label << ": inserted/dropped/repeated/changed sample at frame "
                      << sample / 2 << " channel " << sample % 2
                      << " expected=" << expected[referenceOffset + sample]
                      << " actual=" << actual[first + sample] << '\n';
            return false;
        }
    }
    std::cout << label << ": " << needed / 2
              << " consecutive stereo frames preserved; sourceOffset="
              << referenceOffset / 2 << '\n';
    return ok;
}

bool setState(GstElement *pipeline, GstState target)
{
    if (!pipeline || gst_element_set_state(pipeline, target) == GST_STATE_CHANGE_FAILURE) return false;
    GstState current = GST_STATE_VOID_PENDING;
    return gst_element_get_state(pipeline, &current, nullptr, 3 * GST_SECOND)
        != GST_STATE_CHANGE_FAILURE && current == target;
}

class FileProducer final {
public:
    ~FileProducer()
    {
        (void)stop();
        if (pipeline) gst_object_unref(pipeline);
        if (volume) gst_object_unref(volume);
    }
    bool initialize(const QString &path, GstClock *clock)
    {
        pipeline = gst_element_factory_make("playbin", "sequence-file-player");
        GstElement *video = gst_element_factory_make("fakesink", nullptr);
        GError *error = nullptr;
        GstElement *audio = gst_parse_bin_from_description(
            "audioconvert ! audioresample ! "
            "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
            "scaletempo ! volume name=sequence-item-volume ! "
            "vtrponaudiosink channel=deck-a-audio sync=true qos=false", TRUE, &error);
        if (error || !pipeline || !video || !audio) {
            if (error) { std::cerr << error->message << '\n'; g_error_free(error); }
            if (video) gst_object_unref(video);
            if (audio) gst_object_unref(audio);
            return false;
        }
        volume = gst_bin_get_by_name(GST_BIN(audio), "sequence-item-volume");
        g_object_set(video, "sync", TRUE, "async", FALSE, nullptr);
        const auto uri = QUrl::fromLocalFile(path).toEncoded();
        g_object_set(pipeline, "uri", uri.constData(), "audio-sink", audio,
                     "video-sink", video, nullptr);
        gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
        return volume != nullptr;
    }
    bool seek(qint64 positionMs, double rate = 1.0)
    {
        return gst_element_seek(pipeline, rate, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            GST_SEEK_TYPE_SET, positionMs * GST_MSECOND,
            GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    }
    bool stop() { return !pipeline || setState(pipeline, GST_STATE_NULL); }
    GstElement *pipeline = nullptr;
    GstElement *volume = nullptr;
};

bool fileSequence(ProgramAudioOutput &output, PcmTrace &master,
                  const QString &path, GstClock *clock, int phaseMs, bool transitions,
                  int steadyMilliseconds = 0, const QString &diagnosticDirectory = {},
                  PcmTrace *finalSink = nullptr)
{
    FileProducer file;
    if (!require(file.initialize(path, clock), "File decode path initializes")) return false;
    const bool diagnostics = !diagnosticDirectory.isEmpty();
    PcmTrace source(file.volume, file.pipeline, "src", diagnostics);
    PcmTrace bridge(output.itemLevel(0), output.pipeline(), "src", diagnostics);
    bool ok = require(source.valid() && bridge.valid() && setState(file.pipeline, GST_STATE_PAUSED),
                      "File CUE prerolls without starting the PROGRAM source");
    if (!ok) { (void)file.stop(); return false; }
    pump(phaseMs);
    output.setDeckAudible(0, true);
    const int initialMs = steadyMilliseconds > 0 ? steadyMilliseconds : (transitions ? 5000 : 600);
    const char *initialLabel = steadyMilliseconds > 0 ? "Media steady initial 19 s"
        : (transitions ? "File initial 5 s" : "File second start phase");
    auto mark = master.mark();
    auto bridgeMark = bridge.mark();
    const std::size_t sinkMark = finalSink ? finalSink->mark() : 0;
    const guint64 playCommandMonotonicNs = static_cast<guint64>(g_get_monotonic_time()) * 1000;
    ok &= require(setState(file.pipeline, GST_STATE_PLAYING), "File PLAY succeeds");
    pump(initialMs + 300);
    ok &= checkSequence(source, master, mark, initialMs, initialLabel, false, true);
    ok &= checkSequence(source, bridge, bridgeMark, initialMs,
                        "File initial pre-mixer sequence starts at first frame", false, true, true);
    if (finalSink)
        ok &= checkSequence(source, *finalSink, sinkMark, initialMs,
                            "Physical final sink input PCM", false, true);
    if (transitions) {
        ok &= require(setState(file.pipeline, GST_STATE_PAUSED), "File PAUSE succeeds");
        pump(250);
        mark = master.mark();
        bridgeMark = bridge.mark();
        ok &= require(setState(file.pipeline, GST_STATE_PLAYING), "File resume succeeds");
        pump(850);
        ok &= checkSequence(source, master, mark, 500, "File pause/resume");
        ok &= checkSequence(source, bridge, bridgeMark, 500, "File pause/resume before mixer", false, false, true);
        ok &= require(setState(file.pipeline, GST_STATE_PAUSED) && file.seek(700),
                      "File paused FLUSH seek succeeds");
        pump(250);
        mark = master.mark();
        bridgeMark = bridge.mark();
        ok &= require(setState(file.pipeline, GST_STATE_PLAYING), "File seek resume succeeds");
        pump(850);
        ok &= checkSequence(source, master, mark, 500, "File paused seek/resume", false, true);
        ok &= checkSequence(source, bridge, bridgeMark, 500, "File paused seek before mixer", false, true, true);
        mark = master.mark();
        bridgeMark = bridge.mark();
        ok &= require(file.seek(1700), "File active FLUSH seek succeeds");
        pump(950);
        ok &= checkSequence(source, master, mark, 500, "File active seek", true, true);
        ok &= checkSequence(source, bridge, bridgeMark, 500, "File active seek before mixer", true, true, true);
        reportClockBoundary(source, bridge, bridgeMark, "File active seek boundary");
        mark = master.mark();
        bridgeMark = bridge.mark();
        ok &= require(file.seek(2700, 1.5), "File 1.5x segment seek succeeds");
        pump(950);
        ok &= checkSequence(source, master, mark, 500, "File post-scaletempo 1.5x", true, true);
        ok &= checkSequence(source, bridge, bridgeMark, 500, "File 1.5x before mixer", true, true, true);
        reportClockBoundary(source, bridge, bridgeMark, "File rate boundary");
    }
    output.setDeckAudible(0, false);
    QElapsedTimer stopping;
    stopping.start();
    ok &= require(file.stop() && stopping.elapsed() < 2000,
                  "File producer stops promptly with a waiting consumer");
    if (diagnostics) {
        const std::size_t sourceEnd = source.mark();
        const std::size_t bridgeEnd = bridge.mark();
        const std::size_t masterEnd = master.mark();
        const std::size_t sinkEnd = finalSink ? finalSink->mark() : 0;
        const QString phase = steadyMilliseconds > 0 ? QStringLiteral("steady-19s")
                                                     : QStringLiteral("phase2-600ms");
        QJsonObject manifest;
        manifest.insert(QStringLiteral("media"), QFileInfo(path).absoluteFilePath());
        manifest.insert(QStringLiteral("output_device_id"), output.appliedDevice());
        manifest.insert(QStringLiteral("requested_pcm_ms"), initialMs);
        manifest.insert(QStringLiteral("play_command_monotonic_ns"), QString::number(playCommandMonotonicNs));
        manifest.insert(QStringLiteral("capture_end_monotonic_ns"), QString::number(
            static_cast<guint64>(g_get_monotonic_time()) * 1000));
        manifest.insert(QStringLiteral("producer_mark"), 0);
        manifest.insert(QStringLiteral("producer_end"), static_cast<qint64>(sourceEnd));
        manifest.insert(QStringLiteral("bridge_mark"), static_cast<qint64>(bridgeMark));
        manifest.insert(QStringLiteral("bridge_end"), static_cast<qint64>(bridgeEnd));
        manifest.insert(QStringLiteral("master_mark"), static_cast<qint64>(mark));
        manifest.insert(QStringLiteral("master_end"), static_cast<qint64>(masterEnd));
        manifest.insert(QStringLiteral("sink_mark"), static_cast<qint64>(sinkMark));
        manifest.insert(QStringLiteral("sink_end"), static_cast<qint64>(sinkEnd));
        manifest.insert(QStringLiteral("capture_note"), QStringLiteral(
            "Producer includes preroll. Bridge, MASTER, and final sink marks precede PLAY. "
            "Raw byte offsets and caps are in each JSONL. Sink input is before internal WASAPI clock correction. "
            "No output-device settings are saved."));
        QFile manifestFile(QDir(diagnosticDirectory).filePath(phase + QStringLiteral("-manifest.json")));
        const QByteArray manifestBytes = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
        ok &= require(manifestFile.open(QIODevice::WriteOnly)
                          && manifestFile.write(manifestBytes) == manifestBytes.size() && manifestFile.flush(),
                      "physical phase marks and command timestamp are saved");
        ok &= require(saveTrace(source, 0, diagnosticDirectory, phase + QStringLiteral("-producer"), sourceEnd)
                          && saveTrace(bridge, bridgeMark, diagnosticDirectory, phase + QStringLiteral("-bridge"), bridgeEnd)
                          && saveTrace(master, mark, diagnosticDirectory, phase + QStringLiteral("-master"), masterEnd)
                          && (!finalSink || saveTrace(*finalSink, sinkMark, diagnosticDirectory,
                                                    phase + QStringLiteral("-sink-input"), sinkEnd)),
                      "physical PCM and per-buffer timing diagnostics are saved after producer stop");
    }
    pump(250);
    return ok;
}

bool bgmWithoutConsumer(const QString &path)
{
    BgmPlayback bgm;
    if (!require(bgm.initialize(nullptr, nullptr) && bgm.load(path, 100, 600),
                 "BGM initializes without a PROGRAM consumer")) return false;
    const bool ready = await([&] { bgm.pollBus(); return bgm.status() == BgmPlayback::Status::Ready; });
    bool ok = require(ready && bgm.play(), "unconsumed BGM can preroll and play");
    QString detail;
    bool ended = false;
    ok &= require(await([&] {
        bgm.pollBus();
        ended = bgm.takeEvent(detail) == BgmPlayback::Event::EndOfStream;
        return ended || bgm.status() == BgmPlayback::Status::Error;
    }) && ended, "unconsumed BGM reaches source EOS without blocking");
    ok &= require(bgm.shutdown(), "unconsumed BGM shutdown completes");
    return ok;
}

bool bgmSequence(ProgramAudioOutput &output, PcmTrace &master,
                 const QString &path, GstClock *clock)
{
    BgmPlayback bgm;
    if (!require(bgm.initialize(nullptr, nullptr), "real BGM path initializes")) return false;
    gst_pipeline_use_clock(GST_PIPELINE(bgm.pipeline()), clock);
    GstElement *volume = gst_bin_get_by_name(GST_BIN(bgm.pipeline()), "bgm-item-audio-volume");
    // The configured audio bin is not necessarily added to playbin until load.
    if (!volume) {
        GstElement *audio = nullptr;
        g_object_get(bgm.pipeline(), "audio-sink", &audio, nullptr);
        if (audio) {
            volume = gst_bin_get_by_name(GST_BIN(audio), "bgm-item-audio-volume");
            gst_object_unref(audio);
        }
    }
    PcmTrace source(volume, bgm.pipeline());
    if (volume) gst_object_unref(volume);
    GstElement *receiver = gst_bin_get_by_name(GST_BIN(output.pipeline()), "bgm-audio-source");
    PcmTrace bridge(receiver, output.pipeline());
    if (receiver) gst_object_unref(receiver);
    bool ok = require(source.valid() && bridge.valid() && bgm.load(path, 250, -1),
                      "BGM source and bridge PCM observers attach");
    ok &= require(await([&] { bgm.pollBus(); return bgm.status() == BgmPlayback::Status::Ready; }),
                  "real BGM reaches Ready");
    output.setBgmTransportGain(1.0);
    auto mark = master.mark();
    auto bridgeMark = bridge.mark();
    ok &= require(bgm.play(), "BGM PLAY succeeds");
    pump(1100);
    ok &= checkSequence(source, master, mark, 700, "BGM initial play", false, true);
    ok &= checkSequence(source, bridge, bridgeMark, 700, "BGM initial before mixer", false, true, true);
    ok &= require(bgm.pause(), "BGM PAUSE succeeds");
    pump(250);
    ok &= require(bgm.seek(1700), "BGM paused seek succeeds");
    pump(150);
    mark = master.mark();
    bridgeMark = bridge.mark();
    ok &= require(bgm.play(), "BGM resume succeeds");
    pump(1000);
    ok &= checkSequence(source, master, mark, 600, "BGM pause/seek/resume", false, true);
    ok &= checkSequence(source, bridge, bridgeMark, 600, "BGM seek before mixer", false, true, true);
    mark = master.mark();
    bridgeMark = bridge.mark();
    ok &= require(bgm.seek(250) && bgm.play(), "BGM repeat-style seek succeeds");
    pump(1000);
    ok &= checkSequence(source, master, mark, 600, "BGM repeat segment", true, true);
    ok &= checkSequence(source, bridge, bridgeMark, 600, "BGM repeat before mixer", true, true, true);
    reportClockBoundary(source, bridge, bridgeMark, "BGM repeat boundary");
    output.setBgmTransportGain(0.0);
    QElapsedTimer stopping;
    stopping.start();
    ok &= require(bgm.shutdown() && stopping.elapsed() < 2000, "BGM shutdown promptly wakes its consumer");
    return ok;
}

bool bgmSurvivesFileFlushes(ProgramAudioOutput &output, PcmTrace &master,
                          const QString &filePath, const QString &constantPath,
                          GstClock *clock)
{
    FileProducer file;
    BgmPlayback bgm;
    if (!require(file.initialize(filePath, clock) && bgm.initialize(nullptr, nullptr),
                 "independent File and BGM isolation producers initialize")) return false;
    gst_pipeline_use_clock(GST_PIPELINE(bgm.pipeline()), clock);
    bool ok = require(setState(file.pipeline, GST_STATE_PAUSED)
                          && bgm.load(constantPath, 0, -1),
                      "File preroll and constant BGM load succeed");
    ok &= require(await([&] { bgm.pollBus(); return bgm.status() == BgmPlayback::Status::Ready; }),
                  "constant BGM reaches Ready");
    output.setDeckAudible(0, false);
    output.setDeckAudible(1, false);
    output.setBgmTransportGain(1.0);
    ok &= require(setState(file.pipeline, GST_STATE_PLAYING) && bgm.play(),
                  "muted File and audible constant BGM start together");
    pump(250);
    const auto mark = master.mark();
    pump(100);
    constexpr std::array<std::pair<qint64, double>, 5> operations = {{
        {1400, 1.0}, {2100, 1.5}, {700, 1.0}, {2500, 0.75}, {900, 1.0}}};
    for (const auto &operation : operations) {
        ok &= require(file.seek(operation.first, operation.second),
                      "muted File seek/rate transition succeeds while BGM plays");
        pump(200);
    }
    pump(100);
    const auto chunks = master.since(mark);
    std::size_t frames = 0;
    GstClockTime previousEnd = GST_CLOCK_TIME_NONE;
    bool preserved = master.formatValid();
    for (const auto &chunk : chunks) {
        const auto differs = [](GstClockTime a, GstClockTime b) {
            return a > b ? a - b > 1 : b - a > 1;
        };
        if (!GST_CLOCK_TIME_IS_VALID(chunk.pts)
            || !GST_CLOCK_TIME_IS_VALID(chunk.duration)
            || differs(chunk.duration, gst_util_uint64_scale(chunk.samples.size() / 2, GST_SECOND, kRate))
            || (GST_CLOCK_TIME_IS_VALID(previousEnd) && differs(chunk.pts, previousEnd))) {
            std::cerr << "BGM isolation: discontinuous MASTER timestamp after frame=" << frames
                      << " previousEnd=" << previousEnd << " PTS=" << chunk.pts << '\n';
            preserved = false;
            break;
        }
        for (std::size_t sample = 0; sample < chunk.samples.size(); ++sample) {
            if (!sameSample(chunk.samples[sample], 0.125F)) {
                std::cerr << "BGM isolation: changed MASTER sample at frame=" << frames + sample / 2
                          << " channel=" << sample % 2 << " expected=0.125 actual="
                          << chunk.samples[sample] << '\n';
                preserved = false;
                break;
            }
        }
        if (!preserved) break;
        frames += chunk.samples.size() / 2;
        previousEnd = chunk.pts + chunk.duration;
    }
    ok &= require(preserved && frames >= static_cast<std::size_t>(kRate),
                  "File-only flushing preserves every BGM MASTER sample and timestamp");
    std::cout << "BGM isolation across five File seeks/rate changes: " << frames
              << " consecutive constant stereo frames\n";
    output.setBgmTransportGain(0.0);
    ok &= require(file.stop() && bgm.shutdown(), "isolation producers shut down");
    pump(100);
    return ok;
}

bool largeChunkSequence(ProgramAudioOutput &output, PcmTrace &master, GstClock *clock)
{
    // PCM MOV demuxers can deliver 500.5 ms at once, larger than the bridge's
    // 200 ms backlog limit. That is valid PCM, not a stalled consumer.
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "audiotestsrc samplesperbuffer=24024 wave=saw freq=123.456 volume=0.125 ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
        "volume name=large-chunk-reference ! "
        "vtrponaudiosink channel=deck-a-audio sync=true qos=false", &error);
    if (error || !pipeline) {
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return require(false, "large PCM chunk producer initializes");
    }
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
    GstElement *reference = gst_bin_get_by_name(GST_BIN(pipeline), "large-chunk-reference");
    PcmTrace source(reference, pipeline);
    if (reference) gst_object_unref(reference);
    PcmTrace bridge(output.itemLevel(0), output.pipeline());
    output.setDeckAudible(0, true);
    output.setDeckAudible(1, false);
    output.setBgmTransportGain(0.0);
    const auto mark = master.mark();
    const auto bridgeMark = bridge.mark();
    bool ok = require(source.valid() && bridge.valid() && setState(pipeline, GST_STATE_PLAYING),
                      "large PCM chunk playback starts");
    pump(1600);
    ok &= checkSequence(source, master, mark, 1000, "500.5 ms PCM chunks reach MASTER", false, true);
    ok &= checkSequence(source, bridge, bridgeMark, 1000, "500.5 ms PCM chunks reach ITEM", false, true, true);
    output.setDeckAudible(0, false);
    pump(150);
    const auto mutedMark = master.mark();
    pump(150);
    const auto muted = master.since(mutedMark);
    ok &= require(!muted.empty() && std::all_of(muted.begin(), muted.end(), [](const Chunk &chunk) {
        return std::all_of(chunk.samples.begin(), chunk.samples.end(), [](float sample) { return sample == 0.0F; });
    }), "muting a large PCM block does not leave half a second of old audio in the mixer");
    ok &= require(setState(pipeline, GST_STATE_NULL), "large PCM chunk producer stops");
    gst_object_unref(pipeline);
    pump(100);
    return ok;
}

bool liveSequence(ProgramAudioOutput &output, PcmTrace &master, GstClock *clock)
{
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(
        "audiotestsrc is-live=true samplesperbuffer=1024 wave=saw freq=123.456 volume=0.125 ! "
        "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
        "volume name=live-reference ! "
        "vtrponaudiosink name=retiring-live-sink channel=deck-a-audio sync=true qos=false", &error);
    if (error || !pipeline) {
        if (error) { std::cerr << "Live input: " << error->message << '\n'; g_error_free(error); }
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
    GstElement *reference = gst_bin_get_by_name(GST_BIN(pipeline), "live-reference");
    PcmTrace source(reference, pipeline);
    if (reference) gst_object_unref(reference);
    PcmTrace bridge(output.itemLevel(0), output.pipeline());
    GstBus *bus = gst_element_get_bus(pipeline);
    bool busError = false;
    int sourceLatencyMessages = 0;
    int programLatencyMessages = 0;
    const auto poll = [&busError](GstElement *owner, GstBus *messages, int &latencyMessages) {
        if (!messages) { busError = true; return; }
        while (GstMessage *message = gst_bus_pop(messages)) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_LATENCY) {
                ++latencyMessages;
                if (!gst_bin_recalculate_latency(GST_BIN(owner))) {
                    std::cerr << "Live input: latency recalculation failed for " << GST_OBJECT_NAME(owner) << '\n';
                    busError = true;
                }
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *failure = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &failure, &debug);
                std::cerr << "Live input bus error: " << (failure ? failure->message : "unknown")
                          << ' ' << (debug ? debug : "") << '\n';
                g_clear_error(&failure);
                g_free(debug);
                busError = true;
            }
            gst_message_unref(message);
        }
    };
    QTimer busTimer;
    busTimer.setTimerType(Qt::PreciseTimer);
    busTimer.setInterval(100);
    QObject::connect(&busTimer, &QTimer::timeout, &busTimer, [&] {
        // Match production's 100 ms PROGRAM-before-producer bus polling.
        // Recalculating latency from a streaming-thread sync handler can deadlock.
        poll(output.pipeline(), output.bus(), programLatencyMessages);
        poll(pipeline, bus, sourceLatencyMessages);
    });
    output.setBgmTransportGain(0.0);
    output.setDeckAudible(0, true);
    output.setDeckAudible(1, false);
    const auto mark = master.mark();
    const auto bridgeMark = bridge.mark();
    busTimer.start();
    bool ok = require(source.valid() && bridge.valid()
                          && gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
                      "live input starts against the already-running PROGRAM graph");
    pump(1100);
    busTimer.stop();
    poll(output.pipeline(), output.bus(), programLatencyMessages);
    poll(pipeline, bus, sourceLatencyMessages);
    ok &= require(!busError && sourceLatencyMessages > 0,
                  "live producer latency is discovered and redistributed without bus errors");
    ok &= checkSequence(source, master, mark, 500, "Live source initial MASTER", false, true);
    ok &= checkSequence(source, bridge, bridgeMark, 500, "Live source initial before mixer", false, true, true);
    std::cout << "Live source LATENCY messages: producer=" << sourceLatencyMessages
              << " PROGRAM=" << programLatencyMessages << '\n';
    GstElement *retiringSink = gst_bin_get_by_name(GST_BIN(pipeline), "retiring-live-sink");
    ok &= require(retiringSink != nullptr, "live producer exposes the retiring bridge sink");
    vtrpon::disconnectProgramAudioProducer(retiringSink);
    if (retiringSink) gst_object_unref(retiringSink);
    pump(250); // Let PCM already downstream of the bridge drain.
    const auto disconnectedMark = bridge.mark();
    pump(250);
    ok &= require(bridge.mark() == disconnectedMark,
                  "disconnected live producer cannot deliver more PCM before physical stop");
    output.setDeckAudible(0, false);
    ok &= require(setState(pipeline, GST_STATE_NULL), "live input stops promptly");
    // A late state transition on a retired producer must not reclaim its old
    // channel. Real CUE recovery always creates a new producer instance.
    (void)gst_element_set_state(pipeline, GST_STATE_PLAYING);
    pump(250);
    ok &= require(bridge.mark() == disconnectedMark,
                  "a retired producer cannot reconnect after a late start");
    ok &= require(setState(pipeline, GST_STATE_NULL), "retired producer stops after late start");
    if (bus) gst_object_unref(bus);
    gst_object_unref(pipeline);
    return ok;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    gst_init(nullptr, nullptr);
    if (!require(vtrpon::registerProgramAudioBridge(), "PROGRAM bridge factories register")) return 1;
    QTemporaryDir directory;
    const QString uniquePath = directory.isValid() ? createSequence(directory.path()) : QString{};
    if (!require(!uniquePath.isEmpty(), "unique temporary PCM sequence is created")) return 1;
    QString path = uniquePath;
    const QStringList arguments = app.arguments();
    const int mediaOption = arguments.indexOf(QStringLiteral("--media"));
    const bool mediaTransitions = arguments.contains(QStringLiteral("--media-transitions"));
    const int deviceOption = arguments.indexOf(QStringLiteral("--output-device"));
    const bool physicalOutput = deviceOption >= 0;
    QString deviceId;
    QString diagnosticDirectory;
    if (physicalOutput) {
        if (!require(deviceOption + 1 < arguments.size(), "--output-device requires an explicit nonempty device ID")) return 1;
        deviceId = arguments.at(deviceOption + 1);
        if (!require(!deviceId.trimmed().isEmpty() && !deviceId.startsWith(QStringLiteral("--"))
                          && mediaOption >= 0 && !mediaTransitions,
                      "physical mode requires --output-device <id> --media <path> and permits only steady playback")) return 1;
        const int directoryOption = arguments.indexOf(QStringLiteral("--diagnostic-dir"));
        if (directoryOption >= 0) {
            if (!require(directoryOption + 1 < arguments.size(), "--diagnostic-dir requires a local output directory")) return 1;
            diagnosticDirectory = QFileInfo(arguments.at(directoryOption + 1)).absoluteFilePath();
        } else {
            diagnosticDirectory = QDir(QDir::tempPath()).filePath(QStringLiteral("vtrpon-audio-device-")
                + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
        }
        if (!require(QDir().mkpath(diagnosticDirectory)
                          && !QFileInfo::exists(QDir(diagnosticDirectory).filePath(QStringLiteral("steady-19s-manifest.json"))),
                      "physical diagnostics have a fresh writable output directory")) return 1;
        std::cout << "PHYSICAL OUTPUT: explicit device only; 19 s steady + second start phase; no BGM/seek/live tones.\n"
                  << "Diagnostic directory: " << diagnosticDirectory.toStdString() << '\n';
    }
    if (!require(!mediaTransitions || mediaOption >= 0,
                 "--media-transitions requires --media <path>")) return 1;
    if (mediaOption >= 0) {
        if (!require(mediaOption + 1 < arguments.size(), "--media requires a local media path")) return 1;
        path = QFileInfo(arguments.at(mediaOption + 1)).absoluteFilePath();
        if (!require(QFileInfo::exists(path), "optional media fixture exists")) return 1;
        std::cout << (mediaTransitions
            ? "AAC/media transition diagnostics; original assertion limits retained: "
            : "AAC/media steady validation; active seeks not assessed: ") << path.toStdString() << '\n';
    }
    const QString constantPath = directory.isValid() ? createSequence(directory.path(), true) : QString{};
    if (!require(!constantPath.isEmpty(), "temporary constant PCM isolation fixture is created")) return 1;
    bool ok = physicalOutput || bgmWithoutConsumer(uniquePath);
    ProgramAudioOutput output;
    if (!require(output.initialize(nullptr, nullptr, deviceId, physicalOutput ? "wasapi2sink" : "fakesink"),
                 "PROGRAM graph initializes with the explicitly selected test sink")) return 1;
    GstElement *sink = gst_bin_get_by_name(GST_BIN(output.pipeline()), "program-audio-output");
    if (!require(sink != nullptr, "PROGRAM output sink is available")) return 1;
    if (!physicalOutput) g_object_set(sink, "sync", TRUE, nullptr);
    gboolean synchronized = FALSE;
    g_object_get(sink, "sync", &synchronized, nullptr);
    std::unique_ptr<PcmTrace> sinkInput;
    if (physicalOutput) sinkInput = std::make_unique<PcmTrace>(sink, output.pipeline(), "sink", true);
    gst_object_unref(sink);
    if (!require(synchronized && (!sinkInput || sinkInput->valid()),
                 "output follows the pipeline clock and its diagnostic pad is available")) return 1;
    GstClock *clock = gst_system_clock_obtain();
    gst_pipeline_use_clock(GST_PIPELINE(output.pipeline()), clock);
    output.setBgmTransportGain(0.0);
    PcmTrace master(output.masterLevel(), output.pipeline(), "src", physicalOutput);
    output.updateDevice(true);
    if (!require(master.valid() && await([&] {
            output.updateDevice(true); return output.ready() && !output.deviceChanging();
        }), "PROGRAM runs before any producer starts")) {
        (void)output.stop();
        gst_object_unref(clock);
        return 1;
    }
    if (physicalOutput) {
        GstElement *activeSink = gst_bin_get_by_name(GST_BIN(output.pipeline()), "program-audio-output");
        QJsonObject properties;
        for (const char *name : {"device", "sync", "async", "provide-clock", "slave-method", "buffer-time",
                                 "latency-time", "alignment-threshold", "drift-tolerance", "discont-wait", "low-latency"}) {
            GParamSpec *property = activeSink ? g_object_class_find_property(G_OBJECT_GET_CLASS(activeSink), name) : nullptr;
            if (!property || !(property->flags & G_PARAM_READABLE)) continue;
            GValue value = G_VALUE_INIT;
            g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(property));
            g_object_get_property(G_OBJECT(activeSink), name, &value);
            gchar *description = g_strdup_value_contents(&value);
            properties.insert(QString::fromLatin1(name), QString::fromUtf8(description ? description : ""));
            g_free(description);
            g_value_unset(&value);
        }
        if (activeSink) gst_object_unref(activeSink);
        properties.insert(QStringLiteral("pipeline_clock_name"), QString::fromUtf8(GST_OBJECT_NAME(clock)));
        properties.insert(QStringLiteral("pipeline_base_time_ns"), QString::number(gst_element_get_base_time(output.pipeline())));
        QFile propertyFile(QDir(diagnosticDirectory).filePath(QStringLiteral("device-properties.json")));
        const QByteArray propertyBytes = QJsonDocument(properties).toJson(QJsonDocument::Indented);
        ok &= require(propertyFile.open(QIODevice::WriteOnly)
                          && propertyFile.write(propertyBytes) == propertyBytes.size() && propertyFile.flush(),
                      "physical sink clock and buffer properties are saved");
    }
    ok &= fileSequence(output, master, path, clock, 7, mediaOption < 0 || mediaTransitions,
                       mediaOption >= 0 && !mediaTransitions ? 19000 : 0,
                       diagnosticDirectory, sinkInput.get());
    ok &= fileSequence(output, master, path, clock, 19, false, 0, diagnosticDirectory, sinkInput.get());
    if (!physicalOutput) {
        ok &= bgmSequence(output, master, uniquePath, clock);
        ok &= bgmSurvivesFileFlushes(output, master, uniquePath, constantPath, clock);
        ok &= largeChunkSequence(output, master, clock);
        ok &= liveSequence(output, master, clock);
    }
    QElapsedTimer stopping;
    stopping.start();
    ok &= require(output.stop() && stopping.elapsed() < 2000,
                  "PROGRAM shutdown wakes sources with no remaining producers");
    gst_object_unref(clock);
    std::cout << "PROGRAM audio bridge sequence: " << (ok ? "PASS" : "FAIL") << '\n';
    return ok ? 0 : 1;
}
