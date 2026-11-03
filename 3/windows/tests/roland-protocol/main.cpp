#include <QCoreApplication>
#include <cstdio>
bool runRolandSwitcherTrialRegression();
bool runRolandVr50HdLanRegression();
bool runRolandWinMMOutputRegression();
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().contains("--winmm")) {
#ifdef _WIN32
    return runRolandWinMMOutputRegression() ? 0 : 1;
#else
    std::puts("SKIP: native WinMM output completion requires Windows."); return 77;
#endif
  }
  if (app.arguments().contains("--vr50")) return runRolandVr50HdLanRegression() ? 0 : 1;
  return runRolandSwitcherTrialRegression() ? 0 : 1;
}
