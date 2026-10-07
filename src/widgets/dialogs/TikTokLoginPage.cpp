#include "widgets/dialogs/TikTokLoginPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/dialogs/MoltorinoDialogTheme.hpp"
#include "widgets/dialogs/TikTokLoginView.hpp"

#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QVBoxLayout>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

TikTokLoginPage::TikTokLoginPage(QWidget *parent)
    : BaseWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(10);

    this->title_ = new QLabel(u"Connect TikTok"_s, this);
    auto titleFont = makeResolvedFont(this->title_->font(), QFont::Bold);
    titleFont.setPointSize(titleFont.pointSize() + 2);
    this->title_->setFont(titleFont);
    layout->addWidget(this->title_);

    this->description_ = new QLabel(
        u"Read public TikTok chats without an account. Connect to send messages as yourself."_s,
        this);
    this->description_->setWordWrap(true);
    layout->addWidget(this->description_);
    layout->addSpacing(4);

    this->connect_ = new QPushButton(u"Connect TikTok account"_s, this);
    this->connect_->setMinimumHeight(34);
    this->connect_->setEnabled(TikTokLoginView::available());
    layout->addWidget(this->connect_);

    this->status_ = new QLabel(this);
    this->status_->setWordWrap(true);
    this->status_->setTextFormat(Qt::PlainText);
    this->status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->status_->hide();
    layout->addWidget(this->status_);

    this->progress_ = new QProgressBar(this);
    this->progress_->setRange(0, 0);
    this->progress_->setTextVisible(false);
    this->progress_->setFixedHeight(3);
    this->progress_->hide();
    layout->addWidget(this->progress_);

    if (!TikTokLoginView::available())
    {
        this->status(u"This build does not include TikTok login support."_s,
                     false);
    }
    layout->addStretch();

    QObject::connect(this->connect_, &QPushButton::clicked, this, [this] {
        this->start();
    });

    this->themeChangedEvent();
}

TikTokLoginPage::~TikTokLoginPage()
{
    if (this->login_)
    {
        this->login_->reject();
    }
}

void TikTokLoginPage::status(const QString &text, bool busy)
{
    this->status_->setText(text);
    this->status_->setVisible(!text.isEmpty());
    this->progress_->setVisible(busy);
    this->connect_->setEnabled(!busy && TikTokLoginView::available());
}

void TikTokLoginPage::start()
{
    if (this->login_)
    {
        this->login_->raise();
        this->login_->activateWindow();
        return;
    }
    this->status(u"Waiting for TikTok login..."_s, true);
    QPointer<TikTokLoginPage> weak(this);
    this->login_ = new TikTokLoginView(this, [weak](TikTokLoginExport login) {
        if (!weak)
        {
            return;
        }
        weak->status(u"Checking your TikTok account..."_s, true);
        const auto user = login.context.value("user").toObject();
        TikTokAccountData data;
        data.userID = user.value("uid").toString();
        data.handle = user.value("uniqueId").toString();
        data.displayName = user.value("nickname").toString();
        if (data.displayName.isEmpty())
        {
            data.displayName = data.handle;
        }
        data.session.userAgent = std::move(login.userAgent);
        data.session.cookies = std::move(login.cookies);
        data.session.context = std::move(login.context);
        data.session.context.insert(u"signingGnarly"_s,
                                    std::move(login.gnarly));
        data.session.context.insert(u"signingDynosaur"_s,
                                    std::move(login.dynosaur));
        data.session.ticketGuardStorage = std::move(login.ticketGuardStorage);
        data.session.msToken = std::move(login.msToken);
        data.session.msTokenStatus = 9;
        weak->api_.verifyAccount(
            std::move(data), [weak](ExpectedStr<TikTokAccountData> account) {
                if (!weak)
                {
                    return;
                }
                if (!account)
                {
                    weak->status(account.error(), false);
                    return;
                }
                weak->status(u"Saving your TikTok account..."_s, true);
                const auto name = account->displayName;
                getApp()->getAccounts()->tiktok.addAccount(
                    std::move(*account), [weak, name](ExpectedStr<void> saved) {
                        if (!weak)
                        {
                            return;
                        }
                        weak->status(saved ? u"Connected as %1."_s.arg(name)
                                           : saved.error(),
                                     false);
                    });
            });
    });
    this->login_->setAttribute(Qt::WA_DeleteOnClose);
    installMoltorinoDialogTheme(this->login_);
    QObject::connect(this->login_, &QDialog::finished, this, [this](int result) {
        this->login_.clear();
        if (result == QDialog::Rejected)
        {
            this->status({}, false);
        }
    });
    this->login_->open();
}

void TikTokLoginPage::hideEvent(QHideEvent *event)
{
    if (this->login_)
    {
        this->login_->reject();
    }
    BaseWidget::hideEvent(event);
}

void TikTokLoginPage::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    const auto regular = this->theme->window.text;
    auto muted = regular;
    muted.setAlphaF(0.62F);
    for (auto *label : {this->title_, this->description_, this->status_})
    {
        auto palette = label->palette();
        palette.setColor(QPalette::WindowText,
                         label == this->status_ ? muted : regular);
        label->setPalette(palette);
    }
    auto track = regular;
    track.setAlphaF(0.14F);
    this->progress_->setStyleSheet(
        QStringLiteral("QProgressBar { border: 0; background: %1; }"
                       "QProgressBar::chunk { background: %2; }")
            .arg(track.name(QColor::HexArgb), this->theme->accent.name()));
    this->update();
}

void TikTokLoginPage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(this->rect(), this->theme->window.background);
}
}
