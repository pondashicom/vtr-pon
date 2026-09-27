#pragma once

#include "playlist_item.h"
#include "macro_document.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QVector>

#include <optional>
#include <functional>
#include <memory>

class PlaylistPlaySequence;
class PlaylistMediaAvailability;

struct PlaylistModelSnapshot {
  QList<PlaylistState> playlists;
  QList<PlaylistItemState> items;
  int activePlaylistNumber = 1;
  qint64 manualCrossfadeDurationMs = kDefaultItemFadeDurationMs;
  QString documentId = newMacroId();
  QList<MacroDefinition> macros;
  // Persist the allocation high-water mark even after the highest item is deleted.
  quint64 nextItemId = 1;

  bool operator==(const PlaylistModelSnapshot &) const = default;
};

enum class PlaylistMapStopReason {
  None,
  StartModePause,
  DestinationStartModePause,
  LiveOrUnknownDuration,
  InfiniteRepeat,
  Loop,
  DestinationUnavailable,
  DestinationBgm,
};

struct PlaylistMapRow {
  int sequenceNumber = 0;
  quint64 itemId = 0;
  int playlistNumber = 0;
  int itemNumber = 0;
  QString itemName;
  PlaylistItemSettings settings;
  qint64 durationMs = 0;
  bool infiniteDuration = false;

  bool operator==(const PlaylistMapRow &) const = default;
};

struct PlaylistMapPlaybackProgress {
  qint64 currentRemainingMs = 0;
  int currentPlayCount = 1;
  double playbackRate = 1.0;

  bool operator==(const PlaylistMapPlaybackProgress &) const = default;
};

struct PlaylistMapAnalysis {
  QList<PlaylistMapRow> rows;
  std::optional<qint64> totalDurationMs = std::nullopt;
  qint64 startingItemContributionMs = 0;
  bool infinite = false;
  PlaylistMapStopReason stopReason = PlaylistMapStopReason::None;

  bool operator==(const PlaylistMapAnalysis &) const = default;
};

struct PlaylistTotalRemaining {
  std::optional<qint64> durationMs;
  bool infinite = false;
};

enum class PlaylistBulkPlaybackOperation {
  PlayOnce,
  RepeatAll,
  Shuffle,
  BgmMode,
  StandardMode,
};

class PlaylistModel final : public QObject {
  Q_OBJECT

public:
  explicit PlaylistModel(QObject *parent = nullptr);
  // The probe runs on a background thread and must not access GUI/model state.
  using MediaAvailabilityProbe = std::function<bool(const QString &)>;
  PlaylistModel(QObject *parent, MediaAvailabilityProbe availabilityProbe);
  ~PlaylistModel() override;

  QList<quint64> addItems(const QList<PlaylistItemRegistration> &items);
  // Pending File items perform no filesystem access and remain unavailable for
  // playout/edit until the background probe supplies a validated result.
  quint64 addPendingMediaItem(const QString &path, int playlistNumber,
                              PlaylistSourceType sourceType = PlaylistSourceType::File);
  // Caller has validated the native source on a worker. No filesystem I/O.
  bool completePendingNativeDocument(quint64 itemId);
  bool completePendingMediaItem(quint64 itemId, PlaylistMediaType mediaType,
                                std::optional<qint64> durationMs);
  QList<quint64>
  addItemsAtomically(const QList<PlaylistItemRegistration> &items);
  QList<quint64> replaceItemAtomically(
      quint64 itemId, const QList<PlaylistItemRegistration> &items);
  bool moveItem(quint64 itemId, int offset);
  bool moveItemTo(quint64 itemId, int targetRow);
  bool moveItemToPlaylist(quint64 itemId, int targetPlaylistNumber,
                          int targetInsertionRow);
  quint64 copyItemToPlaylist(quint64 itemId, int targetPlaylistNumber,
                             int targetInsertionRow);
  bool applyBulkPlaybackOperation(int playlistNumber,
                                  PlaylistBulkPlaybackOperation operation);
  void rememberBulkOperation(const PlaylistModelSnapshot &before,
                             int playlistNumber);
  bool hasBulkUndo() const;
  bool undoBulkOperation(QString *errorDetail = nullptr,
                         bool *keptLaterChanges = nullptr);
  bool removeItem(quint64 itemId);

  int itemCount() const;
  int itemCount(int playlistNumber) const;
  quint64 itemIdAt(int row) const;
  quint64 itemIdAt(int playlistNumber, int row) const;
  quint64 nextItemIdForPlayout(quint64 currentItemId,
                               quint64 excludedItemId = 0) const;
  quint64 automaticEndTargetItemIdForPlayout(
      quint64 currentItemId, quint64 excludedItemId = 0) const;
  quint64 endTargetItemIdForPlayout(quint64 currentItemId,
                                   PlaylistEndMode endMode,
                                   quint64 excludedItemId = 0) const;
  PlaylistMapAnalysis analyzePlaySequence(
      quint64 startItemId,
      std::optional<PlaylistMapPlaybackProgress> playbackProgress =
          std::nullopt,
      bool currentItemOnly = false) const;
  PlaylistTotalRemaining totalRemainingForPlayout(
      quint64 onAirItemId, PlaylistMapPlaybackProgress progress) const;
  int indexOf(quint64 itemId) const;
  std::optional<PlaylistItemState> itemState(quint64 itemId) const;
  bool itemMediaOffline(quint64 itemId) const;
  void setUvcDeviceAvailable(const QString &devicePath, bool available);
  std::optional<PlaylistItemConversionState>
  itemConversionState(quint64 itemId) const;
  bool itemConversionInProgress(quint64 itemId) const;
  bool setItemConversionProgress(quint64 itemId,
                                 PlaylistItemConversionStage stage,
                                 int progressPermille);
  // Requests a scan; cached availability remains valid until results arrive.
  void refreshMediaAvailability();
  int itemCurrentPlayCount(quint64 itemId) const;
  void setItemCurrentPlayCount(quint64 itemId, int currentPlayCount);
  bool setItemDisplayName(quint64 itemId, const QString &displayName);
  bool setItemColor(quint64 itemId, PlaylistItemColor color);
  bool setItemSeparatorBelow(quint64 itemId, bool separatorBelow);
  bool updateItemSettings(quint64 itemId, const PlaylistItemSettings &settings);
  void setKnownMediaDuration(quint64 itemId, qint64 durationMs);
  bool itemSettingsWithinKnownDuration(
      quint64 itemId, const PlaylistItemSettings &settings) const;
  bool itemEditPointsLocked(quint64 itemId) const;
  bool itemSettingsLocked(quint64 itemId) const;
  void setItemSettingsLocked(quint64 itemId, bool locked);

  quint64 onAirItemId() const;
  QString onAirErrorDetail() const;
  void setOnAirItemId(quint64 itemId);
  void clearOnAirItem();
  void setOnAirError(const QString &detail);

  quint64 cueItemId() const;
  PlaylistCueState cueState() const;
  QString cueDetail() const;
  void setCueItemState(quint64 itemId, PlaylistCueState state,
                       const QString &detail = QString());
  void clearCueItem();

  quint64 dskItemId() const;
  void setDskItemId(quint64 itemId);
  void clearDskItem();

  quint64 editItemId() const;
  void setEditItemId(quint64 itemId);
  void clearEditItem();

  quint64 bgmItemId() const;
  void setBgmItemId(quint64 itemId);
  void clearBgmItem();

  QList<PlaylistState> playlists() const;
  int createPlaylist();
  bool movePlaylist(int playlistNumber, int destinationIndex);
  // The controller must release and remove all items before deleting a tab.
  bool removeEmptyPlaylist(int playlistNumber);
  int activePlaylistNumber() const;
  bool setActivePlaylistNumber(int playlistNumber);
  QString playlistName(int playlistNumber) const;
  bool setPlaylistName(int playlistNumber, const QString &name);
  PlaylistMode playlistMode(int playlistNumber) const;
  bool setPlaylistMode(int playlistNumber, PlaylistMode mode);
  PlaylistModelSnapshot snapshot() const;
  QString documentId() const;
  QList<MacroDefinition> macros() const;
  bool setMacros(const QList<MacroDefinition> &macros,
                 QString *errorDetail = nullptr);
  QStringList macroReferenceErrors() const;
  bool manualCrossfadeEnabled(int playlistNumber) const;
  void setManualCrossfadeEnabled(int playlistNumber, bool enabled);
  qint64 manualCrossfadeDurationMs() const;
  void setManualCrossfadeDurationMs(qint64 durationMs);
  static bool canRestoreSnapshot(const PlaylistModelSnapshot &snapshot,
                                 QString *errorDetail = nullptr);
  static std::optional<quint64> nextItemIdForSnapshot(
      const PlaylistModelSnapshot &snapshot, QString *errorDetail = nullptr);
  bool restoreSnapshot(const PlaylistModelSnapshot &snapshot,
                       QString *errorDetail = nullptr,
                       const QSet<quint64> *knownOfflineItems = nullptr);
  void requestShutdown();
  bool shutdownComplete() const;

signals:
  // Emitted on each offline-check request, even with no media or a busy scan.
  void mediaAvailabilityCheckRequested();
  void itemAdded(quint64 itemId);
  void itemRemoved(quint64 itemId, int previousRow);
  void itemMoved(quint64 itemId, int previousRow, int currentRow);
  void itemMovedToPlaylist(quint64 itemId, int previousPlaylistNumber,
                           int previousRow, int currentPlaylistNumber,
                           int currentRow);
  void itemCopiedToPlaylist(quint64 sourceItemId, quint64 copiedItemId,
                            int playlistNumber, int currentRow);
  void itemChanged(quint64 itemId);
  void itemMediaAvailabilityChanged(quint64 itemId, bool offline);
  void itemSettingsChanged(quint64 itemId,
                           const PlaylistItemSettings &settings);
  void itemSettingsLockChanged(quint64 itemId, bool locked);
  void itemConversionStateChanged(quint64 itemId);
  void itemReplaced(quint64 retainedItemId);
  void countChanged(int count);
  void onAirStateChanged(quint64 previousItemId, quint64 currentItemId);
  void cueStateChanged(quint64 previousItemId, quint64 currentItemId);
  void dskStateChanged(quint64 previousItemId, quint64 currentItemId);
  void editStateChanged(quint64 previousItemId, quint64 currentItemId);
  void bgmStateChanged(quint64 previousItemId, quint64 currentItemId);
  void playlistAdded(int playlistNumber);
  void playlistOrderChanged();
  void playlistRemoved(int playlistNumber);
  void playlistNameChanged(int playlistNumber, const QString &name);
  void playlistModeChanged(int playlistNumber, PlaylistMode mode);
  void activePlaylistChanged(int previousPlaylistNumber,
                             int currentPlaylistNumber);
  void modelReset();
  void macrosChanged();
  void bulkUndoAvailableChanged(bool available);

private:
  friend class PlaylistPlaySequence;
  friend class PlaylistMediaAvailability;
  struct BulkSettingsChange {
    quint64 itemId = 0;
    PlaylistItemSettings before;
    PlaylistItemSettings after;
  };
  struct BulkPlaylistChange {
    int number = 0;
    PlaylistMode beforeMode = PlaylistMode::Standard;
    PlaylistMode afterMode = PlaylistMode::Standard;
    QVector<quint64> beforeOrder;
    QVector<quint64> afterOrder;
  };
  struct BulkUndo {
    std::optional<PlaylistState> removedPlaylist;
    int removedPlaylistIndex = 0;
    QList<BulkSettingsChange> settings;
    QList<BulkPlaylistChange> playlists;
    QList<PlaylistItemState> removedItems;
    QSet<quint64> replacedItemIds;
  };
  void updateItemNumbers(int playlistNumber, int firstRow, int lastRow = -1);
  std::unique_ptr<PlaylistPlaySequence> playSequence_;
  std::unique_ptr<PlaylistMediaAvailability> mediaAvailability_;

  std::optional<BulkUndo> bulkUndo_;
  QString documentId_ = newMacroId();
  QList<MacroDefinition> macros_;
  QHash<quint64, PlaylistItemState> itemsById_;
  QHash<quint64, PlaylistItemConversionState> conversionStatesById_;
  QHash<quint64, qint64> knownMediaDurationsById_;
  QHash<quint64, int> currentPlayCountsById_;
  QHash<int, QVector<quint64>> itemOrdersByPlaylist_;
  QHash<int, PlaylistState> playlistsByNumber_;
  QVector<int> playlistOrder_;
  quint64 nextItemId_ = 1;
  int activePlaylistNumber_ = 1;
  qint64 manualCrossfadeDurationMs_ = kDefaultItemFadeDurationMs;
  quint64 onAirItemId_ = 0;
  QSet<quint64> settingsLockedItemIds_;
  QString onAirErrorDetail_;
  quint64 cueItemId_ = 0;
  PlaylistCueState cueState_ = PlaylistCueState::None;
  QString cueDetail_;
  quint64 dskItemId_ = 0;
  quint64 editItemId_ = 0;
  quint64 bgmItemId_ = 0;
};
