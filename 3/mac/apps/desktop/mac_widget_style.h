#pragma once

#ifdef __APPLE__
#include <QApplication>
#include <QProxyStyle>

namespace vtrpon {
class MacComboPopupStyle final : public QProxyStyle {
public:
  explicit MacComboPopupStyle(QStyle *base) : QProxyStyle(base) {}
  int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                const QWidget *widget = nullptr, QStyleHintReturn *data = nullptr) const override {
    // Use the bounded list popup, which can move above a bottom-edge control.
    // The Mac menu popup ignores maxVisibleItems and can become a tiny scroller.
    if (hint == SH_ComboBox_Popup || hint == SH_ComboBox_UseNativePopup) return 0;
    return QProxyStyle::styleHint(hint, option, widget, data);
  }
};
inline void installMacComboPopupStyle(QApplication &application) {
  application.setStyle(new MacComboPopupStyle(application.style()));
}
} // namespace vtrpon
#endif
