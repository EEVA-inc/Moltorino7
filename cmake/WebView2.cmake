if(NOT WIN32)
    return()
endif()

include(FetchContent)
FetchContent_Declare(moltorino_webview2
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4191.47/microsoft.web.webview2.1.0.4191.47.nupkg
    URL_HASH SHA256=f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0
    DOWNLOAD_NAME webview2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(moltorino_webview2)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$" OR
   CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$")
    set(_moltorino_webview2_arch arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_moltorino_webview2_arch x64)
else()
    set(_moltorino_webview2_arch x86)
endif()

add_library(MoltorinoWebView2 STATIC IMPORTED GLOBAL)
set_target_properties(MoltorinoWebView2 PROPERTIES
    IMPORTED_LOCATION "${moltorino_webview2_SOURCE_DIR}/build/native/${_moltorino_webview2_arch}/WebView2LoaderStatic.lib"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${moltorino_webview2_SOURCE_DIR}/build/native/include"
    INTERFACE_INCLUDE_DIRECTORIES "${moltorino_webview2_SOURCE_DIR}/build/native/include"
    INTERFACE_LINK_LIBRARIES "ole32;shlwapi;version;advapi32;user32;shell32"
)
set(MOLTORINO_WEBVIEW2_LICENSE "${moltorino_webview2_SOURCE_DIR}/LICENSE.txt")
