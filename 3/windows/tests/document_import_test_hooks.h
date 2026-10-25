#pragma once

#include "pdf_import_service.h"

#include <functional>

// Set before starting an import and clear only after the worker is drained.
// This header and its call site are compiled only into the queue test target.
namespace vtrpon::tests {
inline std::function<void(PdfImportPhase, int)> pdfImportProgress;
}
