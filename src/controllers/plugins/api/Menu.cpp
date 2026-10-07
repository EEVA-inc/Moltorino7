#include "controllers/plugins/api/Menu.hpp"

#ifdef CHATTERINO_HAVE_PLUGINS

#    include "controllers/plugins/Plugin.hpp"
#    include "controllers/plugins/SignalCallback.hpp"
#    include "controllers/plugins/SolTypes.hpp"

#    include <QAction>
#    include <QMenu>
#    include <QPointer>
#    include <sol/sol.hpp>

#    include <utility>
#    include <variant>

namespace {

QAction *findAction(const QMenu &menu, const QString &name)
{
    for (auto *action : menu.actions())
    {
        if (action->text() == name)
        {
            return action;
        }
    }
    return nullptr;
}

QAction *findAction(const QMenu &menu, int oneBasedIndex)
{
    if (oneBasedIndex <= 0)
    {
        return nullptr;
    }
    const auto index = oneBasedIndex - 1;
    const auto actions = menu.actions();
    if (index >= actions.size())
    {
        return nullptr;
    }
    return actions.at(index);
}

QAction *findAction(const QMenu &menu,
                    const std::variant<QString, int> &position)
{
    return std::visit(
        [&](const auto &value) {
            return findAction(menu, value);
        },
        position);
}

void connectPluginAction(QAction *action, chatterino::Plugin &plugin,
                         sol::main_protected_function callback)
{
    auto guardedCallback =
        chatterino::lua::SignalCallback(plugin.weakRef(), std::move(callback));
    QObject::connect(action, &QAction::triggered, action,
                     [callback = std::move(guardedCallback)](auto &&...args) {
                         if (!callback.owner().strong())
                         {
                             return;
                         }
                         callback(std::forward<decltype(args)>(args)...);
                     });

    QPointer<QAction> guardedAction(action);
    auto unloadConnection =
        std::make_shared<boost::signals2::scoped_connection>(
            plugin.onUnloaded.connect([guardedAction] {
                if (guardedAction)
                {
                    guardedAction->setEnabled(false);
                }
            }));
    QObject::connect(action, &QObject::destroyed,
                     [unloadConnection](QObject *) {
                         unloadConnection->disconnect();
                     });
}

}

namespace chatterino::lua::api::menu {

void createUserType(sol::table &c2)
{
    c2.new_usertype<QMenu>(
        "Menu", sol::no_constructor, "add_action",
        [](QMenu &menu, const QString &name, ThisPluginState state,
           sol::main_protected_function callback) {
            auto *action = menu.addAction(name);
            connectPluginAction(action, *state.plugin(), std::move(callback));
        },
        "insert_action",
        [](QMenu &menu, const std::variant<QString, int> &before,
           const QString &name, ThisPluginState state,
           sol::main_protected_function callback) {
            auto *action = new QAction(name, &menu);
            connectPluginAction(action, *state.plugin(), std::move(callback));
            menu.insertAction(findAction(menu, before), action);
        },
        "add_menu",
        [](QMenu &menu, const QString &title) {
            return QPointer(menu.addMenu(title));
        },
        "insert_menu",
        [](QMenu &menu, const std::variant<QString, int> &before,
           const QString &title) {
            auto *submenu = new QMenu(title, &menu);
            menu.insertMenu(findAction(menu, before), submenu);
            return QPointer(submenu);
        },
        "add_separator",
        [](QMenu &menu) {
            menu.addSeparator();
        },
        "insert_separator",
        [](QMenu &menu, const std::variant<QString, int> &before) {
            menu.insertSeparator(findAction(menu, before));
        });
}

}

#endif
