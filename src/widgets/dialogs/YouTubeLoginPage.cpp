#include "widgets/dialogs/YouTubeLoginPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeAccountManager.hpp"
#include "providers/youtube/YouTubeChatServer.hpp"
#include "providers/youtube/YouTubeOAuth.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"

#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

#include <tuple>
#include <utility>

namespace chatterino {

YouTubeLoginPage::YouTubeLoginPage(QWidget *parent)
    : BaseWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    this->titleLabel_ = new QLabel("Connect YouTube", this);
    auto titleFont = makeResolvedFont(this->titleLabel_->font(), QFont::Bold);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    this->titleLabel_->setFont(titleFont);
    layout->addWidget(this->titleLabel_);

    this->descriptionLabel_ = new QLabel(
        "Watch public YouTube chats without an account. Connect to chat as "
        "yourself and use moderation tools where you have access.",
        this);
    this->descriptionLabel_->setWordWrap(true);
    layout->addWidget(this->descriptionLabel_);

    this->securityLabel_ = new QLabel(
        "Google login opens in your browser. Your password is never shared "
        "with Moltorino.",
        this);
    this->securityLabel_->setWordWrap(true);
    layout->addWidget(this->securityLabel_);

    layout->addSpacing(4);
    this->connectButton_ = new QPushButton("Connect YouTube account", this);
    this->connectButton_->setMinimumHeight(34);
    layout->addWidget(this->connectButton_);

    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setWordWrap(true);
    this->statusLabel_->setTextFormat(Qt::PlainText);
    this->statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->statusLabel_->hide();
    layout->addWidget(this->statusLabel_);

    this->statusProgress_ = new QProgressBar(this);
    this->statusProgress_->setRange(0, 0);
    this->statusProgress_->setTextVisible(false);
    this->statusProgress_->setFixedHeight(3);
    this->statusProgress_->hide();
    layout->addWidget(this->statusProgress_);

    this->disconnectLabel_ = new QLabel(
        "Review Moltorino's <a href=\"https://moltorino.com/privacy\">"
        "privacy policy</a>. Disconnect anytime in Settings > "
        "Accounts, or revoke Moltorino from your "
        "<a href=\"https://myaccount.google.com/connections\">Google "
        "Account</a>.",
        this);
    this->disconnectLabel_->setWordWrap(true);
    this->disconnectLabel_->setOpenExternalLinks(true);
    this->disconnectLabel_->setTextInteractionFlags(
        Qt::TextBrowserInteraction);
    layout->addWidget(this->disconnectLabel_);
    layout->addStretch(1);

    QObject::connect(this->connectButton_, &QPushButton::clicked, this, [this] {
        if (this->loginInProgress_)
        {
            if (this->authorizationAccepted_)
            {
                return;
            }
            auto cancelLogin = std::move(this->cancelLogin_);
            if (cancelLogin)
            {
                cancelLogin();
            }
            return;
        }
        this->startLogin();
    });

    this->themeChangedEvent();
}

void YouTubeLoginPage::startLogin()
{
    if (this->loginInProgress_)
    {
        return;
    }

    this->loginSucceeded_ = false;
    this->setBusy(true);
    this->authorizationAccepted_ = false;
    this->setStatus("Waiting for Google in your browser...",
                    StatusKind::Waiting);

    QPointer<YouTubeLoginPage> self(this);
    auto cancelLogin = YouTubeOAuth::start(
        this,
        [self](YouTubeAccountData data) {
            auto *accounts = getApp()->getAccounts();
            auto account = accounts->youtube.addAccount(data);
            if (!account)
            {
                if (self)
                {
                    self->cancelLogin_ = {};
                    self->setBusy(false);
                    self->setStatus(
                        "Google did not return a usable YouTube channel. Make "
                        "sure this Google account has a YouTube channel, then "
                        "try again.",
                        StatusKind::Error);
                }
                return;
            }

            accounts->youtube.selectAccount(account->channelID());
            if (auto *youtube = getApp()->getYouTubeChatServer())
            {
                youtube->refreshAccountRoles();
            }
            std::ignore = getSettings()->requestSave();
            if (!self)
            {
                return;
            }
            self->cancelLogin_ = {};
            self->loginSucceeded_ = true;
            const auto identity = account->handle().isEmpty()
                                      ? account->displayName()
                                      : QStringLiteral("@") + account->handle();
            self->setStatus(QStringLiteral("\u2713 Connected as ") + identity +
                                ".",
                            StatusKind::Success);
            self->setBusy(false);
        },
        [self](const QString &error) {
            if (!self)
            {
                return;
            }
            self->cancelLogin_ = {};
            self->setBusy(false);
            self->setStatus(
                error.isEmpty() ? "YouTube login was cancelled." : error,
                error.isEmpty() ? StatusKind::None : StatusKind::Error);
        },
        [self] {
            if (!self)
            {
                return;
            }
            self->authorizationAccepted_ = true;
            self->cancelLogin_ = {};
            self->setBusy(true);
            self->setStatus(
                "Google approved the login. Adding your YouTube channel...",
                StatusKind::Waiting);
        });

    if (this->loginInProgress_)
    {
        this->cancelLogin_ = std::move(cancelLogin);
    }
}

void YouTubeLoginPage::setBusy(bool busy)
{
    this->loginInProgress_ = busy;
    if (!busy)
    {
        this->authorizationAccepted_ = false;
    }
    const bool finishing = busy && this->authorizationAccepted_;
    this->connectButton_->setEnabled(!finishing);
    QString buttonText;
    if (finishing)
    {
        buttonText = "Finishing connection...";
    }
    else if (busy)
    {
        buttonText = "Cancel login";
    }
    else if (this->loginSucceeded_)
    {
        buttonText = "Connect another YouTube account";
    }
    else
    {
        buttonText = "Connect YouTube account";
    }
    this->connectButton_->setText(buttonText);
}

void YouTubeLoginPage::setStatus(const QString &status, StatusKind kind)
{
    this->statusKind_ = kind;
    this->statusLabel_->setText(status);
    this->statusLabel_->setVisible(!status.isEmpty());
    this->statusProgress_->setVisible(kind == StatusKind::Waiting);
    this->themeChangedEvent();
}

void YouTubeLoginPage::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();

    auto regular = this->theme->window.text;
    auto muted = regular;
    muted.setAlphaF(0.62F);
    auto status = muted;
    if (this->statusKind_ == StatusKind::Success)
    {
        status = this->theme->isLightTheme() ? QColor(u"#167a37")
                                             : QColor(u"#72d88b");
    }
    else if (this->statusKind_ == StatusKind::Error)
    {
        status = this->theme->isLightTheme() ? QColor(u"#a61b1b")
                                             : QColor(u"#ff8d8d");
    }

    auto setTextColor = [](QWidget *widget, const QColor &color) {
        auto palette = widget->palette();
        palette.setColor(QPalette::WindowText, color);
        palette.setColor(QPalette::Text, color);
        widget->setPalette(palette);
    };
    setTextColor(this->titleLabel_, regular);
    setTextColor(this->descriptionLabel_, regular);
    setTextColor(this->securityLabel_, muted);
    setTextColor(this->disconnectLabel_, muted);
    setTextColor(this->statusLabel_, status);

    auto linkPalette = this->disconnectLabel_->palette();
    linkPalette.setColor(QPalette::Link, this->theme->accent);
    linkPalette.setColor(QPalette::LinkVisited, this->theme->accent);
    this->disconnectLabel_->setPalette(linkPalette);

    auto track = regular;
    track.setAlphaF(0.14F);
    this->statusProgress_->setStyleSheet(
        QStringLiteral(
            "QProgressBar { border: 0; background: %1; }"
            "QProgressBar::chunk { background: %2; }")
            .arg(track.name(QColor::HexArgb), this->theme->accent.name()));
    this->update();
}

void YouTubeLoginPage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setBrush(getTheme()->window.background);
    painter.setPen({});
    painter.drawRect(this->rect());
}

}
