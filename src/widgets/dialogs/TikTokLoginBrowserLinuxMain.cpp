#include "TikTokLoginBrowserLinux.hpp"

#include <fcntl.h>
#include <glib-unix.h>
#include <json-glib/json-glib.h>
#include <unistd.h>
#ifdef __linux__
#    include <sys/prctl.h>
#endif

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

extern "C" const chatterino::TikTokGtkApi *moltorinoTikTokGtkApi();

namespace {
constexpr std::size_t MAX_FRAME = 2 * 1024 * 1024;
const chatterino::TikTokGtkApi *api = nullptr;
void *browser = nullptr;
GMainLoop *loop = nullptr;
std::string input;

void stop()
{
    if (browser)
    {
        api->destroy(std::exchange(browser, nullptr));
    }
    g_main_loop_quit(loop);
}

void send(JsonObject *object)
{
    auto *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, object);
    auto *generator = json_generator_new();
    json_generator_set_root(generator, node);
    gsize length = 0;
    auto *data = json_generator_to_data(generator, &length);
    std::string frame;
    if (length <= MAX_FRAME)
    {
        frame.assign(data, length);
    }
    g_free(data);
    g_object_unref(generator);
    json_node_free(node);
    if (frame.empty())
    {
        stop();
        return;
    }
    frame += '\n';
    std::size_t offset = 0;
    while (offset < frame.size())
    {
        const auto written =
            write(STDOUT_FILENO, frame.data() + offset, frame.size() - offset);
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            stop();
            return;
        }
        offset += written;
    }
}

JsonObject *reply(const char *type, std::uint64_t id = 0)
{
    auto *object = json_object_new();
    json_object_set_string_member(object, "type", type);
    if (id)
    {
        json_object_set_string_member(object, "id", std::to_string(id).c_str());
    }
    return object;
}

const char *stringMember(JsonObject *object, const char *name)
{
    auto *node = json_object_get_member(object, name);
    return node && JSON_NODE_HOLDS_VALUE(node) &&
                   json_node_get_value_type(node) == G_TYPE_STRING
               ? json_node_get_string(node)
               : "";
}

void command(const std::string &frame)
{
    auto *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, frame.data(), frame.size(),
                                    nullptr) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    {
        g_object_unref(parser);
        stop();
        return;
    }
    auto *object = json_node_get_object(json_parser_get_root(parser));
    const std::string operation = stringMember(object, "op");
    const auto *idText = stringMember(object, "id");
    char *end = nullptr;
    const auto id = g_ascii_strtoull(idText, &end, 10);
    if (operation == "close")
    {
        stop();
    }
    else if (operation == "navigate")
    {
        api->navigate(browser, stringMember(object, "url"));
    }
    else if (operation == "visible")
    {
        api->setVisible(browser,
                        strcmp(stringMember(object, "value"), "yes") == 0);
    }
    else if (id && end && !*end && operation == "evaluate")
    {
        api->evaluate(browser, stringMember(object, "script"), id);
    }
    else if (id && end && !*end && operation == "cookies")
    {
        api->cookies(browser, id);
    }
    else
    {
        stop();
    }
    g_object_unref(parser);
}

gboolean readCommands(gint fd, GIOCondition condition, gpointer)
{
    char buffer[16384];
    while (browser)
    {
        const auto length = read(fd, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR)
        {
            continue;
        }
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            break;
        }
        if (length <= 0)
        {
            stop();
            return G_SOURCE_REMOVE;
        }
        input.append(buffer, length);
        std::size_t newline;
        while (browser && (newline = input.find('\n')) != std::string::npos)
        {
            if (newline > MAX_FRAME)
            {
                stop();
                return G_SOURCE_REMOVE;
            }
            auto frame = input.substr(0, newline);
            input.erase(0, newline + 1);
            command(frame);
        }
        if (input.size() > MAX_FRAME)
        {
            stop();
            return G_SOURCE_REMOVE;
        }
    }
    if (condition & (G_IO_HUP | G_IO_ERR))
    {
        stop();
    }
    return browser ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
}

int main()
{
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (getppid() == 1)
    {
        return 1;
    }
    loop = g_main_loop_new(nullptr, FALSE);
    api = moltorinoTikTokGtkApi();
    browser = api->create({
        nullptr,
        +[](void *, const char *) {
            send(reply("error"));
        },
        +[](void *, std::uint64_t id, const char *json) {
            auto *object = reply("script", id);
            auto *parser = json_parser_new();
            if (json_parser_load_from_data(parser, json, -1, nullptr) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
            {
                json_object_set_member(
                    object, "value",
                    json_node_copy(json_parser_get_root(parser)));
            }
            g_object_unref(parser);
            send(object);
        },
        +[](void *, std::uint64_t id,
            const chatterino::TikTokGtkCookie *cookies, std::size_t count) {
            auto *object = reply("cookies", id);
            auto *array = json_array_new();
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto &cookie = cookies[i];
                auto *item = json_object_new();
                json_object_set_string_member(item, "name", cookie.name);
                json_object_set_string_member(item, "value", cookie.value);
                json_object_set_string_member(item, "domain", cookie.domain);
                json_object_set_string_member(item, "path", cookie.path);
                json_object_set_int_member(item, "expires", cookie.expires);
                json_object_set_boolean_member(item, "secure", cookie.secure);
                json_object_set_boolean_member(item, "httpOnly",
                                               cookie.httpOnly);
                json_array_add_object_element(array, item);
            }
            json_object_set_array_member(object, "value", array);
            send(object);
        },
    });
    if (!browser)
    {
        send(reply("error"));
        g_main_loop_unref(loop);
        return 1;
    }
    fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
    g_unix_fd_add(STDIN_FILENO, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR),
                  readCommands, nullptr);
    g_unix_signal_add(
        SIGTERM,
        +[](gpointer) -> gboolean {
            stop();
            return G_SOURCE_REMOVE;
        },
        nullptr);
    send(reply("ready"));
    if (browser)
    {
        g_main_loop_run(loop);
    }
    stop();
    g_main_loop_unref(loop);
    return 0;
}
