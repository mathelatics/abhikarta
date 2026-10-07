// pi-agent: core types (from-scratch, Pi-style)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <cstdint>

namespace pi {

struct Usage {
  long input = 0, output = 0, cache_read = 0, cache_write = 0;
  long total() const { return input + output + cache_read + cache_write; }
};

struct ToolCall {
  std::string id;      // provider call id
  std::string name;    // tool name
  std::string args_json; // raw JSON object string
};

struct Message {
  std::string role;                 // system|user|assistant|tool
  std::string text;                 // text content
  std::vector<ToolCall> tool_calls; // assistant may request tools
  std::string tool_call_id;         // for role=tool results
  std::string name;                 // tool name when role=tool
};

struct Node {                       // one JSONL line = one tree node
  std::string id;
  std::string parent_id;            // "" = root
  int64_t timestamp = 0;            // epoch seconds
  std::string type;                 // session|message|summary|compaction
  Message msg;                      // valid if type=message
  std::string summary_json;         // structured checkpoint (type=summary/compaction)
  Usage usage;                      // last known provider usage on this branch
};

using SessionId = std::string;

} // namespace pi
