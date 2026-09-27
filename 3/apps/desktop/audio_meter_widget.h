#pragma once

#include <QElapsedTimer>
#include <QWidget>

#include <array>

class QPaintEvent;
class QEvent;

class AudioMeterWidget final : public QWidget
{
public:
    explicit AudioMeterWidget(QWidget *parent = nullptr);

    void setCompactStripMode(bool compact);
    void setExtendedTopRange(bool extended);
    // Returns true while release, cell fade, or clip hold needs another sample.
    bool setPeakLevelsDbfs(double leftPeakDb, double rightPeakDb, bool clipped);
    void resetToSilence();

protected:
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    static constexpr int kSegmentCount = 60;
    using CellBrightness = std::array<double, kSegmentCount>;

    static bool updateCellBrightness(CellBrightness &brightness, double peakDb,
                                     double elapsedSeconds, bool initialize = false);
    static bool cellFadePending(const CellBrightness &brightness, double peakDb);

    double leftPeakDb_ = -60.0;
    double rightPeakDb_ = -60.0;
    CellBrightness leftCellBrightness_ = {};
    CellBrightness rightCellBrightness_ = {};
    QElapsedTimer releaseClock_;
    QElapsedTimer clipClock_;
    qint64 clipUntilMs_ = 0;
    bool clipIndicatorOn_ = false;
    bool animationPending_ = false;
    bool compactStripMode_ = false;
    bool extendedTopRange_ = false;
};
