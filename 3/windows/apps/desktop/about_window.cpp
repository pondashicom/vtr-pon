#include "about_window.h"
#include "language_text.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace vtrpon {

AboutWindow::AboutWindow(QWidget *parent) : QDialog(parent) {
  setObjectName(QStringLiteral("aboutWindow"));
  setAttribute(Qt::WA_StyledBackground);
  setWindowModality(Qt::NonModal);
  setWindowFlag(Qt::WindowContextHelpButtonHint, false);
  setMinimumSize(600, 320);
  resize(700, 360);

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  auto *body = new QWidget(this);
  auto *bodyLayout = new QVBoxLayout(body);
  bodyLayout->setContentsMargins(16, 16, 16, 18);
  auto *hero = new QWidget(body);
  hero->setObjectName(QStringLiteral("aboutHero"));
  hero->setAttribute(Qt::WA_StyledBackground);
  auto *heroLayout = new QHBoxLayout(hero);
  heroLayout->setContentsMargins(26, 20, 26, 20);
  heroLayout->setSpacing(32);

  auto *icon = new QLabel(hero);
  icon->setObjectName(QStringLiteral("aboutIcon"));
  icon->setFixedSize(180, 180);
  icon->setAlignment(Qt::AlignCenter);
  icon->setPixmap(qApp->windowIcon().pixmap(180, 180));
  heroLayout->addWidget(icon);

  auto *details = new QVBoxLayout;
  details->setSpacing(14);
  details->addStretch();
  auto *title = new QLabel(QStringLiteral("VTR-PON"), hero);
  title->setObjectName(QStringLiteral("aboutProductName"));
  QFont titleFont = title->font();
  titleFont.setPointSize(34);
  titleFont.setWeight(QFont::Bold);
  title->setFont(titleFont);
  details->addWidget(title);

  version_ = new QLabel(hero);
  version_->setObjectName(QStringLiteral("aboutVersion"));
  version_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  details->addWidget(version_);
  details->addStretch();
  heroLayout->addLayout(details, 1);
  bodyLayout->addWidget(hero);
  outer->addWidget(body, 1);

  auto *footer = new QWidget(this);
  footer->setObjectName(QStringLiteral("aboutFooter"));
  footer->setAttribute(Qt::WA_StyledBackground);
  auto *footerLayout = new QVBoxLayout(footer);
  footerLayout->setContentsMargins(32, 13, 32, 16);
  footerLayout->setSpacing(5);
  auto *links = new QHBoxLayout;
  links->setSpacing(28);
  links->addStretch();
  const auto addLink = [footer, links](const char *name) {
    auto *button = new QPushButton(footer);
    button->setObjectName(QString::fromLatin1(name));
    button->setProperty("aboutLink", true);
    button->setCursor(Qt::PointingHandCursor);
    button->setMinimumHeight(36);
    links->addWidget(button);
    return button;
  };
  license_ = addLink("aboutLicenseLink");
  notices_ = addLink("aboutNoticesLink");
  website_ = addLink("aboutWebsiteLink");
  links->addStretch();
  footerLayout->addLayout(links);

  auto *copyright = new QLabel(QStringLiteral("© 2026 Tetsu Suzuki"), footer);
  copyright->setObjectName(QStringLiteral("aboutCopyright"));
  copyright->setAlignment(Qt::AlignCenter);
  footerLayout->addWidget(copyright);

  status_ = new QLabel(footer);
  status_->setObjectName(QStringLiteral("aboutStatus"));
  status_->setAlignment(Qt::AlignCenter);
  status_->setWordWrap(true);
  status_->hide();
  footerLayout->addWidget(status_);
  outer->addWidget(footer);

  connect(license_, &QPushButton::clicked, this, [this] {
    openLocalDocument(QStringLiteral("README.html"));
  });
  connect(notices_, &QPushButton::clicked, this, [this] {
    openLocalDocument(QStringLiteral("THIRD-PARTY-NOTICES.md"));
  });
  connect(website_, &QPushButton::clicked, this, [this] {
    const QUrl url(QStringLiteral("https://pondashi.com/vtr-pon/"));
    if (QDesktopServices::openUrl(url)) {
      status_->hide();
    } else {
      status_->setText(uiText("about.open_failed").arg(url.toString()));
      status_->show();
    }
  });
  retranslate();
}

void AboutWindow::retranslate() {
  setWindowTitle(uiText("menu.help.about"));
  version_->setText(uiText("about.version").arg(QCoreApplication::applicationVersion()));
  license_->setText(uiText("about.license"));
  notices_->setText(uiText("about.notices"));
  website_->setText(uiText("about.website"));
}

void AboutWindow::openLocalDocument(const QString &name) {
  const QString path = QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("LICENSE/") + name);
  if (QFileInfo::exists(path) &&
      QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
    status_->hide();
    return;
  }
  status_->setText(uiText("about.open_failed").arg(path));
  status_->show();
}

} // namespace vtrpon
