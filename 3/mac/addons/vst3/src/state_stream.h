#pragma once
#include "vst_state.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/funknownimpl.h"
#include <algorithm>
#include <cstring>
// Bound vendor-controlled writes as well as reads of saved state.
class StateStream final : public Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::IBStream>> {
public:
    QByteArray bytes;
    Steinberg::int64 position = 0;
    bool failed = false;
    StateStream() = default;
    explicit StateStream(QByteArray data) : bytes(std::move(data)) {}
    Steinberg::tresult PLUGIN_API read(void *buffer, Steinberg::int32 size, Steinberg::int32 *read = nullptr) override {
        if (read) *read = 0;
        if (size < 0 || (size && !buffer)) return Steinberg::kInvalidArgument;
        const auto n = std::min<Steinberg::int64>(size, std::max<Steinberg::int64>(0, bytes.size() - position));
        if (n) std::memcpy(buffer, bytes.constData() + position, size_t(n));
        position += n; if (read) *read = Steinberg::int32(n);
        // Match the SDK MemoryStream: EOF is a successful short read, with
        // its actual byte count in numBytesRead. VST wrappers rely on this.
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API write(void *buffer, Steinberg::int32 size, Steinberg::int32 *written = nullptr) override {
        if (written) *written = 0;
        if (size < 0 || (size && !buffer) || position > vtrpon::vststate::maxStreamBytes - Steinberg::int64(size)) {
            failed = true; return Steinberg::kInvalidArgument;
        }
        if (position + size > bytes.size()) bytes.resize(position + size, '\0');
        if (size) std::memcpy(bytes.data() + position, buffer, size_t(size));
        position += size; if (written) *written = size; return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset, Steinberg::int32 mode, Steinberg::int64 *result = nullptr) override {
        if (mode < kIBSeekSet || mode > kIBSeekEnd) return Steinberg::kInvalidArgument;
        const auto base = mode == kIBSeekCur ? position : mode == kIBSeekEnd ? bytes.size() : 0;
        if (offset < -base || offset > vtrpon::vststate::maxStreamBytes - base) { failed = true; return Steinberg::kInvalidArgument; }
        position = base + offset; if (result) *result = position; return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64 *result) override {
        if (!result) return Steinberg::kInvalidArgument;
        *result = position; return Steinberg::kResultOk;
    }
};
