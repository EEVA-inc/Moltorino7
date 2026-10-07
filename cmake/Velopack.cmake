function(_moltorino_clear_stale_velopack_outputs)
    foreach(_moltorino_stale_file IN ITEMS
            moltorino-build-identity.json
            velopack_libc.dll
            velopack_libc_osx.dylib
            velopack_libc_linux_x64_gnu.so
        )
        file(REMOVE "${CMAKE_BINARY_DIR}/bin/${_moltorino_stale_file}")
    endforeach()
endfunction()

if(NOT MOLTORINO_VELOPACK)
    _moltorino_clear_stale_velopack_outputs()
    return()
endif()

if(NOT WIN32 AND NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS
        "Velopack is unavailable for '${CMAKE_SYSTEM_NAME}'; disabling the "
        "seamless updater for this build."
    )
    _moltorino_clear_stale_velopack_outputs()
    return()
endif()

set(MOLTORINO_VELOPACK_VERSION "1.2.0")
set(MOLTORINO_VELOPACK_ARCHIVE_SHA256
    "547262ed7a1ab1ff62f580aa53851ede2f1a451ac61b8974eb7bc01117488835"
)
set(MOLTORINO_VELOPACK_ROOT "" CACHE PATH
    "Optional path to an extracted velopack_libc_1.2.0 archive"
)

if(WIN32)
    if(NOT MSVC)
        message(WARNING
            "Moltorino's Windows Velopack integration requires MSVC. "
            "The seamless updater will be disabled for this build."
        )
        _moltorino_clear_stale_velopack_outputs()
        return()
    endif()
    string(TOUPPER "${MSVC_CXX_ARCHITECTURE_ID}" _moltorino_msvc_arch)
    if(
        (_moltorino_msvc_arch AND NOT _moltorino_msvc_arch STREQUAL "X64") OR
        (NOT _moltorino_msvc_arch AND
         NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    )
        message(WARNING
            "Moltorino publishes only the win-x64 updater feed. Disabling "
            "Velopack for '${MSVC_CXX_ARCHITECTURE_ID}' "
            "('${CMAKE_SYSTEM_PROCESSOR}')."
        )
        _moltorino_clear_stale_velopack_outputs()
        return()
    endif()
    set(MOLTORINO_VELOPACK_PLATFORM "windows")
    set(MOLTORINO_VELOPACK_RUNTIME_ID "win-x64")
    set(_moltorino_velopack_runtime_name
        "velopack_libc_win_x64_msvc.dll")
    set(_moltorino_velopack_runtime_hash
        "c36d8b984639a8af9d3397088d3ffb8213fe1bd0917f555cf0c6e33f014403ec")
    set(_moltorino_velopack_implib_name
        "velopack_libc_win_x64_msvc.dll.lib")
    set(_moltorino_velopack_implib_hash
        "063def3f77ccdd44fd719536b9cb464015cc34e3c10197add6643aec02408d11")
    set(_moltorino_velopack_staged_name "velopack_libc.dll")
elseif(APPLE)
    set(_moltorino_macos_architectures ${CMAKE_OSX_ARCHITECTURES})
    list(REMOVE_DUPLICATES _moltorino_macos_architectures)
    list(SORT _moltorino_macos_architectures)
    if(NOT _moltorino_macos_architectures STREQUAL "arm64;x86_64")
        message(WARNING
            "Moltorino publishes one universal macOS updater feed. Disabling "
            "Velopack because CMAKE_OSX_ARCHITECTURES is "
            "'${CMAKE_OSX_ARCHITECTURES}', not 'x86_64;arm64'."
        )
        _moltorino_clear_stale_velopack_outputs()
        return()
    endif()
    set(MOLTORINO_VELOPACK_PLATFORM "macos")
    set(MOLTORINO_VELOPACK_RUNTIME_ID "osx-universal")
    set(_moltorino_velopack_runtime_name "velopack_libc_osx.dylib")
    set(_moltorino_velopack_runtime_hash
        "c16af949a9c86d57d52cf0a98ff4453dd17132fd216c05f9f5178ee561a78173")
    set(_moltorino_velopack_staged_name "velopack_libc_osx.dylib")
else()
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        message(WARNING
            "Moltorino publishes only the linux-x64 updater feed. Disabling "
            "Velopack for '${CMAKE_SYSTEM_PROCESSOR}'."
        )
        _moltorino_clear_stale_velopack_outputs()
        return()
    endif()
    set(MOLTORINO_VELOPACK_PLATFORM "linux")
    set(MOLTORINO_VELOPACK_RUNTIME_ID "linux-x64")
    set(_moltorino_velopack_runtime_name
        "velopack_libc_linux_x64_gnu.a")
    set(_moltorino_velopack_runtime_dir "lib-static")
    set(_moltorino_velopack_runtime_hash
        "8cbed3d7504e96216bc5170d3b49a0e0d0d2ee7686725da549045b9eec0f1f31")
endif()

if(MOLTORINO_VELOPACK_ROOT)
    cmake_path(ABSOLUTE_PATH MOLTORINO_VELOPACK_ROOT
        BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
        NORMALIZE
        OUTPUT_VARIABLE _moltorino_velopack_root
    )
else()
    set(_moltorino_velopack_root
        "${CMAKE_BINARY_DIR}/_deps/moltorino_velopack-src")
    include(FetchContent)
    FetchContent_Declare(moltorino_velopack
        URL "https://github.com/velopack/velopack/releases/download/${MOLTORINO_VELOPACK_VERSION}/velopack_libc_${MOLTORINO_VELOPACK_VERSION}.zip"
        URL_HASH "SHA256=${MOLTORINO_VELOPACK_ARCHIVE_SHA256}"
        TLS_VERIFY ON
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_DIR "${_moltorino_velopack_root}"
    )
    FetchContent_MakeAvailable(moltorino_velopack)
endif()

set(_moltorino_velopack_include_dir
    "${_moltorino_velopack_root}/include")
if(NOT _moltorino_velopack_runtime_dir)
    set(_moltorino_velopack_runtime_dir "lib")
endif()
set(_moltorino_velopack_runtime
    "${_moltorino_velopack_root}/${_moltorino_velopack_runtime_dir}/${_moltorino_velopack_runtime_name}")
if(_moltorino_velopack_implib_name)
    set(_moltorino_velopack_implib
        "${_moltorino_velopack_root}/lib/${_moltorino_velopack_implib_name}")
endif()
set(MOLTORINO_VELOPACK_LICENSE_FILE
    "${CMAKE_SOURCE_DIR}/resources/licenses/velopack.txt")

set(_moltorino_required_files
    "${_moltorino_velopack_include_dir}/Velopack.h"
    "${_moltorino_velopack_include_dir}/Velopack.hpp"
    "${_moltorino_velopack_runtime}"
    "${MOLTORINO_VELOPACK_LICENSE_FILE}"
)
if(_moltorino_velopack_implib)
    list(APPEND _moltorino_required_files "${_moltorino_velopack_implib}")
endif()
foreach(_moltorino_required_file IN LISTS _moltorino_required_files)
    if(NOT EXISTS "${_moltorino_required_file}")
        message(FATAL_ERROR
            "Velopack ${MOLTORINO_VELOPACK_VERSION} is incomplete: missing "
            "'${_moltorino_required_file}'."
        )
    endif()
endforeach()

set(_moltorino_pinned_sdk_files
    "${_moltorino_velopack_include_dir}/Velopack.h|e6711fcc565386ef6c4e80079c44b6f3221a98e5c43b33a9331311a28b54c8fc"
    "${_moltorino_velopack_include_dir}/Velopack.hpp|16cfdf96c48360b7d76ba06dd941d464e3427fa7aca2f84626c6ee3af76b14c9"
    "${_moltorino_velopack_runtime}|${_moltorino_velopack_runtime_hash}"
)
if(_moltorino_velopack_implib)
    list(APPEND _moltorino_pinned_sdk_files
        "${_moltorino_velopack_implib}|${_moltorino_velopack_implib_hash}")
endif()
foreach(_moltorino_pinned_sdk_file IN LISTS _moltorino_pinned_sdk_files)
    string(REPLACE "|" ";" _moltorino_pinned_sdk_parts
        "${_moltorino_pinned_sdk_file}")
    list(GET _moltorino_pinned_sdk_parts 0 _moltorino_pinned_sdk_path)
    list(GET _moltorino_pinned_sdk_parts 1 _moltorino_pinned_sdk_hash)
    file(SHA256 "${_moltorino_pinned_sdk_path}"
        _moltorino_actual_sdk_hash)
    if(NOT _moltorino_actual_sdk_hash STREQUAL _moltorino_pinned_sdk_hash)
        message(FATAL_ERROR
            "Velopack SDK file does not match the pinned release: "
            "${_moltorino_pinned_sdk_path}"
        )
    endif()
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(Velopack::Velopack STATIC IMPORTED GLOBAL)
    set_target_properties(Velopack::Velopack PROPERTIES
        IMPORTED_LOCATION "${_moltorino_velopack_runtime}"
        INTERFACE_INCLUDE_DIRECTORIES "${_moltorino_velopack_include_dir}"
        INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS};pthread;rt;m"
        INTERFACE_LINK_DEPENDS "${_moltorino_velopack_runtime}"
    )
else()
    add_library(Velopack::Velopack SHARED IMPORTED GLOBAL)
    set_target_properties(Velopack::Velopack PROPERTIES
        IMPORTED_LOCATION "${_moltorino_velopack_runtime}"
        INTERFACE_INCLUDE_DIRECTORIES "${_moltorino_velopack_include_dir}"
    )
endif()
if(_moltorino_velopack_implib)
    set_target_properties(Velopack::Velopack PROPERTIES
        IMPORTED_IMPLIB "${_moltorino_velopack_implib}")
endif()

set(MOLTORINO_VELOPACK_LICENSE_OUTPUT
    "${CMAKE_BINARY_DIR}/bin/licenses/Velopack.txt")
set(MOLTORINO_VELOPACK_RUNTIME_OUTPUT "")
if(_moltorino_velopack_staged_name)
    set(MOLTORINO_VELOPACK_RUNTIME_OUTPUT
        "${CMAKE_BINARY_DIR}/bin/${_moltorino_velopack_staged_name}")
    add_custom_command(
        OUTPUT
            "${MOLTORINO_VELOPACK_RUNTIME_OUTPUT}"
            "${MOLTORINO_VELOPACK_LICENSE_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/bin/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_moltorino_velopack_runtime}"
            "${MOLTORINO_VELOPACK_RUNTIME_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MOLTORINO_VELOPACK_LICENSE_FILE}"
            "${MOLTORINO_VELOPACK_LICENSE_OUTPUT}"
        DEPENDS
            "${_moltorino_velopack_runtime}"
            "${MOLTORINO_VELOPACK_LICENSE_FILE}"
        COMMENT "Staging Velopack ${MOLTORINO_VELOPACK_VERSION} runtime"
        VERBATIM
    )
    set(_moltorino_velopack_stage_outputs
        "${MOLTORINO_VELOPACK_RUNTIME_OUTPUT}"
        "${MOLTORINO_VELOPACK_LICENSE_OUTPUT}"
    )
else()
    add_custom_command(
        OUTPUT "${MOLTORINO_VELOPACK_LICENSE_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/bin/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${MOLTORINO_VELOPACK_LICENSE_FILE}"
            "${MOLTORINO_VELOPACK_LICENSE_OUTPUT}"
        DEPENDS "${MOLTORINO_VELOPACK_LICENSE_FILE}"
        COMMENT "Staging Velopack ${MOLTORINO_VELOPACK_VERSION} license"
        VERBATIM
    )
    set(_moltorino_velopack_stage_outputs
        "${MOLTORINO_VELOPACK_LICENSE_OUTPUT}"
    )
endif()
add_custom_target(moltorino-velopack-runtime
    DEPENDS ${_moltorino_velopack_stage_outputs})

message(STATUS
    "Velopack ${MOLTORINO_VELOPACK_VERSION}: enabled "
    "(${MOLTORINO_VELOPACK_RUNTIME_ID}, ${_moltorino_velopack_root})"
)
