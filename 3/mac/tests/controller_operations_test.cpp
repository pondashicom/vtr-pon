#include "controller_operations.h"

#include <QCoreApplication>
#include <QStringList>
#include <cstdio>
#include <limits>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } } while (false)

class Input final : public QObject {
  Q_OBJECT
signals:
  void step(int frames);
};

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  using A = ControllerOperations::Action;
  using S = ControllerOperations::Status;
  ControllerOperations operations;
  QObject owner;
  Input keyboard, mouse, device;
  int calls = 0, frames = 0;
  int notifications = 0;
  QObject::connect(&operations, &ControllerOperations::operationAboutToExecute,
                   &owner, [&](A, const QVariantList &, const QString &) { ++notifications; });
  operations.bind<int>(A::EditStepFrames, &owner, [&](int value) {
    if (value == 0) return false;
    ++calls;
    frames += value;
    return true;
  });
  for (auto *input : {&keyboard, &mouse, &device})
    operations.connectInput(input, &Input::step, A::EditStepFrames);
  emit keyboard.step(1);
  emit mouse.step(-1);
  emit device.step(3);
  CHECK(calls == 3 && frames == 3);
  CHECK(operations.execute(A::EditStepFrames, 0).status == S::Rejected);
  CHECK(operations.execute(A::EditStepFrames).status == S::InvalidArguments);
  CHECK(operations.execute(A::EditStepFrames, QStringLiteral("2")).status == S::InvalidArguments);
  CHECK(operations.execute(A::EditStepFrames, 2, 3).status == S::InvalidArguments);
  CHECK(calls == 3);
  CHECK(operations.execute(A::OffAir).status == S::Unavailable);
  CHECK(operations.execute(static_cast<A>(-1)).status == S::Unavailable);
  CHECK(notifications == 4); // Includes the bound handler's own rejection.

  operations.bind<double>(A::ProgramPlaybackRate, &owner, [](double) {});
  CHECK(operations.execute(A::ProgramPlaybackRate, std::numeric_limits<double>::quiet_NaN()).status == S::InvalidArguments);
  operations.bind<PlaylistStartMode>(A::EditStartMode, &owner, [](PlaylistStartMode) {});
  CHECK(operations.execute(A::EditStartMode, static_cast<PlaylistStartMode>(99)).status == S::InvalidArguments);
  operations.bind<qint64>(A::EditSeek, &owner, [](qint64) {});
  CHECK(operations.execute(A::EditSeek, qint64(-1)).status == S::InvalidArguments);
  CHECK(notifications == 4);

  auto *temporary = new QObject;
  operations.bind<>(A::OffAir, temporary, [] {});
  CHECK(operations.execute(A::OffAir).invoked());
  delete temporary;
  CHECK(operations.execute(A::OffAir).status == S::Unavailable);
  CHECK(notifications == 5);

  ControllerOperations::Result result;
  std::thread worker([&] { result = operations.execute(A::EditStepFrames, 1); });
  worker.join();
  CHECK(result.status == S::WrongThread && calls == 3);
  operations.disable();
  emit keyboard.step(1);
  CHECK(calls == 3);
  CHECK(operations.execute(A::EditStepFrames, 1).status == S::Unavailable);
  CHECK(notifications == 5);

  {
    ControllerOperations guarded;
    QStringList order;
    guarded.setGuard<int, QString>(A::SelectRelativeItem, &owner,
        [&](int offset, const QString &target) {
      order.append(QStringLiteral("guard"));
      return offset != 0 && target == QLatin1String("selected");
    });
    CHECK(guarded.execute(A::SelectRelativeItem, 1, QStringLiteral("selected")).status == S::Unavailable);
    CHECK(order.isEmpty());
    guarded.bind<int, QString>(A::SelectRelativeItem, &owner, [&](int, const QString &) {
      order.append(QStringLiteral("handler"));
    });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) {
      order.append(QStringLiteral("notification"));
    });
    CHECK(guarded.execute(A::SelectRelativeItem, 1).status == S::InvalidArguments);
    CHECK(guarded.execute(A::SelectRelativeItem, 1.0, QStringLiteral("selected")).status == S::InvalidArguments);
    CHECK(order.isEmpty());
    CHECK(guarded.execute(A::SelectRelativeItem, 0, QStringLiteral("selected")).status == S::Rejected);
    CHECK(order == QStringList{QStringLiteral("guard")});
    order.clear();
    CHECK(guarded.execute(A::SelectRelativeItem, -1, QStringLiteral("selected")).invoked());
    CHECK(order == (QStringList{QStringLiteral("guard"), QStringLiteral("notification"),
                               QStringLiteral("guard"), QStringLiteral("handler")}));

    guarded.bind<double>(A::ProgramPlaybackRate, &owner, [&](double) { order.append(QStringLiteral("handler")); });
    guarded.setGuard<double>(A::ProgramPlaybackRate, &owner, [&](double value) {
      order.append(QStringLiteral("guard"));
      return value > 0;
    }); // Registration also works after bind().
    order.clear();
    CHECK(guarded.execute(A::ProgramPlaybackRate, std::numeric_limits<double>::infinity()).status == S::InvalidArguments);
    CHECK(order.isEmpty());
    CHECK(guarded.execute(A::ProgramPlaybackRate, -1.0).status == S::Rejected);
    CHECK(order == QStringList{QStringLiteral("guard")});
    order.clear();
    CHECK(guarded.execute(A::ProgramPlaybackRate, 1.0).invoked());
    CHECK(order == (QStringList{QStringLiteral("guard"), QStringLiteral("notification"),
                               QStringLiteral("guard"), QStringLiteral("handler")}));
  }

  {
    ControllerOperations guarded;
    auto *guardOwner = new QObject;
    int notices = 0, invoked = 0;
    guarded.bind(A::ProgramPlay, &owner, [&] { ++invoked; });
    guarded.setGuard(A::ProgramPlay, guardOwner, [] { return true; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) { ++notices; });
    delete guardOwner;
    CHECK(guarded.execute(A::ProgramPlay).status == S::Unavailable);
    CHECK(notices == 0 && invoked == 0);
  }

  {
    ControllerOperations guarded;
    int notices = 0, invoked = 0;
    guarded.bind<int>(A::SelectRelativeItem, &owner, [&](int) { ++invoked; });
    guarded.setGuard<bool>(A::SelectRelativeItem, &owner, [](bool) { return true; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) { ++notices; });
    CHECK(guarded.execute(A::SelectRelativeItem, 1).status == S::InvalidArguments);
    CHECK(notices == 0 && invoked == 0);
  }

  {
    ControllerOperations guarded;
    QThread guardThread;
    auto *guardOwner = new QObject;
    int notices = 0, invoked = 0;
    guarded.bind(A::ProgramPlay, &owner, [&] { ++invoked; });
    guarded.setGuard(A::ProgramPlay, guardOwner, [] { return true; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) { ++notices; });
    QObject::connect(&guardThread, &QThread::finished, guardOwner, &QObject::deleteLater);
    guardOwner->moveToThread(&guardThread);
    guardThread.start();
    const auto moved = guarded.execute(A::ProgramPlay);
    guardThread.quit();
    guardThread.wait();
    CHECK(moved.status == S::WrongThread);
    CHECK(notices == 0 && invoked == 0);
  }

  {
    ControllerOperations guarded;
    QThread handlerThread;
    auto *handlerOwner = new QObject;
    int notices = 0, invoked = 0, guardCalls = 0;
    guarded.bind(A::ProgramPlay, handlerOwner, [&] { ++invoked; });
    guarded.setGuard(A::ProgramPlay, &owner, [&] { ++guardCalls; return true; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) { ++notices; });
    QObject::connect(&handlerThread, &QThread::finished, handlerOwner, &QObject::deleteLater);
    handlerOwner->moveToThread(&handlerThread);
    handlerThread.start();
    const auto moved = guarded.execute(A::ProgramPlay);
    handlerThread.quit();
    handlerThread.wait();
    CHECK(moved.status == S::WrongThread);
    CHECK(notices == 0 && invoked == 0 && guardCalls == 0);
  }

  {
    ControllerOperations guarded;
    auto *handlerOwner = new QObject;
    int notices = 0, invoked = 0;
    guarded.bind(A::ProgramPlay, handlerOwner, [&] { ++invoked; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) {
      ++notices;
      delete handlerOwner;
      handlerOwner = nullptr;
    });
    CHECK(guarded.execute(A::ProgramPlay).status == S::Unavailable);
    CHECK(notices == 1 && invoked == 0);
  }

  {
    ControllerOperations guarded;
    auto *guardOwner = new QObject;
    int notices = 0, invoked = 0;
    guarded.bind(A::ProgramPlay, &owner, [&] { ++invoked; });
    guarded.setGuard(A::ProgramPlay, guardOwner, [] { return true; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) {
      ++notices;
      delete guardOwner;
      guardOwner = nullptr;
    });
    CHECK(guarded.execute(A::ProgramPlay).status == S::Unavailable);
    CHECK(notices == 1 && invoked == 0);
  }

  {
    ControllerOperations guarded;
    int notices = 0, invoked = 0;
    guarded.bind(A::ProgramPlay, &owner, [&] { ++invoked; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) {
      ++notices;
      guarded.disable();
    });
    CHECK(guarded.execute(A::ProgramPlay).status == S::Unavailable);
    CHECK(notices == 1 && invoked == 0);
  }

  {
    ControllerOperations guarded;
    bool allowed = true;
    int notices = 0, invoked = 0;
    guarded.bind(A::ProgramPlay, &owner, [&] { ++invoked; });
    guarded.setGuard(A::ProgramPlay, &owner, [&] { return allowed; });
    QObject::connect(&guarded, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &) {
      ++notices;
      allowed = false;
    });
    CHECK(guarded.execute(A::ProgramPlay).status == S::Rejected);
    CHECK(notices == 1 && invoked == 0);
  }

  {
    ControllerOperations sourced;
    QStringList sources;
    sourced.bind(A::OffAir, &owner, [] {});
    sourced.bind(A::ProgramPause, &owner, [] {});
    sourced.bind(A::ProgramPlay, &owner, [&] {
      sourced.executeFrom("Macro", A::ProgramPause);
      sourced.execute(A::OffAir); // Restores the surrounding Shortcut scope.
    });
    QObject::connect(&sourced, &ControllerOperations::operationAboutToExecute,
                     &owner, [&](A, const QVariantList &, const QString &source) {
      sources.append(source);
    });
    CHECK(sourced.executeFrom("Shortcut", A::ProgramPlay).invoked());
    CHECK(sourced.executeFrom("Switcher", A::ProgramPause).invoked());
    CHECK(sourced.execute(A::OffAir).invoked());
    CHECK(sources == (QStringList{QStringLiteral("Shortcut"), QStringLiteral("Macro"),
        QStringLiteral("Shortcut"), QStringLiteral("Switcher"), QStringLiteral("UI")}));
  }
  return 0;
}

#include "controller_operations_test.moc"
