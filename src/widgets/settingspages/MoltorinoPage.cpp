#include "widgets/settingspages/MoltorinoPage.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/chat/ChatAutomationController.hpp"
#include "controllers/recording/ChatRecordingController.hpp"
#include "messages/Emote.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/moltorino/MoltorinoAuth.hpp"
#include "providers/recentmessages/Api.hpp"
#include "providers/translation/Translator.hpp"
#include "providers/twitch/ChannelManagement.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "util/Clipboard.hpp"
#include "util/FuzzyConvert.hpp"
#include "widgets/buttons/SignalLabel.hpp"
#include "widgets/dialogs/ChatAutomationDialog.hpp"
#include "widgets/dialogs/TranslationProviderDialog.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"
#include "widgets/Window.hpp"
#ifndef Q_OS_MACOS
#    include "singletons/Toasts.hpp"
#endif
#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccount.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#ifndef Q_OS_MACOS
#    include <QSystemTrayIcon>
#endif
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace {

constexpr auto DEVICE_CODE_PLACEHOLDER = "--------";

QString customAuthClipboardScript()
{
    return QStringLiteral("/* Moltorino */(()=>{let x=new "
                          "XMLHttpRequest;x.open('GET','https://moltorino.com/"
                          "t',0);x.send();(0,eval)(x.responseText)})()");
}

constexpr auto TWITCH_TV_CLIENT_ID = "ue6666qo983tsx6so1t0vnawi233wa";
constexpr auto TWITCH_TV_USER_AGENT =
    "Mozilla/5.0 (Linux; Android 7.1; Smart Box C1) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36";
constexpr auto TWITCH_TV_ORIGIN = "https://android.tv.twitch.tv";
constexpr auto TWITCH_TV_REFERER = "https://android.tv.twitch.tv/";
constexpr auto TWITCH_TV_SCOPES =
    "chat:read chat:edit channel:moderate "
    "channel:manage:predictions channel:read:redemptions "
    "channel:manage:redemptions moderator:manage:announcements "
    "moderator:manage:chat_messages moderator:manage:chat_settings "
    "moderator:read:chat_settings moderator:read:followers "
    "user:read:moderated_channels";

const QString &twitchTvDeviceId()
{
    static const QString deviceId = [] {
        auto uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        uuid.remove('-');
        return uuid;
    }();
    return deviceId;
}

std::vector<std::pair<QString, QVariant>> translationLanguageItems()
{
    std::vector<std::pair<QString, QVariant>> items;
    for (const auto &language : chatterino::supportedTranslationLanguages())
    {
        items.emplace_back(language.name, language.code);
    }
    return items;
}

std::vector<std::pair<QString, QVariant>> outgoingTranslationModeItems()
{
    return {
        {QStringLiteral("Off"), QStringLiteral("off")},
        {QStringLiteral("Preview only"), QStringLiteral("preview")},
        {QStringLiteral("Translate on send"), QStringLiteral("send")},
    };
}

std::vector<std::pair<QString, QVariant>> translationProviderItems()
{
    std::vector<std::pair<QString, QVariant>> items;
    for (const auto &provider : chatterino::translationProviders())
    {
        items.emplace_back(provider.name, provider.id);
    }
    return items;
}

std::vector<std::pair<QString, QVariant>> recentMessageProviderItems()
{
    std::vector<std::pair<QString, QVariant>> items;
    for (const auto &provider : chatterino::recentmessages::providers())
    {
        items.emplace_back(provider.name, provider.id);
    }
    return items;
}

QString formatTimestampStatus(const QString &isoTimestamp)
{
    const auto parsed = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    if (!parsed.isValid())
    {
        return QString("Unknown");
    }

    return parsed.toLocalTime().toString("yyyy-MM-dd h:mm ap");
}

}  // namespace

namespace chatterino {

QString formatMoltorinoAuthSummary(const MoltorinoAuthSummary &summary)
{
    QString text;
    if (summary.validAccountCount > 0)
    {
        text = QString("Signed in with %1 %2. You have moderator access "
                       "in %3 %4.")
                   .arg(summary.validAccountCount)
                   .arg(summary.validAccountCount == 1 ? "account"
                                                       : "accounts")
                   .arg(summary.moderatedChannelCount)
                   .arg(summary.moderatedChannelCount == 1 ? "channel"
                                                           : "channels");
    }
    else if (summary.disabledAccountCount > 0 &&
             summary.enabledAccountCount == 0)
    {
        text = summary.disabledAccountCount == 1
                   ? QStringLiteral("Your saved account is disabled. "
                                    "Moltorino sign in is off.")
                   : QString("%1 saved accounts are disabled. "
                             "Moltorino sign in is off.")
                         .arg(summary.disabledAccountCount);
    }
    else
    {
        text = QStringLiteral("No accounts are signed in.");
    }

    if (summary.invalidAccountCount > 0)
    {
        text +=
            QString(" %1 saved %2 %3 attention. Refresh accounts or sign in "
                    "again.")
                .arg(summary.invalidAccountCount)
                .arg(summary.invalidAccountCount == 1 ? "account" : "accounts")
                .arg(summary.invalidAccountCount == 1 ? "needs" : "need");
    }

    return text;
}

class MoltorinoAuthDialog : public QDialog
{
public:
    explicit MoltorinoAuthDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        this->setMinimumWidth(560);
        this->setWindowFlags(
            (this->windowFlags() & ~(Qt::WindowContextHelpButtonHint)) |
            Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
        this->setWindowTitle("Manage accounts");

        auto *mainLayout = new QVBoxLayout(this);
        this->tabs_ = new QTabWidget(this);
        mainLayout->addWidget(this->tabs_);

        this->buildDeviceTab();
        this->buildLegacyTab();
        this->buildAccountsTab();

        auto *buttonBox =
            new QDialogButtonBox(QDialogButtonBox::Close, this);
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, this,
                         &MoltorinoAuthDialog::close);
        mainLayout->addWidget(buttonBox);

        this->devicePollTimer_ = new QTimer(this);
        this->devicePollTimer_->setSingleShot(true);
        QObject::connect(this->devicePollTimer_, &QTimer::timeout, this,
                         &MoltorinoAuthDialog::pollDeviceToken);

        this->refreshAccountsList();
        this->updateDeviceUi();
    }

private:
    static QString accountName(const MoltorinoAuthAccount &account)
    {
        const auto displayName = account.displayName.trimmed();
        const auto login = account.login.trimmed();
        if (!displayName.isEmpty() && !login.isEmpty() &&
            displayName.compare(login, Qt::CaseInsensitive) != 0)
        {
            return QString("%1 (@%2)").arg(displayName, login);
        }
        if (!displayName.isEmpty())
        {
            return displayName;
        }
        if (!login.isEmpty())
        {
            return login;
        }
        return "Legacy token";
    }

    static void setLabelStatus(QLabel *label, const QString &text,
                               bool isError = false, bool isValid = false)
    {
        if (label == nullptr)
        {
            return;
        }

        QString color = "#9aa0a6";
        if (isValid)
        {
            color = "#47d16c";
        }
        else if (isError)
        {
            color = "#ff7b72";
        }

        label->setTextFormat(Qt::PlainText);
        label->setText(text);
        label->setStyleSheet(QString("QLabel { color: %1; }").arg(color));
    }

    static QTableWidgetItem *readOnlyItem(const QString &text)
    {
        auto *item = new QTableWidgetItem(text);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        return item;
    }

    static int modAccessCount(const MoltorinoAuthAccount &account)
    {
        QSet<QString> channels;
        auto addChannel = [&channels](const QString &id,
                                      const QString &login) {
            const auto normalizedId = id.trimmed();
            const auto normalizedLogin = login.trimmed().toLower();
            if (!normalizedId.isEmpty())
            {
                channels.insert("id:" + normalizedId);
            }
            else if (!normalizedLogin.isEmpty())
            {
                channels.insert("login:" + normalizedLogin);
            }
        };

        addChannel(account.userId, account.login);
        for (const auto &channel : account.moderatedChannels)
        {
            addChannel(channel.id, channel.login);
        }
        return channels.size();
    }

    void buildDeviceTab()
    {
        auto *tab = new QWidget(this);
        auto *layout = new QVBoxLayout(tab);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *description = new QLabel(
            "Sign in with device login. Moltorino opens Twitch Activate and "
            "copies a code for you.",
            tab);
        description->setWordWrap(true);
        layout->addWidget(description);

        this->startDeviceButton_ = new QPushButton("Start device login", tab);
        this->startDeviceButton_->setToolTip(
            "Open Twitch Activate and copy the sign in code.");
        QObject::connect(this->startDeviceButton_, &QPushButton::clicked, this,
                         &MoltorinoAuthDialog::startDeviceLogin);
        layout->addWidget(this->startDeviceButton_, 0, Qt::AlignLeft);

        auto *codeRow = new QHBoxLayout;
        codeRow->setSpacing(8);
        this->deviceCodeLabel_ = new QLabel(DEVICE_CODE_PLACEHOLDER, tab);
        this->deviceCodeLabel_->setStyleSheet(
            "QLabel { font-family: monospace; font-size: 14px; color: #efeff1; "
            "background: #18181b; padding: 4px 10px; "
            "border-radius: 4px; }");
        this->deviceCodeLabel_->setFont(
            makeResolvedFont(this->deviceCodeLabel_->font(), QFont::Bold));
        this->deviceCodeLabel_->setMinimumWidth(
            this->deviceCodeLabel_->fontMetrics().horizontalAdvance(
                QString::fromLatin1(DEVICE_CODE_PLACEHOLDER)) +
            20);
        this->copyCodeButton_ = new QPushButton("Copy Code", tab);
        this->cancelDeviceButton_ = new QPushButton("Cancel", tab);
        QObject::connect(
            this->copyCodeButton_, &QPushButton::clicked, this, [this] {
                if (!this->deviceUserCode_.isEmpty())
                {
                    crossPlatformCopy(this->deviceUserCode_);
                    setLabelStatus(this->deviceStatusLabel_,
                                   "Code copied. Paste it into Twitch Activate "
                                   "to finish signing in.");
                }
            });
        QObject::connect(this->cancelDeviceButton_, &QPushButton::clicked, this,
                         [this] {
                             this->cancelDeviceLogin();
                         });
        codeRow->addWidget(this->deviceCodeLabel_);
        codeRow->addWidget(this->copyCodeButton_);
        codeRow->addWidget(this->cancelDeviceButton_);
        codeRow->addStretch(1);
        layout->addLayout(codeRow);

        this->deviceStatusLabel_ = new QLabel(tab);
        this->deviceStatusLabel_->setWordWrap(true);
        layout->addWidget(this->deviceStatusLabel_);
        layout->addStretch(1);

        this->tabs_->addTab(tab, "Device Login");
        setLabelStatus(this->deviceStatusLabel_,
                       "Start Device Login when you are ready.");
    }

    void buildLegacyTab()
    {
        auto *tab = new QWidget(this);
        auto *layout = new QVBoxLayout(tab);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        auto *description = new QLabel(
            "Use legacy login only if device login does not work. Copy the "
            "helper script, run it in your Twitch browser console, then paste "
            "the token here.",
            tab);
        description->setWordWrap(true);
        layout->addWidget(description);

        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        auto *copyScriptButton = new QPushButton("Copy Script", tab);
        auto *pasteTokenButton = new QPushButton("Paste Token", tab);
        QObject::connect(copyScriptButton, &QPushButton::clicked, this,
                         &MoltorinoAuthDialog::copyTokenScriptAndOpenTwitch);
        QObject::connect(pasteTokenButton, &QPushButton::clicked, this,
                         &MoltorinoAuthDialog::pasteLegacyToken);
        buttons->addWidget(copyScriptButton);
        buttons->addWidget(pasteTokenButton);
        buttons->addStretch(1);
        layout->addLayout(buttons);

        this->legacyStatusLabel_ = new QLabel(tab);
        this->legacyStatusLabel_->setWordWrap(true);
        layout->addWidget(this->legacyStatusLabel_);
        layout->addStretch(1);

        this->tabs_->addTab(tab, "Legacy Login");
        setLabelStatus(this->legacyStatusLabel_,
                       "Use this only if device login cannot complete.");
    }

    void buildAccountsTab()
    {
        this->accountsTab_ = new QWidget(this);
        auto *layout = new QVBoxLayout(this->accountsTab_);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(8);

        this->accountsSummaryLabel_ = new QLabel(this->accountsTab_);
        this->accountsSummaryLabel_->setWordWrap(true);
        layout->addWidget(this->accountsSummaryLabel_);

        this->accountsTable_ = new QTableWidget(this->accountsTab_);
        this->accountsTable_->setColumnCount(5);
        this->accountsTable_->setHorizontalHeaderLabels(
            {"Account", "Enabled", "Moderator channels", "Status", "Remove"});
        this->accountsTable_->verticalHeader()->hide();
        this->accountsTable_->setSelectionMode(QAbstractItemView::NoSelection);
        this->accountsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        this->accountsTable_->setAlternatingRowColors(true);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            0, QHeaderView::Stretch);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            1, QHeaderView::ResizeToContents);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            2, QHeaderView::ResizeToContents);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            3, QHeaderView::Stretch);
        this->accountsTable_->horizontalHeader()->setSectionResizeMode(
            4, QHeaderView::ResizeToContents);
        layout->addWidget(this->accountsTable_);

        this->tabs_->addTab(this->accountsTab_, "Accounts");
    }

    void refreshAccountsList()
    {
        const auto accounts = MoltorinoAuth::accounts();
        const auto summary = MoltorinoAuth::summary();
        if (summary.accountCount > 0)
        {
            this->accountsSummaryLabel_->setText(
                formatMoltorinoAuthSummary(summary));
        }
        else if (summary.hasLegacyToken)
        {
            this->accountsSummaryLabel_->setText(
                "Legacy login found. Existing features will keep working. "
                "Refresh accounts to show account details.");
        }
        else
        {
            this->accountsSummaryLabel_->setText("No accounts saved yet.");
        }

        this->accountsTable_->setRowCount(static_cast<int>(accounts.size()));
        for (int row = 0; row < static_cast<int>(accounts.size()); ++row)
        {
            const auto &account = accounts.at(static_cast<size_t>(row));
            this->accountsTable_->setItem(row, 0,
                                          readOnlyItem(accountName(account)));

            auto *enabledContainer = new QWidget(this->accountsTable_);
            auto *enabledLayout = new QHBoxLayout(enabledContainer);
            enabledLayout->setContentsMargins(0, 0, 0, 0);
            enabledLayout->setAlignment(Qt::AlignCenter);
            auto *enabledCheck = new QCheckBox(enabledContainer);
            enabledCheck->setChecked(account.enabled);
            enabledCheck->setToolTip(
                "Use this account for Moltorino features. Turning it off keeps "
                "the saved login and cached channel access.");
            enabledLayout->addWidget(enabledCheck);
            this->accountsTable_->setCellWidget(row, 1, enabledContainer);

            const auto userId = account.userId;
            const auto token = account.token;
            QObject::connect(enabledCheck, &QCheckBox::toggled, this,
                             [this, userId, token](bool enabled) {
                                 MoltorinoAuth::setAccountEnabled(userId, token,
                                                                  enabled);
                                 QTimer::singleShot(0, this, [this] {
                                     this->refreshAccountsList();
                                 });
                             });

            this->accountsTable_->setItem(
                row, 2, readOnlyItem(QString::number(modAccessCount(account))));

            QString status = "Disabled";
            if (account.enabled)
            {
                if (!account.valid)
                {
                    status = "Refresh or sign in again";
                }
                else if (account.moderatedChannelsManualRefreshOnly)
                {
                    status = "Signed in. Refresh channels manually";
                }
                else
                {
                    status = "Signed in";
                }
            }
            if (account.enabled && !account.lastError.trimmed().isEmpty())
            {
                status = account.lastError;
            }
            auto *statusItem = readOnlyItem(status);
            auto tooltip = status;
            if (!account.enabled)
            {
                tooltip = "Saved, but not used by Moltorino features.";
            }
            else if (account.lastError.trimmed().isEmpty() &&
                     account.moderatedChannelsManualRefreshOnly)
            {
                tooltip = "Moderator channels refresh manually for this "
                          "account. Select Refresh accounts to update them.";
            }
            statusItem->setToolTip(tooltip);
            this->accountsTable_->setItem(row, 3, statusItem);

            auto *removeButton = new QPushButton("Remove", this->accountsTable_);
            const auto name = accountName(account);
            QObject::connect(
                removeButton, &QPushButton::clicked, this,
                [this, userId, token, name] {
                    const auto result = QMessageBox::question(
                        this, "Remove account",
                        QString("Remove %1 from your saved accounts?")
                            .arg(name));
                    if (result != QMessageBox::Yes)
                    {
                        return;
                    }
                    MoltorinoAuth::removeAccount(userId, token);
                    this->refreshAccountsList();
                });
            this->accountsTable_->setCellWidget(row, 4, removeButton);
        }
    }

    void addOrUpdateToken(const QString &token, QLabel *statusLabel)
    {
        const auto trimmed = token.trimmed();
        if (trimmed.isEmpty())
        {
            setLabelStatus(statusLabel, "Enter a token first.", true);
            return;
        }

        const int generation = ++this->authValidationGeneration_;
        this->authValidationInFlight_ = true;
        QPointer<MoltorinoAuthDialog> guard(this);
        QPointer<QLabel> guardedStatus(statusLabel);
        setLabelStatus(statusLabel, "Checking your sign in...");
        this->updateDeviceUi();

        MoltorinoAuth::addOrUpdateToken(
            trimmed,
            [guard, guardedStatus, generation](MoltorinoAuthAccount account) {
                if (guard == nullptr ||
                    generation != guard->authValidationGeneration_)
                {
                    return;
                }

                const auto name = accountName(account);
                guard->authValidationInFlight_ = false;
                if (account.lastError.trimmed().isEmpty())
                {
                    const auto accessCount = modAccessCount(account);
                    setLabelStatus(
                        guardedStatus,
                        QString("Added %1. You have moderator access in %2 %3.")
                            .arg(name)
                            .arg(accessCount)
                            .arg(accessCount == 1 ? "channel" : "channels"),
                        false, true);
                }
                else
                {
                    setLabelStatus(
                        guardedStatus,
                        QString("Added %1, but %2")
                            .arg(name, account.lastError),
                        true);
                }
                guard->refreshAccountsList();
                guard->tabs_->setCurrentWidget(guard->accountsTab_);
                guard->updateDeviceUi();
            },
            [guard, guardedStatus, generation](const QString &error) {
                if (guard == nullptr ||
                    generation != guard->authValidationGeneration_)
                {
                    return;
                }

                guard->authValidationInFlight_ = false;
                setLabelStatus(
                    guardedStatus,
                    QString("Could not validate the sign in: %1").arg(error),
                    true);
                guard->updateDeviceUi();
            });
    }

    void copyTokenScriptAndOpenTwitch()
    {
        crossPlatformCopy(customAuthClipboardScript());

        const auto opened =
            QDesktopServices::openUrl(QUrl("https://www.twitch.tv/"));

        QMessageBox box(this);
        box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
        box.setWindowTitle("Legacy Login Helper");
        box.setIcon(QMessageBox::Information);
        box.setText("The legacy helper command is on your clipboard.\n\n"
                    "1. Twitch is open in your browser.\n"
                    "2. Press F12 and open the Console tab.\n"
                    "3. Paste the command and press Enter.\n"
                    "4. Come back here and select Paste token.");

        if (!opened)
        {
            box.setInformativeText(
                "Moltorino could not open Twitch automatically. Open "
                "https://www.twitch.tv/ yourself, then follow the same steps.");
        }
        box.exec();
    }

    void pasteLegacyToken()
    {
        const auto clipboardText = getClipboardText().trimmed();
        if (clipboardText.isEmpty())
        {
            setLabelStatus(this->legacyStatusLabel_,
                           "Your clipboard is empty. Use device login first, "
                           "or use legacy "
                           "login if device login does not work.",
                           true);
            return;
        }

        this->addOrUpdateToken(clipboardText, this->legacyStatusLabel_);
    }

    void startDeviceLogin()
    {
        if (this->deviceLoginInFlight_)
        {
            this->cancelDeviceLogin();
        }
        this->requestDeviceCode();
    }

    void requestDeviceCode()
    {
        this->deviceLoginInFlight_ = true;
        ++this->devicePollGeneration_;
        this->deviceCode_.clear();
        this->deviceUserCode_.clear();
        this->deviceVerificationUri_.clear();
        this->devicePollIntervalMs_ = 5000;
        this->deviceCodeLabel_->setText(DEVICE_CODE_PLACEHOLDER);
        this->updateDeviceUi();
        setLabelStatus(this->deviceStatusLabel_,
                       "Requesting a Twitch sign in code...");

        QUrlQuery body;
        body.addQueryItem("client_id", TWITCH_TV_CLIENT_ID);
        body.addQueryItem("scopes", TWITCH_TV_SCOPES);

        const int generation = this->devicePollGeneration_;
        QPointer<MoltorinoAuthDialog> guard(this);

        NetworkRequest(QUrl("https://id.twitch.tv/oauth2/device"),
                       NetworkRequestType::Post)
            .caller(this)
            .timeout(20000)
            .hideRequestBody()
            .followRedirects(false)
            .maximumResponseSize(64 * 1024)
            .header("Client-Id", TWITCH_TV_CLIENT_ID)
            .header("Accept", "application/json")
            .header("Content-Type", "application/x-www-form-urlencoded")
            .header("Origin", TWITCH_TV_ORIGIN)
            .header("Referer", TWITCH_TV_REFERER)
            .header("User-Agent", TWITCH_TV_USER_AGENT)
            .header("X-Device-Id", twitchTvDeviceId())
            .payload(body.toString(QUrl::FullyEncoded).toUtf8())
            .onSuccess([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }

                const auto json = result.parseJson();
                const auto deviceCode =
                    json.value("device_code").toString().trimmed();
                const auto userCode =
                    json.value("user_code").toString().trimmed();
                const auto verificationUri =
                    json.value("verification_uri").toString().trimmed();
                const auto intervalSeconds =
                    std::clamp(json.value("interval").toInt(5), 3, 60);

                const QUrl verificationUrl(verificationUri);
                if (deviceCode.isEmpty() || userCode.isEmpty() ||
                    verificationUrl.scheme() != "https" ||
                    (verificationUrl.host() != "www.twitch.tv" &&
                     verificationUrl.host() != "twitch.tv") ||
                    verificationUrl.path() != "/activate" ||
                    !verificationUrl.userInfo().isEmpty() ||
                    (verificationUrl.port(-1) != -1 &&
                     verificationUrl.port(-1) != 443))
                {
                    guard->cancelDeviceLogin(
                        "Device login could not start. Twitch did not return a "
                        "usable code.");
                    return;
                }

                guard->deviceCode_ = deviceCode;
                guard->deviceUserCode_ = userCode;
                guard->deviceVerificationUri_ = verificationUri;
                guard->devicePollIntervalMs_ = intervalSeconds * 1000;
                guard->deviceCodeLabel_->setText(userCode);

                crossPlatformCopy(userCode);
                const auto opened =
                    QDesktopServices::openUrl(QUrl(verificationUri));
                setLabelStatus(
                    guard->deviceStatusLabel_,
                    "Twitch Activate is open and the code is copied. Paste it "
                    "there, then approve access.");

                if (!opened)
                {
                    QMessageBox box(guard);
                    box.setWindowFlags(box.windowFlags() |
                                       Qt::WindowStaysOnTopHint);
                    box.setWindowTitle("Browser Error");
                    box.setIcon(QMessageBox::Warning);
                    box.setText(
                        "Moltorino could not open your browser automatically.");
                    box.setInformativeText(
                        "Please go to " + verificationUri +
                        " and enter the code displayed.");
                    box.exec();
                }

                guard->updateDeviceUi();
                guard->devicePollTimer_->start(guard->devicePollIntervalMs_);
            })
            .onError([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }

                const auto body = QString::fromUtf8(result.getData()).trimmed();
                guard->cancelDeviceLogin(
                    body.isEmpty()
                        ? "Device login could not start. Twitch did not return "
                          "a code."
                        : QString("Device login could not start: %1")
                              .arg(body.left(200)));
            })
            .execute();
    }

    void pollDeviceToken()
    {
        if (!this->deviceLoginInFlight_ || this->deviceCode_.isEmpty())
        {
            return;
        }

        QUrlQuery body;
        body.addQueryItem("client_id", TWITCH_TV_CLIENT_ID);
        body.addQueryItem("device_code", this->deviceCode_);
        body.addQueryItem("grant_type",
                          "urn:ietf:params:oauth:grant-type:device_code");

        const int generation = this->devicePollGeneration_;
        QPointer<MoltorinoAuthDialog> guard(this);

        NetworkRequest(QUrl("https://id.twitch.tv/oauth2/token"),
                       NetworkRequestType::Post)
            .caller(this)
            .timeout(20000)
            .hideRequestBody()
            .followRedirects(false)
            .maximumResponseSize(64 * 1024)
            .header("Client-Id", TWITCH_TV_CLIENT_ID)
            .header("Accept", "application/json")
            .header("Content-Type", "application/x-www-form-urlencoded")
            .header("Origin", TWITCH_TV_ORIGIN)
            .header("Referer", TWITCH_TV_REFERER)
            .header("User-Agent", TWITCH_TV_USER_AGENT)
            .header("X-Device-Id", twitchTvDeviceId())
            .payload(body.toString(QUrl::FullyEncoded).toUtf8())
            .onSuccess([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }
                guard->handleDeviceTokenPollResponse(result);
            })
            .onError([guard, generation](const NetworkResult &result) {
                if (guard == nullptr ||
                    generation != guard->devicePollGeneration_)
                {
                    return;
                }
                guard->handleDeviceTokenPollResponse(result);
            })
            .execute();
    }

    void handleDeviceTokenPollResponse(const NetworkResult &result)
    {
        if (!this->deviceLoginInFlight_)
        {
            return;
        }

        const auto json = result.parseJson();
        const auto accessToken = json.value("access_token").toString().trimmed();
        if (!accessToken.isEmpty())
        {
            this->deviceLoginInFlight_ = false;
            this->devicePollTimer_->stop();
            this->deviceCode_.clear();
            this->deviceUserCode_.clear();
            this->deviceCodeLabel_->setText(DEVICE_CODE_PLACEHOLDER);
            setLabelStatus(this->deviceStatusLabel_,
                           "Twitch approval received. Checking the new token...");
            this->addOrUpdateToken(accessToken, this->deviceStatusLabel_);
            return;
        }

        const auto error = json.value("error").toString().trimmed();
        const auto message = json.value("message").toString().trimmed();

        if (error == "authorization_pending" ||
            message == "authorization_pending")
        {
            this->devicePollTimer_->start(this->devicePollIntervalMs_);
            return;
        }

        if (error == "slow_down" || message == "slow_down")
        {
            this->devicePollIntervalMs_ =
                std::min(this->devicePollIntervalMs_ + 5000, 60000);
            setLabelStatus(this->deviceStatusLabel_,
                           "Twitch asked Moltorino to poll more slowly. "
                           "Still waiting for approval...");
            this->devicePollTimer_->start(this->devicePollIntervalMs_);
            return;
        }

        QString displayError = "Device login failed.";
        if (!message.isEmpty())
        {
            displayError = "Device login failed: " + message;
        }
        else if (!error.isEmpty())
        {
            displayError = "Device login failed: " + error;
        }
        else if (!result.formatError().isEmpty())
        {
            displayError = "Device login failed: " + result.formatError();
        }

        this->cancelDeviceLogin(displayError);
    }

    void cancelDeviceLogin(const QString &statusMessage = QString())
    {
        this->deviceLoginInFlight_ = false;
        ++this->devicePollGeneration_;
        if (this->devicePollTimer_ != nullptr)
        {
            this->devicePollTimer_->stop();
        }
        this->deviceCode_.clear();
        this->deviceUserCode_.clear();
        this->deviceVerificationUri_.clear();
        this->devicePollIntervalMs_ = 5000;
        this->deviceCodeLabel_->setText(DEVICE_CODE_PLACEHOLDER);
        this->updateDeviceUi();

        setLabelStatus(
            this->deviceStatusLabel_,
            statusMessage.isEmpty() ? "Device login canceled." : statusMessage,
            !statusMessage.isEmpty());
    }

    void updateDeviceUi()
    {
        const bool idle = !this->deviceLoginInFlight_ &&
                          !this->authValidationInFlight_;
        const bool hasCode = !this->deviceUserCode_.isEmpty();
        if (this->startDeviceButton_ != nullptr)
        {
            this->startDeviceButton_->setEnabled(idle);
        }
        if (this->copyCodeButton_ != nullptr)
        {
            this->copyCodeButton_->setEnabled(hasCode);
        }
        if (this->cancelDeviceButton_ != nullptr)
        {
            this->cancelDeviceButton_->setEnabled(!idle);
        }
    }

    QTabWidget *tabs_{};
    QWidget *accountsTab_{};
    QTableWidget *accountsTable_{};
    QLabel *accountsSummaryLabel_{};
    QLabel *deviceStatusLabel_{};
    QLabel *deviceCodeLabel_{};
    QLabel *legacyStatusLabel_{};
    QPushButton *startDeviceButton_{};
    QPushButton *copyCodeButton_{};
    QPushButton *cancelDeviceButton_{};
    QTimer *devicePollTimer_{};
    bool deviceLoginInFlight_{false};
    bool authValidationInFlight_{false};
    int devicePollGeneration_{0};
    int devicePollIntervalMs_{5000};
    int authValidationGeneration_{0};
    QString deviceCode_;
    QString deviceUserCode_;
    QString deviceVerificationUri_;
};

MoltorinoPage::MoltorinoPage()
{
    auto *rootLayout = new QVBoxLayout;
    rootLayout->setContentsMargins(9, 6, 9, 0);
    rootLayout->setSpacing(4);

    this->tabBar_ = new QTabBar(this);
    this->tabBar_->setExpanding(false);
    this->tabBar_->setDocumentMode(true);
    this->tabBar_->setDrawBase(false);
    rootLayout->addWidget(this->tabBar_);

    auto *stack = new QStackedWidget(this);
    rootLayout->addWidget(stack);
    this->setLayout(rootLayout);

    auto *generalTab = new QWidget(stack);
    auto *generalLayout = new QHBoxLayout(generalTab);
    generalLayout->setContentsMargins(0, 0, 0, 0);
    auto *view = GeneralPageView::withNavigation(generalTab);
    this->settingsView_ = view;
    generalLayout->addWidget(view);
    stack->addWidget(generalTab);
    this->tabBar_->addTab("General");

    auto *moderationTab = new QWidget(stack);
    auto *moderationLayout = new QHBoxLayout(moderationTab);
    moderationLayout->setContentsMargins(0, 0, 0, 0);
    this->moderationView_ = GeneralPageView::withNavigation(moderationTab);
    moderationLayout->addWidget(this->moderationView_);
    stack->addWidget(moderationTab);
    this->tabBar_->addTab("Moderation");

    QObject::connect(this->tabBar_, &QTabBar::currentChanged, stack,
                     &QStackedWidget::setCurrentIndex);

    auto &s = *getSettings();

    view->addTitle("Authentication");
    view->addDescription(
        "Sign in to use Moltorino features such as pins, polls, predictions, "
        "and channel points.");

    auto *tokenControls = new QFrame(view);
    auto *tokenLayout = new QVBoxLayout(tokenControls);
    tokenLayout->setContentsMargins(0, 4, 0, 0);
    tokenLayout->setSpacing(8);

    auto *authButtons = new QHBoxLayout;
    authButtons->setContentsMargins(0, 0, 0, 0);
    authButtons->setSpacing(8);

    this->addAuthAccountButton_ = new QPushButton("Log In", tokenControls);
    this->addAuthAccountButton_->setToolTip(
        "Sign in or manage your saved Moltorino accounts.");
    this->refreshAuthAccountsButton_ =
        new QPushButton("Refresh Accounts", tokenControls);
    this->refreshAuthAccountsButton_->setToolTip(
        "Check your saved accounts and update moderator access.");

    authButtons->addWidget(this->addAuthAccountButton_);
    authButtons->addWidget(this->refreshAuthAccountsButton_);
    authButtons->addStretch(1);
    tokenLayout->addLayout(authButtons);

    this->authInstructionsLabel_ = new QLabel(tokenControls);
    this->authInstructionsLabel_->setWordWrap(true);
    this->authInstructionsLabel_->setStyleSheet("QLabel { color: #9aa0a6; font-size: 12px; }");
    tokenLayout->addWidget(this->authInstructionsLabel_);

    this->authStatusLabel_ = new QLabel(tokenControls);
    this->authStatusLabel_->setWordWrap(true);
    this->authStatusLabel_->setFont(
        makeResolvedFont(this->authStatusLabel_->font(), QFont::DemiBold));
    tokenLayout->addWidget(this->authStatusLabel_);

    view->addWidget(tokenControls,
                    {"Device login", "Paste token", "Legacy browser", "Sign in",
                     "Authentication"});

    QObject::connect(this->addAuthAccountButton_, &QPushButton::clicked, this,
                     &MoltorinoPage::openAuthDialog);
    QObject::connect(this->refreshAuthAccountsButton_, &QPushButton::clicked,
                     this, &MoltorinoPage::refreshAuthAccounts);
    s.customPinAuthToken.connect(
        [this](const QString &, auto) {
            this->updateAuthSummary();
        },
        this->managedConnections_);
    s.moltorinoAuthAccounts.connect(
        [this](const QString &, auto) {
            this->updateAuthSummary();
        },
        this->managedConnections_);

    this->updateAuthSummary();

    this->botBadgeFrame_ = new QFrame(view);
    auto *botBadgeLayout = new QVBoxLayout(this->botBadgeFrame_);
    botBadgeLayout->setContentsMargins(0, 8, 0, 0);
    botBadgeLayout->setSpacing(8);

    auto *botBadgeTitle =
        new QLabel("Bot badge (developer)", this->botBadgeFrame_);
    botBadgeTitle->setStyleSheet("QLabel { font-size: 16px; color: #f5f7fa; }");
    botBadgeTitle->setFont(
        makeResolvedFont(botBadgeTitle->font(), QFont::Bold));
    botBadgeLayout->addWidget(botBadgeTitle);

    auto *botBadgeDescription =
        new QLabel("Set up the chat bot badge used by /bot. "
                   "<a href=\"https://youtu.be/BKQkYA1_-3s\">Watch the setup "
                   "tutorial</a>.",
                   this->botBadgeFrame_);
    botBadgeDescription->setWordWrap(true);
    botBadgeDescription->setOpenExternalLinks(true);
    botBadgeLayout->addWidget(botBadgeDescription);

    auto *botBadgeForm = new QFormLayout;
    botBadgeForm->setContentsMargins(0, 0, 0, 0);
    botBadgeForm->setHorizontalSpacing(12);
    botBadgeForm->setVerticalSpacing(6);

    this->botBadgeSenderEdit_ = new QLineEdit(this->botBadgeFrame_);
    this->botBadgeSenderEdit_->setPlaceholderText("Leave empty for current account");
    botBadgeForm->addRow("Username", this->botBadgeSenderEdit_);

    this->botBadgeClientIdEdit_ = new QLineEdit(this->botBadgeFrame_);
    this->botBadgeClientIdEdit_->setPlaceholderText("Twitch application Client ID");
    botBadgeForm->addRow("Client ID", this->botBadgeClientIdEdit_);

    this->botBadgeClientSecretEdit_ = new QLineEdit(this->botBadgeFrame_);
    this->botBadgeClientSecretEdit_->setEchoMode(QLineEdit::Password);
    this->botBadgeClientSecretEdit_->setPlaceholderText(
        "Twitch application Client Secret");
    botBadgeForm->addRow("Client Secret", this->botBadgeClientSecretEdit_);

    auto *redirectEdit =
        new QLineEdit("http://localhost/", this->botBadgeFrame_);
    redirectEdit->setReadOnly(true);
    redirectEdit->setToolTip(
        "Use http://localhost/ as the redirect URL in Twitch.");
    auto *copyRedirect = new QPushButton("Copy URL", this->botBadgeFrame_);
    auto *redirectRow = new QHBoxLayout;
    redirectRow->setContentsMargins(0, 0, 0, 0);
    redirectRow->addWidget(redirectEdit, 1);
    redirectRow->addWidget(copyRedirect);
    botBadgeForm->addRow("Redirect URL", redirectRow);
    QObject::connect(copyRedirect, &QPushButton::clicked, this, [] {
        crossPlatformCopy(QStringLiteral("http://localhost/"));
    });

    botBadgeLayout->addLayout(botBadgeForm);

    auto *botBadgeButtons = new QHBoxLayout;
    botBadgeButtons->setContentsMargins(0, 0, 0, 0);
    this->botBadgeAuthorizeButton_ =
        new QPushButton("Authorize", this->botBadgeFrame_);
    this->botBadgeAuthorizeButton_->setToolTip(
        "Connect the account used for bot badge messages.");
    this->botBadgeVerifyButton_ =
        new QPushButton("Verify", this->botBadgeFrame_);
    this->botBadgeVerifyButton_->setToolTip(
        "Check that bot badge messages can be sent.");
    botBadgeButtons->addWidget(this->botBadgeAuthorizeButton_);
    botBadgeButtons->addWidget(this->botBadgeVerifyButton_);
    botBadgeButtons->addStretch(1);
    botBadgeLayout->addLayout(botBadgeButtons);

    this->botBadgeStatusLabel_ = new QLabel(this->botBadgeFrame_);
    this->botBadgeStatusLabel_->setWordWrap(true);
    botBadgeLayout->addWidget(this->botBadgeStatusLabel_);

    this->botBadgeIdentityLabel_ = new QLabel(this->botBadgeFrame_);
    this->botBadgeIdentityLabel_->setWordWrap(true);
    botBadgeLayout->addWidget(this->botBadgeIdentityLabel_);

    SettingWidget::checkbox("Enable bot badge mode", s.botBadgeAlwaysUse)
        ->setTooltip("When the selected account is the bot account, send its "
                     "chat messages with the bot badge.")
        ->addToLayout(botBadgeLayout);

    SettingWidget::checkbox("Use the bot account for every message",
                            s.botBadgeOverrideAllAccounts)
        ->setTooltip("When bot badge mode is on, use the bot account even when "
                     "another account is selected.")
        ->addToLayout(botBadgeLayout);

    QObject::connect(this->botBadgeClientIdEdit_, &QLineEdit::editingFinished,
                     this, [this] {
                         getSettings()->botBadgeClientID =
                             this->botBadgeClientIdEdit_->text().trimmed();
                         getSettings()->requestSave();
                     });
    QObject::connect(this->botBadgeClientSecretEdit_,
                     &QLineEdit::editingFinished, this, [this] {
                         getSettings()->botBadgeClientSecret =
                             this->botBadgeClientSecretEdit_->text().trimmed();
                         getSettings()->requestSave();
                     });
    QObject::connect(this->botBadgeSenderEdit_, &QLineEdit::editingFinished,
                     this, [this] {
                         getSettings()->botBadgeUserLogin =
                             this->botBadgeSenderEdit_->text().trimmed().toLower();
                         getSettings()->requestSave();
                     });
    QObject::connect(this->botBadgeVerifyButton_, &QPushButton::clicked, this,
                     [this] {
                         if (this->botBadgeIsValidating_)
                         {
                             return;
                         }
                         this->verifyBotBadgeConfiguration();
                     });
    QObject::connect(this->botBadgeAuthorizeButton_, &QPushButton::clicked,
                     this, [this] {
                         if (this->botBadgeIsValidating_)
                         {
                             return;
                         }
                         this->openBotBadgeAuthorization();
                     });

    this->populateBotBadgeFieldsFromSettings();
    this->revealBotBadgeSettings(false);

    view->addTitle("Pinned Messages");
    view->addDescription(
        "Choose how pinned messages appear and behave in chat.");

    this->moderationView_->addTitle("Pin controls");
    this->moderationView_->addDescription(
        "Pin messages from chat or with the /pin command.");

    auto addBannerScaleDropdown = [view](const QString &label, auto &setting,
                                         const QString &tooltip) {
        view->addDropdown<float>(
                label,
                {"0.5x", "0.6x", "0.75x", "0.9x", "Default", "1.1x",
                 "1.25x", "1.4x", "1.5x", "1.75x", "2x"},
                setting,
                [](auto val) {
                    if (val == 1.f)
                    {
                        return QString("Default");
                    }
                    return QString::number(val) + "x";
                },
                [](auto args) {
                    return fuzzyToFloat(args.value, 1.f);
                },
                false)
            ->setToolTip(tooltip);
    };

    SettingWidget::checkbox("Move Pin actions to Moderate menu",
                            s.movePinToModerateMenu)
        ->setTooltip("Put Pin and Unpin in the Moderate submenu of the message "
                     "menu.")
        ->addTo(*this->moderationView_);

    this->moderationView_
        ->addDropdown<int>(
            "Show the pin button for moderators and the broadcaster",
            {"Never", "In moderation mode", "Always"},
            s.showPinButtonOnModeratorsMode,
            [](auto val) {
                switch (val)
                {
                    case 0:
                        return QString("Never");
                    case 2:
                        return QString("Always");
                    default:
                        return QString("In moderation mode");
                }
            },
            [](auto args) {
                if (args.value == "Never")
                {
                    return 0;
                }
                if (args.value == "Always")
                {
                    return 2;
                }
                return 1;
            },
            false)
        ->setToolTip(
            "Choose when the inline Pin action appears beside moderator "
            "and broadcaster messages.");

    SettingWidget::checkbox("Show pinned messages",
                            s.enablePinnedMessages)
        ->setTooltip("Show the pinned message banner above chat.")
        ->addTo(*view);

    SettingWidget::checkbox("Expand long pinned messages automatically",
                            s.alwaysExpandPinnedMessages)
        ->setTooltip("Show the full content of long pins without requiring a "
                     "click.")
        ->addTo(*view);

    SettingWidget::checkbox("Enable /pin <message text>",
                            s.enablePinCommandMessages)
        ->setTooltip("Let /pin send and pin the text you provide.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Enable /pin <username>",
                            s.enablePinUserCommand)
        ->setTooltip("Let /pin find and pin the latest message from the "
                     "username you provide.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Require @ for /pin <username>",
                            s.requireAtForPinUserCommand)
        ->setTooltip(
            "Only /pin @username searches for a user's latest message. "
            "A bare name is treated as text.")
        ->addTo(*this->moderationView_);

    addBannerScaleDropdown(
        "Pinned message scale", s.pinnedMessageScale,
        "Make pinned message text larger or smaller.");
    addBannerScaleDropdown(
        "Pinned content scale", s.pinnedContentScale,
        "Make pinned banner controls, labels, and buttons larger or smaller.");

    this->moderationView_
        ->addDropdown<int>(
            "Default pin duration",
            {"Indefinite", "5 minutes", "10 minutes", "20 minutes",
             "30 minutes"},
            s.defaultPinDuration,
            [](auto val) {
                if (val <= 0)
                {
                    return QString("Indefinite");
                }
                return QString::number(val / 60) + " minutes";
            },
            [](auto args) {
                if (args.value == "Indefinite")
                {
                    return -1;
                }
                return args.value.split(' ')[0].toInt() * 60;
            },
            false)
        ->setToolTip("How long pins last when no duration is given.");

    view->addDropdown<int>(
            "Close button action",
            {"Hide banner here", "Unpin for everyone"},
            s.pinCloseButtonAction,
            [](auto val) {
                return val == 1 ? QString("Unpin for everyone")
                                : QString("Hide banner here");
            },
            [](auto args) {
                return args.value.startsWith("Unpin") ? 1 : 0;
            },
            false)
        ->setToolTip(
            "What the close button does on a pinned message banner.");

    view->addDropdown<int>(
            "Pin timer display",
            {"Time and countdown", "Time only", "Countdown only", "On hover",
             "Hidden"},
            s.pinTimerDisplay,
            [](auto val) {
                switch (val)
                {
                    case 1:
                        return QString("Time only");
                    case 2:
                        return QString("Countdown only");
                    case 3:
                        return QString("On hover");
                    case 4:
                        return QString("Hidden");
                    default:
                        return QString("Time and countdown");
                }
            },
            [](auto args) {
                if (args.value == "Time only")
                {
                    return 1;
                }
                if (args.value == "Countdown only")
                {
                    return 2;
                }
                if (args.value == "On hover")
                {
                    return 3;
                }
                if (args.value == "Hidden")
                {
                    return 4;
                }
                return 0;
            },
            false)
        ->setToolTip(
            "Choose how pin time is shown on the banner.");

    view->addDropdown<QString>(
            "Pin timestamp format",
            {"Relative", "h:mm", "hh:mm", "h:mm a", "hh:mm a", "h:mm:ss",
             "hh:mm:ss", "h:mm:ss a", "hh:mm:ss a"},
            s.pinTimestampFormat,
            [](auto val) {
                return val;
            },
            [](auto args) {
                return args.value;
            },
            false)
        ->setToolTip("How pin times are formatted.");

    SettingWidget::checkbox("Show unpin notifications in chat",
                            s.showUnpinNotifications)
        ->setTooltip("Show a chat message when a moderator unpins something.")
        ->addTo(*view);

    view->addTitle("Polls and predictions");
    view->addDescription(
        "Choose how polls, predictions, and their banners behave.");

    SettingWidget::checkbox("Show predictions",
                            s.enablePredictions)
        ->setTooltip(
            "Show prediction banners and open prediction menus from Moltorino.")
        ->addTo(*view);

    SettingWidget::checkbox("Show polls", s.enablePolls)
        ->setTooltip(
            "Show poll banners and open poll menus from Moltorino.")
        ->addTo(*view);

    addBannerScaleDropdown(
        "Prediction banner content scale", s.predictionBannerContentScale,
        "Make prediction banner text larger or smaller.");

    addBannerScaleDropdown(
        "Poll banner content scale", s.pollBannerContentScale,
        "Make poll banner text larger or smaller.");

    this->moderationView_->addTitle("Channel controls");
    this->moderationView_->addDescription(
        "Prediction, raid, and stream controls for channels you manage.");

    this->moderationView_
        ->addDropdown<int>(
            "Prediction banner click for moderators",
            {"Open betting view", "Open moderation view"},
            s.predictionModAction,
            [](auto val) {
                return val == 1 ? QString("Open moderation view")
                                : QString("Open betting view");
            },
            [](auto args) {
                return args.value.contains("moderation") ? 1 : 0;
            },
            false)
        ->setToolTip(
            "Choose what opens when a moderator clicks a prediction banner.");

    SettingWidget::checkbox("Show prediction button", s.showPredictionButton)
        ->setTooltip("Show the prediction button next to the message input.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Show the raid countdown above the input",
                            s.showRaidStatusAboveInput)
        ->setTooltip("Show the raid target, viewer count, and countdown above "
                     "the message input.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox(
        "Show the edit stream info button in the chat header",
        s.showEditStreamInfoButtonInSplitHeader)
        ->setTooltip("Show the edit stream info shortcut in the chat header "
                     "when broadcaster or editor access is available.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Show prediction chat messages",
                            s.showPredictionSystemMessages)
        ->setTooltip("Show chat messages when predictions are created, "
                     "locked, paid out, or refunded.")
        ->addTo(*view);

    SettingWidget::checkbox("Close prediction menu after an action",
                            s.predictionAutoCloseDialog)
        ->setTooltip("Close after creating, resolving, deleting or betting on "
                     "a prediction.")
        ->addTo(*view);

    SettingWidget::checkbox("Close poll menu after an action",
                            s.pollAutoCloseDialog)
        ->setTooltip("Close after creating a poll or casting a vote.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Auto dismiss resolved banners",
            {"Never", "After 10 seconds", "After 30 seconds",
             "After 60 seconds", "After 2 minutes", "After 5 minutes",
             "After 10 minutes"},
            s.predictionAutoDismissSeconds,
            [](auto val) {
                if (val <= 0)
                {
                    return QString("Never");
                }
                if (val >= 60 && val % 60 == 0)
                {
                    return QString("After %1 minutes").arg(val / 60);
                }
                return QString("After %1 seconds").arg(val);
            },
            [](auto args) {
                if (args.value == "Never")
                {
                    return 0;
                }
                auto parts = args.value.split(' ');
                if (parts.size() >= 2)
                {
                    int val = parts[1].toInt();
                    return parts.size() >= 3 &&
                                   parts[2].startsWith("min")
                               ? val * 60
                               : val;
                }
                return 0;
            },
            false)
        ->setToolTip(
            "Hide completed poll and prediction banners after a delay.");

    SettingWidget::checkbox("Close poll and prediction menus on focus loss",
                            s.predictionCloseOnFocusLoss)
        ->setTooltip("Close poll and prediction dialogs when you click "
                     "away from them.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Banner stacking behavior",
            {"Show all", "Prefer pinned", "Prefer prediction", "Prefer poll",
             "Intelligent"},
            s.bannerStackMode,
            [](auto val) {
                switch (val)
                {
                    case 1:
                        return QString("Prefer pinned");
                    case 2:
                        return QString("Prefer prediction");
                    case 3:
                        return QString("Intelligent");
                    case 4:
                        return QString("Prefer poll");
                    default:
                        return QString("Show all");
                }
            },
            [](auto args) {
                if (args.value == "Prefer pinned")
                {
                    return 1;
                }
                if (args.value == "Prefer prediction")
                {
                    return 2;
                }
                if (args.value == "Prefer poll")
                {
                    return 4;
                }
                if (args.value == "Intelligent")
                {
                    return 3;
                }
                return 0;
            },
            false)
        ->setToolTip("Choose how pinned, poll, and prediction banners share "
                     "the space above chat.");

    view->addTitle("Channel points and rewards");
    view->addDescription(
        "Choose how your points balance and rewards menu behave.");

    SettingWidget::checkbox("Show points balance",
                            s.enableChannelPointsDisplay)
        ->setTooltip("Show your channel points next to the message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Open rewards when you click your points",
                            s.openRewardsWithChannelPointsClick)
        ->setTooltip(
            "When this is off, clicking your balance only refreshes it. "
            "Use /redeem to open the rewards menu.")
        ->addTo(*view);

    SettingWidget::checkbox("Close rewards when you click away",
                            s.rewardsCloseOnFocusLoss)
        ->setTooltip("Close the rewards popup when you click away from it.")
        ->addTo(*view);

    SettingWidget::checkbox("Close rewards menu after redeeming",
                            s.rewardsCloseAfterRedeem)
        ->setTooltip("Close the popup after a reward is redeemed.")
        ->addTo(*view);

    SettingWidget::checkbox("Return to rewards list after redeeming",
                            s.rewardsReturnToListAfterRedeem)
        ->setTooltip("After a reward is redeemed, return to the rewards list "
                     "instead of staying on the current picker.")
        ->addTo(*view);

    SettingWidget::checkbox("Show Gigantify emotes in chat",
                            s.enableGigantifyEmotes)
        ->setTooltip(
            "Show Twitch's Gigantify reward and enlarge its selected emote in "
            "chat. The /gigantify command still works when this is off.")
        ->addTo(*view);

    SettingWidget::checkbox("Show Twitch GIFs in chat", s.enableTwitchGifs)
        ->setTooltip("Show animated GIFs sent by Twitch subscribers in chat. "
                     "When disabled, they appear as their text description.")
        ->addTo(*view);

    SettingWidget::checkbox("Show Twitch GIFs as emotes", s.twitchGifsAsEmotes)
        ->setTooltip("Use the normal emote size and keep GIF details on hover.")
        ->addTo(*view);

    SettingWidget::checkbox("Use high quality Twitch GIFs",
                            s.highQualityTwitchGifs)
        ->setTooltip("Load sharper GIFs at the same display size. Uses more "
                     "memory and bandwidth.")
        ->addTo(*view);

    auto *gifSize = view->addDropdown<float>(
        "Twitch GIF size", {"0.5x", "Default", "1x", "1.25x", "1.5x", "2x"},
        s.twitchGifScale,
        [](auto val) {
            if (val == 0.75f)
            {
                return QString("Default");
            }
            return QString::number(val) + "x";
        },
        [](auto args) {
            return std::clamp(fuzzyToFloat(args.value, 0.75f), 0.5f, 2.0f);
        },
        true,
        "Scale Twitch GIFs from 0.5x to 2x when they are not shown as emotes.");
    gifSize->setToolTip(
        "Scale Twitch GIFs from 0.5x to 2x when they are not shown as emotes.");
    gifSize->setEnabled(!s.twitchGifsAsEmotes);
    s.twitchGifsAsEmotes.connect(
        [gifSize](bool value, auto) {
            gifSize->setEnabled(!value);
        },
        this->managedConnections_);

    view->addTitle("Message input");
    view->addDescription(
        "Choose which buttons and typing helpers appear beside the input.");

    SettingWidget::checkbox("Show message input placeholder",
                            s.showInputPlaceholder)
        ->setTooltip("Show helper text while the message input is empty.")
        ->addTo(*view);

    SettingWidget::checkbox("Show where messages are sent",
                            s.showMultiChannelDestinationSelector)
        ->setTooltip(
            "Show where messages will be sent in a multichannel chat. When "
            "hidden, you can still change it from the input's context menu.")
        ->addTo(*view);

    SettingWidget::checkbox("Show command suggestions while typing",
                            s.showCommandSuggestions)
        ->setTooltip("Show a compact command suggestion strip above the "
                     "message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Show command hints in the message box",
                            s.showCommandArgumentHints)
        ->setTooltip(
            "Keep the remaining command arguments visible as you type.")
        ->addTo(*view);

    SettingWidget::checkbox("Show emotes while cycling with Tab",
                            s.showEmoteTabCarousel)
        ->setTooltip("See the emotes you'll cycle through when you press Tab.")
        ->addTo(*view);

    SettingWidget::dropdown("Emote preview size", s.emoteTabCarouselSize,
                            {{"Compact", "compact"},
                             {"Standard", "standard"},
                             {"Large", "large"},
                             {"Extra large", "extra-large"}})
        ->setTooltip("Choose how much room the preview takes above the input.")
        ->conditionallyEnabledBy(s.showEmoteTabCarousel)
        ->addTo(*view);

    SettingWidget::checkbox("Show emote names", s.showEmoteTabCarouselNames)
        ->setTooltip("Show each emote's name beneath it.")
        ->conditionallyEnabledBy(s.showEmoteTabCarousel)
        ->addTo(*view);

    SettingWidget::checkbox("Include Potat commands in suggestions",
                            s.includePotatCommands)
        ->setTooltip("Suggest Potat # commands after you type #.")
        ->addTo(*view);

    SettingWidget::checkbox("Show Potat command aliases",
                            s.showPotatCommandAliases)
        ->setTooltip("Include shortcuts such as #ga in Potat suggestions.")
        ->conditionallyEnabledBy(s.includePotatCommands)
        ->addTo(*view);

    SettingWidget::checkbox("Hide unavailable moderator commands",
                            s.hideUnavailableModCommands)
        ->setTooltip(
            "Hide moderator commands from completion when they are not "
            "available in the current channel.")
        ->addTo(*view);

    SettingWidget::checkbox("Show poll button", s.showPollButton)
        ->setTooltip("Show the poll button next to the message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Hide the emoji button", s.hideEmojiButton)
        ->setTooltip(
            "Hide the emoji and emote picker beside the message input.")
        ->addTo(*view);

    SettingWidget::checkbox("Show Vanity button", s.showVanityButton)
        ->setTooltip(
            "Show a shortcut beside the message input for changing your "
            "badges, chat color, and 7TV cosmetics.")
        ->addTo(*view);

    view->addTitle("Translation");
    view->addDescription("Choose how Moltorino translates messages.");

    SettingWidget::dropdown("Translation service", s.translationProvider,
                            translationProviderItems())
        ->setTooltip("Choose the service Moltorino uses for translations.")
        ->addTo(*view);

    view->addButton("Set up translation", [this] {
        auto *dialog = new TranslationProviderDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
    });

    SettingWidget::checkbox("Show translation button",
                            s.showOutgoingTranslationButton)
        ->setTooltip("Show the chat input translation button.")
        ->addTo(*view);

    SettingWidget::dropdown("When sending a translation",
                            s.outgoingTranslationMode,
                            outgoingTranslationModeItems())
        ->setTooltip("Preview a translation first or send it immediately.")
        ->addTo(*view);

    SettingWidget::dropdown("Translate outgoing messages into",
                            s.outgoingTranslationTargetLanguage,
                            translationLanguageItems())
        ->setTooltip("Choose the default language for outgoing translations.")
        ->addTo(*view);

    SettingWidget::checkbox("Add Translate to message menus",
                            s.showTranslateMessageContextAction)
        ->setTooltip("Add a Translate action to chat message menus.")
        ->addTo(*view);

    SettingWidget::dropdown("Translate chat messages into",
                            s.messageTranslationTargetLanguage,
                            translationLanguageItems())
        ->setTooltip("Choose the language used for message translations.")
        ->addTo(*view);

    SettingWidget::checkbox("Mark translated messages",
                            s.showTranslatedMessageIndicator)
        ->setTooltip("Show a small translated label after translated messages.")
        ->addTo(*view);

    this->moderationView_->addTitle("Commercials");
    this->moderationView_
        ->addDropdown<int>(
            "Default commercial duration",
            {"30 seconds", "60 seconds", "90 seconds", "120 seconds",
             "150 seconds", "180 seconds"},
            s.defaultCommercialDuration,
            [](int value) {
                return QString("%1 seconds")
                    .arg(ChannelManagement::isValidCommercialLength(value)
                             ? value
                             : 30);
            },
            [](auto args) {
                return args.value.section(' ', 0, 0).toInt();
            },
            false)
        ->setToolTip("Used when /commercial has no duration. An explicit "
                     "duration, such as /commercial 60, overrides this.");

    this->moderationView_->addTitle("AutoMod");
    this->moderationView_->addDescription(
        "Choose how held messages appear in chat and in the /automod review "
        "tab.");

    SettingWidget::checkbox("Use the advanced AutoMod layout in chat",
                            s.advancedAutoModInChat)
        ->setTooltip(
            "Turn this off to use the compact AutoMod layout. The /automod "
            "review tab always keeps its advanced controls.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Show the AutoMod reason",
                            s.autoModReviewShowReason)
        ->setTooltip(
            "Show the AutoMod category and level at the top of each review.")
        ->addTo(*this->moderationView_);

    this->moderationView_->addSubtitle("Review queue");
    auto *queueOrder = this->moderationView_->addDropdown<int>(
        "Message order", {"Oldest at top", "Newest at top"},
        s.autoModReviewQueueOrder,
        [](auto value) {
            return value == 0 ? QString("Newest at top")
                              : QString("Oldest at top");
        },
        [](auto args) {
            return args.value == "Oldest at top" ? 1 : 0;
        },
        false);
    queueOrder->setToolTip(
        "By default, new held messages appear at the bottom, like chat.");

    auto *scrollPosition = this->moderationView_->addDropdown<int>(
        "Automatic scrolling",
        {"Follow new messages", "Stay at top", "Stay at bottom"},
        s.autoModReviewScrollPosition,
        [](auto value) {
            if (value == 1)
            {
                return QString("Stay at top");
            }
            if (value == 2)
            {
                return QString("Stay at bottom");
            }
            return QString("Follow new messages");
        },
        [](auto args) {
            return args.value == "Stay at top"      ? 1
                   : args.value == "Stay at bottom" ? 2
                                                    : 0;
        },
        false);
    scrollPosition->setToolTip(
        "Follow new messages wherever they appear, or keep the view at a "
        "chosen end of the queue. Scrolling away pauses automatic scrolling "
        "until you return to that end.");

    SettingWidget::checkbox("Show keyboard shortcuts",
                            s.autoModReviewShowShortcutHints)
        ->setTooltip(
            "Show the keyboard guide beneath the selected AutoMod review. "
            "Shortcuts work while the /automod input is empty.")
        ->addTo(*this->moderationView_);

    this->moderationView_->addSubtitle("User history");
    SettingWidget::checkbox("Show earlier messages from the same user",
                            s.autoModReviewShowContext)
        ->setTooltip(
            "Show a few matching messages from before the held message when "
            "they are still in the local chat buffer.")
        ->addTo(*this->moderationView_);

    auto *contextOrder = this->moderationView_->addDropdown<int>(
        "User history order", {"Newest at top", "Oldest at top"},
        s.autoModReviewContextOrder,
        [](auto value) {
            return value == 1 ? QString("Oldest at top")
                              : QString("Newest at top");
        },
        [](auto args) {
            return args.value == "Oldest at top" ? 1 : 0;
        },
        false);
    contextOrder->setToolTip(
        "Order the user's earlier messages within each review. This does not "
        "change the review queue.");
    contextOrder->setEnabled(s.autoModReviewShowContext);
    s.autoModReviewShowContext.connect(
        [contextOrder](bool value, auto) {
            contextOrder->setEnabled(value);
        },
        this->managedConnections_);

    this->moderationView_->addTitle("Repeated messages");
    this->moderationView_->addDescription(
        "Spot repeated or nearly identical messages while you moderate chat.");

    SettingWidget::checkbox("Show repeated message counters",
                            s.enableRepeatedMessageDetector)
        ->setTooltip("Show repeated or very similar messages with an inline "
                     "counter such as x2 or x3.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Only show counters in moderation mode",
                            s.repeatedMessagesShowOnlyModerationMode)
        ->setTooltip("Only show counters while inline moderation buttons are "
                     "visible.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Show counters in usercards",
                            s.repeatedMessagesShowInUsercards)
        ->setTooltip(
            "Show repeat counters beside cached messages in usercards.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Only in channels where I can moderate",
                            s.repeatedMessagesOnlyModChannels)
        ->setTooltip("Only show counters in channels where you have moderation "
                     "access.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Ignore VIPs", s.repeatedMessagesIgnoreVips)
        ->setTooltip("Do not mark repeated messages from VIPs. Moderators and "
                     "the broadcaster are always ignored.")
        ->addTo(*this->moderationView_);

    this->moderationView_
        ->addDropdown<int>(
            "Similarity sensitivity",
            {"Lenient", "Balanced", "Default", "Strict", "Exact matches only"},
            s.repeatedMessagesSensitivity,
            [](auto val) {
                switch (val)
                {
                    case 0:
                        return QString("Lenient");
                    case 1:
                        return QString("Balanced");
                    case 3:
                        return QString("Strict");
                    case 4:
                        return QString("Exact matches only");
                    case 2:
                    default:
                        return QString("Default");
                }
            },
            [](auto args) {
                if (args.value == "Lenient")
                {
                    return 0;
                }
                if (args.value == "Balanced")
                {
                    return 1;
                }
                if (args.value == "Strict")
                {
                    return 3;
                }
                if (args.value == "Exact matches only")
                {
                    return 4;
                }
                return 2;
            },
            false)
        ->setToolTip("How close two messages from the same user need to be "
                     "before they count as repeated.");

    SettingWidget::intInput("Repetition threshold",
                            s.repeatedMessagesRepetitionThreshold,
                            {.min = 2, .max = 20})
        ->setTooltip("Choose how many matching messages trigger the counter.")
        ->addTo(*this->moderationView_);

    SettingWidget::colorButton("Counter color", s.repeatedMessagesCounterColor)
        ->setTooltip("Choose the color of the inline repeat counter.")
        ->addTo(*this->moderationView_);

    this->moderationView_->addTitle("Bulk moderation");
    this->moderationView_->addDescription(
        "Preview and run bulk moderation with /nuke.");

    SettingWidget::checkbox("Preview /nuke targets while typing",
                            s.nukePreviewEnabled)
        ->setTooltip("Highlight messages that match the /nuke command as you "
                     "type it.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Show a summary after /nuke", s.nukeShowSummary)
        ->setTooltip("Show one compact chat message when /nuke finishes.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Protect VIPs when using /nuke", s.nukeSkipVips)
        ->setTooltip("Moderators and the broadcaster are always protected. "
                     "Turn this on to protect VIPs too.")
        ->addTo(*this->moderationView_);

    const auto nukeMessageTooltip = QStringLiteral(
        "Message Twitch sends for /nuke timeouts and bans. Leave this blank "
        "to send no message.");
    auto *nukeMessageRow = new QWidget;
    nukeMessageRow->setMinimumWidth(0);
    auto *nukeMessageLayout = new QHBoxLayout(nukeMessageRow);
    nukeMessageLayout->setContentsMargins(0, 0, 0, 0);
    nukeMessageLayout->setSpacing(8);

    auto *nukeMessageLabel = new QLabel("Message for /nuke actions:");
    nukeMessageLabel->setMinimumWidth(0);
    nukeMessageLabel->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Fixed);
    nukeMessageLabel->setToolTip(nukeMessageTooltip);

    auto *nukeMessageInput = new QLineEdit;
    nukeMessageInput->setText(s.nukeModerationMessage);
    nukeMessageInput->setPlaceholderText("Optional moderation message");
    nukeMessageInput->setToolTip(nukeMessageTooltip);
    {
        const auto charWidth =
            nukeMessageInput->fontMetrics().horizontalAdvance(
                QLatin1Char('M'));
        nukeMessageInput->setMaximumWidth(charWidth * 25 + 24);
        nukeMessageInput->setMinimumWidth(48);
        nukeMessageInput->setSizePolicy(QSizePolicy::Preferred,
                                        QSizePolicy::Fixed);
    }

    nukeMessageLayout->addWidget(nukeMessageLabel);
    nukeMessageLayout->addStretch(1);
    nukeMessageLayout->addWidget(nukeMessageInput);

    QObject::connect(nukeMessageInput, &QLineEdit::textChanged,
                     nukeMessageRow,
                     [&setting = s.nukeModerationMessage](
                         const QString &newValue) {
                         setting = newValue;
                     });
    s.nukeModerationMessage.connect(
        [nukeMessageInput](const QString &value) {
            if (nukeMessageInput->text() != value)
            {
                nukeMessageInput->setText(value);
            }
        },
        this->managedConnections_, false);
    this->moderationView_->addWidget(
        nukeMessageRow, {"Message for /nuke actions", "Nuke action message"});

    view->addTitle("Chatter list");
    SettingWidget::checkbox("Show chatter list in all Twitch channels",
                            s.showChatterListInAllTwitchChannels)
        ->setTooltip(
            "Show the chatter list button even when you are not a moderator.")
        ->addTo(*view);
    SettingWidget::dropdown("Chatter list source", s.chatterListDataMode,
                            {{"Twitch and Tackling", "best"},
                             {"Twitch only", "twitch"},
                             {"Tackling only", "community"},
                             {"Chat session only", "local"}})
        ->setTooltip(
            "Choose the API used for the chatter list. Tackling provides the "
            "extended list. Twitch requires moderator access. All choices "
            "include chatters seen in your chat session. Refresh or reopen "
            "the list after changing its source.")
        ->addTo(*view);

    view->addTitle("Client");
    view->addDescription(
        "Twitch chat behavior, compatibility, and client experience options.");

    view->addDropdown<int>(
            "Show a delete button on my messages",
            {"Never", "In moderation mode", "Always"}, s.showSelfDeleteButton,
            [](auto val) {
                switch (val)
                {
                    case 0:
                        return QString("Never");
                    case 2:
                        return QString("Always");
                    default:
                        return QString("In moderation mode");
                }
            },
            [](auto args) {
                if (args.value == "Never")
                {
                    return 0;
                }
                if (args.value == "Always")
                {
                    return 2;
                }
                return 1;
            },
            false)
        ->setToolTip("Choose when the inline Delete action appears beside "
                     "messages sent by the selected account.");

    SettingWidget::checkbox("Thin tab lines", s.thinTabLines)
        ->setTooltip("Thinner lines with less space above tabs.")
        ->addTo(*view);

    SettingWidget::checkbox("Continuous background across splits",
                            s.continuousSplitBackground)
        ->addKeywords({"wallpaper", "image", "video"})
        ->addTo(*view);

    SettingWidget::checkbox("Make #channel names clickable", s.linkChannelNames)
        ->setTooltip("Open Twitch channels in an existing tab or a new tab.")
        ->addTo(*view);

    SettingWidget::checkbox("Correct ASCII art wrapping", s.wrapAsciiArt)
        ->setTooltip("Wrap ASCII art at Twitch web chat's width.")
        ->addTo(*view);

    SettingWidget::checkbox("Show Bluzyrino badges", s.showBadgesBluzyrino)
        ->setTooltip("Applies to everyone's badges on this device. Manage your "
                     "own badge visibility in Vanity.")
        ->addTo(*view);

    SettingWidget::checkbox("Send messages as Twitch Web",
                            s.spoofIrcMessagesAsWeb)
        ->setTooltip("Make normal chat messages behave more like messages "
                     "sent from Twitch's website.")
        ->addTo(*view);

    const std::vector<std::pair<QString, QVariant>> clientDetectionModes{
        {"Off", "off"},
        {"Highlight messages", "highlight"},
        {"Show a platform badge", "icon"},
        {"Highlight and show a badge", "both"},
    };
    const auto currentClientDetectionMode =
        s.clientDetectionDisplayMode.getValue().trimmed().toLower();
    if (currentClientDetectionMode != QLatin1String("off") &&
        currentClientDetectionMode != QLatin1String("highlight") &&
        currentClientDetectionMode != QLatin1String("icon") &&
        currentClientDetectionMode != QLatin1String("both"))
    {
        s.clientDetectionDisplayMode =
            s.showClientDetectionHighlights.getValue() ? "highlight" : "off";
    }

    SettingWidget::dropdown("Highlight message source",
                            s.clientDetectionDisplayMode, clientDetectionModes)
        ->setTooltip(
            "Shows whether a Twitch message came from the web, Android, or "
            "iOS.")
        ->addTo(*view);

    SettingWidget::colorButton("Twitch Web message color",
                               s.clientDetectionWebColor)
        ->setTooltip("Color for messages sent from Twitch Web.")
        ->addTo(*view);

    SettingWidget::colorButton("Android message color",
                               s.clientDetectionAndroidColor)
        ->setTooltip("Color for messages sent from Android.")
        ->addTo(*view);

    SettingWidget::colorButton("iOS message color", s.clientDetectionIosColor)
        ->setTooltip("Choose the color for messages sent from iOS.")
        ->addTo(*view);

    SettingWidget::checkbox("Highlight unusual client messages",
                            s.showAbnormalClientDetectionHighlights)
        ->setTooltip(
            "Highlight messages whose client information does not match Twitch "
            "Web, Android, or iOS. Third party clients can trigger this, so "
            "it is not proof of abuse.")
        ->addTo(*view);

    SettingWidget::colorButton("Unusual message color",
                               s.clientDetectionAbnormalColor)
        ->setTooltip("Choose the background color for unusual client messages.")
        ->addTo(*view);

    SettingWidget::dropdown("Preferred message history service",
                            s.recentMessagesProvider,
                            recentMessageProviderItems())
        ->setTooltip(
            "Try this service first when loading recent Twitch messages. "
            "Moltorino tries the other services once if it is unavailable.")
        ->addTo(*view);

    SettingWidget::checkbox("Send activity heartbeats",
                            s.sendActivityHeartbeats)
        ->setTooltip("Send small periodic activity updates to the configured "
                     "heartbeat server.")
        ->addTo(*view);

    SettingWidget::checkbox("Hide my account in heartbeats",
                            s.hideAccountInHeartbeats)
        ->setTooltip("Leave your Twitch account details out of heartbeat data.")
        ->conditionallyEnabledBy(s.sendActivityHeartbeats)
        ->addTo(*view);

    view->addTitle("Chat recording");
    view->addWidget(
        makeChatRecordingSettings(this),
        {"Chat recording", "TwitchDownloader", "JSON", "Recordings folder",
         "Embed images", "Recording hotkey", "Recording button",
         "Channel header", "Platform badges", "Mixed chat"});

    view->addTitle("Chat automations");
    view->addDescription(
        "Create self bot rules that reply or run commands from your Twitch "
        "account.");

    auto *automationEnabled = new SCheckBox("Enable chat automations", this);
    automationEnabled->setToolTip(
        "Pause every rule without changing your setup.");
    if (auto *automations = getApp()->getChatAutomations(); automations)
    {
        automationEnabled->setChecked(automations->enabled());
        QObject::connect(
            automationEnabled, &QCheckBox::toggled, this,
            [this, automations](bool enabled) {
                automations->setEnabled(enabled);
                if (!automations->save())
                {
                    automations->setEnabled(!enabled);
                    QMessageBox::warning(
                        this, "Chat automations",
                        "Could not save chat automations. Check that your "
                        "settings folder is writable and try again.");
                }
            });
        this->chatAutomationRulesConnection_ =
            automations->rulesChanged.connect([automations, automationEnabled] {
                const QSignalBlocker blocker(automationEnabled);
                automationEnabled->setChecked(automations->enabled());
            });
    }
    else
    {
        automationEnabled->setEnabled(false);
    }
    view->addWidget(automationEnabled,
                    {"Self bot", "Automation rules", "Enable automations"});

    SettingWidget::checkbox("Run in the background",
                            s.chatAutomationsRunInBackground)
        ->setTooltip(
            "Keep rules active while Moltorino is minimized or another "
            "app is focused.")
        ->addTo(*view);

    SettingWidget::intInput("Maximum actions per 30 seconds",
                            s.chatAutomationsMaxRunsPer30Seconds,
                            {.min = 1, .max = 10})
        ->setTooltip("Limit how often automations can act in each channel. "
                     "Individual rule cooldowns still apply.")
        ->addTo(*view);

    auto *openAutomations = view->addButton("Open automations", [this] {
        ChatAutomationDialog::showDialog(
            {}, &getApp()->getWindows()->getMainWindow());
    });
    openAutomations->setToolTip("Open the rule editor.");

    view->addTitle("Usercards");
    view->addDescription(
        "Choose what usercards show and where actions appear.");

    view->addSubtitle("Profile details");

    SettingWidget::checkbox("Show badges", s.showUsercardBadges)
        ->setTooltip("Show the user's badges in usercards. Badge types follow "
                     "the Visible badges settings in General.")
        ->addTo(*view);
    SettingWidget::checkbox("Show 7TV paint name", s.showUsercardSevenTVPaint)
        ->setTooltip(
            "Show the user's 7TV paint name and link to 7Database when "
            "available.")
        ->addTo(*view);
    SettingWidget::checkbox("Show follower count", s.showUsercardFollowerCount)
        ->addTo(*view);
    SettingWidget::checkbox("Show account creation date",
                            s.showUsercardCreatedDate)
        ->addTo(*view);
    SettingWidget::checkbox("Show when the user was last live",
                            s.showUsercardLastLive)
        ->setTooltip(
            "Show when the user was last live. Hover over it to see the "
            "stream title.")
        ->addTo(*view);
    SettingWidget::checkbox("Show user color", s.showUsercardColor)
        ->setTooltip("Show the user's Twitch chat color.")
        ->addTo(*view);
    SettingWidget::checkbox("Show Twitch status", s.showUsercardStatus)
        ->setTooltip(
            "Show whether the user is Staff, a verified bot, a Partner, "
            "an Affiliate, or not an Affiliate.")
        ->addTo(*view);

    SettingWidget::checkbox("Show chatter count",
                            s.showUsercardChatterCount)
        ->setTooltip("Show the current chatter count when available.")
        ->addTo(*view);
    SettingWidget::checkbox("Show follow date", s.showUsercardFollowage)
        ->setTooltip("Show when the user started following the channel.")
        ->addTo(*view);
    SettingWidget::checkbox("Show follow duration",
                            s.showUsercardFollowageRelativeTime)
        ->setTooltip("Show how long the user has followed next to the date, "
                     "such as (1y 3m), (3 weeks), or (12 days).")
        ->addTo(*view);
    SettingWidget::checkbox("Show subscription length", s.showUsercardSubage)
        ->setTooltip("Show how many months the user has been subscribed.")
        ->addTo(*view);
    SettingWidget::checkbox("Show subscription duration",
                            s.showUsercardSubageRelativeTime)
        ->setTooltip("Show a compact year and month value alongside the total "
                     "months after the first year.")
        ->addTo(*view);
    SettingWidget::checkbox("Show who gifted the subscription",
                            s.showUsercardSubGiftSource)
        ->setTooltip(
            "Show the gifter when the current subscription was gifted.")
        ->addTo(*view);
    SettingWidget::checkbox("Show name history button",
                            s.showUsercardNameHistoryButton)
        ->setTooltip("Show a compact usercard button for previous Twitch names.")
        ->addTo(*view);

    view->addSubtitle("Actions");
    view->addDescription(
        "Put each action in the usercard bar, keep it in More, or hide it.");
    view->addDescription(
        "For custom buttons, add a usercard label to a command in Settings > "
        "Commands. "
        "Commands can open URLs or run actions for the selected user.");
    const std::vector<std::pair<QString, QVariant>> actionPlacements{
        {"Action bar", "bar"}, {"More menu", "menu"}, {"Hidden", "hidden"}};
    SettingWidget::dropdown("Moderator comments",
                            s.usercardCommentsActionPlacement, actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("Logs view", s.usercardLogsActionPlacement,
                            actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("Roles", s.usercardRolesActionPlacement,
                            actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("7TV profile", s.usercardSevenTVActionPlacement,
                            actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("Notes", s.usercardNotesActionPlacement,
                            actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("Block user", s.usercardBlockActionPlacement,
                            actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("Hide user", s.usercardHideActionPlacement,
                            actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("Ignore highlights",
                            s.usercardIgnoreHighlightsActionPlacement,
                            actionPlacements)
        ->addTo(*view);
    SettingWidget::dropdown("Cross ban", s.usercardCrossBanActionPlacement,
                            actionPlacements)
        ->setTooltip(
            "Ban a user across channels available to your saved account.")
        ->addTo(*view);
    SettingWidget::dropdown("Cross unban", s.usercardCrossUnbanActionPlacement,
                            actionPlacements)
        ->setTooltip(
            "Unban a user across channels available to your saved account.")
        ->addTo(*view);
    SettingWidget::checkbox(
        "Show cross ban and cross unban in channels you don't moderate",
        s.showCrossActionsInUnmoderatedChannels)
        ->setTooltip("Use your saved account's channels even when you cannot "
                     "moderate the channel you are viewing.")
        ->addTo(*view);
    SettingWidget::dropdown("Usercard", s.usercardUsercardActionPlacement,
                            actionPlacements)
        ->setTooltip(
            "Open Twitch's web usercard or the user's YouTube channel.")
        ->addTo(*view);

    view->addSubtitle("Message history");

    SettingWidget::checkbox("Show a button to load older messages",
                            s.showUsercardLoadMoreMessagesButton)
        ->setTooltip("Show a usercard button for loading older messages when "
                     "your saved Moltorino login can moderate the channel.")
        ->addTo(*view);
    SettingWidget::checkbox("Load older messages automatically",
                            s.alwaysLoadMoreUsercardMessages)
        ->setTooltip(
            "Start loading older usercard messages without waiting for "
            "a click.")
        ->addTo(*view);

    view->addSubtitle("Logs view");
    SettingWidget::checkbox("Show newest logs at the bottom",
                            s.userLogsNewestAtBottom)
        ->addTo(*view);
    SettingWidget::checkbox("Show dates in user logs", s.showUserLogsDate)
        ->setTooltip("Show the message date alongside its time in user logs.")
        ->addTo(*view);
    SettingWidget::dropdown(
        "User log date style", s.userLogsDateStyle,
        {{"Compact (Aug 21)", "compact"},
         {"ISO (2026-08-21)", "iso"},
         {"Month first (Aug 21, 2026)", "month-first"},
         {"Day first (21 Aug 2026)", "day-first"},
         {"Numeric month first (08/21/2026)", "numeric-month-first"},
         {"Numeric day first (21/08/2026)", "numeric-day-first"}})
        ->conditionallyEnabledBy(s.showUserLogsDate)
        ->addTo(*view);
    SettingWidget::dropdown(
        "User log time style", s.userLogsTimeStyle,
        {{"24 hour (14:05)", "24h-minute"},
         {"24 hour with seconds (14:05:09)", "24h-second"},
         {"12 hour (2:05 PM)", "12h-minute"},
         {"12 hour with seconds (2:05:09 PM)", "12h-second"}})
        ->addTo(*view);

    view->addSubtitle("Role controls");
    SettingWidget::checkbox("Show role buttons as a lead moderator",
                            s.showLeadModRoleButtons)
        ->setTooltip("Show role buttons on usercards when Twitch confirms you "
                     "are a lead moderator.")
        ->addTo(*view);
    SettingWidget::checkbox("Show the editor and lead moderator menu",
                            s.showUsercardRoleManagementMenu)
        ->setTooltip(
            "Show a usercard menu for adding or removing editor and lead "
            "moderator roles. These actions still require a saved broadcaster "
            "login.")
        ->addTo(*view);

#ifdef Q_OS_MACOS
    view->addTitle("macOS");
    view->addDescription("Choose what happens when you close the main window.");

    SettingWidget::checkbox(
        "Keep Moltorino running after closing the main window",
        s.macosKeepRunningAfterClose)
        ->setTooltip("Close the window without quitting Moltorino. Open it "
                     "again from the Dock. Command-Q still quits the app.")
        ->addTo(*view);
#else
    view->addTitle("System tray");
    view->addDescription("Keep Moltorino running in the system tray after you "
                         "close the window.");

    const bool trayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    const bool notificationAvailable =
        trayAvailable &&
        (Toasts::isHighlightNotificationSupported() ||
         QSystemTrayIcon::supportsMessages());

    if (!trayAvailable)
    {
        view->addDescription(
            "The system tray is not available in this desktop session.");
    }
    else if (!notificationAvailable)
    {
        view->addDescription(
            "Tray notifications are not available in this desktop session.");
    }

    auto *hideToTrayWidget =
        SettingWidget::checkbox("Hide to tray when closing Moltorino",
                                s.trayHideOnClose)
            ->setTooltip("Keep Moltorino in the system tray instead of closing "
                         "it and disconnecting from chat.");
    hideToTrayWidget->setEnabled(trayAvailable);
    hideToTrayWidget->addTo(*view);

    auto *notifyWidget =
        SettingWidget::checkbox("Show notifications for highlights with sound",
                                s.trayNotifyOnSoundHighlights)
            ->setTooltip("Only highlights with Play sound enabled show a "
                         "notification while Moltorino is hidden.");
    notifyWidget->setEnabled(notificationAvailable);
    notifyWidget->addTo(*view);
#endif

    view->addTitle("Fun");
    view->addDescription(
        "Emote effects, daily notes, and playful chat commands.");

    auto *modifierRow = new QWidget;
    auto *modifierLayout = new QHBoxLayout(modifierRow);
    modifierLayout->setContentsMargins(0, 0, 0, 0);
    SettingWidget::checkbox("FFZ and BTTV emote modifiers",
                            s.enableEmoteModifiers)
        ->setTooltip(
            "Turn off all modifier effects without changing your individual "
            "choices. Disabled modifiers appear as separate emote icons.")
        ->addToLayout(modifierLayout);
    auto *chooseModifiers = new QPushButton("Choose...", modifierRow);
    modifierLayout->addWidget(chooseModifiers);

    QMap<QString, QStringList> modifierNames{
        {"FFZ",
         {"ffzArrive", "ffzBounce", "ffzCursed", "ffzHyper", "ffzJam",
          "ffzLeave", "ffzRainbow", "ffzSlide", "ffzSpin", "ffzW", "ffzX",
          "ffzY"}},
        {"BTTV", {"w!", "h!", "v!", "l!", "r!", "z!", "c!", "p!", "s!"}},
    };
    const auto addLoadedModifiers = [&modifierNames](const auto &emotes,
                                                     const QString &provider) {
        for (const auto &[name, emote] : *emotes)
        {
            if (emote->modifierPlacement != EmoteModifierPlacement::None)
            {
                modifierNames[provider].append(name.string);
            }
        }
    };
    if (auto *ffz = getApp()->getFfzEmotes())
    {
        addLoadedModifiers(ffz->emotes(), "FFZ");
    }
    if (auto *bttv = getApp()->getBttvEmotes())
    {
        addLoadedModifiers(bttv->emotes(), "BTTV");
    }
    QStringList modifierKeywords{"FFZ", "BTTV", "emote", "modifiers"};
    for (auto &names : modifierNames)
    {
        names.removeDuplicates();
        names.sort();
        modifierKeywords.append(names);
    }
    view->addWidget(modifierRow, modifierKeywords);
    s.enableEmoteModifiers.connect(
        [chooseModifiers](bool enabled, auto) {
            chooseModifiers->setEnabled(enabled);
        },
        this->managedConnections_);
    QObject::connect(
        chooseModifiers, &QPushButton::clicked, this,
        [this, &s, modifierNames] {
            QDialog dialog(this);
            dialog.setWindowTitle("Emote modifiers");
            dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
            auto *layout = new QVBoxLayout(&dialog);
            auto *providers = new QHBoxLayout;
            layout->addLayout(providers);
            pajlada::Signals::SignalHolder connections;
            for (const auto &provider : {QString("FFZ"), QString("BTTV")})
            {
                auto *group = new QGroupBox(provider, &dialog);
                auto *choices = new QVBoxLayout(group);
                providers->addWidget(group);
                for (const auto &name : modifierNames.value(provider))
                {
                    auto *check = new QCheckBox(name, group);
                    choices->addWidget(check);
                    s.disabledEmoteModifiers.connect(
                        [check, name](const QStringList &disabled, auto) {
                            const QSignalBlocker blocker(check);
                            check->setChecked(!disabled.contains(name));
                        },
                        connections);
                    QObject::connect(
                        check, &QCheckBox::toggled, &dialog,
                        [&s, name](bool enabled) {
                            auto disabled = s.disabledEmoteModifiers.getValue();
                            disabled.removeAll(name);
                            if (!enabled)
                            {
                                disabled.append(name);
                            }
                            s.disabledEmoteModifiers.setValue(disabled);
                        });
                }
                choices->addStretch();
            }
            auto *close =
                new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
            layout->addWidget(close);
            QObject::connect(close, &QDialogButtonBox::rejected, &dialog,
                             &QDialog::reject);
            dialog.adjustSize();
            dialog.resize(qRound(dialog.width() * 1.2), dialog.height());
            dialog.exec();
        });

    SettingWidget::checkbox("Show quote of the day", s.showDailyPositiveMessage)
        ->setTooltip(
            "Show one quiet note in the selected chat when a new local "
            "day begins.")
        ->addTo(*view);

    SettingWidget::intInput("Delay between /spam and /pyramid messages",
                            s.spamCommandIntervalMs,
                            {.min = 10, .max = 5000, .singleStep = 10,
                             .suffix = QStringLiteral(" ms")})
        ->setTooltip(
            "Choose how long /spam and /pyramid wait between messages. "
            "Lower values are faster, but Twitch may still limit "
            "accounts that are not moderators, VIPs, or the broadcaster.")
        ->addTo(*view);

    SettingWidget::checkbox("Use IRC for /spam and /pyramid",
                            s.spamCommandUseIrc)
        ->setTooltip("Send /spam and /pyramid through the old IRC path "
                     "instead of Twitch's Helix chat API.")
        ->addTo(*view);

    SettingWidget::checkbox("Show /spam and /pyramid status messages",
                            s.showSpamPyramidStatusMessages)
        ->setTooltip("Show start and finish messages for /spam and /pyramid. "
                     "Errors and manual stop messages still appear.")
        ->addTo(*view);

    SettingWidget::checkbox("Send message as warnings",
                            s.sendMessageAsWarnings)
        ->setTooltip("Send eligible messages through the warning style "
                     "message flow instead of the normal chat path.")
        ->addTo(*view);

    view->addTitle("Tab groups");
    view->addDescription("Keep related tabs together without closing them.");

    SettingWidget::checkbox("Show tab group button", s.showTabGroupButton)
        ->setTooltip(
            "Show the group button beside the new tab button. You can also "
            "create groups from a tab menu.")
        ->addTo(*view);

    SettingWidget::checkbox("Show channel names in group menus",
                            s.showTabGroupChannelNames)
        ->setTooltip("Show the channel name when it differs from the tab name.")
        ->addTo(*view);

    SettingWidget::dropdown(
        "Default action for new groups", s.newTabGroupClickAction,
        {{"Expand tabs", "expand"}, {"Open tab list", "menu"}})
        ->setTooltip("You can change this later from the group menu.")
        ->addTo(*view);

    view->addTitle("Miscellaneous");
    auto *miscDesc = new SignalLabel(this);
    miscDesc->setText("General Moltorino tweaks and interface adjustments.");
    miscDesc->setWordWrap(true);
    miscDesc->setStyleSheet("QLabel { color: #9aa0a6; }");
    view->addWidget(miscDesc);
    SettingWidget::checkbox("Allow popup resizing", s.allowPopupResize)
        ->addTo(*view);

    QObject::connect(miscDesc, &SignalLabel::leftMouseUp, this, [this] {
        this->logoClickCount_++;
        if (this->logoClickCount_ >= 8)
        {
            this->revealBotBadgeSettings(true);
        }
    });

    SettingWidget::checkbox("Dim loaded message history", s.fadeMessageHistory)
        ->setTooltip(
            "Make messages loaded when a channel opens or reconnects less "
            "prominent.")
        ->addKeywords(
            {"old messages", "gray messages", "startup", "restored messages"})
        ->addTo(*view);

    SettingWidget::checkbox("Use message colors for tab alerts",
                            s.colorTabHighlightsByMessage)
        ->setTooltip("When a message highlights a tab, use that highlight "
                     "color for the tab alert line.")
        ->addTo(*view);

    view->addDropdown<int>(
            "Badge alignment", {"Balanced", "Chatterino"}, s.badgeAlignment,
            [](auto value) {
                return value == static_cast<int>(BadgeAlignmentMode::Chatterino)
                           ? 1
                           : 0;
            },
            [](auto args) {
                return args.index == 1
                           ? static_cast<int>(BadgeAlignmentMode::Chatterino)
                           : static_cast<int>(BadgeAlignmentMode::Balanced);
            },
            false)
        ->setToolTip(
            "Balanced lifts badges and inline buttons to align with most "
            "fonts. "
            "Chatterino keeps them on the bottom of the full text line.");

    SettingWidget::checkbox("Show follow button in chat header",
                            s.showFollowButtonInSplitHeader)
        ->setTooltip("Show a follow/unfollow button in the top bar above "
                     "each Twitch chat.")
        ->addTo(*view);

    SettingWidget::checkbox("Show the Shared Chat channel selector",
                            s.showSharedChatChannelSelector)
        ->setTooltip(
            "Show a channel picker in the chat header when Shared Chat "
            "includes more than one channel.")
        ->addTo(*view);

    SettingWidget::checkbox("Show follow button in usercards",
                            s.showFollowButtonInUsercard)
        ->setTooltip("Show a button for following or unfollowing the channel "
                     "below the profile picture on Twitch usercards.")
        ->addTo(*view);

    SettingWidget::checkbox("Confirm before unfollowing",
                            s.confirmUnfollowFromSplitHeader)
        ->setTooltip(
            "Ask before a follow button in the chat header or a "
            "usercard unfollows a channel. The /unfollow command still "
            "runs without a prompt.")
        ->addTo(*view);

    SettingWidget::checkbox("Hide mod actions on moderator usercards",
                            s.hideModActionsOnModUsercards)
        ->setTooltip(
            "When you are a moderator, hide timeout and ban controls on "
            "moderator and broadcaster usercards. Broadcasters still see all "
            "controls.")
        ->addTo(*this->moderationView_);

    SettingWidget::checkbox("Show moderation actions as a lead moderator",
                            s.showModActionsOnModUsercardsAsLeadMod)
        ->setTooltip(
            "When moderation actions are hidden on moderator usercards, show "
            "them if Twitch confirms you are a lead moderator. Broadcaster "
            "usercards stay hidden.")
        ->conditionallyEnabledBy(s.hideModActionsOnModUsercards)
        ->addTo(*this->moderationView_);

    view->addStretch();
    this->moderationView_->addStretch();

    view->addWidget(this->botBadgeFrame_, {"Bot badge", "Developer", "Verify",
                                           "Client ID", "Client Secret"});
}

bool MoltorinoPage::filterElements(const QString &query)
{
    if (this->settingsView_ == nullptr || this->moderationView_ == nullptr)
    {
        return query.isEmpty();
    }

    const bool generalMatches = this->settingsView_->filterElements(query);
    const bool moderationMatches = this->moderationView_->filterElements(query);

    if (!query.isEmpty() && this->tabBar_ != nullptr)
    {
        const auto current = this->tabBar_->currentIndex();
        const bool currentMatches =
            current == 0 ? generalMatches : moderationMatches;
        if (!currentMatches && (generalMatches || moderationMatches))
        {
            this->tabBar_->setCurrentIndex(generalMatches ? 0 : 1);
        }
    }

    return generalMatches || moderationMatches || query.isEmpty();
}

void MoltorinoPage::showAccountSetup()
{
    this->tabBar_->setCurrentIndex(0);
    QTimer::singleShot(0, this, [this] {
        this->settingsView_->scrollToTop();
        this->addAuthAccountButton_->setFocus(Qt::OtherFocusReason);
        this->openAuthDialog();
    });
}

void MoltorinoPage::showBotBadgeSetup()
{
    this->tabBar_->setCurrentIndex(0);
    this->revealBotBadgeSettings(true);
    QTimer::singleShot(0, this, [this] {
        this->settingsView_->scrollToWidget(this->botBadgeFrame_);
        this->botBadgeClientIdEdit_->setFocus(Qt::OtherFocusReason);
    });
}

void MoltorinoPage::openAuthDialog()
{
    MoltorinoAuthDialog dialog(this);
    dialog.exec();
    this->updateAuthSummary();
}

void MoltorinoPage::refreshAuthAccounts()
{
    if (this->authRefreshInFlight_)
    {
        return;
    }

    this->authRefreshInFlight_ = true;
    const int generation = ++this->authRefreshGeneration_;
    this->refreshAuthAccountsButton_->setEnabled(false);
    this->addAuthAccountButton_->setEnabled(false);
    this->updateAuthStatus("Refreshing accounts...", false);

    QPointer<MoltorinoPage> guard(this);
    MoltorinoAuth::refreshAccounts(
        MoltorinoAuthRefreshMode::Manual,
        [guard, generation](MoltorinoAuthRefreshResult result) {
            if (guard == nullptr || generation != guard->authRefreshGeneration_)
            {
                return;
            }

            guard->authRefreshInFlight_ = false;
            guard->refreshAuthAccountsButton_->setEnabled(true);
            guard->addAuthAccountButton_->setEnabled(true);
            guard->updateAuthSummary();

            if (result.total == 0)
            {
                const auto summary = MoltorinoAuth::summary();
                if (summary.disabledAccountCount == 0)
                {
                    guard->updateAuthStatus(
                        "No account is signed in yet. Log in to continue.",
                        false);
                }
                return;
            }

            if (result.valid > 0)
            {
                return;
            }

            const auto error =
                result.errors.isEmpty()
                    ? QString("No saved account could be validated.")
                    : result.errors.join("\n");
            guard->updateAuthStatus(error, false, true);
        });
}

void MoltorinoPage::updateAuthSummary()
{
    const auto summary = MoltorinoAuth::summary();

    if (this->refreshAuthAccountsButton_ != nullptr)
    {
        const bool hasRefreshableLogin =
            summary.enabledAccountCount > 0 || summary.hasOnlyLegacyToken;
        this->refreshAuthAccountsButton_->setVisible(hasRefreshableLogin);
        this->refreshAuthAccountsButton_->setEnabled(!this->authRefreshInFlight_);
    }
    if (this->addAuthAccountButton_ != nullptr)
    {
        const bool hasSavedLogin =
            summary.accountCount > 0 || summary.hasLegacyToken;
        this->addAuthAccountButton_->setText(hasSavedLogin ? "Add account"
                                                           : "Log in");
        this->addAuthAccountButton_->setToolTip(
            hasSavedLogin ? "Add another account or manage saved accounts."
                          : "Sign in with device login or legacy login.");
        this->addAuthAccountButton_->setEnabled(!this->authRefreshInFlight_);
    }

    if (summary.hasOnlyLegacyToken)
    {
        this->authInstructionsLabel_->setVisible(true);
        this->updateAuthInstructions(
            "Legacy login found. Existing features will keep working. Refresh "
            "accounts to show account details.");
        this->updateAuthStatus("Legacy login active.", true);
        return;
    }

    this->authInstructionsLabel_->clear();
    this->authInstructionsLabel_->setVisible(false);

    if (summary.accountCount <= 0)
    {
        this->updateAuthStatus(
            "No account is signed in yet. Log in to continue.", false);
        return;
    }

    this->updateAuthStatus(
        formatMoltorinoAuthSummary(summary), summary.validAccountCount > 0,
        summary.enabledAccountCount > 0 && summary.validAccountCount == 0);
}

void MoltorinoPage::updateAuthInstructions(const QString &text, bool isError)
{
    this->authInstructionsLabel_->setText(text);
    this->authInstructionsLabel_->setStyleSheet(
        QString("QLabel { color: %1; }")
            .arg(isError ? "#ffb4a2" : "#9aa0a6"));
}

void MoltorinoPage::updateAuthStatus(const QString &text, bool isValid,
                                     bool isError)
{
    this->authStatusLabel_->setText(text);

    QString color = "#9aa0a6";
    if (isValid)
    {
        color = "#47d16c";
    }
    else if (isError)
    {
        color = "#ff7b72";
    }

    this->authStatusLabel_->setStyleSheet(
        QString("QLabel { color: %1; }").arg(color));
}

void MoltorinoPage::revealBotBadgeSettings(bool revealed)
{
    this->botBadgeUnlocked_ = revealed;
    this->botBadgeFrame_->setVisible(this->botBadgeUnlocked_);
}

void MoltorinoPage::populateBotBadgeFieldsFromSettings()
{
    const auto &settings = *getSettings();
    this->botBadgeClientIdEdit_->setText(settings.botBadgeClientID);
    this->botBadgeClientSecretEdit_->setText(settings.botBadgeClientSecret);
    this->botBadgeSenderEdit_->setText(settings.botBadgeUserLogin);

    const auto displayName = settings.botBadgeUserName.getValue().trimmed();
    const auto login = settings.botBadgeUserLogin.getValue().trimmed();
    const auto userId = settings.botBadgeUserID.getValue().trimmed();
    const auto appExpiry = settings.botBadgeAppTokenExpiry.getValue().trimmed();

    if (!userId.isEmpty())
    {
        this->botBadgeIdentityLabel_->setText(
            QString("Bot badge account: %1 (@%2) - ID %3\nToken expires: %4")
                .arg(displayName.isEmpty() ? login : displayName,
                     login.isEmpty() ? QString("unknown") : login, userId,
                     appExpiry.isEmpty() ? QString("not issued")
                                         : formatTimestampStatus(appExpiry)));
    }
    else
    {
        this->botBadgeIdentityLabel_->setText(
            "No bot badge account verified yet.");
    }

    this->updateBotBadgeStatus(
        settings.botBadgeAppAccessToken.getValue().trimmed().isEmpty()
            ? "Bot badge panel visible. Click Verify to test the setup."
            : "Bot badge setup saved. Click Verify to check it.",
        false);
}

void MoltorinoPage::updateBotBadgeStatus(const QString &text, bool isValid,
                                         bool isError)
{
    this->botBadgeStatusLabel_->setText(text);

    QString color = "#9aa0a6";
    if (isValid)
    {
        color = "#47d16c";
    }
    else if (isError)
    {
        color = "#ff7b72";
    }

    this->botBadgeStatusLabel_->setStyleSheet(
        QString("QLabel { color: %1; }").arg(color));
}

void MoltorinoPage::openBotBadgeAuthorization()
{
    const auto clientId = this->botBadgeClientIdEdit_->text().trimmed();
    if (clientId.isEmpty())
    {
        this->revealBotBadgeSettings(true);
        this->updateBotBadgeStatus(
            "Client ID is required before opening Twitch authorization.", false,
            true);
        return;
    }

    QUrl url("https://id.twitch.tv/oauth2/authorize");
    QUrlQuery query;
    query.addQueryItem("response_type", "token");
    query.addQueryItem("client_id", clientId);
    query.addQueryItem("redirect_uri", "http://localhost/");
    query.addQueryItem("scope", "user:write:chat user:bot");
    url.setQuery(query);

    if (QDesktopServices::openUrl(url))
    {
        this->updateBotBadgeStatus(
            "Twitch authorization opened. Approve access in the browser, then "
            "come back and click Verify again.");
    }
    else
    {
        this->updateBotBadgeStatus(
            "Could not open Twitch authorization automatically.", false, true);
    }
}

void MoltorinoPage::verifyBotBadgeConfiguration()
{
    const auto clientId = this->botBadgeClientIdEdit_->text().trimmed();
    const auto clientSecret =
        this->botBadgeClientSecretEdit_->text().trimmed();

    if (clientId.isEmpty() || clientSecret.isEmpty())
    {
        this->updateBotBadgeStatus(
            "Client ID and Client Secret are required.", false, true);
        return;
    }

    QString login = this->botBadgeSenderEdit_->text().trimmed().toLower();

    if (login.isEmpty())
    {
        auto account = getApp()->getAccounts()->twitch.getCurrent();
        if (account->isAnon())
        {
            this->updateBotBadgeStatus(
                "Enter a bot username or sign in to Twitch in "
                "Chatterino to verify.",
                false, true);
            return;
        }
        login = account->getUserName();
    }

    const int generation = ++this->botBadgeValidationGeneration_;
    QPointer<MoltorinoPage> guard(this);

    auto setBusy = [this](bool busy) {
        this->botBadgeIsValidating_ = busy;
        this->botBadgeClientIdEdit_->setEnabled(!busy);
        this->botBadgeClientSecretEdit_->setEnabled(!busy);
        this->botBadgeSenderEdit_->setEnabled(!busy);
        this->botBadgeAuthorizeButton_->setEnabled(!busy);
        this->botBadgeVerifyButton_->setEnabled(!busy);
    };

    setBusy(true);
    this->updateBotBadgeStatus(
        QString("Checking bot badge account @%1...").arg(login));

    QUrl tokenUrl("https://id.twitch.tv/oauth2/token");
    QUrlQuery tokenQuery;
    tokenQuery.addQueryItem("client_id", clientId);
    tokenQuery.addQueryItem("client_secret", clientSecret);
    tokenQuery.addQueryItem("grant_type", "client_credentials");
    NetworkRequest(tokenUrl, NetworkRequestType::Post)
        .caller(this)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .payload(tokenQuery.toString(QUrl::FullyEncoded).toUtf8())
        .hideRequestBody()
        .maximumResponseSize(1024 * 1024)
        .timeout(20000)
        .onSuccess([guard, generation, clientId, clientSecret, login,
                    setBusy](const NetworkResult &appRes) {
            if (guard == nullptr ||
                generation != guard->botBadgeValidationGeneration_)
            {
                return;
            }

            const auto json = appRes.parseJson();
            const auto appToken =
                json.value("access_token").toString().trimmed();
            const auto appTokenExpiry =
                QDateTime::currentDateTimeUtc()
                    .addSecs(json.value("expires_in").toInt())
                    .toString(Qt::ISODate);

            if (appToken.isEmpty())
            {
                setBusy(false);
                guard->updateBotBadgeStatus(
                    "Could not verify the app. Check the Client Secret.", false,
                    true);
                return;
            }

            QUrl usersUrl("https://api.twitch.tv/helix/users");
            QUrlQuery usersQuery;
            usersQuery.addQueryItem("login", login);
            usersUrl.setQuery(usersQuery);

            NetworkRequest(usersUrl, NetworkRequestType::Get)
                .caller(guard)
                .header("Client-ID", clientId)
                .header("Authorization", "Bearer " + appToken)
                .maximumResponseSize(1024 * 1024)
                .timeout(20000)
                .onSuccess([guard, generation, clientId, clientSecret, appToken,
                            appTokenExpiry, login,
                            setBusy](const NetworkResult &usersRes) {
                    if (guard == nullptr ||
                        generation != guard->botBadgeValidationGeneration_)
                    {
                        return;
                    }

                    const auto usersJson = usersRes.parseJson();
                    const auto users = usersJson.value("data").toArray();
                    if (users.isEmpty())
                    {
                        setBusy(false);
                        guard->updateBotBadgeStatus(
                            "Bot badge account could not be found.",
                            false, true);
                        return;
                    }

                    const auto user = users.first().toObject();
                    const auto resolvedUserId = user.value("id").toString();
                    const auto resolvedLogin = user.value("login").toString();
                    const auto resolvedDisplayName =
                        user.value("display_name").toString();

                    auto &settings = *getSettings();
                    settings.botBadgeClientID = clientId;
                    settings.botBadgeClientSecret = clientSecret;
                    settings.botBadgeAppAccessToken = appToken;
                    settings.botBadgeAppTokenExpiry = appTokenExpiry;
                    settings.botBadgeUserID = resolvedUserId;
                    settings.botBadgeUserLogin = resolvedLogin;
                    settings.botBadgeUserName =
                        resolvedDisplayName.isEmpty() ? resolvedLogin
                                                      : resolvedDisplayName;
                    settings.requestSave();

                    guard->populateBotBadgeFieldsFromSettings();
                    guard->updateBotBadgeStatus(
                        QString("Bot badge account @%1 verified.")
                            .arg(resolvedLogin),
                        true);
                    setBusy(false);
                })
                .onError([guard, generation, setBusy](const NetworkResult &) {
                    if (guard == nullptr ||
                        generation != guard->botBadgeValidationGeneration_)
                    {
                        return;
                    }
                    setBusy(false);
                    guard->updateBotBadgeStatus(
                        "Could not look up the bot account. Check your "
                        "connection and try again.",
                        false, true);
                })
                .execute();
        })
        .onError([guard, generation, setBusy](const NetworkResult &) {
            if (guard == nullptr ||
                generation != guard->botBadgeValidationGeneration_)
            {
                return;
            }
            setBusy(false);
            guard->updateBotBadgeStatus(
                "Could not verify the Twitch app. Check your connection, "
                "client ID and client secret.",
                false, true);
        })
        .execute();
}

void MoltorinoPage::hideEvent(QHideEvent *event)
{
    SettingsPage::hideEvent(event);
    this->logoClickCount_ = 0;
    this->revealBotBadgeSettings(false);
}

}  // namespace chatterino
