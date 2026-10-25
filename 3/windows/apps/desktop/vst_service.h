#pragma once
#include <QObject>
#include <QJsonArray>
#include <QString>
#include <memory>

namespace vtrpon {
enum class VstSlot { Edit, Item, Master, Count };
struct VstScanProgress {
    QString currentPath, error;
    int checked = 0, total = 0, skipped = 0;
    bool discovering = true, canceled = false;
};
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
    void cancelScan();
    bool scanning() const;
    bool catalogRequested() const;
    VstScanProgress scanProgress() const;
    QJsonArray plugins() const;
    // Prepare a replacement; commit the selection only after successful startup.
    void select(VstSlot slot, const QString &path, const QString &uid);
    bool applying(VstSlot slot) const;
    QString applyError(VstSlot slot) const;
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
    void scanProgressChanged();
    void failure(const QString &channel, const QString &reason);
    void applicationFailed(const QString &channel, const QString &reason);
private:
    VstService();
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace vtrpon
