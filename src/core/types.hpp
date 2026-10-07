// pi-agent: core types (from-scratch, Pi-style)
#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <cstdint>

namespace pi {

// Core message-structure types, used across the codebase.

// Usage: token usage metrics from provider responses
struct Usage {
  long input = 0, output = 0, cache_read = 0, cache_write = 0;
  long total() const { return input + output + cache_read + cache_write; }
};

// ToolCall: normalized tool call from LLM response
struct ToolCall {
  std::string id;      // provider call id
  std::string name;    // tool name
  std::string args_json; // raw JSON object string
};

// Message: OpenAI-format message envelope
struct Message {
  std::string role;                 // system|user|assistant|tool
  std::string text;                 // text content
  std::vector<ToolCall> tool_calls; // assistant may request tools
  std::string tool_call_id;         // for role=tool results
  std::string name;                 // tool name when role=tool
};

// Node: one JSONL line = one tree node (session state)
struct Node {                       // one JSONL line = one tree node
  std::string id;
  std::string parent_id;            // "" = root
  int64_t timestamp = 0;            // epoch seconds
  std::string type;                 // session|message|summary|compaction
  Message msg;                      // valid if type=message
  std::string summary_json;         // structured checkpoint (type=summary/compaction)
  Usage usage;                      // last known provider usage on this branch
};

// SessionId type alias
using SessionId = std::string;

// EventBus event names (for monitoring/extensibility)
namespace EventNames {
  inline const char* user_message = "user_message";
  inline const char* tool_call = "tool_call";
  inline const char* tool_result = "tool_result";
  inline const char* agent_response = "agent_response";
  inline const char* agent_end = "agent_end";
  inline const char* compaction = "compaction";
  inline const char* goal_achieved = "goal_achieved";
}

// Session lifecycle commands (CLI slash commands)
namespace SessionCommands {
  inline const char* tree = "tree";
  inline const char* compact = "compact";
  inline const char* fork = "fork:";
  inline const char* skill = "skill:";
  inline const char* exit = "exit";
  inline const char* quit = "quit";
}

} // namespace pi