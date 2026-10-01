int32_t VTRPON_DECKLINK_CALL channels(void *handle, VtrPonDeckLinkChannel *out, uint32_t cap, uint32_t *count, char *err, uint32_t errCap) {
    const auto status = check(handle, err, errCap); if (status) return status;
    if (!count || (!out && cap)) return VtrPonDeckLinkInvalidArgument;
    if (std::getenv("VTRPON_DECKLINK_TEST_NO_DEVICES")) { *count = 0; return VtrPonDeckLinkOk; }
    *count = 5; if (cap < 5) return VtrPonDeckLinkBufferTooSmall;
    for (uint32_t i = 0; i < 5; ++i) {
        out[i] = {}; out[i].structSize = sizeof(out[i]); out[i].deviceIndex = i == 2 ? 1 : 0;
        out[i].direction = i == 1 || i > 2 ? 2 : 1; out[i].connection = 1; out[i].persistentId = i == 2 ? 101 : 100;
        out[i].subDeviceIndex = i > 2 ? i - 1 : i == 2 ? 1 : 0; out[i].stableId = 1;
        message(out[i].nameUtf8, 256, "TEST ONLY: DeckLink channel");
    }
    return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL openInput(void *handle, const VtrPonDeckLinkStreamOptions *options, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    auto &c = *static_cast<Context *>(handle);
    if (!options || options->structSize != sizeof(*options) || c.input || options->connection != 1 || options->persistentId != 100) return VtrPonDeckLinkInvalidArgument;
    // Deliberately exclusive: tests fail if the host opens video/audio twice.
    uint32_t expected = 0; if (!inputs.compare_exchange_strong(expected, 1)) { message(err, cap, "Mock channel is already open."); return VtrPonDeckLinkBackendError; }
    ++inputOpens; c.input = true; c.frame = 0; c.video.assign(64 * 36 * 4, 0); c.audio.assign(1602, 0x20000000);
    for (size_t i = 0; i < c.video.size(); i += 4) { c.video[i] = 10; c.video[i+1] = 40; c.video[i+2] = 200; c.video[i+3] = 255; }
    return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL readInput(void *handle, VtrPonDeckLinkPacket *out, uint32_t timeout, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    if (std::getenv("VTRPON_DECKLINK_TEST_NO_DEVICES")) { message(err, cap, "DeckLink device disconnected."); return VtrPonDeckLinkBackendError; }
    auto &c = *static_cast<Context *>(handle); if (!out || !c.input || timeout > 50) return VtrPonDeckLinkInvalidArgument;
    std::this_thread::sleep_for(std::chrono::milliseconds(16)); *out = {}; out->structSize = sizeof(*out);
    out->pixelFormat = 1; out->width = 64; out->height = 36; out->rowBytes = 256; out->videoBytes = static_cast<uint32_t>(c.video.size()); out->video = c.video.data();
    out->videoTime100ns = static_cast<int64_t>(c.frame * 1001 * 10000000 / 60000); out->frameDuration100ns = 166833;
    out->audio = c.audio.data(); out->audioFrames = 801; out->audioTime100ns = out->videoTime100ns;
    out->frameDuration = 1001; out->timeScale = 60000; ++c.frame;
    if (std::getenv("VTRPON_DECKLINK_TEST_NO_SIGNAL")) { out->noSignal = 1; out->videoBytes = 0; out->audioFrames = 0; }
    return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL closeInput(void *handle, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    auto &c = *static_cast<Context *>(handle); if (c.input) --inputs; c.input = false; return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL openOutput(void *handle, const VtrPonDeckLinkStreamOptions *options, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    if (!options || options->structSize != sizeof(*options) || options->persistentId != 100 || options->connection != 1 || (options->modeId != 1000 && options->modeId != 1001)) return VtrPonDeckLinkInvalidArgument;
    auto &c = *static_cast<Context *>(handle);
    if (c.output || (options->subDeviceIndex != 0 && options->subDeviceIndex != 2 && options->subDeviceIndex != 3)) return VtrPonDeckLinkInvalidArgument;
    c.route = options->subDeviceIndex ? options->subDeviceIndex - 1 : 0;
    uint64_t expected = 0;
    if (!outputs[c.route].active.compare_exchange_strong(expected, 1)) return VtrPonDeckLinkBackendError;
    ++outputs[c.route].opens;
    c.output = c.running = true; c.group = 0; c.epoch = options->outputStartTime100ns; c.mode = options->modeId;
    outputs[c.route].group = 0; return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL sendVideo(void *handle, const uint8_t *pixels, uint32_t w, uint32_t h, int64_t time100ns, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    if (!static_cast<Context *>(handle)->output || !pixels || w != 1920 || h != 1080 || time100ns <= 0) return VtrPonDeckLinkInvalidArgument;
    auto &c = *static_cast<Context *>(handle); uint32_t color = 0; std::memcpy(&color, pixels, 4);
    if (!c.running || color != outputs[c.route].expected || std::memcmp(pixels, pixels + size_t(w*h-1)*4, 4)) {
        message(err, cap, "Unexpected pixel content or send before group start."); return VtrPonDeckLinkBackendError;
    }
    ++outputs[c.route].video; outputs[c.route].color = color; outputs[c.route].time = static_cast<uint64_t>(time100ns);
    ++videoSends; return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL sendAudio(void *handle, const float *samples, uint32_t frames, int64_t time100ns, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    if (!static_cast<Context *>(handle)->output || !samples || !frames || frames > 48000 || time100ns <= 0) return VtrPonDeckLinkInvalidArgument;
    ++outputs[static_cast<Context *>(handle)->route].audio; ++audioSends; return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL pumpOutput(void *handle, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    if (!static_cast<Context *>(handle)->output) return VtrPonDeckLinkInvalidArgument;
    ++outputPumps;
    if ((std::getenv("VTRPON_DECKLINK_TEST_FAIL_KEY") && static_cast<Context *>(handle)->route == 2) || std::getenv("VTRPON_DECKLINK_TEST_FAIL_OUTPUT") || std::getenv("VTRPON_DECKLINK_TEST_NO_DEVICES")) { message(err, cap, "Injected output failure."); return VtrPonDeckLinkBackendError; }
    return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL closeOutput(void *handle, char *err, uint32_t cap) {
    const auto status = check(handle, err, cap); if (status) return status;
    auto &c = *static_cast<Context *>(handle); if (c.output) --outputs[c.route].active; c.output = c.running = false; return VtrPonDeckLinkOk;
}
int32_t VTRPON_DECKLINK_CALL openGroupedOutput(void *handle, const VtrPonDeckLinkStreamOptions *options, int64_t group, char *err, uint32_t cap) {
    if (group <= 0) return VtrPonDeckLinkInvalidArgument;
    if (std::getenv("VTRPON_DECKLINK_TEST_NO_SYNC")) { message(err, cap, "Mock common reference is unavailable."); return VTRPON_DECKLINK_SYNC_UNAVAILABLE; }
    const auto result = openOutput(handle, options, err, cap);
    if (!result) { auto &c = *static_cast<Context *>(handle); c.group = group; c.running = false; outputs[c.route].group = static_cast<uint64_t>(group); }
    return result;
}
int32_t VTRPON_DECKLINK_CALL startGroup(void *const *handles, uint32_t count, char *err, uint32_t cap) {
    if (!handles || count != 2) return VtrPonDeckLinkInvalidArgument;
    if (std::getenv("VTRPON_DECKLINK_TEST_FAIL_GROUP")) { message(err, cap, "Injected group start failure."); return VtrPonDeckLinkBackendError; }
    const Context *leader = nullptr;
    for (unsigned i = 0; i < count; ++i) {
        const auto status = check(handles[i], err, cap); if (status) return status;
        auto &c = *static_cast<Context *>(handles[i]); if (!leader) leader = &c;
        if (!c.output || c.running || c.group <= 0 || c.group != leader->group || c.epoch != leader->epoch || c.mode != leader->mode || (i && handles[i] == handles[0])) return VtrPonDeckLinkInvalidArgument;
    }
    for (unsigned i = 0; i < count; ++i) static_cast<Context *>(handles[i])->running = true;
    return VtrPonDeckLinkOk;
}
const VtrPonDeckLinkOutputApi outputApi{1, sizeof(VtrPonDeckLinkOutputApi), VtrPonDeckLinkTestOnly, 0, "vtr-pon-decklink-test-addon", openGroupedOutput, startGroup};
const VtrPonDeckLinkMediaApi mediaApi{1, sizeof(VtrPonDeckLinkMediaApi), VtrPonDeckLinkTestOnly, 0, "vtr-pon-decklink-test-addon",
    channels, openInput, readInput, closeInput, openOutput, sendVideo, sendAudio, pumpOutput, closeOutput};
