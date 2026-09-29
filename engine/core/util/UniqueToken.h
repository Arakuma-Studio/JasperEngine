// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UNIQUETOKEN_H
#define UNIQUETOKEN_H

#include "Export.h"
#include <string>

namespace jasperengine {

    class JASPERENGINE_API UniqueToken {
    private:
        static int id;

        static std::string randString(const int len);

    public:
        static std::string get();

    };

}

#endif //UNIQUETOKEN_H
