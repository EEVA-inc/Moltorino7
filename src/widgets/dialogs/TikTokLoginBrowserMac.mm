#include "TikTokLoginBrowser.hpp"

#import <Cocoa/Cocoa.h>
#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>
#include <QTimeZone>
#include <QWidget>
#import <WebKit/WebKit.h>

#include <utility>

@interface MoltorinoTikTokNavigation
    : NSObject <WKNavigationDelegate, WKUIDelegate>
@property(nonatomic, copy) void (^failed)(void);
- (void)webView:(WKWebView *)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
                          initiatedByFrame:(WKFrameInfo *)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:
                               (void (^)(WKPermissionDecision))decisionHandler
    API_AVAILABLE(macos(12.0));
@end

@implementation MoltorinoTikTokNavigation
- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:
                        (void (^)(WKNavigationActionPolicy))decisionHandler
{
    if (!action.targetFrame)
    {
        decisionHandler(WKNavigationActionPolicyCancel);
        return;
    }
    const auto url =
        QUrl(QString::fromNSString(action.request.URL.absoluteString));
    decisionHandler(
        !action.targetFrame.mainFrame ||
                chatterino::TikTokLoginBrowser::allowedNavigation(url)
            ? WKNavigationActionPolicyAllow
            : WKNavigationActionPolicyCancel);
}
- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse *)response
                      decisionHandler:
                          (void (^)(WKNavigationResponsePolicy))decisionHandler
{
    decisionHandler(response.canShowMIMEType
                        ? WKNavigationResponsePolicyAllow
                        : WKNavigationResponsePolicyCancel);
}
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)action
                    windowFeatures:(WKWindowFeatures *)features
{
    return nil;
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView
{
    if (self.failed)
        self.failed();
}
- (void)webView:(WKWebView *)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin *)origin
                          initiatedByFrame:(WKFrameInfo *)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:
                               (void (^)(WKPermissionDecision))decisionHandler
{
    decisionHandler(WKPermissionDecisionDeny);
}
- (void)webView:(WKWebView *)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters
              initiatedByFrame:(WKFrameInfo *)frame
             completionHandler:(void (^)(NSArray<NSURL *> *))completionHandler
{
    completionHandler(nil);
}
@end

namespace chatterino {
using namespace Qt::Literals::StringLiterals;

namespace {
class MacLoginBrowser final : public TikTokLoginBrowser
{
public:
    using TikTokLoginBrowser::TikTokLoginBrowser;
    ~MacLoginBrowser() override
    {
        this->close();
    }

    void start(QWidget *surface, Callbacks callbacks) override
    {
        this->surface_ = surface;
        this->callbacks_ = std::move(callbacks);
        auto *configuration = [[WKWebViewConfiguration alloc] init];
        configuration.websiteDataStore =
            [WKWebsiteDataStore nonPersistentDataStore];
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = NO;
        configuration.mediaTypesRequiringUserActionForPlayback =
            WKAudiovisualMediaTypeAll;
        configuration.allowsAirPlayForMediaPlayback = NO;
        this->web_ = [[WKWebView alloc]
            initWithFrame:NSMakeRect(0, 0, surface->width(), surface->height())
            configuration:configuration];
        this->delegate_ = [[MoltorinoTikTokNavigation alloc] init];
        QPointer<MacLoginBrowser> weak(this);
        this->delegate_.failed = ^{
          if (weak && !weak->closed_)
          {
              const auto callback = weak->callbacks_.error;
              if (callback)
                  callback(
                      u"TikTok login stopped responding. Close this window and try again."_s);
          }
        };
        this->web_.navigationDelegate = this->delegate_;
        this->web_.UIDelegate = this->delegate_;
        this->web_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        this->web_.hidden = YES;
        auto *host = (__bridge NSView *)reinterpret_cast<void *>(surface->winId());
        [host addSubview:this->web_];
        const auto ready = this->callbacks_.ready;
        if (ready)
            ready();
    }

    void navigate(const QUrl &url) override
    {
        if (this->closed_ || !this->web_ || !allowedNavigation(url))
            return;
        auto *destination =
            [NSURL URLWithString:url.toString(QUrl::FullyEncoded).toNSString()];
        [this->web_ loadRequest:[NSURLRequest requestWithURL:destination]];
    }

    void evaluate(const QString &script, ScriptCallback callback) override
    {
        if (this->closed_ || !this->web_)
        {
            callback({});
            return;
        }
        QPointer<MacLoginBrowser> weak(this);
        [this->web_
            evaluateJavaScript:script.toNSString()
             completionHandler:^(id value, NSError *error) {
               if (!weak || weak->closed_)
                   return;
               QJsonObject object;
               if (!error && value &&
                   [NSJSONSerialization isValidJSONObject:value])
               {
                   NSData *data =
                       [NSJSONSerialization dataWithJSONObject:value
                                                       options:0
                                                         error:nullptr];
                   if (data.length <= 1048576)
                       object =
                           QJsonDocument::fromJson(
                               QByteArray(static_cast<const char *>(data.bytes),
                                          data.length))
                               .object();
               }
               callback(std::move(object));
             }];
    }

    void cookies(CookiesCallback callback) override
    {
        if (this->closed_ || !this->web_)
        {
            callback({});
            return;
        }
        QPointer<MacLoginBrowser> weak(this);
        [this->web_.configuration.websiteDataStore.httpCookieStore
            getAllCookies:^(NSArray<NSHTTPCookie *> *cookies) {
              if (!weak || weak->closed_)
                  return;
              QList<QNetworkCookie> exported;
              if (cookies.count <= 512)
                  for (NSHTTPCookie *cookie in cookies)
                  {
                      const auto domain = QString::fromNSString(cookie.domain);
                      if (domain != u"tiktok.com" &&
                          !domain.endsWith(u".tiktok.com"))
                          continue;
                      QNetworkCookie saved(
                          QString::fromNSString(cookie.name).toLatin1(),
                          QString::fromNSString(cookie.value).toLatin1());
                      saved.setDomain(domain);
                      saved.setPath(QString::fromNSString(cookie.path));
                      saved.setSecure(cookie.secure);
                      saved.setHttpOnly(cookie.HTTPOnly);
                      if (cookie.expiresDate)
                      {
                          const auto seconds =
                              cookie.expiresDate.timeIntervalSince1970;
                          if (seconds <= QDateTime::currentSecsSinceEpoch() ||
                              seconds >= 253402300799.0)
                              continue;
                          saved.setExpirationDate(QDateTime::fromSecsSinceEpoch(
                              qint64(seconds), QTimeZone::UTC));
                      }
                      exported.push_back(std::move(saved));
                  }
              callback(std::move(exported));
            }];
    }

    void setVisible(bool visible) override
    {
        this->web_.hidden = !visible;
    }
    void resize() override
    {
        if (this->web_ && this->surface_)
            this->web_.frame = NSMakeRect(0, 0, this->surface_->width(),
                                          this->surface_->height());
    }
    void close() override
    {
        if (std::exchange(this->closed_, true))
            return;
        this->callbacks_ = {};
        this->delegate_.failed = nil;
        this->web_.navigationDelegate = nil;
        this->web_.UIDelegate = nil;
        [this->web_ stopLoading];
        [this->web_ removeFromSuperview];
        this->web_ = nil;
        this->delegate_ = nil;
    }

private:
    QPointer<QWidget> surface_;
    Callbacks callbacks_;
    WKWebView *__strong web_ = nil;
    MoltorinoTikTokNavigation *__strong delegate_ = nil;
    bool closed_ = false;
};
}

std::unique_ptr<TikTokLoginBrowser> createPlatformTikTokLoginBrowser(
    QObject *parent)
{
    return std::make_unique<MacLoginBrowser>(parent);
}
}
