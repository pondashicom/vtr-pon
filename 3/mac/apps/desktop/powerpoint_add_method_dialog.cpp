#include "powerpoint_add_method_dialog.h"

PowerPointAddMethodDialog::PowerPointAddMethodDialog(QWidget *parent)
    : DocumentAddMethodDialog(QStringLiteral("powerpoint.add"),
                              QStringLiteral("powerPoint"),
                              QStringLiteral("slide"), parent) {}

PowerPointAddSettings PowerPointAddMethodDialog::settings() const {
  const DocumentAddSettings document = documentSettings();
  PowerPointAddSettings result;
  if (document.method == DocumentAddMethod::VideoConversion) {
    result.method = PowerPointAddMethod::VideoConversion;
  } else if (document.method == DocumentAddMethod::PngSequence) {
    result.method = PowerPointAddMethod::PngSequence;
  }
  result.slideDurationSeconds = document.pageDurationSeconds;
  result.crossDissolveEnabled = document.crossDissolveEnabled;
  result.crossDissolveDurationSeconds =
      document.crossDissolveDurationSeconds;
  result.rememberForSession = document.rememberForSession;
  return result;
}
