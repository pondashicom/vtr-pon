#pragma once

#include "document_add_method_dialog.h"

enum class PowerPointAddMethod {
  NativeSlideShow,
  VideoConversion,
  PngSequence,
};

struct PowerPointAddSettings {
  PowerPointAddMethod method = PowerPointAddMethod::NativeSlideShow;
  double slideDurationSeconds = 5.0;
  bool crossDissolveEnabled = true;
  double crossDissolveDurationSeconds = 1.0;
  bool rememberForSession = false;

  bool operator==(const PowerPointAddSettings &) const = default;
};

class PowerPointAddMethodDialog final : public DocumentAddMethodDialog {
public:
  explicit PowerPointAddMethodDialog(QWidget *parent = nullptr);

  PowerPointAddSettings settings() const;
};
