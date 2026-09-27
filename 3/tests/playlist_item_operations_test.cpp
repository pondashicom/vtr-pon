#include "controller_operations.h"
#include "playlist_item_operations.h"
#include "playlist_model.h"

#include <QCoreApplication>
#include <QTemporaryFile>
#include <cstdio>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression);          \
            return 1;                                                           \
        }                                                                       \
    } while (false)

int main(int argc, char **argv)
{
    // A Core-only target proves that id-based operations need no Panel or Widgets.
    QCoreApplication application(argc, argv);
    using A = ControllerOperations::Action;
    using S = ControllerOperations::Status;
    ControllerOperations operations;
    PlaylistModel model;
    QTemporaryFile file;
    CHECK(file.open());
    PlaylistItemRegistration source;
    source.filePath = file.fileName();
    source.settings.startMode = PlaylistStartMode::Play;
    source.settings.endMode = PlaylistEndMode::Next;
    PlaylistItemRegistration target = source;
    target.settings.startMode = PlaylistStartMode::Pause;
    target.settings.endMode = PlaylistEndMode::Stop;
    target.settings.inPointMs = 1000;
    target.settings.outPointMs = 5000;
    target.settings.nominalVolumeTenthsDb = -230;
    target.settings.fadeInEnabled = true;
    target.settings.fadeOutEnabled = true;
    target.settings.endCrossfadeEnabled = true;
    target.settings.repeatCount = 5;
    target.color = PlaylistItemColor::Yellow;
    target.separatorBelow = true;
    const auto ids = model.addItems({source, target});
    CHECK(ids.size() == 2);
    model.setItemCurrentPlayCount(ids[1], 4);
    const auto original = *model.itemState(ids[1]);

    {
        PlaylistItemOperations itemOperations(&model, &operations);
        int blocked = 0;
        QObject::connect(&itemOperations, &PlaylistItemOperations::actionBlocked,
                         &application, [&](PlaylistItemAction) { ++blocked; });

        CHECK(operations.executeFrom("test-device", A::CreatePlaylist).invoked());
        const int secondPlaylist = model.activePlaylistNumber();
        CHECK(secondPlaylist != 1);
        CHECK(operations.executeFrom("test-device", A::RenamePlaylist,
                                     secondPlaylist, QStringLiteral("External")).invoked());
        CHECK(model.playlistName(secondPlaylist) == QStringLiteral("External"));
        // Id-based clipboard works on a hidden playlist without changing selection/tab.
        CHECK(operations.executeFrom("test-device", A::CopyItemState, ids[0]).invoked());
        CHECK(operations.executeFrom("test-device", A::PasteItemState, ids[1]).invoked());
        auto expected = original;
        expected.settings.startMode = source.settings.startMode;
        expected.settings.endMode = source.settings.endMode;
        expected.settings.fadeInEnabled = source.settings.fadeInEnabled;
        expected.settings.fadeOutEnabled = source.settings.fadeOutEnabled;
        expected.settings.endCrossfadeEnabled = source.settings.endCrossfadeEnabled;
        CHECK(*model.itemState(ids[1]) == expected);
        CHECK(model.itemCurrentPlayCount(ids[1]) == 4);
        CHECK(model.activePlaylistNumber() == secondPlaylist);
        // Change only the requested mode, retaining timing, fades and repeat details.
        CHECK(operations.execute(A::SetItemStartMode, ids[1], PlaylistStartMode::Pause).invoked());
        expected.settings.startMode = PlaylistStartMode::Pause;
        CHECK(*model.itemState(ids[1]) == expected);
        CHECK(operations.execute(A::SetItemEndMode, ids[1], PlaylistEndMode::Stop).invoked());
        expected.settings.endMode = PlaylistEndMode::Stop;
        CHECK(*model.itemState(ids[1]) == expected);
        CHECK(model.itemCurrentPlayCount(ids[1]) == 4);
        CHECK(model.activePlaylistNumber() == secondPlaylist);
        CHECK(operations.execute(A::SetItemEndMode, ids[1], PlaylistEndMode::Repeat).invoked());
        expected.settings.endMode = PlaylistEndMode::Repeat;
        CHECK(*model.itemState(ids[1]) == expected);
        CHECK(model.itemState(ids[1])->settings.repeatCount == 5);
        CHECK(operations.execute(A::SetItemFadeInEnabled, ids[1], false).invoked());
        expected.settings.fadeInEnabled = false;
        CHECK(*model.itemState(ids[1]) == expected);
        CHECK(operations.execute(A::SetItemFadeOutEnabled, ids[1], false).invoked());
        expected.settings.fadeOutEnabled = false;
        CHECK(*model.itemState(ids[1]) == expected);
        CHECK(operations.execute(A::SetItemEndCrossfadeEnabled, ids[1], true).invoked());
        expected.settings.endCrossfadeEnabled = true;
        CHECK(operations.execute(A::SetItemEndMode, ids[1], PlaylistEndMode::Goto).status == S::Rejected);
        CHECK(*model.itemState(ids[1]) == expected);
        CHECK(operations.execute(A::ActivatePlaylist, 1).invoked());
        CHECK(operations.execute(A::ActivatePlaylist, 99999).status == S::Rejected);
        CHECK(model.activePlaylistNumber() == 1);

        CHECK(operations.execute(A::SetItemDisplayName, ids[1],
                                  QStringLiteral("  Renamed  ")).invoked());
        CHECK(model.itemState(ids[1])->displayName == QStringLiteral("Renamed"));
        CHECK(model.itemState(ids[1])->filePath == original.filePath);
        CHECK(operations.execute(A::SetItemColor, ids[1], PlaylistItemColor::Blue).invoked());
        CHECK(operations.execute(A::SetItemSeparatorBelow, ids[1], false).invoked());
        CHECK(model.itemState(ids[1])->color == PlaylistItemColor::Blue);
        CHECK(!model.itemState(ids[1])->separatorBelow);
        CHECK(operations.execute(A::SetItemColor, ids[1],
                                  static_cast<PlaylistItemColor>(999)).status == S::Rejected);
        CHECK(operations.execute(A::SetItemDisplayName, quint64{0},
                                  QStringLiteral("Missing")).status == S::Rejected);
        CHECK(operations.execute(A::SetItemColor, ids[1], 2).status == S::InvalidArguments);
        CHECK(operations.execute(A::RenamePlaylist, secondPlaylist).status == S::InvalidArguments);

        // Runtime guards are used when the request comes from a device as well as UI.
        auto converting = source;
        converting.conversionState = PlaylistItemConversionState{};
        const auto convertingIds = model.addItems({converting});
        CHECK(convertingIds.size() == 1);
        const quint64 convertingId = convertingIds[0];
        for (auto action : {PlaylistItemAction::OnAir, PlaylistItemAction::Edit,
                            PlaylistItemAction::Dsk, PlaylistItemAction::CopyItem}) {
            CHECK(!itemOperations.allowItemAction(convertingId, action));
        }
        CHECK(operations.executeFrom("test-device", A::CopyItemState, convertingId).status == S::Rejected);
        CHECK(operations.executeFrom("test-device", A::PasteItemState, convertingId).status == S::Rejected);
        CHECK(blocked == 6);
        CHECK(operations.execute(A::SetItemStartMode, convertingId, PlaylistStartMode::Pause).status == S::Rejected);
        CHECK(operations.execute(A::SetItemEndMode, convertingId, PlaylistEndMode::Stop).status == S::Rejected);
        model.setItemSettingsLocked(ids[1], true);
        CHECK(operations.execute(A::SetItemFadeInEnabled, ids[1], true).status == S::Rejected);
        CHECK(operations.execute(A::SetItemFadeOutEnabled, ids[1], true).status == S::Rejected);
        CHECK(operations.execute(A::SetItemEndCrossfadeEnabled, ids[1], false).status == S::Rejected);
        CHECK(operations.execute(A::SetItemStartMode, ids[1], PlaylistStartMode::Play).status == S::Rejected);
        CHECK(operations.execute(A::SetItemEndMode, ids[1], PlaylistEndMode::Next).status == S::Rejected);
        CHECK(operations.execute(A::PasteItemState, ids[1]).status == S::Rejected);
        model.setItemSettingsLocked(ids[1], false);
        CHECK(model.removeItem(ids[0]));
        CHECK(operations.execute(A::CopyItemState, ids[0]).status == S::Rejected);
        // Failed copies retain the previously copied modes and fade flags.
        CHECK(operations.execute(A::PasteItemState, ids[1]).invoked());
    }
    CHECK(operations.execute(A::CopyItemState, ids[1]).status == S::Unavailable);
    CHECK(operations.execute(A::SetItemColor, ids[1], PlaylistItemColor::Red).status == S::Unavailable);

    ControllerOperations disabledOperations;
    PlaylistItemOperations itemOperations(&model, &disabledOperations);
    const auto beforeDisable = model.snapshot();
    disabledOperations.disable();
    CHECK(disabledOperations.execute(A::CreatePlaylist).status == S::Unavailable);
    CHECK(disabledOperations.execute(A::RenamePlaylist, 1, QStringLiteral("Disabled")).status == S::Unavailable);
    CHECK(disabledOperations.execute(A::SetItemDisplayName, ids[1], QStringLiteral("Disabled")).status == S::Unavailable);
    CHECK(disabledOperations.execute(A::SetItemColor, ids[1], PlaylistItemColor::Red).status == S::Unavailable);
    CHECK(disabledOperations.execute(A::SetItemSeparatorBelow, ids[1], true).status == S::Unavailable);
    CHECK(disabledOperations.execute(A::PasteItemState, ids[1]).status == S::Unavailable);
    CHECK(model.snapshot() == beforeDisable);
    qInfo() << "Playlist item operations: Core-only, input-independent mutations, clipboard and guards passed";
    return 0;
}
