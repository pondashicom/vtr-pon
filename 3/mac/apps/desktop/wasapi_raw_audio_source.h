#pragma once

namespace vtrpon {
// Windows capture only. Requires a selected endpoint or the explicit Default capture alias.
// RAW initialization failures are reported on the pipeline bus, without fallback.
bool registerWasapiRawAudioSource();
}
