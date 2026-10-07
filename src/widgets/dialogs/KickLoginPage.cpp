#include "widgets/dialogs/KickLoginPage.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "singletons/Theme.hpp"
#include "util/HttpServer.hpp"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QCryptographicHash>
#include <QTimer>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpressionValidator>
#include <QSpacerItem>
#include <QString>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <utility>

using namespace Qt::Literals::StringLiterals;

namespace {

using namespace chatterino;

const QString REDIRECT_URL = u"http://localhost:38275"_s;
constexpr uint16_t SERVER_PORT = 38275;

QByteArray generateRandomBytes(qsizetype size)
{
    assert((size % 4) == 0);
    QByteArray bytes;
    bytes.resize(size);
    auto *gen = QRandomGenerator::system();
    for (qsizetype i = 0; i < bytes.size() / 4; i++)
    {
        quint32 v = gen->generate();
        std::memcpy(bytes.data() + (i * 4), &v, 4);
    }
    return bytes;
}

QString formatAPIError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    auto error =
        json["error_description"_L1].toString(json["message"_L1].toString());
    if (!error.isEmpty())
    {
        return u"Error: " % error % u" (" % result.formatError() % ')';
    }
    return u"Error: " % result.formatError() % u" (no further information)";
}

struct AuthParams {
    QByteArray codeVerifier;
    QByteArray codeChallenge;
    QByteArray state;
};

AuthParams startAuthSession()
{
    auto base64Opts =
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
    auto codeVerifier = generateRandomBytes(32).toBase64(base64Opts);

    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(codeVerifier);
    auto codeChallenge = h.result().toBase64(base64Opts);

    return {
        .codeVerifier = codeVerifier,
        .codeChallenge = codeChallenge,
        .state = generateRandomBytes(32).toBase64(base64Opts),
    };
}

class AuthDialog : public QDialog
{
public:
    AuthDialog(KickAccountData credentials, QWidget *parent = nullptr)
        : QDialog(parent)
        , credentials(std::move(credentials))
        , authParams(startAuthSession())
        , statusLabel("Waiting...")
    {
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setWindowTitle("Waiting...");
        this->setWindowModality(Qt::WindowModal);
        this->statusLabel.setWordWrap(true);
        this->statusLabel.setTextFormat(Qt::PlainText);

        QUrlQuery query{
            {"response_type", "code"},
            {"client_id", this->credentials.clientID},
            {"redirect_uri", REDIRECT_URL},
            {"scope", "user:read channel:read channel:write chat:write "
                      "moderation:ban moderation:chat_message:manage"},
            {"code_challenge", this->authParams.codeChallenge},
            {"code_challenge_method", "S256"},
            {"state", this->authParams.state},
        };
        this->authURL = u"https://id.kick.com/oauth/authorize?" %
                        query.toString(QUrl::FullyEncoded);

        this->server = new HttpServer(SERVER_PORT, this);
        this->server->setRequestHandler(
            [this](const HttpServer::Request &request) {
                return request.method == u"GET"
                           ? this->handleRequest(request.target)
                           : std::pair<unsigned, QByteArray>{405, "Use GET"_ba};
            });
        QObject::connect(this, &QDialog::finished, this, [this] {
            this->active = false;
            this->server->close();
        });

        auto *root = new QVBoxLayout(this);
        root->addWidget(&this->statusLabel, 1, Qt::AlignCenter);
        root->addWidget(new QLabel("This window will close automatically."), 1,
                        Qt::AlignCenter);

        auto *urlButtons = new QWidget;
        auto *urlButtonLayout = new QHBoxLayout(urlButtons);

        auto *openUrl = new QPushButton(u"Log in (Opens in browser)"_s);
        QObject::connect(openUrl, &QPushButton::clicked, this, [this] {
            this->openBrowser();
        });
        urlButtonLayout->addWidget(openUrl, 1);

        auto *copyUrl = new QPushButton(u"Copy URL"_s);
        QObject::connect(copyUrl, &QPushButton::clicked, this, [this] {
            qApp->clipboard()->setText(
                this->authURL.toString(QUrl::FullyEncoded));
        });
        urlButtonLayout->addWidget(copyUrl, 1);

        root->addWidget(urlButtons, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        root->addWidget(buttons);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        if (!this->server->isListening())
        {
            this->statusLabel.setText("Could not start Kick login. Close any "
                                      "other Kick login window and try again.");
            openUrl->setEnabled(false);
            copyUrl->setEnabled(false);
        }
        else
        {
            QTimer::singleShot(0, this, [this] {
                this->openBrowser();
            });
        }
    }

    std::pair<unsigned, QByteArray> handleRequest(const QString &path)
    {
        if (!this->active)
        {
            return {410, "Login closed"_ba};
        }
        if (QUrl(path).path() != u"/")
        {
            return {404, "Not found"_ba};
        }
        auto queryIdx = path.indexOf('?');
        if (queryIdx < 0)
        {
            return {404, "No query"_ba};
        }
        auto queryStr = path.mid(queryIdx + 1);
        QUrlQuery query(queryStr);
        if (query.hasQueryItem("done"))
        {
            return {200, "You can close this tab now."_ba};
        }

        if (query.queryItemValue("state", QUrl::FullyDecoded) !=
            this->authParams.state)
        {
            return {400, "State mismatch!"_ba};
        }
        if (this->callbackReceived)
        {
            return {409, "Login already received"_ba};
        }
        if (query.hasQueryItem("error"))
        {
            this->callbackReceived = true;
            this->statusLabel.setText("Kick login was not completed. Close "
                                      "this window to try again.");
            return {200,
                    "Login was not completed. You can close this tab."_ba};
        }
        const auto code = query.queryItemValue("code", QUrl::FullyDecoded);
        if (code.isEmpty())
        {
            return {400, "No code"_ba};
        }
        this->callbackReceived = true;
        this->statusLabel.setText("Connecting your Kick account...");
        this->requestToken(code);

        return {
            200,
            "<!DOCTYPE html><html><head></head><body><script>location.search='?done=1'</script></body></html>"_ba,
        };
    }

private:
    void openBrowser()
    {
        if (this->active && this->server->isListening() &&
            !this->callbackReceived &&
            !QDesktopServices::openUrl(this->authURL))
        {
            this->statusLabel.setText("Could not open your browser. Copy the "
                                      "link and open it manually.");
        }
    }

    void requestToken(const QString &code)
    {
        QUrlQuery payload{
            {"grant_type", "authorization_code"},
            {"client_id", this->credentials.clientID},
            {"redirect_uri", REDIRECT_URL},
            {"code_verifier", this->authParams.codeVerifier},
            {"code", code},
        };
        if (this->credentials.publicProxy.isEmpty())
        {
            payload.addQueryItem(u"client_secret"_s,
                                 this->credentials.clientSecret);
        }
        NetworkRequest(this->credentials.tokenUrl(), NetworkRequestType::Post)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .hideRequestBody()
            .timeout(20'000)
            .maximumResponseSize(64 * 1024)
            .payload(payload.toString(QUrl::FullyEncoded).toUtf8())
            .caller(this)
            .onError([this](const NetworkResult &result) {
                if (!this->active)
                {
                    return;
                }
                auto error = formatAPIError(result);
                qCWarning(chatterinoKick) << "Getting token failed" << error;
                this->statusLabel.setText(
                    error + QStringLiteral("\nClose this window to try again."));
            })
            .onSuccess([this](const NetworkResult &result) {
                if (!this->active)
                {
                    return;
                }
                if (!this->credentials.setTokens(result.parseJson()))
                {
                    this->statusLabel.setText(
                        "Kick returned an incomplete login. Close this window "
                        "and try again.");
                    return;
                }
                this->getAuthenticatedUser();
            })
            .execute();
    }

    void getAuthenticatedUser()
    {
        NetworkRequest("https://api.kick.com/public/v1/users")
            .header("Authorization", u"Bearer " % this->credentials.authToken)
            .timeout(20'000)
            .maximumResponseSize(64 * 1024)
            .caller(this)
            .onError([this](const NetworkResult &result) {
                if (!this->active)
                {
                    return;
                }
                auto error = formatAPIError(result);
                qCWarning(chatterinoKick) << "Getting user failed" << error;
                this->statusLabel.setText(
                    error + QStringLiteral("\nClose this window to try again."));
            })
            .onSuccess([this](const NetworkResult &result) {
                if (!this->active)
                {
                    return;
                }
                const auto users =
                    result.parseJson().value("data"_L1).toArray();
                const auto obj =
                    users.isEmpty() ? QJsonObject{} : users.first().toObject();
                const auto name = obj["name"].toString().trimmed();
                const auto id = obj["user_id"].toInteger();
                if (name.isEmpty() || id <= 0)
                {
                    this->statusLabel.setText(
                        "Kick did not return your account. Close this window "
                        "and try again.");
                    return;
                }
                this->credentials.username = name.toLower();
                this->credentials.userID = static_cast<uint64_t>(id);
                this->credentials.save();
                getApp()->getAccounts()->kick.reloadUsers();
                getApp()->getAccounts()->kick.currentUsername =
                    this->credentials.username;
                this->accept();
                this->close();
            })
            .execute();
    }

    KickAccountData credentials;
    AuthParams authParams;
    QUrl authURL;

    QLabel statusLabel;
    HttpServer *server = nullptr;
    bool active = true;
    bool callbackReceived = false;
};

}

namespace chatterino {

KickLoginPage::KickLoginPage()
{
    static const QRegularExpression nonEmptyRe{u".*\\S.*"_s};

    auto *root = new QFormLayout(this);
    this->ui.layout = root;
    this->ui.method = new QComboBox(this);
    this->ui.method->addItems({"Browser login", "Developer credentials"});
    root->addRow("Login method", this->ui.method);

    auto *topLabel = new QLabel(this);
    this->ui.description = topLabel;
    topLabel->setWordWrap(true);
    topLabel->setOpenExternalLinks(true);
    topLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    root->addRow(topLabel);

    this->ui.clientID = new QLineEdit;
    this->ui.clientID->setPlaceholderText("ABCD123");
    this->ui.clientID->setValidator(
        new QRegularExpressionValidator(nonEmptyRe, this));
    root->addRow("Client ID:", this->ui.clientID);

    this->ui.clientSecret = new QLineEdit;
    this->ui.clientSecret->setPlaceholderText("12345abcd");
    this->ui.clientSecret->setEchoMode(QLineEdit::Password);
    this->ui.clientSecret->setValidator(
        new QRegularExpressionValidator(nonEmptyRe, this));
    root->addRow("Client Secret:", this->ui.clientSecret);

    auto currentAccount = getApp()->getAccounts()->kick.current();
    if (!currentAccount->isAnonymous() &&
        currentAccount->publicProxy().isEmpty())
    {
        this->ui.clientID->setText(currentAccount->clientID());
        this->ui.clientSecret->setText(currentAccount->clientSecret());
    }

    auto *startButton = new QPushButton("Start");
    root->addRow(startButton);
    QObject::connect(startButton, &QPushButton::clicked, this, [this] {
        if (this->authDialog_)
        {
            this->authDialog_->raise();
            this->authDialog_->activateWindow();
            return;
        }
        KickAccountData credentials;
        if (this->ui.method->currentIndex() == 0)
        {
            credentials.clientID = kick::AUTH_CLIENT_ID;
            credentials.publicProxy = kick::AUTH_PROXY;
        }
        else
        {
            if (!this->ui.clientID->hasAcceptableInput())
            {
                this->ui.clientID->setFocus();
                return;
            }
            if (!this->ui.clientSecret->hasAcceptableInput())
            {
                this->ui.clientSecret->setFocus();
                return;
            }
            credentials.clientID = this->ui.clientID->text().trimmed();
            credentials.clientSecret = this->ui.clientSecret->text().trimmed();
        }
        auto *diag = new AuthDialog(std::move(credentials), this);
        this->authDialog_ = diag;
        QObject::connect(diag, &QDialog::finished, this, [this, diag] {
            if (this->authDialog_ == diag)
            {
                this->authDialog_.clear();
            }
        });
        QObject::connect(diag, &QDialog::accepted, this, [this] {
            this->authDialog_.clear();
            this->window()->close();
        });
        diag->show();
    });
    QObject::connect(this->ui.method, &QComboBox::currentIndexChanged, this,
                     &KickLoginPage::refreshState);
    this->refreshState();
}

void KickLoginPage::refreshState()
{
    const bool manual = this->ui.method->currentIndex() == 1;
    this->ui.layout->setRowVisible(this->ui.clientID, manual);
    this->ui.layout->setRowVisible(this->ui.clientSecret, manual);
    if (manual)
    {
        this->ui.description->setText(
            "Use an app from <a "
            "href=\"https://kick.com/settings/developer\">Kick developer "
            "settings</a>. "
            "Add this redirect URL: <code>" %
            REDIRECT_URL % "</code>");
    }
    else
    {
        this->ui.description->setText(
            "Sign in through Chatterino7's <a "
            "href=\"https://c7-auth.nerixyz.de\">login service</a>. "
            "No developer credentials needed.");
    }
}

void KickLoginPage::paintEvent(QPaintEvent * )
{
    QPainter painter(this);

    painter.setBrush(getTheme()->window.background);
    painter.setPen({});
    painter.drawRect(this->rect());
}

void KickLoginPage::hideEvent(QHideEvent *event)
{
    if (this->authDialog_)
    {
        this->authDialog_->reject();
    }
    QWidget::hideEvent(event);
}

}
