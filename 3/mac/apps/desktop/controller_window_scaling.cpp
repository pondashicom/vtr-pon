#include "controller_window_scaling.h"
#include "controller_scaling.h"

#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QGridLayout>
#include <QHash>
#include <QLayout>
#include <QListView>
#include <QPointer>
#include <QScopedValueRollback>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vtrpon {
namespace {

constexpr qreal kMinimumWindowScale = 0.75;

QSize normalClientSize(QWidget *window) {
#ifdef _WIN32
  // A maximized QWidget can still report its larger layout minimum at startup
  // even though Windows has fitted the native window to the work area.
  if (window->isMaximized()) {
    RECT client{};
    if (GetClientRect(reinterpret_cast<HWND>(window->winId()), &client) &&
        client.right > 0 && client.bottom > 0) {
      const HWND handle = reinterpret_cast<HWND>(window->winId());
      POINT origin{};
      MONITORINFO monitor{};
      monitor.cbSize = sizeof(monitor);
      if (ClientToScreen(handle, &origin) &&
          GetMonitorInfo(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST),
                         &monitor)) {
        // The initial oversized layout may also extend the native client below
        // the taskbar. Use the visible maximized work area, excluding the title.
        client.right = std::min(client.right, monitor.rcWork.right - origin.x);
        client.bottom = std::min(client.bottom, monitor.rcWork.bottom - origin.y);
      }
      const qreal dpr = window->devicePixelRatioF();
      return {qRound(client.right / dpr), qRound(client.bottom / dpr)};
    }
  }
#endif
  return window->size();
}

int scaledDimension(int value, qreal ratio) {
  // Preserve automatic spacing and unconstrained maximum dimensions.
  if (value < 0 || value == QWIDGETSIZE_MAX)
    return value;
  return value == 0 ? 0 : std::max(1, qRound(value * ratio));
}

QSize scaled(const QSize &size, qreal ratio) {
  return {scaledDimension(size.width(), ratio),
          scaledDimension(size.height(), ratio)};
}

QMargins scaled(const QMargins &margins, qreal ratio) {
  return {scaledDimension(margins.left(), ratio),
          scaledDimension(margins.top(), ratio),
          scaledDimension(margins.right(), ratio),
          scaledDimension(margins.bottom(), ratio)};
}

int scaled(int value, qreal ratio) { return scaledDimension(value, ratio); }

QFont scaled(QFont font, qreal ratio) {
  if (font.pointSizeF() > 0)
    font.setPointSizeF(font.pointSizeF() * ratio);
  else if (font.pixelSize() > 0)
    font.setPixelSize(scaledDimension(font.pixelSize(), ratio));
  return font;
}

QString scaled(const QString &sheet, qreal ratio) {
  return scaledStyleSheet(sheet, ratio);
}

// Always derive from the captured value, never from the previous rounded size.
// An external change (e.g. timer visibility, clock font or list/tile mode)
// starts a new baseline at the current scale without restoring obsolete
// settings.
template <typename T> struct Metric {
  T base{};
  T last{};
  qreal baseScale = 0;

  void capture(const T &value, qreal scale) {
    if (baseScale == 0 || last != value) {
      base = value;
      baseScale = scale;
    }
    last = value;
  }
  T at(qreal scale) const { return scaled(base, scale / baseScale); }
};

struct WidgetMetrics {
  QPointer<QWidget> widget;
  Metric<QSize> minimum, maximum, icon, grid;
  Metric<QFont> font;
  Metric<QString> sheet;
  Metric<int> listSpacing;

  void capture(QWidget *value, qreal scale) {
    if (!widget)
      *this = WidgetMetrics{};
    widget = value;
    minimum.capture(value->minimumSize(), scale);
    maximum.capture(value->maximumSize(), scale);
    font.capture(value->font(), scale);
    sheet.capture(value->styleSheet(), scale);
    if (auto *button = qobject_cast<QAbstractButton *>(value))
      icon.capture(button->iconSize(), scale);
    if (auto *list = qobject_cast<QListView *>(value)) {
      grid.capture(list->gridSize(), scale);
      listSpacing.capture(list->spacing(), scale);
    }
  }

  void apply(qreal scale) {
    if (!widget)
      return;
    widget->setFont(font.at(scale));
    if (!sheet.base.isEmpty())
      widget->setStyleSheet(sheet.at(scale));
    widget->setMinimumSize(minimum.at(scale));
    widget->setMaximumSize(maximum.at(scale));
    if (auto *button = qobject_cast<QAbstractButton *>(widget.data()))
      button->setIconSize(icon.at(scale));
    if (auto *list = qobject_cast<QListView *>(widget.data())) {
      list->setGridSize(grid.at(scale));
      list->setSpacing(listSpacing.at(scale));
      list->doItemsLayout();
    }
    widget->updateGeometry();
    widget->update();
  }
};

struct SpacerMetrics {
  QSpacerItem *item = nullptr;
  Metric<QSize> size;
};

struct LayoutMetrics {
  QPointer<QLayout> layout;
  Metric<QMargins> margins;
  Metric<int> horizontalSpacing, verticalSpacing;
  QList<SpacerMetrics> spacers;

  void capture(QLayout *value, qreal scale) {
    if (!layout)
      *this = LayoutMetrics{};
    layout = value;
    margins.capture(value->contentsMargins(), scale);
    auto *grid = qobject_cast<QGridLayout *>(value);
    horizontalSpacing.capture(
        grid ? grid->horizontalSpacing() : value->spacing(), scale);
    verticalSpacing.capture(grid ? grid->verticalSpacing() : value->spacing(),
                            scale);
    QList<SpacerMetrics> current;
    for (int i = 0; i < value->count(); ++i) {
      if (auto *spacer = value->itemAt(i)->spacerItem()) {
        SpacerMetrics metrics;
        for (const auto &previous : spacers) {
          if (previous.item == spacer)
            metrics = previous;
        }
        metrics.item = spacer;
        metrics.size.capture(spacer->sizeHint(), scale);
        current.append(metrics);
      }
    }
    spacers = current;
  }

  void apply(qreal scale) {
    if (!layout)
      return;
    layout->setContentsMargins(margins.at(scale));
    if (auto *grid = qobject_cast<QGridLayout *>(layout.data())) {
      grid->setHorizontalSpacing(horizontalSpacing.at(scale));
      grid->setVerticalSpacing(verticalSpacing.at(scale));
    } else {
      layout->setSpacing(horizontalSpacing.at(scale));
    }
    for (auto &spacer : spacers) {
      const QSize size = spacer.size.at(scale);
      const QSizePolicy policy = spacer.item->sizePolicy();
      spacer.item->changeSize(size.width(), size.height(),
                              policy.horizontalPolicy(),
                              policy.verticalPolicy());
    }
    layout->invalidate();
  }
};

} // namespace

class ControllerWindowScaling::Impl {
public:
  explicit Impl(QWidget *value)
      : window(value), normalSize(normalClientSize(value)),
        normalScale(uiScale()), currentScale(uiScale()) {
#ifdef Q_OS_MACOS
    // Cocoa can fit the maximized window to the screen before this baseline
    // is captured, leaving the fixed-size controls taller than its client area.
    // Scale from the space the layout actually needs, including native metrics.
    normalSize = normalSize.expandedTo(window->minimumSizeHint());
#endif
    applicationFont.capture(qApp->font(), currentScale);
    applicationSheet.capture(qApp->styleSheet(), currentScale);
    capture();
    // The top-level minimum must allow a smaller resize event to arrive.
    // Children retain their own scaled layout constraints.
    window->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    window->setMinimumSize(scaled(normalSize, kMinimumWindowScale));
    timer.setSingleShot(true);
    timer.setInterval(32);
    QObject::connect(&timer, &QTimer::timeout, window, [this] { resize(); });
#ifdef Q_OS_MACOS
    timer.start();
#endif
  }

  void capture() {
    for (auto it = widgets.begin(); it != widgets.end();) {
      if (it->widget.isNull())
        it = widgets.erase(it);
      else
        ++it;
    }
    for (auto it = layouts.begin(); it != layouts.end();) {
      if (it->layout.isNull())
        it = layouts.erase(it);
      else
        ++it;
    }
    const auto children = window->findChildren<QWidget *>();
    widgetOrder.clear();
    for (auto *widget : children)
      widgetOrder.append(widget);
    for (auto *widget : children)
      widgets[widget].capture(widget, currentScale);
    const auto childLayouts = window->findChildren<QLayout *>();
    for (auto *layout : childLayouts)
      layouts[layout].capture(layout, currentScale);
  }

  void resize() {
    if (applying || window->isMinimized())
      return;
    const qreal ratio =
        std::clamp(std::min(qreal(window->width()) / normalSize.width(),
                            qreal(window->height()) / normalSize.height()),
                   kMinimumWindowScale, 1.0);
    const qreal targetScale = normalScale * ratio;
    if (qAbs(targetScale - currentScale) < 0.002 * normalScale)
      return;
    QScopedValueRollback guard(applying, true);
    capture();
    applicationSheet.capture(qApp->styleSheet(), currentScale);
    applicationFont.capture(qApp->font(), currentScale);
    setUiScale(targetScale);
    window->setUpdatesEnabled(false);
    qApp->setFont(applicationFont.at(targetScale));
    qApp->setFont(applicationFont.at(targetScale), "QMenuBar");
    qApp->setFont(applicationFont.at(targetScale), "QMenu");
    const QVariant source = qApp->property("controllerUnscaledStyleSheet");
    qApp->setStyleSheet(source.isValid() ? scaledStyleSheet(source.toString())
                                         : applicationSheet.at(targetScale));
    for (auto &layout : layouts)
      layout.apply(targetScale);
    // Apply parent styles before child constraints and explicit fonts.
    for (const auto &widget : widgetOrder)
      if (widget)
        widgets[widget.data()].apply(targetScale);
    currentScale = targetScale;
    window->layout()->activate();
    // Record actual post-polish values without replacing their baselines.
    for (auto &widget : widgets) {
      if (!widget.widget)
        continue;
      auto *value = widget.widget.data();
      widget.minimum.last = value->minimumSize();
      widget.maximum.last = value->maximumSize();
      widget.font.last = value->font();
      widget.sheet.last = value->styleSheet();
      if (auto *button = qobject_cast<QAbstractButton *>(value))
        widget.icon.last = button->iconSize();
      if (auto *list = qobject_cast<QListView *>(value)) {
        widget.grid.last = list->gridSize();
        widget.listSpacing.last = list->spacing();
      }
    }
    for (auto &layout : layouts) {
      if (!layout.layout)
        continue;
      layout.margins.last = layout.layout->contentsMargins();
      auto *grid = qobject_cast<QGridLayout *>(layout.layout.data());
      layout.horizontalSpacing.last =
          grid ? grid->horizontalSpacing() : layout.layout->spacing();
      layout.verticalSpacing.last =
          grid ? grid->verticalSpacing() : layout.layout->spacing();
      for (auto &spacer : layout.spacers)
        spacer.size.last = spacer.item->sizeHint();
    }
    applicationSheet.last = qApp->styleSheet();
    window->setUpdatesEnabled(true);
  }

  QWidget *window;
  QSize normalSize;
  qreal normalScale;
  qreal currentScale;
  bool applying = false;
  QTimer timer;
  Metric<QFont> applicationFont;
  Metric<QString> applicationSheet;
  QHash<QWidget *, WidgetMetrics> widgets;
  QList<QPointer<QWidget>> widgetOrder;
  QHash<QLayout *, LayoutMetrics> layouts;
};

ControllerWindowScaling::ControllerWindowScaling(QWidget *window)
    : QObject(window), impl_(std::make_unique<Impl>(window)) {
  window->installEventFilter(this);
}

ControllerWindowScaling::~ControllerWindowScaling() = default;

bool ControllerWindowScaling::eventFilter(QObject *object, QEvent *event) {
  if (object == impl_->window && event->type() == QEvent::Resize &&
      !impl_->applying && !impl_->timer.isActive())
    impl_->timer.start();
  return QObject::eventFilter(object, event);
}

} // namespace vtrpon
