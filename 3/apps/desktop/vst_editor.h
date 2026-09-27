#pragma once
#include "vst_service.h"
class QPushButton;
namespace vtrpon {
void bindVstButton(QPushButton *button, VstSlot slot);
bool showVstEditor(QPushButton *button, VstSlot slot);
bool updateVstButton(QPushButton *button);
}
