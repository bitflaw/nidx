#include <unordered_map>
#include <string>
#include <variant>
#include <int.h>
#include <vector>

template <typename... Ts>
struct overloaded : Ts... { using Ts::operator()...; };

template<typename... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

struct JsonValue;

using JsonArray = std::vector<JsonValue>;
using JsonObject = std::unordered_map<std::string, JsonValue>;
using JsonValue_T = std::variant<std::monostate, std::string, i64, f32, bool, JsonObject, JsonArray>;

struct JsonValue
{
  JsonValue_T value;
};

inline std::string quote (std::string str) { return "\"" + str + "\""; }

inline void builder(JsonValue json_value, std::string& stringified)
{
  auto visitor = overloaded {
    [&](std::monostate)
    {
      stringified.append("null");
    },
    [&](std::string val)
    {
      stringified.append(quote(val));
    },
    [&](i64 val)
    {
      stringified.append(std::to_string(val));
    },
    [&](f32 val)
    {
      stringified.append(std::to_string(val));
    },
    [&](bool val)
    {
      stringified.append(val ? "true" : "false");
    },
    [&](JsonObject json_obj)
    {
      stringified.append("{");
      size_t count = 0;
      for (auto& [key, value] : json_obj)
      {
        if (count > 0) stringified.append(",");
        stringified.append(quote(key) + ":");
        builder(value, stringified);
        count++;
      }
      stringified.append("}");
    },
    [&](JsonArray json_arr)
    {
      stringified.append("[");
      size_t count = 0;
      for (JsonValue& val : json_arr)
      {
        if (count > 0) stringified.append(",");
        builder(val, stringified);
        count++;
      }
      stringified.append("]");
    }
  };

  std::visit(visitor, json_value.value);
}
