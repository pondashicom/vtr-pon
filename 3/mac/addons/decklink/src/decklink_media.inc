// Included inside the SDK implementation's anonymous namespace.
struct OwnedPacket {
    VtrPonDeckLinkPacket info{};
    std::vector<uint8_t> video;
    std::vector<int32_t> audio;
};
struct InputState {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<OwnedPacket> packets;
    OwnedPacket borrowed;
    BMDDisplayMode pendingMode = bmdModeUnknown;
    BMDPixelFormat pendingPixel = bmdFormat8BitYUV;
    BMDVideoInputFlags flags = bmdVideoInputFlagDefault;
    uint32_t field = 0, duration = 1001, scale = 30000;
    bool stopping = false;
};
struct InputSession {
    ComPtr<IDeckLinkInput> input;
    ComPtr<IDeckLinkConfiguration> config;
    std::shared_ptr<InputState> state = std::make_shared<InputState>();
    bool stopped = false;
    bool callbackSet = false, videoEnabled = false, audioEnabled = false, streamsStarted = false;
    void stop() {
        if (stopped) return; stopped = true;
        { std::lock_guard lock(state->mutex); state->stopping = true; }
        if (input) {
            if (streamsStarted) input->StopStreams();
            if (callbackSet) input->SetCallback(nullptr);
            if (audioEnabled) input->DisableAudioInput();
            if (videoEnabled) input->DisableVideoInput();
        }
    }
    ~InputSession() { stop(); }
};
class InputCallback final : public IDeckLinkInputCallback {
    std::atomic<ULONG> references{1};
    std::weak_ptr<InputState> state;
public:
    explicit InputCallback(const std::shared_ptr<InputState> &value) : state(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (sameIid(iid, IUnknownUUID) || sameIid(iid, IID_IDeckLinkInputCallback)) { *out = static_cast<IDeckLinkInputCallback *>(this); AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --references; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
        IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags detected) override {
        auto s = state.lock(); if (!s || !mode) return S_OK;
        BMDTimeValue duration = 0; BMDTimeScale scale = 0;
        if (FAILED(mode->GetFrameRate(&duration, &scale)) || duration <= 0 || scale <= 0 || scale > UINT32_MAX) return E_FAIL;
        std::lock_guard lock(s->mutex);
        s->pendingMode = mode->GetDisplayMode();
        s->pendingPixel = (detected & bmdDetectedVideoInputRGB444) ? bmdFormat8BitBGRA : bmdFormat8BitYUV;
        s->field = mode->GetFieldDominance(); s->duration = static_cast<uint32_t>(duration); s->scale = static_cast<uint32_t>(scale);
        s->wake.notify_one(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame *frame, IDeckLinkAudioInputPacket *audio) override {
        auto s = state.lock(); if (!s) return S_OK;
        try {
            OwnedPacket packet; auto &p = packet.info; p.structSize = sizeof(p);
            { std::lock_guard lock(s->mutex); if (s->stopping) return S_OK; p.fieldDominance = s->field; p.frameDuration = s->duration; p.timeScale = s->scale; }
            if (frame) {
                p.noSignal = !!(frame->GetFlags() & bmdFrameHasNoInputSource);
                p.width = frame->GetWidth(); p.height = frame->GetHeight(); p.rowBytes = frame->GetRowBytes();
                p.pixelFormat = frame->GetPixelFormat() == bmdFormat8BitBGRA ? VtrPonDeckLinkBgra : VtrPonDeckLinkUyvy;
                frame->GetStreamTime(&p.videoTime100ns, &p.frameDuration100ns, 10000000);
                const uint64_t size = uint64_t(p.rowBytes) * p.height;
                if (!p.noSignal && p.width && p.width <= 4096 && p.height && p.height <= 2160 &&
                    p.rowBytes >= p.width * (p.pixelFormat == VtrPonDeckLinkBgra ? 4u : 2u) && size <= 64u * 1024u * 1024u) {
                    ComPtr<IDeckLinkVideoBuffer> buffer; void *bytes = nullptr; uint64_t available = 0;
                    if (SUCCEEDED(frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void **>(buffer.GetAddressOf()))) &&
                        SUCCEEDED(buffer->StartAccess(bmdBufferAccessRead))) {
                        if (SUCCEEDED(buffer->GetBytes(&bytes)) && SUCCEEDED(buffer->GetSize(&available)) && bytes && available >= size) {
                            // End access even when the owned frame allocation fails.
                            try { packet.video.assign(static_cast<uint8_t *>(bytes), static_cast<uint8_t *>(bytes) + size); }
                            catch (...) { buffer->EndAccess(bmdBufferAccessRead); throw; }
                        }
                        buffer->EndAccess(bmdBufferAccessRead);
                    }
                    p.videoBytes = static_cast<uint32_t>(packet.video.size());
                }
            }
            if (audio) {
                const auto count = audio->GetSampleFrameCount(); void *bytes = nullptr;
                if (count > 0 && count <= 48000 && SUCCEEDED(audio->GetBytes(&bytes)) && bytes) {
                    packet.audio.assign(static_cast<int32_t *>(bytes), static_cast<int32_t *>(bytes) + size_t(count) * 2);
                    p.audioFrames = count; audio->GetPacketTime(&p.audioTime100ns, 10000000);
                }
            }
            std::lock_guard lock(s->mutex); if (s->stopping) return S_OK;
            if (s->packets.size() == 3) s->packets.pop_front();
            s->packets.push_back(std::move(packet)); s->wake.notify_one(); return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
};
struct OutputState {
    ComPtr<IDeckLinkOutput> output;
    ComPtr<IDeckLinkConfiguration> config;
    ComPtr<IDeckLinkStatus> status;
    int64_t groupId = 0, previousGroup = 0, boardId = 0;
    bool internalReference = false;
    bool videoEnabled = false, audioEnabled = false;
    ComPtr<IDeckLinkVideoConversion> converter;
    int width = 0, height = 0, rowBytes = 0;
    BMDPixelFormat pixel = bmdFormat8BitBGRA;
    BMDTimeValue duration = 0, nextVideo = 0, nextAudio = 0;
    BMDTimeScale scale = 0;
    std::vector<uint8_t> latest;
    struct TimedVideo { int64_t time; std::vector<uint8_t> bytes; };
    struct TimedAudio { int64_t start; std::vector<int32_t> samples; };
    std::deque<TimedVideo> video;
    std::deque<TimedAudio> audio;
    int64_t epoch = 0, latency100ns = 0, latencySamples = 0;
    size_t audioSamples = 0;
    bool running = false;
    ~OutputState() {
        if (output) {
            if (running) { BMDTimeValue stop = 0; output->StopScheduledPlayback(0, &stop, scale); }
            if (audioEnabled) output->DisableAudioOutput();
            if (videoEnabled) output->DisableVideoOutput();
        }
        if (config && groupId) config->SetInt(bmdDeckLinkConfigPlaybackGroup, previousGroup);
    }
};
void closeMedia(Context *c) noexcept { if (c->inputState) std::static_pointer_cast<InputSession>(c->inputState)->stop(); c->inputState.reset(); c->outputState.reset(); }
template<class Fn> int32_t mediaCall(void *handle, char *error, uint32_t capacity, Fn &&fn) noexcept {
    const auto status = check(handle, error, capacity); if (status) return status;
    try { return fn(*static_cast<Context *>(handle)); }
    catch (...) { message(error, capacity, "DeckLink media allocation or backend operation failed."); return VtrPonDeckLinkBackendError; }
}
// Older cards (including HD Extreme 3D) have no PersistentID. As in the SDK
// samples, their topology ID identifies the installed PCIe location instead.
// Use exactly the same choice for discovery and opening a saved channel.
bool channelIdentity(IDeckLinkProfileAttributes *attrs, int64_t &id) {
    return SUCCEEDED(attrs->GetInt(BMDDeckLinkPersistentID, &id)) ||
        SUCCEEDED(attrs->GetInt(BMDDeckLinkTopologicalID, &id));
}
int32_t VTRPON_DECKLINK_CALL channels(void *handle, VtrPonDeckLinkChannel *out, uint32_t capacity,
    uint32_t *count, char *error, uint32_t errorCapacity) noexcept {
    if (count) *count = 0;
    return mediaCall(handle, error, errorCapacity, [&](Context &c) -> int32_t {
        if (!count || (!out && capacity) || !c.snapshot) return VtrPonDeckLinkInvalidArgument;
        std::vector<VtrPonDeckLinkChannel> records;
        for (uint32_t i = 0; i < c.devices.size(); ++i) {
            ComPtr<IDeckLinkProfileAttributes> attrs;
            if (FAILED(c.devices[i].As(std::addressof(attrs)))) continue;
            int64_t id = 0, sub = 0, support = 0;
            const bool stable = channelIdentity(attrs.Get(), id);
            attrs->GetInt(BMDDeckLinkSubDeviceIndex, &sub); attrs->GetInt(BMDDeckLinkVideoIOSupport, &support);
            for (uint32_t direction : {1u, 2u}) {
                if (!(support & (direction == 1 ? bmdDeviceSupportsCapture : bmdDeviceSupportsPlayback))) continue;
                int64_t connections = 0;
                if (FAILED(attrs->GetInt(direction == 1 ? BMDDeckLinkVideoInputConnections : BMDDeckLinkVideoOutputConnections, &connections))) continue;
                for (uint32_t bit = 1; bit && bit <= 0x80; bit <<= 1) if (connections & bit) {
                    VtrPonDeckLinkChannel channel{}; channel.structSize = sizeof(channel); channel.deviceIndex = i;
                    channel.direction = direction; channel.connection = bit; channel.persistentId = id;
                    channel.subDeviceIndex = static_cast<uint32_t>(sub); channel.stableId = stable;
                    SdkString name; if (SUCCEEDED(c.devices[i]->GetDisplayName(&name.value))) utf8(name.value, channel.nameUtf8, sizeof(channel.nameUtf8));
                    records.push_back(channel);
                }
            }
        }
        // Never silently select one of two devices with the same fallback key.
        for (auto &record : records) for (const auto &other : records)
            if (record.deviceIndex != other.deviceIndex && record.persistentId == other.persistentId &&
                record.subDeviceIndex == other.subDeviceIndex) record.stableId = 0;
        return copy(records, out, capacity, count, error, errorCapacity);
    });
}
ComPtr<IDeckLink> selected(Context &c, const VtrPonDeckLinkStreamOptions *options, uint32_t direction) {
    if (!options || options->structSize != sizeof(*options) || !options->connection ||
        (options->connection & (options->connection - 1))) return {};
    ComPtr<IDeckLink> match;
    for (auto &device : c.devices) {
        ComPtr<IDeckLinkProfileAttributes> a; int64_t id = 0, sub = 0, connections = 0;
        if (FAILED(device.As(std::addressof(a))) || !channelIdentity(a.Get(), id)) continue;
        a->GetInt(BMDDeckLinkSubDeviceIndex, &sub);
        a->GetInt(direction == 1 ? BMDDeckLinkVideoInputConnections : BMDDeckLinkVideoOutputConnections, &connections);
        if (id == options->persistentId && sub == options->subDeviceIndex && (connections & options->connection)) {
            if (match) return {};
            match = device;
        }
    }
    return match;
}
bool outputFailed(HRESULT result, const char *operation, char *error, uint32_t cap);
bool directionAvailable(const ComPtr<IDeckLink> &device, bool input, char *error, uint32_t cap) {
    ComPtr<IDeckLinkProfileAttributes> attrs; int64_t duplex = 0, busy = 0;
    ComPtr<IDeckLinkStatus> status;
    if (FAILED(device.As(std::addressof(status))) || FAILED(status->GetInt(bmdDeckLinkStatusBusy, &busy))) return true;
    if (busy & (input ? bmdDeviceCaptureBusy : bmdDevicePlaybackBusy)) {
        message(error, cap, input ? "This DeckLink input channel is already in use." : "This DeckLink output channel is already in use.");
        return false;
    }
    if (SUCCEEDED(device.As(std::addressof(attrs))) && SUCCEEDED(attrs->GetInt(BMDDeckLinkDuplex, &duplex)) && duplex == bmdDuplexHalf &&
        (busy & (input ? bmdDevicePlaybackBusy : bmdDeviceCaptureBusy))) {
        message(error, cap, input ? "This DeckLink channel is half-duplex and is outputting. Disable its output before capturing."
            : "This DeckLink channel is half-duplex and is capturing. Release its input before enabling output.");
        return false;
    }
    return true;
}
int32_t VTRPON_DECKLINK_CALL openInput(void *handle, const VtrPonDeckLinkStreamOptions *options, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [&](Context &c) -> int32_t {
        if (c.inputState) return VtrPonDeckLinkInvalidArgument;
        auto device = selected(c, options, 1); if (!device) { message(error, cap, "Selected input channel is unavailable."); return VtrPonDeckLinkInvalidArgument; }
        ComPtr<IDeckLinkProfileAttributes> attrs; bool detect = false;
        if (SUCCEEDED(device.As(std::addressof(attrs)))) attrs->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &detect);
        // Do not silently start an unrelated first mode (often NTSC). No
        // configuration is changed until the requested mode is validated.
        if (!options->modeId && !detect) {
            message(error, cap, "This DeckLink device does not support automatic input format detection. Select the source's input format in Settings > DeckLink, then add the input again.");
            return VtrPonDeckLinkInvalidArgument;
        }
        if (!directionAvailable(device, true, error, cap)) return VtrPonDeckLinkBackendError;
        auto session = std::make_shared<InputSession>(); auto s = session->state; auto &config = session->config;
        if (outputFailed(device.As(std::addressof(session->input)), "QueryInterface(Input)", error, cap)) return VtrPonDeckLinkBackendError;
        if (!options->modeId) s->flags = bmdVideoInputEnableFormatDetection;
        ComPtr<IDeckLinkDisplayMode> mode;
        if (options->modeId) {
            bool supported = false; BMDDisplayMode actual{};
            if (outputFailed(session->input->DoesSupportVideoMode(static_cast<BMDVideoConnection>(options->connection),
                    static_cast<BMDDisplayMode>(options->modeId), bmdFormat8BitYUV, bmdNoVideoInputConversion,
                    bmdSupportedVideoModeDefault, &actual, &supported), "DoesSupportVideoMode(Input)", error, cap)) return VtrPonDeckLinkBackendError;
            if (!supported) { message(error, cap, "The selected input format is unsupported on this DeckLink connector."); return VtrPonDeckLinkInvalidArgument; }
            if (outputFailed(session->input->GetDisplayMode(static_cast<BMDDisplayMode>(options->modeId), &mode), "GetDisplayMode(Input)", error, cap)) return VtrPonDeckLinkBackendError;
        } else {
            ComPtr<IDeckLinkDisplayModeIterator> it; session->input->GetDisplayModeIterator(&it);
            if (it) for (;;) {
                ComPtr<IDeckLinkDisplayMode> candidate; if (it->Next(&candidate) != S_OK) break;
                bool supported = false; BMDDisplayMode actual{};
                session->input->DoesSupportVideoMode(static_cast<BMDVideoConnection>(options->connection), candidate->GetDisplayMode(),
                    bmdFormat8BitYUV, bmdNoVideoInputConversion, bmdSupportedVideoModeDefault, &actual, &supported);
                if (supported && candidate->GetWidth() <= 4096 && candidate->GetHeight() <= 2160) { mode = candidate; break; }
            }
        }
        if (!mode) { message(error, cap, "No supported input format was found."); return VtrPonDeckLinkBackendError; }
        if (outputFailed(device.As(std::addressof(config)), "QueryInterface(InputConfiguration)", error, cap) ||
            outputFailed(config->SetInt(bmdDeckLinkConfigVideoInputConnection, options->connection), "SetVideoInputConnection", error, cap)) return VtrPonDeckLinkBackendError;
        // Use audio embedded in the selected digital video connector. Fixed
        // audio connections need no configuration; analog uses the device setup.
        if (options->connection & (bmdVideoConnectionSDI | bmdVideoConnectionHDMI | bmdVideoConnectionOpticalSDI)) {
            int64_t audioConnection = 0;
            if (SUCCEEDED(config->GetInt(bmdDeckLinkConfigAudioInputConnection, &audioConnection)) &&
                audioConnection != bmdAudioConnectionEmbedded &&
                FAILED(config->SetInt(bmdDeckLinkConfigAudioInputConnection, bmdAudioConnectionEmbedded))) {
                message(error, cap, "Embedded audio input could not be selected."); return VtrPonDeckLinkBackendError;
            }
        }
        BMDTimeValue duration = 0; BMDTimeScale scale = 0;
        if (outputFailed(mode->GetFrameRate(&duration, &scale), "GetFrameRate(Input)", error, cap) || duration <= 0 || scale <= 0 || duration > UINT32_MAX || scale > UINT32_MAX) return VtrPonDeckLinkBackendError;
        s->field = mode->GetFieldDominance(); s->duration = static_cast<uint32_t>(duration); s->scale = static_cast<uint32_t>(scale);
        ComPtr<IDeckLinkInputCallback> callback; callback.Attach(new InputCallback(s));
        if (outputFailed(session->input->SetCallback(callback.Get()), "SetCallback(Input)", error, cap)) return VtrPonDeckLinkBackendError;
        session->callbackSet = true;
        if (outputFailed(session->input->EnableVideoInput(mode->GetDisplayMode(), bmdFormat8BitYUV, s->flags), "EnableVideoInput", error, cap)) return VtrPonDeckLinkBackendError;
        session->videoEnabled = true;
        if (outputFailed(session->input->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2), "EnableAudioInput", error, cap)) return VtrPonDeckLinkBackendError;
        session->audioEnabled = true;
        if (outputFailed(session->input->StartStreams(), "StartStreams(Input)", error, cap)) return VtrPonDeckLinkBackendError;
        session->streamsStarted = true;
        c.inputState = session; return VtrPonDeckLinkOk;
    });
}
int32_t VTRPON_DECKLINK_CALL readInput(void *handle, VtrPonDeckLinkPacket *out, uint32_t timeout, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [&](Context &c) -> int32_t {
        if (!out || !c.inputState || timeout > 50) return VtrPonDeckLinkInvalidArgument;
        *out = {}; out->structSize = sizeof(*out);
        auto session = std::static_pointer_cast<InputSession>(c.inputState); auto s = session->state;
        std::unique_lock lock(s->mutex);
        s->wake.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return !s->packets.empty() || s->pendingMode != bmdModeUnknown; });
        if (s->pendingMode != bmdModeUnknown) {
            const auto mode = s->pendingMode; const auto pixel = s->pendingPixel; s->pendingMode = bmdModeUnknown; s->packets.clear(); lock.unlock();
            if (FAILED(session->input->StopStreams()) || FAILED(session->input->EnableVideoInput(mode, pixel, s->flags)) ||
                FAILED(session->input->FlushStreams()) || FAILED(session->input->StartStreams())) {
                message(error, cap, "Detected input format could not be applied."); return VtrPonDeckLinkBackendError;
            }
            return VtrPonDeckLinkOk;
        }
        if (s->packets.empty()) return VtrPonDeckLinkOk;
        s->borrowed = std::move(s->packets.front()); s->packets.pop_front(); *out = s->borrowed.info;
        out->video = s->borrowed.video.data(); out->audio = s->borrowed.audio.data(); return VtrPonDeckLinkOk;
    });
}
int32_t VTRPON_DECKLINK_CALL closeInput(void *handle, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [](Context &c) -> int32_t { if (c.inputState) std::static_pointer_cast<InputSession>(c.inputState)->stop(); c.inputState.reset(); return VtrPonDeckLinkOk; });
}
bool outputFailed(HRESULT result, const char *operation, char *error, uint32_t cap) {
    if (SUCCEEDED(result)) return false;
    char detail[256]{};
    std::snprintf(detail, sizeof(detail), "DeckLink %s failed (0x%08lX).", operation, static_cast<unsigned long>(result));
    message(error, cap, detail);
    return true;
}
int32_t pump(OutputState &s, char *error, uint32_t cap) {
    if (s.groupId && s.running && !s.internalReference) {
        bool locked = false;
        if (!s.status || FAILED(s.status->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &locked)) || !locked)
            return VtrPonDeckLinkBackendError;
    }
    unsigned buffered = 0;
    if (outputFailed(s.output->GetBufferedVideoFrameCount(&buffered), "GetBufferedVideoFrameCount", error, cap)) return VtrPonDeckLinkBackendError;
    BMDTimeValue clock = 0; double speed = 0;
    if (s.running && SUCCEEDED(s.output->GetScheduledStreamTime(s.scale, &clock, &speed)))
        s.nextVideo = std::max(s.nextVideo, (clock / s.duration + 1) * s.duration);
    while (buffered++ < 3) {
        const auto elapsed = vtrpon::decklink::frameTime100ns(s.nextVideo / s.duration, s.duration, s.scale);
        if (!elapsed || s.epoch > INT64_MAX - *elapsed) return VtrPonDeckLinkBackendError;
        const auto cutoff = s.epoch + *elapsed - s.latency100ns;
        while (!s.video.empty() && s.video.front().time <= cutoff) {
            s.latest = std::move(s.video.front().bytes); s.video.pop_front();
        }
        ComPtr<IDeckLinkMutableVideoFrame> source, frame;
        if (outputFailed(s.output->CreateVideoFrame(s.width, s.height, s.width * 4, bmdFormat8BitBGRA, bmdFrameFlagDefault, &source), "CreateVideoFrame(BGRA)", error, cap)) return VtrPonDeckLinkBackendError;
        ComPtr<IDeckLinkVideoBuffer> buffer; void *bytes = nullptr;
        if (outputFailed(source.As(std::addressof(buffer)), "QueryInterface(VideoBuffer)", error, cap) || outputFailed(buffer->StartAccess(bmdBufferAccessWrite), "StartAccess", error, cap)) return VtrPonDeckLinkBackendError;
        const auto hr = buffer->GetBytes(&bytes);
        if (SUCCEEDED(hr) && bytes) std::memcpy(bytes, s.latest.data(), s.latest.size());
        buffer->EndAccess(bmdBufferAccessWrite); if (outputFailed(hr, "GetBytes", error, cap) || !bytes) return VtrPonDeckLinkBackendError;
        frame = source;
        if (s.pixel != bmdFormat8BitBGRA) {
            if (outputFailed(s.output->CreateVideoFrame(s.width, s.height, s.rowBytes, s.pixel, bmdFrameFlagDefault, &frame), "CreateVideoFrame(YUV)", error, cap) ||
                outputFailed(s.converter->ConvertFrame(source.Get(), frame.Get()), "ConvertFrame", error, cap)) return VtrPonDeckLinkBackendError;
        }
        if (outputFailed(s.output->ScheduleVideoFrame(frame.Get(), s.nextVideo, s.duration, s.scale), "ScheduleVideoFrame", error, cap)) return VtrPonDeckLinkBackendError;
        s.nextVideo += s.duration;
    }
    if (outputFailed(s.output->GetBufferedAudioSampleFrameCount(&buffered), "GetBufferedAudioSampleFrameCount", error, cap)) return VtrPonDeckLinkBackendError;
    if (s.running && SUCCEEDED(s.output->GetScheduledStreamTime(48000, &clock, &speed))) s.nextAudio = std::max(s.nextAudio, clock);
    const auto target = static_cast<unsigned>((48000 * s.duration * 3 + s.scale - 1) / s.scale);
    if (buffered < target) {
        const unsigned count = target - buffered; std::vector<int32_t> samples(size_t(count) * 2, 0);
        for (const auto &clip : s.audio) {
            const auto begin = std::max(s.nextAudio, clip.start);
            const auto end = std::min(s.nextAudio + count, clip.start + static_cast<int64_t>(clip.samples.size() / 2));
            if (begin < end) std::copy_n(clip.samples.data() + (begin - clip.start) * 2,
                size_t(end - begin) * 2, samples.data() + (begin - s.nextAudio) * 2);
        }
        unsigned written = 0;
        if (outputFailed(s.output->ScheduleAudioSamples(samples.data(), count, s.nextAudio, 48000, &written), "ScheduleAudioSamples", error, cap) || written > count) return VtrPonDeckLinkBackendError;
        s.nextAudio += written;
        while (!s.audio.empty() && s.audio.front().start + static_cast<int64_t>(s.audio.front().samples.size() / 2) <= s.nextAudio) {
            s.audioSamples -= s.audio.front().samples.size(); s.audio.pop_front();
        }
    }
    return VtrPonDeckLinkOk;
}
int32_t openOutputImpl(void *handle, const VtrPonDeckLinkStreamOptions *options, int64_t groupId, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [&](Context &c) -> int32_t {
        if (c.outputState || !options || options->structSize != sizeof(*options) || !options->modeId || options->outputStartTime100ns <= 0) return VtrPonDeckLinkInvalidArgument;
        auto device = selected(c, options, 2); if (!device) return VtrPonDeckLinkInvalidArgument;
        if (!directionAvailable(device, false, error, cap)) return VtrPonDeckLinkBackendError;
        auto s = std::make_shared<OutputState>(); auto &config = s->config; ComPtr<IDeckLinkDisplayMode> mode;
        if (outputFailed(device.As(std::addressof(s->output)), "QueryInterface(Output)", error, cap) || outputFailed(device.As(std::addressof(config)), "QueryInterface(Configuration)", error, cap) ||
            outputFailed(config->SetInt(bmdDeckLinkConfigVideoOutputConnection, options->connection), "SetVideoOutputConnection", error, cap) ||
            outputFailed(s->output->GetDisplayMode(static_cast<BMDDisplayMode>(options->modeId), &mode), "GetDisplayMode", error, cap)) return VtrPonDeckLinkBackendError;
        s->width = mode->GetWidth(); s->height = mode->GetHeight();
        if (groupId) {
            ComPtr<IDeckLinkProfileAttributes> attributes; bool supported = false, locked = false;
            SdkString model; device->GetModelName(&model.value);
            s->internalReference = model.value && CFStringFind(model.value, CFSTR("DeckLink 8K Pro"), 0).location != kCFNotFound;
            if (FAILED(device.As(std::addressof(attributes))) || FAILED(attributes->GetFlag(BMDDeckLinkSupportsSynchronizeToPlaybackGroup, &supported)) || !supported) {
                message(error, cap, "This device does not support synchronized playback groups."); return VTRPON_DECKLINK_SYNC_UNAVAILABLE;
            }
            attributes->GetInt(BMDDeckLinkDeviceGroupID, &s->boardId);
            device.As(std::addressof(s->status));
            if (!s->internalReference && (FAILED(device.As(std::addressof(s->status))) ||
                FAILED(s->status->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &locked)) || !locked)) {
                message(error, cap, "Fill/Key synchronization requires a locked common reference input."); return VTRPON_DECKLINK_SYNC_UNAVAILABLE;
            }
            if (FAILED(config->GetInt(bmdDeckLinkConfigPlaybackGroup, &s->previousGroup)) ||
                FAILED(config->SetInt(bmdDeckLinkConfigPlaybackGroup, groupId))) return VtrPonDeckLinkBackendError;
            s->groupId = groupId;
        }
        if (s->width <= 0 || s->width > 4096 || s->height <= 0 || s->height > 2160 || FAILED(mode->GetFrameRate(&s->duration, &s->scale)) || s->duration <= 0 || s->scale <= 0) return VtrPonDeckLinkInvalidArgument;
        if (s->duration > 1000000000 || s->scale > 1000000000 || s->scale < s->duration * 10 || s->scale > s->duration * 240) return VtrPonDeckLinkInvalidArgument;
        bool supported = false; BMDDisplayMode actual{};
        auto supports = [&](BMDPixelFormat pixel) { return SUCCEEDED(s->output->DoesSupportVideoMode(static_cast<BMDVideoConnection>(options->connection),
            mode->GetDisplayMode(), pixel, bmdNoVideoOutputConversion, bmdSupportedVideoModeDefault, &actual, &supported)) && supported && actual == mode->GetDisplayMode(); };
        if (!supports(s->pixel)) {
            s->pixel = bmdFormat8BitYUV;
            s->converter.Attach(CreateVideoConversionInstance());
            if (!supports(s->pixel) || !s->converter) {
                message(error, cap, "Selected output mode or pixel format is unsupported."); return VtrPonDeckLinkBackendError;
            }
        }
        // A previous application may have left SDI in RGB 4:4:4 mode. Keep the
        // hardware setting aligned with the actual scheduled frame format.
        const auto colorResult = config->SetFlag(bmdDeckLinkConfig444SDIVideoOutput, s->pixel == bmdFormat8BitBGRA);
        if (FAILED(colorResult) && colorResult != E_NOTIMPL) {
            message(error, cap, "Output color format could not be configured."); return VtrPonDeckLinkBackendError;
        }
        if (outputFailed(s->output->RowBytesForPixelFormat(s->pixel, s->width, &s->rowBytes), "RowBytesForPixelFormat", error, cap) ||
            outputFailed(s->output->EnableVideoOutput(mode->GetDisplayMode(), groupId ? bmdVideoOutputSynchronizeToPlaybackGroup : bmdVideoOutputFlagDefault), "EnableVideoOutput", error, cap)) return VtrPonDeckLinkBackendError;
        s->videoEnabled = true;
        if (outputFailed(s->output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2, bmdAudioOutputStreamTimestamped), "EnableAudioOutput", error, cap)) return VtrPonDeckLinkBackendError;
        s->audioEnabled = true;
        s->epoch = options->outputStartTime100ns;
        s->latency100ns = *vtrpon::decklink::frameTime100ns(3, s->duration, s->scale);
        s->latencySamples = (s->duration * 3 * 48000 + s->scale - 1) / s->scale;
        s->latest.resize(size_t(s->width) * s->height * 4, 0);
        for (size_t i = 3; i < s->latest.size(); i += 4) s->latest[i] = 255;
        // Enter the driver's preroll state before querying/scheduling audio.
        // Audio samples are supplied explicitly by this owner worker.
        if (outputFailed(s->output->BeginAudioPreroll(), "BeginAudioPreroll", error, cap)) return VtrPonDeckLinkBackendError;
        if (pump(*s, error, cap) != VtrPonDeckLinkOk) return VtrPonDeckLinkBackendError;
        if (outputFailed(s->output->EndAudioPreroll(), "EndAudioPreroll", error, cap)) return VtrPonDeckLinkBackendError;
        if (!groupId) {
            if (outputFailed(s->output->StartScheduledPlayback(0, s->scale, 1.0), "StartScheduledPlayback", error, cap)) return VtrPonDeckLinkBackendError;
            s->running = true;
        }
        c.outputState = s; return VtrPonDeckLinkOk;
    });
}
int32_t VTRPON_DECKLINK_CALL openOutput(void *handle, const VtrPonDeckLinkStreamOptions *options, char *error, uint32_t cap) noexcept {
    return openOutputImpl(handle, options, 0, error, cap);
}
int32_t VTRPON_DECKLINK_CALL openGroupedOutput(void *handle, const VtrPonDeckLinkStreamOptions *options, int64_t groupId, char *error, uint32_t cap) noexcept {
    if (groupId <= 0) return VtrPonDeckLinkInvalidArgument;
    return openOutputImpl(handle, options, groupId, error, cap);
}
int32_t VTRPON_DECKLINK_CALL startGroup(void *const *handles, uint32_t count, char *error, uint32_t cap) noexcept {
    if (!handles || count < 2 || count > 3) return VtrPonDeckLinkInvalidArgument;
    std::shared_ptr<OutputState> leader; bool internalReference = true;
    for (uint32_t i = 0; i < count; ++i) {
        const auto result = check(handles[i], error, cap); if (result) return result;
        auto s = std::static_pointer_cast<OutputState>(static_cast<Context *>(handles[i])->outputState);
        if (!s || !s->groupId || s->running) return VtrPonDeckLinkInvalidArgument;
        for (uint32_t j = 0; j < i; ++j) if (handles[j] == handles[i]) return VtrPonDeckLinkInvalidArgument;
        if (!leader) leader = s;
        if (s->groupId != leader->groupId || s->duration != leader->duration || s->scale != leader->scale || s->epoch != leader->epoch || s->width != leader->width || s->height != leader->height)
            return VtrPonDeckLinkInvalidArgument;
        internalReference = internalReference && s->internalReference && s->boardId && s->boardId == leader->boardId;
    }
    // The 8K Pro exception applies only to subdevices of the same card.
    for (uint32_t i = 0; i < count; ++i) {
        auto s = std::static_pointer_cast<OutputState>(static_cast<Context *>(handles[i])->outputState);
        bool locked = false;
        if (!internalReference && (!s->status || FAILED(s->status->GetFlag(bmdDeckLinkStatusReferenceSignalLocked, &locked)) || !locked)) {
            message(error, cap, "Fill/Key synchronization requires a locked common reference input."); return VTRPON_DECKLINK_SYNC_UNAVAILABLE;
        }
        s->internalReference = internalReference;
    }
    if (FAILED(leader->output->StartScheduledPlayback(0, leader->scale, 1.0))) return VtrPonDeckLinkBackendError;
    for (uint32_t i = 0; i < count; ++i) std::static_pointer_cast<OutputState>(static_cast<Context *>(handles[i])->outputState)->running = true;
    return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL sendVideo(void *handle, const uint8_t *bytes, uint32_t width, uint32_t height, int64_t time, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [&](Context &c) -> int32_t {
        if (!c.outputState || !bytes || time <= 0) return VtrPonDeckLinkInvalidArgument;
        auto s = std::static_pointer_cast<OutputState>(c.outputState);
        if (width != static_cast<uint32_t>(s->width) || height != static_cast<uint32_t>(s->height)) return VtrPonDeckLinkInvalidArgument;
        if (!s->video.empty() && time <= s->video.back().time) return VtrPonDeckLinkInvalidArgument;
        OutputState::TimedVideo frame{time, std::vector<uint8_t>(bytes, bytes + s->latest.size())};
        if (s->video.size() == 8) s->video.pop_front();
        s->video.push_back(std::move(frame)); return VtrPonDeckLinkOk;
    });
}
int32_t VTRPON_DECKLINK_CALL sendAudio(void *handle, const float *samples, uint32_t frames, int64_t time, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [&](Context &c) -> int32_t {
        if (!c.outputState || !samples || frames > 48000 || time <= 0) return VtrPonDeckLinkInvalidArgument;
        auto s = std::static_pointer_cast<OutputState>(c.outputState);
        for (size_t i = 0; i < size_t(frames) * 2; ++i) if (!std::isfinite(samples[i])) return VtrPonDeckLinkInvalidArgument;
        const auto relative = time - s->epoch;
        const auto offset = vtrpon::decklink::audioTime48k(relative);
        if (!offset || *offset > INT64_MAX - s->latencySamples) return VtrPonDeckLinkInvalidArgument;
        const auto start = *offset + s->latencySamples;
        OutputState::TimedAudio clip{start, {}}; clip.samples.reserve(size_t(frames) * 2);
        for (size_t i = 0; i < size_t(frames) * 2; ++i)
            clip.samples.push_back(static_cast<int32_t>(std::clamp(double(samples[i]), -1.0, 1.0) * 2147483647.0));
        if (s->audioSamples + clip.samples.size() > 96000) { s->audio.clear(); s->audioSamples = 0; }
        s->audioSamples += clip.samples.size(); s->audio.push_back(std::move(clip));
        return VtrPonDeckLinkOk;
    });
}
int32_t VTRPON_DECKLINK_CALL pumpOutput(void *handle, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [&](Context &c) -> int32_t {
        if (!c.outputState) return VtrPonDeckLinkInvalidArgument;
        const auto status = pump(*std::static_pointer_cast<OutputState>(c.outputState), error, cap);
        if (status && (!error || !cap || !*error)) message(error, cap, "DeckLink scheduled output failed; the device may have disconnected."); return status;
    });
}
int32_t VTRPON_DECKLINK_CALL closeOutput(void *handle, char *error, uint32_t cap) noexcept {
    return mediaCall(handle, error, cap, [](Context &c) -> int32_t { c.outputState.reset(); return VtrPonDeckLinkOk; });
}
const VtrPonDeckLinkMediaApi mediaApi{1, sizeof(VtrPonDeckLinkMediaApi), 0, 0, "vtr-pon-decklink-addon",
    channels, openInput, readInput, closeInput, openOutput, sendVideo, sendAudio, pumpOutput, closeOutput};
const VtrPonDeckLinkOutputApi outputApi{1, sizeof(VtrPonDeckLinkOutputApi), 0, 0, "vtr-pon-decklink-addon", openGroupedOutput, startGroup};
