#pragma once

#include <QSlider>
#include <functional>

class QMouseEvent;
class QPaintEvent;
class QEvent;
class QKeyEvent;

class AudioFaderWidget final : public QSlider
{
public:
    enum class Role {
        Master,
        Item,
        NominalItem,
    };

    explicit AudioFaderWidget(QWidget *parent = nullptr);
    AudioFaderWidget(Role role, QWidget *parent = nullptr);

    double linearGain() const;
    void setLinearGain(double gain);
    void setExtendedTopRange(bool extended);
    void setUserValueHandler(std::function<void(int)> handler);

protected:
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    QRect trackRect() const;
    QRect faderCapRect() const;
    int yForValue(int value) const;
    void setValueFromY(int y);
    void retranslateUi();
    void updateValueDescription();

    void requestUserValue(int value);
    std::function<void(int)> userValueHandler_;
    Role role_ = Role::Master;
    bool extendedTopRange_ = false;
    bool dragging_ = false;
    bool unityDetentLatched_ = false;
    int dragOffsetY_ = 0;
};
