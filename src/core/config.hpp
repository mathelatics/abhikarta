// pi-agent: provider profiles — named logins for OpenAI-compatible endpoints
// (openai / nvidia / huggingface / ollama / lmstudio / llama.cpp / custom).
// Stored at ~/.pi/config.json with chmod 600; keys are never logged or echoed.
#pragma once
#include "jsonutil.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <map>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace pi {

struct ProviderProfile {
  std::string base_url;
  std::string api_key;
  std::string model;
  std::vector<std::string> models;   // cached from GET /v1/models
};

class PiConfig {
public:
  std::string path;
  std::string active;                                   // active profile name
  std::map<std::string, ProviderProfile> profiles;

  static std::string default_path() {
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.pi/config.json";
  }

  static std::string home_dir() {
    const char* h = getenv("HOME");
    return h ? h : ".";
  }

  // Known provider presets (base URL conventions verified against docs).
  static ProviderProfile preset(const std::string& name) {
    ProviderProfile p;
    if (name == "openai")             p.base_url = "https://api.openai.com/v1";
    else if (name == "nvidia")        p.base_url = "https://integrate.api.nvidia.com/v1";
    else if (name == "hf" || name == "huggingface")
                                       p.base_url = "https://router.huggingface.co/v1";
    else if (name == "ollama")        p.base_url = "http://localhost:11434/v1";
    else if (name == "lmstudio")      p.base_url = "http://localhost:1234/v1";
    else if (name == "llamacpp" || name == "llama.cpp")
                                       p.base_url = "http://localhost:8080/v1";
    return p;   // unknown name -> empty base_url ("custom", ask user)
  }

  static bool is_local(const std::string& base_url) {
    return base_url.find("localhost") != std::string::npos ||
           base_url.find("127.0.0.1") != std::string::npos;
  }

  static std::string mask_key(const std::string& key) {
    if (key.empty()) return "(none)";
    if (key.size() <= 8) return std::string(key.size(), '*');
    return std::string(key.size() - 4, '*') + key.substr(key.size() - 4);
  }

  void load(const std::string& p = default_path()) {
    path = p;
    std::ifstream f(path);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    ju::Doc d; if (!d.parse(ss.str())) return;
    active = ju::str(d.root, "active");
    auto* provs = ju::member(d.root, "providers");
    if (!provs || provs->type != json_type_object) return;
    for (auto* e = json_value_as_object(provs)->start; e; e = e->next) {
      if (!e->name) continue;
      ProviderProfile p;
      p.base_url = ju::str(e->value, "base_url");
      p.api_key  = ju::str(e->value, "api_key");
      p.model    = ju::str(e->value, "model");
      p.models   = ju::str_array(e->value, "models");
      profiles[std::string(e->name->string, e->name->string_size)] = p;
    }
  }

  bool save(const std::string& p = "") {
    if (!p.empty()) path = p;
    if (path.empty()) path = default_path();
    // ~/.pi must exist (0700)
    std::string dir = path.substr(0, path.find_last_of('/'));
    mkdir(dir.c_str(), 0700);
    chmod(dir.c_str(), 0700);   // umask may have loosened it
    std::ostringstream j;
    j << "{\"active\":" << ju::q(active) << ",\"providers\":{";
    bool first = true;
    for (auto& [name, pr] : profiles) {
      if (!first) j << ",";
      first = false;
      j << ju::q(name) << ":{\"base_url\":" << ju::q(pr.base_url)
        << ",\"api_key\":" << ju::q(pr.api_key)
        << ",\"model\":" << ju::q(pr.model) << ",\"models\":[";
      for (size_t i = 0; i < pr.models.size(); ++i) {
        if (i) j << ",";
        j << ju::q(pr.models[i]);
      }
      j << "]}";
    }
    j << "}}";
    {
      std::ofstream o(path, std::ios::trunc);
      if (!o) return false;
      o << j.str();
    }
    chmod(path.c_str(), 0600);   // keys are secrets
    return true;
  }

  // Choose a sensible chat model from a catalog when none is configured —
  // prefers the given `preferred` id, then known chat families, and skips
  // obvious embed/guard/reward endpoints.
  static std::string pick_chat_model(const std::vector<std::string>& ids,
                                     const std::string& preferred = "") {
    auto is_chat = [](const std::string& id) {
      return id.find("embed") == std::string::npos &&
             id.find("reward") == std::string::npos &&
             id.find("safety") == std::string::npos &&
             id.find("guard") == std::string::npos &&
             id.find("classif") == std::string::npos;
    };
    if (!preferred.empty())
      for (auto& id : ids) if (id == preferred) return preferred;
    const std::vector<std::string> order = {"nemotron-3.5", "gpt-oss",
                                            "instruct", "nemotron",
                                            "chat", "yi-large"};
    for (auto& pat : order)
      for (auto& id : ids)
        if (is_chat(id) && id.find(pat) != std::string::npos) return id;
    for (auto& id : ids) if (is_chat(id)) return id;
    return ids.empty() ? "" : ids.front();
  }

  // Replace '<provider>' placeholders in an OpenAI-style model id:
  // "provider:model" or "provider/model" when switching profiles.
  static bool split_provider_model(const std::string& spec,
                                   std::string& provider, std::string& model) {
    for (char sep : {':', '/'}) {
      auto pos = spec.find(sep);
      if (pos != std::string::npos && pos > 0 && pos + 1 < spec.size()) {
        provider = spec.substr(0, pos);
        model = spec.substr(pos + 1);
        return true;
      }
    }
    return false;
  }
};

} // namespace pi
