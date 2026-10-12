#pragma once
#include "deck_source_session.h"
#include "program_gpu_compositor.h"
#include "async_shutdown_test_utils.h"
#include <QWidget>
#include <QThread>
#include <atomic>
#include <iostream>

namespace CueRetirementTest {
inline std::atomic_bool holdStop{false};
inline std::atomic_bool enteredStop{false};
struct Blocker { GstBin parent; };
struct BlockerClass { GstBinClass parent; };
G_DEFINE_TYPE(Blocker, cue_stop_blocker, GST_TYPE_BIN)
inline void cue_stop_blocker_class_init(BlockerClass *klass) {
    GST_ELEMENT_CLASS(klass)->change_state = [](GstElement *element, GstStateChange change) {
        if (change == GST_STATE_CHANGE_READY_TO_NULL) {
            enteredStop = true;
            while (holdStop.load()) QThread::msleep(5);
        }
        return GST_ELEMENT_CLASS(cue_stop_blocker_parent_class)->change_state(element, change);
    };
}
inline void cue_stop_blocker_init(Blocker *) {}
}

class DeckSourceSessionTest {
public:
    static bool run(const QString &path) {
        using namespace CueRetirementTest;
        struct Release { ~Release() { holdStop = false; } } release;
        QWidget video, monitor;
        ProgramGpuCompositor compositor(video.winId(), monitor.winId());
        DeckSourceSession deck;
        bool ok = compositor.start() && deck.initialize(0, 1920, 1080);
        if (!ok) return false;
        deck.attachSourceManagers(nullptr, nullptr, nullptr, &compositor);
        ok &= compositor.configureDeckInput(0, deck.pipeline(), deck.appSink());
        const DualDeckPlayer::DeckSource source{DualDeckPlayer::DeckSource::Type::File, path};
        ok &= deck.prepare(source).state == DualDeckPlayer::CueStatus::Loading;
        deck.refreshPreparation(); // Dispatch file opening before forcing the UI deadline.
        // Drive the actual deadline branch without a ten-second test sleep.
        deck.preparationDeadline_.setRemainingTime(0);
        ok &= deck.refreshPreparation().state == DualDeckPlayer::CueStatus::Error;
        ok &= deck.mediaOffline() && deck.preparationError_.contains(QStringLiteral("Media offline"));
        // The preparation worker may still own the state transition after the
        // UI deadline. Wait before modifying its graph in this test fixture.
        ok &= awaitShutdown([&] { return deck.filePreparation_->done(); });
        auto *blocker = GST_ELEMENT(g_object_new(cue_stop_blocker_get_type(), nullptr));
        gst_bin_add(GST_BIN(deck.pipeline()), blocker);
        gst_element_set_state(blocker, GST_STATE_PAUSED);
        auto destroyed = std::make_shared<std::atomic_bool>(false);
        g_object_weak_ref(G_OBJECT(deck.pipeline()), [](gpointer data, GObject *) {
            auto *flag = static_cast<std::shared_ptr<std::atomic_bool> *>(data);
            **flag = true;
            delete flag;
        }, new std::shared_ptr<std::atomic_bool>(destroyed));
        holdStop = true;
        enteredStop = false;
        const auto old = deck.pipeline();
        ok &= deck.replaceFilePipeline();
        ok &= deck.pipeline() != old;
        ok &= awaitShutdown([&] { return enteredStop.load(); });
        ok &= !destroyed->load();
        ok &= deck.prepare(source).state == DualDeckPlayer::CueStatus::Loading;
        ok &= awaitShutdown([&] { return deck.refreshPreparation().state == DualDeckPlayer::CueStatus::Ready; });
        // Confirm destruction/shutdown of the current graph does not wait on
        // the retired graph or release anything its late stop still needs.
        deck.detachCallbacks();
        ok &= deck.stopPipelinesForShutdown();
        ok &= compositor.stop();
        deck.releaseResourcesAfterConfirmedShutdown();
        holdStop = false;
        ok &= awaitShutdown([&] { return destroyed->load(); });
        std::cout << "CUE timeout, isolated replacement and late retirement: " << (ok ? "PASS" : "FAIL") << '\n';
        return ok;
    }
};
