#include "switcher_addon_api.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <thread>

namespace {
// Test controls: 0 failure mode, 1 input, 2 FTB, 3 open ms, 4 close ms,
// 5 created, 6 destroyed, 7 opens, 8 closes, 9 writes, 10 polls,
// 11 enforce worker, 12 GUI violations, 13 nonempty address, 14 live objects.
std::array<std::atomic<int>, 15> state{};
void checkThread() { if (state[11] && pthread_main_np()) ++state[12]; }
void message(char *out, uint32_t size, const char *text) {
  if (out && size) { std::strncpy(out, text, size); out[size - 1] = 0; }
}
void *create() { checkThread(); ++state[5]; ++state[14]; return new int(0); }
void destroy(void *p) { checkThread(); if (p) { delete static_cast<int *>(p); ++state[6]; --state[14]; } }
int open(void *, const char *address, char *error, uint32_t size) {
  checkThread(); ++state[7]; if (address && *address) ++state[13];
  std::this_thread::sleep_for(std::chrono::milliseconds(state[3].load()));
  if (state[0] == 1) { message(error, size, "fixture open failed"); return 0; }
  return 1;
}
void close(void *) { checkThread(); ++state[8]; std::this_thread::sleep_for(std::chrono::milliseconds(state[4].load())); }
int product(void *, char *name, uint32_t capacity, char *, uint32_t) {
  checkThread(); message(name, capacity, "ATEM テスト"); return 1;
}
int getProgram(void *, int64_t *input, char *error, uint32_t size) {
  checkThread(); ++state[10];
  if (state[0] == 2) { message(error, size, "fixture poll failed"); return 0; }
  *input = state[1]; return 1;
}
int setProgram(void *, int64_t input, char *error, uint32_t size) {
  checkThread(); ++state[9];
  if (state[0] == 3) { message(error, size, "fixture program failed"); return 0; }
  state[1] = static_cast<int>(input); return 1;
}
int setFtb(void *, int black, char *error, uint32_t size) {
  checkThread(); ++state[9];
  if (state[0] == 4) { message(error, size, "fixture FTB failed"); return 0; }
  state[2] = black; return 1;
}
const VtrPonSwitcherAddonApiV1 api{1, sizeof(VtrPonSwitcherAddonApiV1), "vtr-pon-atem-addon", "Fixture",
    create, destroy, open, close, product, getProgram, setProgram, setFtb, nullptr};
}
extern "C" void fixtureSet(int key, int value) { state.at(key) = value; }
extern "C" int fixtureGet(int key) { return state.at(key).load(); }
extern "C" const VtrPonSwitcherAddonApiV1 *vtrpon_switcher_addon_get_api(uint32_t version) {
  static auto bad = api;
  bad = api;
  if (state[0] == 5) bad.abiVersion = 999;
  if (state[0] == 6) bad.addonId = "wrong-addon";
  if (state[0] == 7) bad.setProgramInput = nullptr;
  if (state[0] == 8) bad.structSize = 8;
  return version == 1 ? &bad : nullptr;
}
