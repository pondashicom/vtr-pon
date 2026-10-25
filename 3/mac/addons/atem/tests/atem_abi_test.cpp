#include "switcher_addon_api.h"
#include "addon_runtime_api.h"
#include <cstdio>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main() {
  CHECK(!vtrpon_switcher_addon_get_api(0));
  const auto *api = vtrpon_switcher_addon_get_api(1);
  CHECK(api && api->structSize == sizeof(*api) && !std::strcmp(api->addonId, "vtr-pon-atem-addon"));
  char error[256]{}, name[32]{};
  int64_t input = -77;
  int black = -1, transition = -1;
  CHECK(!api->open(nullptr, "", error, sizeof(error)) && error[0]);
  for (int i = 0; i < 10; ++i) {
    void *device = api->create(); CHECK(device);
    CHECK(!api->getProductName(device, name, sizeof(name), error, sizeof(error)));
    CHECK(!api->getProgramInput(device, &input, error, sizeof(error)) && input == -77);
    CHECK(!api->setProgramInput(device, 1, error, sizeof(error)));
    CHECK(!api->setFadeToBlack(device, 1, error, sizeof(error)));
    CHECK(!api->getFadeToBlackState(device, &black, &transition, error, sizeof(error)) && black == -1 && transition == -1);
    api->close(device); api->close(device); api->destroy(device);
  }
  char guard[] = {'a','b','c'};
  CHECK(!api->getProgramInput(nullptr, &input, guard + 1, 1) && guard[0] == 'a' && guard[1] == 0 && guard[2] == 'c');
  CHECK(!api->getProgramInput(nullptr, nullptr, nullptr, 0));
  api->close(nullptr); api->destroy(nullptr);
  const int status = vtrpon_addon_runtime_status_v1(error, sizeof(error));
  CHECK(status >= 0 && status <= 3);
  std::printf("ABI/disconnected lifecycle PASS; runtime status=%d detail=%s; no hardware connection requested\n", status, error);
  return 0;
}
