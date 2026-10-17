// common/json.hpp — single include point for nlohmann::json across both targets.
// CMake FetchContent pulls nlohmann/json into third_party/.
#pragma once

#include <nlohmann/json.hpp>

namespace cheat { using json = nlohmann::json; }
