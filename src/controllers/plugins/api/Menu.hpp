#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include <sol/forward.hpp>

namespace chatterino::lua::api::menu {

void createUserType(sol::table &c2);

}

#endif
