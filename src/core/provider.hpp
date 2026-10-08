// pi-agent: OpenAI-compatible provider client (works with NVIDIA NIM,
// local llama.cpp-server / Ollama / vLLM). Streaming SSE + tool calls.
#pragma once
#include "types.hpp"
#include "jsonutil.hpp"
#include "console.hpp"
#include <httplib.h>
#include <sstream>
#include <iostream>
#include <map>
#include <thread>
#include <atomic>
#include <chrono>

namespace pi {

struct ProviderConfig {
  std::string base_url = "http://localhost:8000/v1"; // or https://integrate.api.nvidia.com/v1
  std::string api_key;                              // Bearer token (nvapi-...)
  std::string model = "meta/llama-3.1-70b-instruct";
  long context_window = 8192;                       // for compaction math
};

struct ChatResponse {
  std::string text;
  std::string reasoning;
  std::vector<ToolCall> tool_calls;
  Usage usage;
  bool ok = false;
  std::string error;
};

class Provider {
public:
  Provider(ProviderConfig cfg, std::ostream& stream_out = std::cout)
    : cfg_(std::move(cfg)), out_(stream_out) {}

  static void split_base(const std::string& url, std::string& scheme_host, std::string& prefix) {
    auto p = url.find("://");
    size_t hs = (p == std::string::npos) ? 0 : p + 3;
    auto slash = url.find('/', hs);
    if (slash == std::string::npos) { scheme_host = url; prefix = ""; }
    else { scheme_host = url.substr(0, slash); prefix = url.substr(slash); }
  }

  // OpenAI-format messages array from our Message list
  static std::string messages_json(const std::vector<Message>& msgs) {
    std::ostringstream o; o << "[";
    for (size_t i = 0; i < msgs.size(); ++i) {
      const Message& m = msgs[i];
      o << (i ? "," : "") << "{\"role\":" << ju::q(m.role);
      if (m.role == "tool") {
        o << ",\"content\":" << ju::q(m.text)
          << ",\"tool_call_id\":" << ju::q(m.tool_call_id);
      } else if (!m.tool_calls.empty()) {
        o << ",\"content\":" << (m.text.empty() ? "null" : ju::q(m.text))
          << ",\"tool_calls\":[";
        for (size_t j = 0; j < m.tool_calls.size(); ++j) {
          auto& tc = m.tool_calls[j];
          o << (j ? "," : "") << "{\"id\":" << ju::q(tc.id)
            << ",\"type\":\"function\",\"function\":{\"name\":" << ju::q(tc.name)
            << ",\"arguments\":" << ju::q(tc.args_json) << "}}";
        }
        o << "]";
      } else {
        o << ",\"content\":" << ju::q(m.text);
      }
      o << "}";
    }
    o << "]";
    return o.str();
  }

  // Parse one SSE "data:" payload into accumulators. Public for unit testing.
  struct Acc {
    std::string text;
    std::string reasoning;
    std::map<int, ToolCall> tcs;
    std::map<int, std::string> tc_args;
    Usage usage;
  };
  void feed_payload(const std::string& payload, Acc& a) {
    if (payload == "[DONE]") return;
    ju::Doc d;
    if (!d.parse(payload)) return;
    if (auto* u = ju::member(d.root, "usage")) {
      a.usage.input = ju::num(u, "prompt_tokens");
      a.usage.output = ju::num(u, "completion_tokens");
      if (auto* det = ju::member(u, "prompt_tokens_details"))
        a.usage.cache_read = ju::num(det, "cached_tokens");
    }
    auto* choices = ju::arr(d.root, "choices");
    for (auto* e = choices ? choices->start : nullptr; e; e = e->next) {
      json_value_s* dl = ju::member(e->value, "delta");
      if (!dl) dl = ju::member(e->value, "message");   // non-stream fallback
      if (!dl) continue;
      // Nemotron / reasoning models stream chain-of-thought in reasoning_content.
      // Capture it but do NOT treat it as the answer text.
      std::string rc = ju::str(dl, "reasoning_content");
      if (!rc.empty()) {
        a.reasoning += rc;
        Console::ref().thinking(rc);
      }
      std::string c = ju::str(dl, "content");
      if (!c.empty()) { a.text += c; Console::ref().content(out_, c); }
      auto* tca = ju::arr(dl, "tool_calls");
      for (auto* te = tca ? tca->start : nullptr; te; te = te->next) {
        int idx = (int)ju::num(te->value, "index", 0);
        auto& slot = a.tcs[idx];
        std::string id = ju::str(te->value, "id");
        if (!id.empty()) slot.id = id;
        if (auto* fn = ju::member(te->value, "function")) {
          std::string nm = ju::str(fn, "name");
          if (!nm.empty()) slot.name += nm;
          std::string ar = ju::str(fn, "arguments");
          if (!ar.empty()) {
            a.tc_args[idx] += ar;
            std::cerr << "." << std::flush;
          }
        }
      }
    }
  }

  ChatResponse chat(const std::vector<Message>& msgs, const std::string& tools_json) {
    ChatResponse r;
    std::string host, prefix; split_base(cfg_.base_url, host, prefix);
    httplib::Client cli(host);
    cli.set_read_timeout(600, 0);
    cli.set_connection_timeout(15, 0);
    cli.set_keep_alive(true);

    // Nemotron-3 reasoning models stream chain-of-thought into content unless
    // reasoning is explicitly disabled (NIM API param: extra_body.reasoning).
    bool nemotron = cfg_.model.find("nemotron") != std::string::npos;
    std::ostringstream body;
    body << "{\"model\":" << ju::q(cfg_.model)
         << ",\"messages\":" << messages_json(msgs)
         << ",\"stream\":true,\"temperature\":0.2"
         << ",\"stream_options\":{\"include_usage\":true}"
         << ",\"max_tokens\":" << (nemotron ? 8192 : 4096);
    if (nemotron)
      body << ",\"reasoning\":{\"exclude\":false,\"effort\":\"high\"}";
    if (!tools_json.empty() && tools_json != "[]")
      body << ",\"tools\":" << tools_json << ",\"tool_choice\":\"auto\"";
    body << "}";

    httplib::Headers h;
    if (!cfg_.api_key.empty()) h.emplace("Authorization", "Bearer " + cfg_.api_key);
    h.emplace("Accept", "text/event-stream");

    // RAII spinner: every early-return/error path must join, otherwise the
    // joinable std::thread destructor calls std::terminate and kills the
    // whole process mid-turn (session file then has no assistant node).
    struct Spinner {
      std::atomic<bool> stop{false};
      std::thread th;
      Spinner() : th([this]() {
        const char spin[] = "-\\|/";
        int i = 0;
        while (!stop.load()) {
          Console::ref().live("\x1b[2mThinking " + std::string(1, spin[i++ % 4]) + "\x1b[0m");
          std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
      }) {}
      ~Spinner() {
        stop.store(true);
        Console::ref().release();
        if (th.joinable()) th.join();
      }
      void arrived() { stop.store(true); }
    } spinner;
    Console::ref().reset_request();

    int attempts = 0;
    bool success = false;
    bool aborted = false;
    Acc acc;
    for (attempts = 0; attempts < 2 && !success; ++attempts) {
      acc = Acc{};
      std::string line_buf;   // per-request buffer (no stale state across calls)
      auto res = cli.Post((prefix + "/chat/completions").c_str(), h,
                          body.str(), "application/json",
                          [&](const char* data, size_t len) -> bool {
        if (cancel_ && cancel_->load()) { aborted = true; return false; }
        line_buf.append(data, len);
        size_t nl;
        while ((nl = line_buf.find('\n')) != std::string::npos) {
          std::string line = line_buf.substr(0, nl);
          line_buf.erase(0, nl + 1);
          if (!line.empty() && line.back() == '\r') line.pop_back();
          if (line.rfind("data:", 0) != 0) continue;
          std::string payload = line.substr(5);
          if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
          feed_payload(payload, acc);
          if (!acc.text.empty() || !acc.reasoning.empty()) spinner.arrived();
        }
        return true;
      });

      if (!res) {
        if (aborted) { r.error = "aborted by user"; return r; }
        if (attempts + 1 < 2) continue;   // transient network; retry once
        r.error = "HTTP request failed: " + httplib::to_string(res.error());
        return r;                          // Spinner dtor joins safely
      }
      if (res->status >= 400) {
        // 429/5xx may be transient; retry once, otherwise fail
        if ((res->status == 429 || res->status >= 500) && attempts + 1 < 2) continue;
        r.error = "HTTP " + std::to_string(res->status) + ": " + res->body;
        return r;                          // Spinner dtor joins safely
      }
      success = true;
    }

    if (aborted) { r.error = "aborted by user"; return r; }
    spinner.arrived();

    r.text = acc.text;
    r.reasoning = acc.reasoning;
    // Fallback: if the model only produced reasoning (e.g. server ignored the
    // exclude flag), salvage a clean answer by stripping the thinking block.
    if (r.text.empty() && !acc.reasoning.empty()) {
      std::string& R = acc.reasoning;
      size_t s = R.find("\n\n");            // heuristic: answer after thinking
      if (s != std::string::npos && R.size() - s > 40) r.text = R.substr(s);
      else r.text = R.substr(0, 2000);
      Console::ref().content(out_, r.text);
    }
    for (auto& [idx, tc] : acc.tcs) {
      if (tc.name.empty()) continue;
      if (tc.id.empty()) tc.id = "call_" + std::to_string(idx);
      tc.args_json = acc.tc_args[idx].empty() ? "{}" : acc.tc_args[idx];
      ju::Doc chk; if (!chk.parse(tc.args_json)) tc.args_json = "{}";
      r.tool_calls.push_back(tc);
    }
    r.usage = acc.usage;
    r.ok = true;
    return r;
  }

  const ProviderConfig& cfg() const { return cfg_; }
  void set_model(const std::string& m) { cfg_.model = m; }
  void set_base_url(const std::string& u) { cfg_.base_url = u; }
  void set_api_key(const std::string& k) { cfg_.api_key = k; }
  // Optional abort flag (agent's /abort): checked on every SSE chunk, the
  // content receiver then returns false and httplib cancels the request.
  void set_cancel(std::atomic<bool>* p) { cancel_ = p; }

  // ---- model catalog: GET {base_url}/models (OpenAI-compatible, one path
  // covers openai / nvidia / huggingface router / ollama / lmstudio / llama.cpp)
  static std::vector<std::string> parse_models(const std::string& body) {
    std::vector<std::string> out;
    ju::Doc d; if (!d.parse(body)) return out;
    auto* data = ju::member(d.root, "data");
    if (data && data->type == json_type_array) {
      for (auto* e = json_value_as_array(data)->start; e; e = e->next) {
        std::string id = ju::str(e->value, "id");
        if (!id.empty()) out.push_back(id);
      }
    }
    return out;
  }

  static bool list_models(const std::string& base_url, const std::string& api_key,
                          std::vector<std::string>& out, std::string& err) {
    out.clear(); err.clear();
    std::string host, prefix; split_base(base_url, host, prefix);
    httplib::Client cli(host);
    cli.set_connection_timeout(8, 0);
    cli.set_read_timeout(20, 0);
    httplib::Headers h;
    if (!api_key.empty()) h.emplace("Authorization", "Bearer " + api_key);
    auto res = cli.Get((prefix + "/models").c_str(), h);
    if (!res) { err = "connection failed: " + httplib::to_string(res.error()); return false; }
    if (res->status >= 400) {
      err = "HTTP " + std::to_string(res->status);
      if (res->body.size() > 200) res->body.resize(200);
      err += ": " + res->body;
      return false;
    }
    out = parse_models(res->body);
    if (out.empty()) { err = "endpoint returned no models"; return false; }
    return true;
  }


private:
  ProviderConfig cfg_;
  std::ostream& out_;
  std::atomic<bool>* cancel_ = nullptr;
};

} // namespace pi
