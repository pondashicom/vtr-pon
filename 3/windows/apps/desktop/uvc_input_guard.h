#pragma once

#include <QString>
#include <QStringList>
#include <mutex>

namespace vtrpon {
namespace uvcInputGuardDetail {
struct State {
    std::mutex mutex;
    QStringList ownDevicePaths;
    bool registering = false;
};
inline State &state() { static State value; return value; }
}

// The exact symbolic link returned by Windows is authoritative. Friendly names
// can be localized/renamed, so they must never decide whether input is allowed.
class UvcSelfCameraRegistration final {
public:
    UvcSelfCameraRegistration() {
        auto &state=uvcInputGuardDetail::state();
        std::lock_guard lock(state.mutex);
        state.registering=true;
    }
    ~UvcSelfCameraRegistration() { finish(); }
    UvcSelfCameraRegistration(const UvcSelfCameraRegistration&)=delete;
    UvcSelfCameraRegistration& operator=(const UvcSelfCameraRegistration&)=delete;
    void finish(const QString &symbolicLink={}) {
        if(finished_) return;
        auto &state=uvcInputGuardDetail::state();
        std::lock_guard lock(state.mutex);
        if(!symbolicLink.isEmpty() && !state.ownDevicePaths.contains(symbolicLink,Qt::CaseInsensitive))
            state.ownDevicePaths.append(symbolicLink);
        state.registering=false;
        finished_=true;
    }
private:
    bool finished_=false;
};

inline bool uvcInputAllowed(const QString &devicePath, QString *reason=nullptr) {
    auto &state=uvcInputGuardDetail::state();
    std::lock_guard lock(state.mutex);
    if(state.registering) {
        if(reason) *reason=QStringLiteral("Virtual camera identification is initializing. Retry when device discovery completes.");
        return false;
    }
    if(state.ownDevicePaths.contains(devicePath,Qt::CaseInsensitive)) {
        if(reason) *reason=QStringLiteral("VTR-PON cannot read its own virtual camera because this would create a video feedback loop.");
        return false;
    }
    if(reason) reason->clear();
    return true;
}
}
