// pi-agent: OpenAI-compatible provider client (works with NVIDIA NIM,
// local llama.cpp-server / Ollama / vLLM). Streaming SSE + tool calls.
#pragma once
#include "types.hpp"
#include "jsonutil.hpp"
#include <httplib.h>
#include <sstream>
#include <iostream>
#include <map>

namespace pi {

struct ProviderConfig {
  std::string base_url = "http://localhost:8000/v1"; // or https://integrate.api.nvidia.com/v1
  std::string api_key;                              // Bearer token (nvapi-...)
  std::string model = "meta/llama-3.1-70b-instruct";
  long context_window = 8192;                       // for compaction math
};

struct ChatResponse {
  std::string text;
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
      std::string c = ju::str(dl, "content");
      if (!c.empty()) { a.text += c; out_ << c << std::flush; }
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
          if (!ar.empty()) a.tc_args[idx] += ar;
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

    std::ostringstream body;
    body << "{\"model\":" << ju::q(cfg_.model)
         << ",\"messages\":" << messages_json(msgs)
         << ",\"stream\":true,\"temperature\":0.2";
    if (!tools_json.empty() && tools_json != "[]")
      body << ",\"tools\":" << tools_json << ",\"tool_choice\":\"auto\"";
    body << "}";

    httplib::Headers h;
    if (!cfg_.api_key.empty()) h.emplace("Authorization", "Bearer " + cfg_.api_key);
    h.emplace("Accept", "text/event-stream");

    Acc acc;
    std::string line_buf;   // per-request buffer (no stale state across calls)
    auto res = cli.Post((prefix + "/chat/completions").c_str(), h,
                        body.str(), "application/json",
                        [&](const char* data, size_t len) -> bool {
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
      }
      return true;
    });

    if (!res) { r.error = "HTTP request failed: " + httplib::to_string(res.error()); return r; }
    if (res->status >= 400) { r.error = "HTTP " + std::to_string(res.status) + ": " + res->body; return r; }

    r.text = acc.text;
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

private:
  ProviderConfig cfg_;
  std::ostream& out_;
};

} // namespace pi
