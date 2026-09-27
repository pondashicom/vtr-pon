#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

class QSettings;

namespace vtrpon {
struct GpuPreferences {
  QString adapterId; // Empty selects the Program display's adapter.
  bool softwareDecoding = false;
  bool operator==(const GpuPreferences &) const = default;
};

struct GpuAdapter {
  QString id; // Hardware identity, not the boot-specific DXGI LUID.
  QString name;
  qint64 luid = 0;
};

struct GpuSelection {
  GpuAdapter adapter;
  bool requestedAdapterMissing = false;
};

GpuPreferences readGpuPreferences(const QSettings &settings);
bool saveGpuPreferences(QSettings &settings, const GpuPreferences &preferences);
void freezeGpuPreferences();
GpuPreferences startupGpuPreferences();
QVector<GpuAdapter> availableGpuAdapters();
GpuSelection selectGpuAdapter(const QVector<GpuAdapter> &adapters,
                              const QString &requestedId, qint64 displayLuid);
// Called at startup, before any media pipeline is created. Never during playback.
void configureVideoDecoders(qint64 adapterLuid, bool softwareOnly);
void publishGpuStatus(const GpuSelection &selection, bool softwareOnly);
} // namespace vtrpon
