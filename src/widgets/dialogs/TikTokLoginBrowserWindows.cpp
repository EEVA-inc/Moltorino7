#ifdef _WIN32
#    define NOMINMAX
#    include <WebView2.h>
#    include <WebView2EnvironmentOptions.h>
#    include <Windows.h>
#    include <wrl.h>
#endif

#include "TikTokLoginBrowser.hpp"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QWidget>

#include <utility>

namespace chatterino {
using namespace Qt::Literals::StringLiterals;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
QString takeString(LPWSTR value)
{
    const auto text = QString::fromWCharArray(value ? value : L"");
    CoTaskMemFree(value);
    return text;
}

struct LoginProfile : std::enable_shared_from_this<LoginProfile> {
    QTemporaryDir directory{QDir::tempPath() +
                            u"/moltorino-tiktok-login-XXXXXX"_s};
    ComPtr<ICoreWebView2Environment> environment;
    EventRegistrationToken exitToken{};
    bool closed = false;

    LoginProfile()
    {
        this->directory.setAutoRemove(false);
    }
    void observeExit()
    {
        ComPtr<ICoreWebView2Environment5> env5;
        if (FAILED(this->environment.As(&env5)))
            return;
        auto self = this->shared_from_this();
        env5->add_BrowserProcessExited(
            Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
                [self](
                    ICoreWebView2Environment *,
                    ICoreWebView2BrowserProcessExitedEventArgs *) -> HRESULT {
                    ComPtr<ICoreWebView2Environment5> environment5;
                    if (SUCCEEDED(self->environment.As(&environment5)))
                        environment5->remove_BrowserProcessExited(
                            self->exitToken);
                    self->environment.Reset();
                    self->directory.remove();
                    return S_OK;
                })
                .Get(),
            &this->exitToken);
    }
    ~LoginProfile()
    {
        this->directory.remove();
    }
};

class WindowsLoginBrowser final : public TikTokLoginBrowser
{
public:
    using TikTokLoginBrowser::TikTokLoginBrowser;
    ~WindowsLoginBrowser() override
    {
        this->close();
        if (SUCCEEDED(this->comResult_))
            CoUninitialize();
    }

    void start(QWidget *surface, Callbacks callbacks) override
    {
        this->surface_ = surface;
        this->callbacks_ = std::move(callbacks);
        this->comResult_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(this->comResult_))
        {
            this->error(
                u"TikTok login could not start. Restart Moltorino and try again."_s);
            return;
        }
        this->profile_ = std::make_shared<LoginProfile>();
        if (!this->profile_->directory.isValid())
        {
            this->error(u"A private login session could not be created."_s);
            return;
        }
        const auto path = this->profile_->directory.path();
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        options->put_Language(L"en-US");
        options->put_AdditionalBrowserArguments(
            L"--autoplay-policy=user-gesture-required");
        QPointer<WindowsLoginBrowser> weak(this);
        const auto result = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, reinterpret_cast<LPCWSTR>(path.utf16()), options.Get(),
            Callback<
                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [weak, profile = this->profile_](
                    HRESULT result,
                    ICoreWebView2Environment *environment) -> HRESULT {
                    if (FAILED(result) || !environment)
                    {
                        if (weak && !weak->closed_)
                            weak->error(
                                u"Microsoft Edge WebView2 is needed for TikTok login. Install its Evergreen Runtime and try again."_s);
                        return S_OK;
                    }
                    profile->environment = environment;
                    if (!weak || profile->closed)
                        return S_OK;
                    ComPtr<ICoreWebView2Environment10> env10;
                    ComPtr<ICoreWebView2ControllerOptions> options;
                    if (FAILED(environment->QueryInterface(
                            IID_PPV_ARGS(&env10))) ||
                        FAILED(env10->CreateCoreWebView2ControllerOptions(
                            &options)))
                    {
                        weak->error(
                            u"Update Microsoft Edge WebView2 to open a private TikTok login session."_s);
                        return S_OK;
                    }
                    options->put_ProfileName(L"TikTokLogin");
                    options->put_IsInPrivateModeEnabled(TRUE);
                    const auto created = env10->CreateCoreWebView2ControllerWithOptions(
                        reinterpret_cast<HWND>(weak->surface_->winId()),
                        options.Get(),
                        Callback<
                            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [weak, profile](HRESULT result,
                                            ICoreWebView2Controller *controller)
                                -> HRESULT {
                                if (SUCCEEDED(result) && controller)
                                    profile->observeExit();
                                if (!weak || weak->closed_)
                                {
                                    if (controller)
                                        controller->Close();
                                    return S_OK;
                                }
                                if (FAILED(result) || !controller)
                                {
                                    weak->error(
                                        u"TikTok login could not open. Try again."_s);
                                    return S_OK;
                                }
                                weak->controller_ = controller;
                                controller->get_CoreWebView2(&weak->web_);
                                if (!weak->web_)
                                {
                                    weak->error(
                                        u"TikTok login could not open. Try again."_s);
                                    return S_OK;
                                }
                                weak->resize();
                                weak->setVisible(false);
                                weak->configure();
                                const auto ready = weak->callbacks_.ready;
                                if (ready)
                                    ready();
                                return S_OK;
                            })
                            .Get());
                    if (FAILED(created))
                        weak->error(
                            u"TikTok login could not open. Try again."_s);
                    return S_OK;
                })
                .Get());
        if (FAILED(result))
            this->error(
                u"TikTok login could not start. Check that Microsoft Edge WebView2 is installed."_s);
    }

    void navigate(const QUrl &url) override
    {
        if (this->web_ && !this->closed_ && allowedNavigation(url))
        {
            const auto encoded = url.toString(QUrl::FullyEncoded);
            this->web_->Navigate(reinterpret_cast<LPCWSTR>(encoded.utf16()));
        }
    }

    void evaluate(const QString &script, ScriptCallback callback) override
    {
        if (!this->web_ || this->closed_)
        {
            callback({});
            return;
        }
        QPointer<WindowsLoginBrowser> weak(this);
        auto completion = std::make_shared<ScriptCallback>(std::move(callback));
        const auto result = this->web_->ExecuteScript(
            reinterpret_cast<LPCWSTR>(script.utf16()),
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [weak, completion](HRESULT result, LPCWSTR raw) -> HRESULT {
                    if (!weak || weak->closed_ || !*completion)
                        return S_OK;
                    QJsonObject document;
                    if (SUCCEEDED(result) && raw &&
                        wcsnlen(raw, 1048577) <= 1048576)
                        document = QJsonDocument::fromJson(
                                       QString::fromWCharArray(raw).toUtf8())
                                       .object();
                    std::exchange(*completion, {})(std::move(document));
                    return S_OK;
                })
                .Get());
        if (FAILED(result) && *completion)
            std::exchange(*completion, {})({});
    }

    void cookies(CookiesCallback callback) override
    {
        ComPtr<ICoreWebView2_2> web2;
        ComPtr<ICoreWebView2CookieManager> manager;
        if (!this->web_ || this->closed_ || FAILED(this->web_.As(&web2)) ||
            FAILED(web2->get_CookieManager(&manager)))
        {
            callback({});
            return;
        }
        QPointer<WindowsLoginBrowser> weak(this);
        auto completion =
            std::make_shared<CookiesCallback>(std::move(callback));
        const auto result = manager->GetCookies(
            nullptr,
            Callback<ICoreWebView2GetCookiesCompletedHandler>(
                [weak, completion](HRESULT result,
                                   ICoreWebView2CookieList *list) -> HRESULT {
                    if (!weak || weak->closed_ || !*completion)
                        return S_OK;
                    QList<QNetworkCookie> exported;
                    UINT count = 0;
                    if (SUCCEEDED(result) && list)
                        list->get_Count(&count);
                    for (UINT i = 0; i < count && count <= 512; ++i)
                    {
                        ComPtr<ICoreWebView2Cookie> cookie;
                        if (FAILED(list->GetValueAtIndex(i, &cookie)))
                            continue;
                        LPWSTR raw = nullptr;
                        cookie->get_Domain(&raw);
                        const auto domain = takeString(raw);
                        if (domain != u"tiktok.com" &&
                            !domain.endsWith(u".tiktok.com"))
                            continue;
                        cookie->get_Name(&raw);
                        auto name = takeString(raw).toLatin1();
                        cookie->get_Value(&raw);
                        QNetworkCookie saved(name, takeString(raw).toLatin1());
                        saved.setDomain(domain);
                        cookie->get_Path(&raw);
                        saved.setPath(takeString(raw));
                        BOOL flag = FALSE;
                        cookie->get_IsHttpOnly(&flag);
                        saved.setHttpOnly(flag);
                        cookie->get_IsSecure(&flag);
                        saved.setSecure(flag);
                        cookie->get_IsSession(&flag);
                        if (!flag)
                        {
                            double expires = 0;
                            cookie->get_Expires(&expires);
                            if (expires <= QDateTime::currentSecsSinceEpoch() ||
                                expires >= 253402300799.0)
                                continue;
                            saved.setExpirationDate(
                                QDateTime::fromSecsSinceEpoch(qint64(expires),
                                                              QTimeZone::UTC));
                        }
                        exported.push_back(std::move(saved));
                    }
                    std::exchange(*completion, {})(std::move(exported));
                    return S_OK;
                })
                .Get());
        if (FAILED(result) && *completion)
            std::exchange(*completion, {})({});
    }

    void setVisible(bool visible) override
    {
        if (this->controller_)
            this->controller_->put_IsVisible(visible);
    }
    void resize() override
    {
        if (!this->controller_ || !this->surface_)
            return;
        RECT bounds{};
        GetClientRect(reinterpret_cast<HWND>(this->surface_->winId()), &bounds);
        this->controller_->put_Bounds(bounds);
    }
    void close() override
    {
        if (std::exchange(this->closed_, true))
            return;
        this->callbacks_ = {};
        if (this->profile_)
            this->profile_->closed = true;
        if (this->controller_)
            this->controller_->Close();
        this->web_.Reset();
        this->controller_.Reset();
        this->profile_.reset();
    }

private:
    void error(const QString &message)
    {
        const auto callback = this->callbacks_.error;
        if (!this->closed_ && callback)
            callback(message);
    }
    void configure()
    {
        ComPtr<ICoreWebView2Settings> settings;
        this->web_->get_Settings(&settings);
        if (settings)
        {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            ComPtr<ICoreWebView2Settings4> settings4;
            if (SUCCEEDED(settings.As(&settings4)))
            {
                settings4->put_IsPasswordAutosaveEnabled(FALSE);
                settings4->put_IsGeneralAutofillEnabled(FALSE);
            }
        }
        ComPtr<ICoreWebView2_8> web8;
        if (SUCCEEDED(this->web_.As(&web8)))
            web8->put_IsMuted(TRUE);
        EventRegistrationToken token{};
        ComPtr<ICoreWebView2_4> web4;
        if (SUCCEEDED(this->web_.As(&web4)))
            web4->add_DownloadStarting(
                Callback<ICoreWebView2DownloadStartingEventHandler>(
                    [](ICoreWebView2 *,
                       ICoreWebView2DownloadStartingEventArgs *args)
                        -> HRESULT {
                        args->put_Cancel(TRUE);
                        args->put_Handled(TRUE);
                        return S_OK;
                    })
                    .Get(),
                &token);
        this->web_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2 *,
                   ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT {
                    args->put_Handled(TRUE);
                    return S_OK;
                })
                .Get(),
            &token);
        this->web_->add_PermissionRequested(
            Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [](ICoreWebView2 *,
                   ICoreWebView2PermissionRequestedEventArgs *args) -> HRESULT {
                    args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                    return S_OK;
                })
                .Get(),
            &token);
        this->web_->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2 *,
                   ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                    LPWSTR raw = nullptr;
                    args->get_Uri(&raw);
                    if (!TikTokLoginBrowser::allowedNavigation(
                            QUrl(takeString(raw))))
                        args->put_Cancel(TRUE);
                    return S_OK;
                })
                .Get(),
            &token);
        QPointer<WindowsLoginBrowser> weak(this);
        this->web_->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [weak](ICoreWebView2 *,
                       ICoreWebView2ProcessFailedEventArgs *) -> HRESULT {
                    if (weak && !weak->closed_)
                        weak->error(
                            u"TikTok login stopped responding. Close this window and try again."_s);
                    return S_OK;
                })
                .Get(),
            &token);
    }

    QPointer<QWidget> surface_;
    Callbacks callbacks_;
    std::shared_ptr<LoginProfile> profile_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> web_;
    HRESULT comResult_ = E_FAIL;
    bool closed_ = false;
};
}

std::unique_ptr<TikTokLoginBrowser> createPlatformTikTokLoginBrowser(
    QObject *parent)
{
    return std::make_unique<WindowsLoginBrowser>(parent);
}
}
