#pragma once

#include <cstddef>
#include <cstdint>

namespace chatterino {

struct TikTokGtkCookie {
    const char *name;
    const char *value;
    const char *domain;
    const char *path;
    std::int64_t expires;
    bool secure;
    bool httpOnly;
};

struct TikTokGtkCallbacks {
    void *owner;
    void (*error)(void *, const char *);
    void (*script)(void *, std::uint64_t, const char *);
    void (*cookies)(void *, std::uint64_t, const TikTokGtkCookie *,
                    std::size_t);
};

struct TikTokGtkApi {
    std::uint32_t version;
    void *(*create)(TikTokGtkCallbacks);
    void (*destroy)(void *);
    void (*navigate)(void *, const char *);
    void (*evaluate)(void *, const char *, std::uint64_t);
    void (*cookies)(void *, std::uint64_t);
    void (*setVisible)(void *, bool);
};

}
