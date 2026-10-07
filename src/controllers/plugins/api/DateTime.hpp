#pragma once

#ifdef CHATTERINO_HAVE_PLUGINS

#    include <sol/forward.hpp>

namespace chatterino::lua::api::datetime {

void createUserTypes(sol::table &c2);

}

#endif
