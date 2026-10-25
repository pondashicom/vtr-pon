#pragma once

#include <memory>

class QWidget;
class ControllerOperations;

namespace vtrpon {
class UiLanguageManager;
}

class MainController final
{
public:
    MainController(QWidget *videoWindow, QWidget *controlWindow,
                   bool use4kProgramOutput = false,
                   vtrpon::UiLanguageManager *languageManager = nullptr);
    ~MainController();

    MainController(const MainController &) = delete;
    MainController &operator=(const MainController &) = delete;

    bool isAvailable() const;
    ControllerOperations *operations() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
