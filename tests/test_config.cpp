#include "core/config.hpp"
#include "core/provider.hpp"
#include <cassert>
#include <cstdio>
#include <sys/stat.h>
using namespace pi;

int main() {
  // --- Provider::parse_models (OpenAI-compatible GET /v1/models body) ---
  const std::string body = R"({"object":"list","data":[
    {"id":"nvidia/nemotron-3.5-lightning-30b-a3b","object":"model","owned_by":"nvidia"},
    {"id":"meta/llama-3.3-70b-instruct","object":"model"},
    {"id":"qwen2.5:7b","object":"model"}
  ]})";
  auto ids = Provider::parse_models(body);
  assert(ids.size() == 3);
  assert(ids[0] == "nvidia/nemotron-3.5-lightning-30b-a3b");
  assert(ids[2] == "qwen2.5:7b");
  assert(Provider::parse_models("not json").empty());
  assert(Provider::parse_models("{\"data\":[]}").empty());

  // --- PiConfig save/load round-trip ---
  const std::string path = "/tmp/pi_config_test.json";
  std::remove(path.c_str());
  {
    PiConfig c; c.path = path;
    c.active = "nvidia";
    c.profiles["nvidia"] = {"https://integrate.api.nvidia.com/v1", "nvapi-secret-12345678", "nemotron", {"a", "b"}};
    c.profiles["ollama"] = {"http://localhost:11434/v1", "", "", {}};
    assert(c.save());
  }
  {
    PiConfig c; c.load(path);
    assert(c.path == path);
    assert(c.active == "nvidia");
    assert(c.profiles.size() == 2);
    assert(c.profiles["nvidia"].base_url == "https://integrate.api.nvidia.com/v1");
    assert(c.profiles["nvidia"].api_key == "nvapi-secret-12345678");
    assert(c.profiles["nvidia"].model == "nemotron");
    assert(c.profiles["nvidia"].models.size() == 2 && c.profiles["nvidia"].models[1] == "b");
    assert(c.profiles["ollama"].api_key.empty());
    // config must refuse to expose the key in a listing
    const std::string key = c.profiles["nvidia"].api_key;
    std::string masked = PiConfig::mask_key(key);
    assert(masked.size() == key.size());
    assert(masked.find("secret") == std::string::npos);
    assert(masked.rfind("5678") == masked.size() - 4);
    // file permissions must prevent other users reading the key
    struct stat st;
    if (stat(path.c_str(), &st) == 0) assert((st.st_mode & 0777) == 0600);
  }

  // --- presets ---
  assert(PiConfig::preset("openai").base_url == "https://api.openai.com/v1");
  assert(PiConfig::preset("hf").base_url == "https://router.huggingface.co/v1");
  assert(PiConfig::preset("ollama").base_url == "http://localhost:11434/v1");
  assert(PiConfig::preset("lmstudio").base_url == "http://localhost:1234/v1");
  assert(PiConfig::preset("llamacpp").base_url == "http://localhost:8080/v1");
  assert(PiConfig::preset("weird").base_url.empty());
  assert(PiConfig::is_local("http://localhost:11434/v1"));
  assert(!PiConfig::is_local("https://api.openai.com/v1"));

  // --- provider:model split ---
  std::string prov, model;
  assert(PiConfig::split_provider_model("nvidia:nemotron", prov, model));
  assert(prov == "nvidia" && model == "nemotron");

  // --- pick_chat_model ---
  std::vector<std::string> cat = {"nvidia/embed-qa-4", "nvidia/llama-3.1-nemoguard-8b-content-safety",
                                  "meta/llama-3.3-70b-instruct", "nvidia/llama-3.1-nemotron-70b-instruct",
                                  "01-ai/yi-large"};
  assert(PiConfig::pick_chat_model(cat) == "meta/llama-3.3-70b-instruct"); // first is_chat+instruct
  assert(PiConfig::pick_chat_model(cat, "nvidia/llama-3.1-nemotron-70b-instruct")
         == "nvidia/llama-3.1-nemotron-70b-instruct");   // explicit preferred wins
  assert(PiConfig::pick_chat_model({"a/embed-x", "a/reward-y"}) == "a/embed-x"); // all skipped -> first
  assert(PiConfig::pick_chat_model({}, "x").empty());

  std::remove(path.c_str());
  printf("test_config OK (models parse, profiles round-trip, 0600, presets, pick_chat_model)\n");
  return 0;
}