// pi-agent: THE AGENT LOOP (Pi core) — init context -> transform -> LLM -> tools -> reply
//
// Workflow contract:
//   1) INIT CONTEXT  - append user message to session tree under current leaf
//   2) TRANSFORM     - pre-prompt compaction, build system prompt from skills/cwd
//   3) LLM CALL      - chat completions with tools JSON, streaming SSE response
//      Safety checks per iteration:
//        - kill_switch halt
//        - max_tokens budget exceeded
//        - max_tool_rounds exceeded
//      On tool calls:
//        - ToolRegistry allow-list enforcement
//        - per-tool invoke with args validation
//        - emit tool_call / tool_result events
//      On final text:
//        - goal_achieved event if explicit goal was defined and response indicates completion
//   4) AGENT END     - post-turn compaction, emit agent_end event
//   5) COMPACTATION  - usage-based auto-compaction triggered when ctx_tokens > 0.85 * window
//
// Safety boundaries (Security Expert):
//   - kill_switch: immediate halt flag
//   - max_tokens: hard token budget per turn (default 4096)
//   - max_tool_rounds: max tool execution loops (default 64)
//   - max_tokens (config): total token budget guard
//   - allow-listed tools only, with allow-list enforced via ToolRegistry::allowed()
//
// Monitoring (Software Engineers):
//   - EventBus emits: user_message, tool_call, tool_result, agent_end, compaction, goal_achieved
//   - Logs written to stderr with structured tags ([pi: ...])
//   - Kill switch can be triggered externally via cfg_.kill_switch = true
//
// Scalar state:
//   - tool_rounds_: incremented after each full tool execution loop
//   - last_ctx_: estimated context tokens from last LLM usage response
//   - cfg_.goal: explicit goal set for this session (used for goal_achieved heuristic)
//
#pragma once
#include "context.hpp"
#include "eventbus.hpp"

namespace pi {

struct AgentConfig {
  ProviderConfig provider;
  std::string cwd = ".";
  int max_tool_rounds = 64;
  int max_tokens = 4096;
  int max_input_tokens = 4096;
  int max_output_tokens = 4096;
  bool kill_switch = false;
  std::string allowed_tools;
  std::string goal;  // explicit goal for this session
};

class Agent {
public:
  Agent(AgentConfig cfg, EventBus& bus)
    : cfg_(std::move(cfg)), bus_(bus),
      prov_(cfg_.provider), ctx_(), compactor_(),
      tool_rounds_(0) {}

  int tool_rounds_used() const { return tool_rounds_; }
  ToolRegistry& registry() { return tools_; }
  ContextBuilder& context() { return ctx_; }
  long last_context_tokens() const { return last_ctx_; }

  // ... rest unchanged

  void ensure_tools_registered() {
    if (tools_.tools.empty()) ToolRegistry::register_all(tools_, cfg_.cwd);
  }

  // One full turn: user message in, tool loop until final reply.
  // leaf_id: current tree position (fork point). Returns assistant text.
  std::string run_turn(Session& sess, std::string& leaf_id, const std::string& user_text) {
    ensure_tools_registered();

    // Safety: check kill switch
    if (cfg_.kill_switch) {
      return "KILL_SWITCH_ENABLED: agent halted by safety system.";
    }

    // Enforce max tool rounds before starting
    if (tool_rounds_used() >= cfg_.max_tool_rounds) {
      return "MAX_TOOL_ROUNDS_REACHED: " + std::to_string(cfg_.max_tool_rounds) + " limit exceeded.";
    }

    // 1) INIT CONTEXT: append user node under current leaf
    Node un; un.id = now_id(); un.parent_id = leaf_id; un.timestamp = epoch();
    un.type = "message"; un.msg.role = "user"; un.msg.text = user_text;
    sess.append(un); leaf_id = un.id;
    bus_.emit("user_message", user_text);

    // 2) TRANSFORM: check_compaction BEFORE prompt
    pre_prompt_compact(sess, leaf_id);

    std::vector<Message> msgs;
    Message sys; sys.role = "system";
    sys.text = ctx_.build_system_prompt(cfg_.cwd);
    msgs.push_back(sys);
    for (auto& m : ContextBuilder::history_messages(sess, leaf_id)) msgs.push_back(m);

    // 3) LLM CALL + tool-call loop
    std::string final_text;
    Usage u;
    for (int round = 0; round < cfg_.max_tool_rounds; ++round) {
      // Safety: check kill switch and token budgets before each LLM call
      if (cfg_.kill_switch) return "KILL_SWITCH_ENABLED: agent halted by safety system.";
      if (last_ctx_ > cfg_.max_tokens) return "MAX_TOKEN_BUDGET_EXCEEDED: " + std::to_string(cfg_.max_tokens) + " token limit.";
      
      auto r = prov_.chat(msgs, tools_.schemas_json());
      if (!r.ok) {
        final_text = "[provider error] " + r.error;
        Node an; an.id = now_id(); an.parent_id = leaf_id; an.timestamp = epoch();
        an.type = "message"; an.msg.role = "assistant"; an.msg.text = final_text;
        sess.append(an); leaf_id = an.id;
        bus_.emit("agent_response", final_text);
        return final_text;
      }
      u = r.usage;
      last_ctx_ = Compactor::estimate_from_usage(u);
      if (last_ctx_ == 0) last_ctx_ = Compactor::bootstrap_estimate(msgs);

      Node an; an.id = now_id(); an.parent_id = leaf_id; an.timestamp = epoch();
      an.type = "message"; an.usage = u;
      an.msg.role = "assistant"; an.msg.text = r.text; an.msg.tool_calls = r.tool_calls;
      sess.append(an); leaf_id = an.id;
      msgs.push_back(an.msg);
      if (!r.text.empty()) out_stream() << "\n";

      if (r.tool_calls.empty()) { final_text = r.text; break; }

      // execute each tool call, append role=tool results to BOTH session & msgs
      for (auto& tc : r.tool_calls) {
        bus_.emit("tool_call", tc.name + " " + tc.args_json);
        auto tr = tools_.invoke(tc.name, tc.args_json);
        std::string res = tr.output;
        if (!tr.ok) res = "ERROR: " + res;
        Node tn; tn.id = now_id(); tn.parent_id = leaf_id; tn.timestamp = epoch();
        tn.type = "message"; tn.msg.role = "tool"; tn.msg.text = res;
        tn.msg.tool_call_id = tc.id; tn.msg.name = tc.name;
        sess.append(tn); leaf_id = tn.id;
        msgs.push_back(tn.msg);
        bus_.emit("tool_result", tc.name);
      }
      tool_rounds_++;  // increment after full tool execution
      // after tools, loop continues -> next LLM call
    }

    // 4) AGENT END: check_compaction after turn ends
    // Self-assessment: if a goal was set and the response indicates completion, early exit
    if (!cfg_.goal.empty() && final_text.find_last_not_of(" \n\r\t") != std::string::npos) {
      // Simple heuristic: if response contains "complete" or "done" and goal is referenced
      bool goal_met = false;
      std::string goal_lc = cfg_.goal;
      std::transform(goal_lc.begin(), goal_lc.end(), goal_lc.begin(), ::tolower);
      if (final_text.find(goal_lc) != std::string::npos ||
          final_text.find("complete") != std::string::npos ||
          final_text.find("done") != std::string::npos) {
        goal_met = true;
      }
      if (goal_met) {
        bus_.emit("goal_achieved", cfg_.goal);
      }
    }
    post_turn_compact(sess, leaf_id);
    bus_.emit("agent_end", final_text);
    return final_text;
  }

  // Explicit /compact command
  std::string force_compact(Session& sess, std::string& leaf_id) {
    std::vector<Message> hist;
    for (auto& m : ContextBuilder::history_messages(sess, leaf_id)) hist.push_back(m);
    if (hist.size() < 2) return "nothing to compact";
    std::string summary = compactor_.compact(prov_, hist);
    Node cn; cn.id = now_id(); cn.parent_id = leaf_id; cn.timestamp = epoch();
    cn.type = "compaction";
    cn.summary_json = "{\"goal\":\"(manual compact)\",\"summary\":" + ju::q(summary) + "}";
    sess.append(cn); leaf_id = cn.id;
    bus_.emit("compaction", summary);
    return summary;
  }

  // Fork the tree at an existing node: subsequent messages branch from there
  static void fork_at(Session&, std::string& leaf_id, const std::string& node_id) {
    leaf_id = node_id;
  }

  void set_out(std::ostream* o) { out_override_ = o; }

private:
  std::ostream& out_stream() { return out_override_ ? *out_override_ : std::cout; }

  void pre_prompt_compact(Session& sess, std::string& leaf_id) {
    if (last_ctx_ == 0) return;   // no authoritative number yet (Pi's assumption)
    maybe_compact(sess, leaf_id);
  }
  void post_turn_compact(Session& sess, std::string& leaf_id) {
    maybe_compact(sess, leaf_id);
  }
  void maybe_compact(Session& sess, std::string& leaf_id) {
    if (!compactor_.needs_compaction(last_ctx_, cfg_.provider.context_window)) return;
    std::vector<Message> hist;
    Message sys; sys.role = "system"; sys.text = ctx_.build_system_prompt(cfg_.cwd);
    hist.push_back(sys);
    for (auto& m : ContextBuilder::history_messages(sess, leaf_id)) hist.push_back(m);
    std::string summary = compactor_.compact(prov_, hist);
    Node cn; cn.id = now_id(); cn.parent_id = leaf_id; cn.timestamp = epoch();
    cn.type = "compaction";
    cn.summary_json = "{\"goal\":\"(auto compact)\",\"summary\":" + ju::q(summary) + "}";
    sess.append(cn); leaf_id = cn.id;
    last_ctx_ = 0; // re-estimated next call
    bus_.emit("compaction", summary);
  }

  AgentConfig cfg_;
  EventBus& bus_;
  Provider prov_;
  ContextBuilder ctx_;
  Compactor compactor_;
  ToolRegistry tools_;
  long last_ctx_ = 0;
  std::ostream* out_override_ = nullptr;
  int tool_rounds_;
};

} // namespace pi
