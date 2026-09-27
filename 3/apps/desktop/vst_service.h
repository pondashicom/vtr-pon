#pragma once
#include <QObject>
#include <QJsonArray>
#include <QString>
#include <memory>

namespace vtrpon {
enum class VstSlot { Edit, Item, Master, Count };
bool vstModeEnabled();
void processVst(VstSlot slot, float *samples, size_t frames);

// SDK-free Controller bridge. Vendor code runs only in the optional add-on.
class VstService final : public QObject {
    Q_OBJECT
public:
    static VstService &instance();
    ~VstService() override;
    static QString addonPath();
    bool available() const;
    bool setEnabled(bool enabled);
    void reloadSettings();
    void scan();
    bool scanning() const;
    QJsonArray plugins() const;
    void select(VstSlot slot, const QString &path, const QString &uid);
    QString selectedPath(VstSlot slot) const;
    QString selectedUid(VstSlot slot) const;
    void activate(VstSlot slot);
    void bypass(VstSlot slot, bool bypassed);
    bool active(VstSlot slot) const;
    bool bypassed(VstSlot slot) const;
    QString statusKey(VstSlot slot) const;
    QString detail(VstSlot slot) const;
    int latency(VstSlot slot) const;
    void openEditor(VstSlot slot, quintptr owner, int x, int y);
signals:
    void changed();
    void catalogChanged();
    void failure(const QString &channel, const QString &reason);
private:
    VstService();
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace vtrpon
