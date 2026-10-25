#include "switcher_addon_api.h"
#include "network_video_addon_api.h"
#include "addon_runtime_api.h"
#include <cstdlib>
#include <cstring>
#include <unistd.h>
extern "C" const VtrPonSwitcherAddonApiV1 *vtrpon_switcher_addon_get_api(uint32_t version) {
  static const VtrPonSwitcherAddonApiV1 api{1, sizeof(VtrPonSwitcherAddonApiV1), "vtr-pon-atem-addon", "Fixture"};
  return version == 1 ? &api : nullptr;
}
extern "C" const VtrPonNetworkVideoApi *vtrpon_network_video_get_api(uint32_t version) {
  static const VtrPonNetworkVideoApi api{2, sizeof(VtrPonNetworkVideoApi), "vtr-pon-network-video-addon"};
  return version == 2 ? &api : nullptr;
}
extern "C" int32_t vtrpon_addon_runtime_status_v1(char *detail, uint32_t capacity) {
  const char *mode = std::getenv("VTRPON_FIXTURE_RUNTIME");
  if (mode && std::strcmp(mode, "hang") == 0) sleep(30);
  if (mode && std::strcmp(mode, "crash") == 0) std::abort();
  if (detail && capacity) { std::strncpy(detail, "Runtime fixture only", capacity); detail[capacity - 1] = 0; }
  return mode ? std::atoi(mode) : 0;
}
