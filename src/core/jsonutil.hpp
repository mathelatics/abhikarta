// pi-agent: thin helpers over sheredom/json.h (which has NO find-member API)
#pragma once
#if defined(__GNUC__) || defined(__clang__)
#define json_weak __attribute__((unused))  // header-only use: silence weak-symbol attr
#endif
#include "json.h"
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace pi::ju {

struct Doc {                       // owns parse buffer
  void* mem = nullptr;
  json_value_s* root = nullptr;
  ~Doc() { free(mem); }
  bool parse(const std::string& s) {
    free(mem); mem = nullptr; root = nullptr;
    mem = json_parse(s.c_str(), s.size());
    root = mem ? static_cast<json_value_s*>(mem) : nullptr;
    return root != nullptr;
  }
};

inline json_value_s* member(json_value_s* obj, const char* key) {
  if (!obj || obj->type != json_type_object) return nullptr;
  auto* o = json_value_as_object(obj);
  for (auto* e = o->start; e; e = e->next)
    if (e->name && strcmp(e->name->string, key) == 0) return e->value;
  return nullptr;
}
inline std::string str(json_value_s* obj, const char* key, const std::string& def = "") {
  auto* v = member(obj, key);
  if (!v || v->type != json_type_string) return def;
  auto* s = json_value_as_string(v);
  return s ? std::string(s->string, s->string_size) : def;
}
inline long long num(json_value_s* obj, const char* key, long long def = 0) {
  auto* v = member(obj, key);
  if (!v || v->type != json_type_number) return def;
  auto* n = json_value_as_number(v);
  return n ? strtoll(n->number, nullptr, 10) : def;
}
inline json_array_s* arr(json_value_s* obj, const char* key) {
  auto* v = member(obj, key);
  return (v && v->type == json_type_array) ? json_value_as_array(v) : nullptr;
}
inline std::vector<std::string> str_array(json_value_s* obj, const char* key) {
  std::vector<std::string> out;
  auto* a = arr(obj, key);
  if (!a) return out;
  for (auto* e = a->start; e; e = e->next) {
    auto* v = e->value;
    if (v->type == json_type_string) {
      auto* s = json_value_as_string(v);
      out.emplace_back(s->string, s->string_size);
    } else { // nested object -> serialize its slice? keep raw via write on element value
      size_t sz = 0; void* w = json_write_minified(v, &sz);
      if (w) { out.emplace_back(static_cast<char*>(w), sz); free(w); }
    }
  }
  return out;
}

// ---- Builder: minimal string-based JSON writer with escaping ----
inline std::string esc(const std::string& in) {
  std::string o; o.reserve(in.size() + 8);
  for (unsigned char c : in) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
  }
  return o;
}
inline std::string q(const std::string& s) { return "\"" + esc(s) + "\""; }

} // namespace pi::ju
