// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JASPERENGINE_HTTPJSON_H
#define JASPERENGINE_HTTPJSON_H

#include "json.hpp"

namespace jasperengine {

    // HttpResponse::json of a HttpResponseType::Json request: the body parsed on the worker
    // thread (LuaJson::parse rules: 512 levels at most)
    struct HttpJson {
        nlohmann::json value;
    };

}

#endif // JASPERENGINE_HTTPJSON_H
