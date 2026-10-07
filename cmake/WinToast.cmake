set(_wintoast_dir "${CMAKE_CURRENT_BINARY_DIR}/autogen/wintoast")
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/.." _wintoast_root)
file(MAKE_DIRECTORY "${_wintoast_dir}")
file(READ "${_wintoast_root}/lib/WinToast/src/wintoastlib.cpp" _wintoast_source)
file(READ "${_wintoast_root}/lib/WinToast/include/wintoastlib.h" _wintoast_header)

set(_wintoast_callback "[this, id]() { markAsReadyForDeletion(id); }")
string(FIND "${_wintoast_source}" "${_wintoast_callback}" _wintoast_callback_pos)
set(_wintoast_declaration "void markAsReadyForDeletion(_In_ INT64 id);")
string(FIND "${_wintoast_header}" "${_wintoast_declaration}" _wintoast_declaration_pos)
if(_wintoast_callback_pos LESS 0 OR _wintoast_declaration_pos LESS 0)
    message(FATAL_ERROR "WinToast changed; review the notification cleanup patch")
endif()
string(REPLACE "${_wintoast_callback}"
    "[this, id]() { queueMarkAsReadyForDeletion(id); }"
    _wintoast_source "${_wintoast_source}")
string(REPLACE "${_wintoast_declaration}"
    "${_wintoast_declaration}\n        void queueMarkAsReadyForDeletion(INT64 id);"
    _wintoast_header "${_wintoast_header}")
string(PREPEND _wintoast_source "#include <QCoreApplication>\n#include <QMetaObject>\n")
string(APPEND _wintoast_source [=[

void WinToastLib::WinToast::queueMarkAsReadyForDeletion(INT64 id) {
    if (auto *app = QCoreApplication::instance()) {
        QMetaObject::invokeMethod(app, [this, id] { markAsReadyForDeletion(id); },
                                  Qt::QueuedConnection);
    }
}
]=])
file(CONFIGURE OUTPUT "${_wintoast_dir}/wintoastlib.cpp" CONTENT "${_wintoast_source}" @ONLY)
file(CONFIGURE OUTPUT "${_wintoast_dir}/wintoastlib.h" CONTENT "${_wintoast_header}" @ONLY)
get_target_property(_wintoast_sources WinToast SOURCES)
list(REMOVE_ITEM _wintoast_sources "${_wintoast_root}/lib/WinToast/src/wintoastlib.cpp")
list(APPEND _wintoast_sources "${_wintoast_dir}/wintoastlib.cpp")
set_property(TARGET WinToast PROPERTY SOURCES "${_wintoast_sources}")
target_include_directories(WinToast BEFORE PUBLIC "${_wintoast_dir}")
target_link_libraries(WinToast Qt${MAJOR_QT_VERSION}::Core)
