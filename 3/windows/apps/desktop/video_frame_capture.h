#pragma once

#include <QImage>
#include <functional>

// Acquire on the player thread; run the owned frame's readback on a worker.
// The function retains the frame, never the player or its widgets.
using VideoFrameCapture = std::function<QImage()>;
