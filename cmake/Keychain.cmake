find_package(keychain 1.3.0 QUIET CONFIG)
if(TARGET keychain::keychain)
    return()
endif()

function(_moltorino_fetch_keychain)
    set(BUILD_TESTS OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(CODE_COVERAGE OFF)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    include(FetchContent)
    set(keychain_options)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
        list(APPEND keychain_options SYSTEM)
    endif()
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
        list(APPEND keychain_options EXCLUDE_FROM_ALL)
    endif()
    FetchContent_Declare(keychain
        URL https://github.com/hrantzsch/keychain/archive/refs/tags/v1.3.0.tar.gz
        URL_HASH SHA256=0e2eb3c6ca2c62253f7d28a478d0cb3eeb4b9656b33d2946e1a294361f72809c
        ${keychain_options}
    )
    FetchContent_MakeAvailable(keychain)
    add_library(keychain::keychain ALIAS keychain)
endfunction()

_moltorino_fetch_keychain()
