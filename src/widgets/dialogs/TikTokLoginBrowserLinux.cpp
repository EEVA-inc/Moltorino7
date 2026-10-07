#include "TikTokLoginBrowserLinux.hpp"

#include <webkit2/webkit2.h>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace chatterino {
namespace {

bool allowedNavigation(const char *uri)
{
    auto *url = g_uri_parse(uri, G_URI_FLAGS_NONE, nullptr);
    if (!url)
        return false;
    const auto *scheme = g_uri_get_scheme(url);
    const auto *host = g_uri_get_host(url);
    auto *lowerHost = host ? g_ascii_strdown(host, -1) : nullptr;
    const auto port = g_uri_get_port(url);
    const bool allowed = scheme && g_ascii_strcasecmp(scheme, "https") == 0 &&
                         !g_uri_get_userinfo(url) &&
                         (port == -1 || port == 443) && lowerHost &&
                         (strcmp(lowerHost, "tiktok.com") == 0 ||
                          g_str_has_suffix(lowerHost, ".tiktok.com"));
    g_free(lowerHost);
    g_uri_unref(url);
    return allowed;
}

struct State {
    TikTokGtkCallbacks callbacks{};
    GtkWidget *window = nullptr;
    WebKitWebView *web = nullptr;
    WebKitWebContext *context = nullptr;
    GCancellable *cancel = nullptr;
    bool closed = false;

    void error(const char *message) const
    {
        if (!this->closed && this->callbacks.error)
            this->callbacks.error(this->callbacks.owner, message);
    }

    void close()
    {
        if (std::exchange(this->closed, true))
            return;
        this->callbacks = {};
        if (this->cancel)
            g_cancellable_cancel(this->cancel);
        if (this->web)
        {
            g_signal_handlers_disconnect_by_data(this->web, this);
            webkit_web_view_stop_loading(this->web);
            webkit_web_view_terminate_web_process(this->web);
        }
        if (this->context)
            g_signal_handlers_disconnect_by_data(this->context, this);
        if (this->window)
        {
            g_signal_handlers_disconnect_by_data(this->window, this);
            gtk_widget_destroy(this->window);
            g_object_unref(this->window);
        }
        this->web = nullptr;
        this->window = nullptr;
        g_clear_object(&this->context);
        g_clear_object(&this->cancel);
    }
};

using Handle = std::shared_ptr<State>;
struct Request {
    Handle owner;
    std::uint64_t id;
};

void *create(TikTokGtkCallbacks callbacks)
{
    if (!gtk_init_check(nullptr, nullptr))
        return nullptr;
    auto state = std::make_shared<State>();
    auto *self = state.get();
    self->callbacks = callbacks;
    self->context = webkit_web_context_new_ephemeral();
    self->cancel = g_cancellable_new();
    self->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(self->window);
    gtk_window_set_title(GTK_WINDOW(self->window), "TikTok login");
    gtk_window_set_default_size(GTK_WINDOW(self->window), 570, 700);
    self->web =
        WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(self->context));
    gtk_container_add(GTK_CONTAINER(self->window), GTK_WIDGET(self->web));
    gtk_widget_set_size_request(GTK_WIDGET(self->web), 570, 660);
    gtk_widget_show(GTK_WIDGET(self->web));
    gtk_widget_realize(self->window);
    auto *settings = webkit_web_view_get_settings(self->web);
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings,
                                                                  FALSE);
    webkit_settings_set_media_playback_requires_user_gesture(settings, TRUE);
    webkit_web_view_set_is_muted(self->web, TRUE);
    webkit_web_context_set_cache_model(self->context,
                                       WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    g_signal_connect(
        self->window, "delete-event",
        G_CALLBACK(+[](GtkWidget *window, GdkEvent *, gpointer) -> gboolean {
            gtk_widget_hide(window);
            return TRUE;
        }),
        self);
    g_signal_connect(
        self->web, "decide-policy",
        G_CALLBACK(+[](WebKitWebView *, WebKitPolicyDecision *decision,
                       WebKitPolicyDecisionType type, gpointer) -> gboolean {
            if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
            {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
            {
                auto *action =
                    webkit_navigation_policy_decision_get_navigation_action(
                        WEBKIT_NAVIGATION_POLICY_DECISION(decision));
                auto *request = webkit_navigation_action_get_request(action);
                if (!allowedNavigation(webkit_uri_request_get_uri(request)))
                {
                    webkit_policy_decision_ignore(decision);
                    return TRUE;
                }
            }
            if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE &&
                !webkit_response_policy_decision_is_mime_type_supported(
                    WEBKIT_RESPONSE_POLICY_DECISION(decision)))
            {
                webkit_policy_decision_ignore(decision);
                return TRUE;
            }
            return FALSE;
        }),
        self);
    g_signal_connect(
        self->web, "permission-request",
        G_CALLBACK(+[](WebKitWebView *, WebKitPermissionRequest *request,
                       gpointer) -> gboolean {
            webkit_permission_request_deny(request);
            return TRUE;
        }),
        self);
    g_signal_connect(
        self->web, "context-menu",
        G_CALLBACK(+[](WebKitWebView *, WebKitContextMenu *, GdkEvent *,
                       WebKitHitTestResult *, gpointer) -> gboolean {
            return TRUE;
        }),
        self);
    g_signal_connect(
        self->web, "run-file-chooser",
        G_CALLBACK(+[](WebKitWebView *, WebKitFileChooserRequest *request,
                       gpointer) -> gboolean {
            webkit_file_chooser_request_cancel(request);
            return TRUE;
        }),
        self);
    g_signal_connect(
        self->context, "download-started",
        G_CALLBACK(+[](WebKitWebContext *, WebKitDownload *download, gpointer) {
            webkit_download_cancel(download);
        }),
        self);
    g_signal_connect(
        self->web, "web-process-terminated",
        G_CALLBACK(+[](WebKitWebView *, WebKitWebProcessTerminationReason,
                       gpointer data) {
            static_cast<State *>(data)->error(
                "TikTok login stopped responding. Close this window and try "
                "again.");
        }),
        self);
    return new Handle(std::move(state));
}

void destroy(void *handle)
{
    std::unique_ptr<Handle> owner(static_cast<Handle *>(handle));
    (*owner)->close();
}

void navigate(void *handle, const char *url)
{
    const auto &self = *static_cast<Handle *>(handle);
    if (!self->closed && allowedNavigation(url))
        webkit_web_view_load_uri(self->web, url);
}

void evaluate(void *handle, const char *script, std::uint64_t id)
{
    const auto &self = *static_cast<Handle *>(handle);
    auto *request = new Request{self, id};
    webkit_web_view_evaluate_javascript(
        self->web, script, -1, nullptr, nullptr, self->cancel,
        +[](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Request> request(static_cast<Request *>(data));
            GError *error = nullptr;
            auto *value = webkit_web_view_evaluate_javascript_finish(
                WEBKIT_WEB_VIEW(source), result, &error);
            auto *json = value ? jsc_value_to_json(value, 0) : nullptr;
            const auto &self = request->owner;
            if (!self->closed && self->callbacks.script)
                self->callbacks.script(
                    self->callbacks.owner, request->id,
                    json && strnlen(json, 1048577) <= 1048576 ? json : "{}");
            g_free(json);
            g_clear_object(&value);
            g_clear_error(&error);
        },
        request);
}

void cookies(void *handle, std::uint64_t id)
{
    const auto &self = *static_cast<Handle *>(handle);
    auto *request = new Request{self, id};
    auto *manager = webkit_web_context_get_cookie_manager(self->context);
    webkit_cookie_manager_get_all_cookies(
        manager, self->cancel,
        +[](GObject *source, GAsyncResult *result, gpointer data) {
            std::unique_ptr<Request> request(static_cast<Request *>(data));
            GError *error = nullptr;
            auto *cookies = webkit_cookie_manager_get_all_cookies_finish(
                WEBKIT_COOKIE_MANAGER(source), result, &error);
            std::vector<TikTokGtkCookie> exported;
            if (g_list_length(cookies) <= 512)
                for (auto *item = cookies; item; item = item->next)
                {
                    auto *cookie = static_cast<SoupCookie *>(item->data);
                    const auto *domain = soup_cookie_get_domain(cookie);
                    if (strcmp(domain, "tiktok.com") != 0 &&
                        !g_str_has_suffix(domain, ".tiktok.com"))
                        continue;
                    auto *expires = soup_cookie_get_expires(cookie);
                    const auto seconds =
                        expires ? g_date_time_to_unix(expires) : 0;
                    if (expires &&
                        (seconds <= g_get_real_time() / G_USEC_PER_SEC ||
                         seconds >= 253402300799LL))
                        continue;
                    exported.push_back({soup_cookie_get_name(cookie),
                                        soup_cookie_get_value(cookie), domain,
                                        soup_cookie_get_path(cookie), seconds,
                                        !!soup_cookie_get_secure(cookie),
                                        !!soup_cookie_get_http_only(cookie)});
                }
            const auto &self = request->owner;
            if (!self->closed && self->callbacks.cookies)
                self->callbacks.cookies(self->callbacks.owner, request->id,
                                        exported.data(), exported.size());
            g_list_free_full(
                cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
            g_clear_error(&error);
        },
        request);
}

void setVisible(void *handle, bool visible)
{
    const auto &self = *static_cast<Handle *>(handle);
    if (visible)
        gtk_window_present(GTK_WINDOW(self->window));
    else
        gtk_widget_hide(self->window);
}

}
}

extern "C" __attribute__((visibility("default")))
const chatterino::TikTokGtkApi *
    moltorinoTikTokGtkApi()
{
    using namespace chatterino;
    static const TikTokGtkApi api{1, create, destroy, navigate,
                                evaluate, cookies, setVisible};
    return &api;
}
