#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <vector>

namespace {
AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}

QString stringProperty(AudioDeviceID device, AudioObjectPropertySelector selector) {
    const auto property = address(selector);
    CFStringRef value = nullptr;
    UInt32 size = sizeof(value);
    const auto status = AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &value);
    if (status != noErr || !value) return {};
    const auto length = CFStringGetLength(value);
    QString result(length, Qt::Uninitialized);
    CFStringGetCharacters(value, CFRangeMake(0, length), reinterpret_cast<UniChar *>(result.data()));
    CFRelease(value);
    return result;
}

bool scalarProperty(AudioDeviceID device, AudioObjectPropertySelector selector, UInt32 &value) {
    const auto property = address(selector);
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(device, &property, 0, nullptr, &size, &value) == noErr;
}

bool availableOutput(AudioDeviceID device) {
    UInt32 alive = 0;
    if (!scalarProperty(device, kAudioDevicePropertyDeviceIsAlive, alive) || !alive) return false;
    UInt32 hidden = 0;
    if (scalarProperty(device, kAudioDevicePropertyIsHidden, hidden) && hidden) return false;
    const auto property = address(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput);
    UInt32 size = 0;
    return AudioObjectGetPropertyDataSize(device, &property, 0, nullptr, &size) == noErr && size > 0;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().mid(1) != QStringList{QStringLiteral("--list")}) return 2;

    // Enumeration never opens an AudioUnit. Run it in a disposable process so
    // a slow HAL/third-party driver cannot block the Controller or on-air mix.
    const auto property = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0, nullptr, &size) != noErr
        || size % sizeof(AudioDeviceID) != 0) return 1;
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (size && AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr,
            &size, devices.data()) != noErr) return 1;
    devices.resize(size / sizeof(AudioDeviceID));

    struct Device { QString id; QString name; };
    QList<Device> outputs;
    for (const auto device : devices) {
        if (!availableOutput(device)) continue;
        const auto uid = stringProperty(device, kAudioDevicePropertyDeviceUID);
        const auto name = stringProperty(device, kAudioObjectPropertyName);
        if (!uid.isEmpty() && !name.isEmpty()) outputs.append({uid, name});
    }
    std::sort(outputs.begin(), outputs.end(), [](const auto &left, const auto &right) {
        const auto order = QString::localeAwareCompare(left.name, right.name);
        return order != 0 ? order < 0 : left.id < right.id;
    });
    UInt32 defaultDevice = kAudioDeviceUnknown;
    if (!scalarProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                        defaultDevice)) return 1;
    QString defaultUid;
    if (defaultDevice != kAudioDeviceUnknown && availableOutput(defaultDevice))
        defaultUid = stringProperty(defaultDevice, kAudioDevicePropertyDeviceUID);

    QJsonArray entries;
    for (const auto &device : outputs)
        entries.append(QJsonObject{{QStringLiteral("id"), device.id}, {QStringLiteral("name"), device.name}});
    const QJsonObject result{{QStringLiteral("devices"), entries}, {QStringLiteral("default"), defaultUid}};
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    return 0;
}
