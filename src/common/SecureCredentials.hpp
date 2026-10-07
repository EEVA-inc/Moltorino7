#pragma once

#include "util/Expected.hpp"

#include <QString>

#include <functional>

namespace chatterino {

class SecureCredentials
{
public:
    using ReadCallback = std::function<void(ExpectedStr<QString>)>;
    using WriteCallback = std::function<void(ExpectedStr<void>)>;

    static void read(const QString &key, ReadCallback callback);
    static void write(const QString &key, const QString &secret,
                      WriteCallback callback);
    static void remove(const QString &key, WriteCallback callback);
};

}
