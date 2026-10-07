#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include <sol/forward.hpp>

namespace chatterino {
class Plugin;
}

namespace chatterino::lua::api::images {

void createUserTypes(sol::table &c2);

}

#endif
