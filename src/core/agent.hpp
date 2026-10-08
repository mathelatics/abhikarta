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
//   - max_tokens: cumulative session token budget (default 1000000, 0 = off)
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
  int max_tokens = 1000000;       // cumulative session token budget (0 = unlimited)
  int max_input_tokens = 0;       // per-request input guard (0 = derive from context_window)
  int max_output_tokens = 0;      // per-request output guard (0 = disabled)
  bool kill_switch = false;
  std::string allowed_tools;
  std::string goal;  // explicit goal for this session (used for goal_achieved heuristic)
};

class Agent {
public:
  Agent(AgentConfig cfg, EventBus& bus)
    : cfg_(std::move(cfg)), bus_(bus),
      prov_(cfg_.provider), ctx_(), compactor_(),
      tool_rounds_(0) {}

  int tool_rounds_used() const { return tool_rounds_; }
  const ProviderConfig& provider_cfg() const { return cfg_.provider; }
  void set_model(const std::string& m) { cfg_.provider.model = m; prov_.set_model(m); }
  ToolRegistry& registry() { return tools_; }
  ContextBuilder& context() { return ctx_; }
  long last_context_tokens() const { return last_ctx_; }

  // ... rest unchanged

  void ensure_tools_registered() {
    if (tools_.tools.empty()) ToolRegistry::register_all(tools_, cfg_.cwd);
  }

  // Called when a fresh session starts: forget cumulative safety state so a
  // finished/expired session cannot poison the next one (last_ctx_ was kept
  // across /new, which made every follow-up turn die on the budget guard).
  void reset_context() {
    last_ctx_ = 0;
    session_tokens_ = 0;
    tool_rounds_ = 0;
  }

  // One full turn: user message in, tool loop until final reply.
  // leaf_id: current tree position (fork point). Returns assistant text.
  std::string run_turn(Session& sess, std::string& leaf_id, const std::string& user_text) {
    ensure_tools_registered();

    // Reset cumulative per-turn tool counter
    tool_rounds_ = 0;

    // Safety: check kill switch
    if (cfg_.kill_switch) return guard_stop("KILL_SWITCH_ENABLED: agent halted by safety system.");

    // Enforce max tool rounds before starting
    if (tool_rounds_used() >= cfg_.max_tool_rounds)
      return guard_stop("MAX_TOOL_ROUNDS_REACHED: " + std::to_string(cfg_.max_tool_rounds) + " limit exceeded.");

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
      if (cfg_.kill_switch) return guard_stop("KILL_SWITCH_ENABLED: agent halted by safety system.");
      if (cfg_.max_tokens > 0 && session_tokens_ > cfg_.max_tokens)
        return guard_stop("MAX_TOKEN_BUDGET_EXCEEDED: " + std::to_string(cfg_.max_tokens) +
                          " session token limit (used " + std::to_string(session_tokens_) + ").");
      if (last_ctx_ > cfg_.provider.context_window)
        return guard_stop("CONTEXT_WINDOW_EXCEEDED: " + std::to_string(last_ctx_) +
                          " > " + std::to_string(cfg_.provider.context_window) +
                          " tokens; run /compact or start with a larger --window.");

      auto r = prov_.chat(msgs, tools_.schemas_json());
      if (!r.ok) {
        final_text = "[provider error] " + r.error;
        Node an; an.id = now_id(); an.parent_id = leaf_id; an.timestamp = epoch();
        an.type = "message"; an.msg.role = "assistant"; an.msg.text = final_text;
        sess.append(an); leaf_id = an.id;
        out_stream() << final_text << "\n";
        bus_.emit("agent_response", final_text);
        return final_text;
      }
      u = r.usage;
      session_tokens_ += u.input + u.output;
      last_ctx_ = Compactor::estimate_from_usage(u);
      if (last_ctx_ == 0) last_ctx_ = Compactor::bootstrap_estimate(msgs);
      if (!r.reasoning.empty()) bus_.emit("reasoning", r.reasoning);

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
        bus_.emit("tool_result", tc.name + ":\n" + res);
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
    long prev_ctx = last_ctx_;
    last_ctx_ = 0;
    bus_.emit("compaction", summary);
    bus_.emit("compaction_meta", "{\"kind\":\"manual\",\"summary\":" + ju::q(summary) + ",\"prev_ctx_tokens\":" + std::to_string(prev_ctx) + "}");
    return summary;
  }

  // Fork the tree at an existing node: subsequent messages branch from there
  static void fork_at(Session&, std::string& leaf_id, const std::string& node_id) {
    leaf_id = node_id;
  }

  void set_out(std::ostream* o) { out_override_ = o; }
  void set_kill_switch(bool on) { cfg_.kill_switch = on; }

private:
  std::ostream& out_stream() { return out_override_ ? *out_override_ : std::cout; }

  // Safety guards return early without an LLM call, so the REPL would never
  // stream this text — print it explicitly and return it for RPC consumers.
  std::string guard_stop(const std::string& msg) {
    out_stream() << msg << "\n";
    return msg;
  }

  void pre_prompt_compact(Session& sess, std::string& leaf_id) {
    if (last_ctx_ == 0) return;   // no authoritative number yet (Pi's assumption)
    maybe_compact(sess, leaf_id);
  }
  void post_turn_compact(Session& sess, std::string& leaf_id) {
    maybe_compact(sess, leaf_id);
  }
  void maybe_compact(Session& sess, std::string& leaf_id) {
    if (!compactor_.needs_compaction(last_ctx_, cfg_.provider.context_window)) return;
    long prev_ctx_ = last_ctx_;
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
    bus_.emit("compaction_meta", "{\"kind\":\"auto\",\"summary\":" + ju::q(summary) + ",\"prev_ctx_tokens\":" + std::to_string(prev_ctx_) + "}");
  }

  AgentConfig cfg_;
  EventBus& bus_;
  Provider prov_;
  ContextBuilder ctx_;
  Compactor compactor_;
  ToolRegistry tools_;
  long last_ctx_ = 0;
  long session_tokens_ = 0;   // cumulative input+output across this session's turns
  std::ostream* out_override_ = nullptr;
  int tool_rounds_;
};

// AgentSession: session lifecycle façade (CLI + RPC + SDK consumers).
class AgentSession {
public:
  AgentSession(AgentConfig cfg, EventBus& bus, std::string resume_path = "")
    : agent_(cfg, bus), bus_(bus), cfg_(cfg) {
    if (!resume_path.empty()) {
      if (sess_.resume(resume_path)) {
        agent_.reset_context();
        leaf_id_ = "";
        for (auto it = sess_.nodes.rbegin(); it != sess_.nodes.rend(); ++it) {
          if (it->type != "diagnostic") { leaf_id_ = it->id; break; }
        }
        if (leaf_id_.empty() && !sess_.nodes.empty()) leaf_id_ = sess_.nodes.back().id;
        bool same = false;
        for (auto it = sess_.nodes.rbegin(); it != sess_.nodes.rend(); ++it) {
          if (it->type == "model_change") {
            same = it->summary_json.find(cfg_.provider.model) != std::string::npos;
            break;
          }
        }
        if (!same) {
          Node mc; mc.id = now_id(); mc.parent_id = leaf_id_; mc.timestamp = epoch();
          mc.type = "model_change";
          mc.summary_json = "{\"model\":" + ju::q(cfg_.provider.model) + ",\"base_url\":" + ju::q(cfg_.provider.base_url) + "}";
          sess_.append(mc);
          leaf_id_ = mc.id;
        }
      } else {
        leaf_id_ = "";
      }
    }
  }

  bool open_new(const std::string& cwd) {
    sess_.open_new(cwd);
    agent_.reset_context();   // fresh session must not inherit last_ctx_/budget
    Node mc; mc.id = now_id(); mc.parent_id = sess_.nodes.back().id; mc.timestamp = epoch();
    mc.type = "model_change";
    mc.summary_json = "{\"model\":" + ju::q(cfg_.provider.model) + ",\"base_url\":" + ju::q(cfg_.provider.base_url) + "}";
    sess_.append(mc);
    leaf_id_ = sess_.id;
    return true;
  }

  std::string prompt(const std::string& text) {
    if (leaf_id_.empty()) return "[session error] no active session";
    return agent_.run_turn(sess_, leaf_id_, text);
  }

  void new_session() {
    open_new(cfg_.cwd);
  }

  std::string resume_session(const std::string& path) {
    sess_ = Session{};
    agent_.reset_context();
    if (!sess_.resume(path)) return "";
    leaf_id_ = "";
    for (auto it = sess_.nodes.rbegin(); it != sess_.nodes.rend(); ++it) {
      if (it->type != "diagnostic") { leaf_id_ = it->id; break; }
    }
    if (leaf_id_.empty() && !sess_.nodes.empty()) leaf_id_ = sess_.nodes.back().id;
    bool same = false;
    for (auto it = sess_.nodes.rbegin(); it != sess_.nodes.rend(); ++it) {
      if (it->type == "model_change") {
        same = it->summary_json.find(cfg_.provider.model) != std::string::npos;
        break;
      }
    }
    if (!same) {
      Node mc; mc.id = now_id(); mc.parent_id = leaf_id_; mc.timestamp = epoch();
      mc.type = "model_change";
      mc.summary_json = "{\"model\":" + ju::q(cfg_.provider.model) + ",\"base_url\":" + ju::q(cfg_.provider.base_url) + "}";
      sess_.append(mc);
      leaf_id_ = mc.id;
    }
    return sess_.file;
  }

  void set_model(const std::string& m) {
    cfg_.provider.model = m;
    agent_.set_model(m);
    Node mc; mc.id = now_id(); mc.parent_id = leaf_id_; mc.timestamp = epoch();
    mc.type = "model_change";
    mc.summary_json = "{\"model\":" + ju::q(m) + ",\"base_url\":" + ju::q(cfg_.provider.base_url) + "}";
    sess_.append(mc);
    leaf_id_ = mc.id;
  }

  void set_name(const std::string& n) { name_ = n; Node nm; nm.id = now_id(); nm.parent_id = leaf_id_; nm.timestamp = epoch(); nm.type = "label"; nm.summary_json = "{\"name\":" + ju::q(n) + "}"; sess_.append(nm); leaf_id_ = nm.id; }
  const std::string& name() const { return name_; }

  std::string session_info() const {
    std::ostringstream out;
    out << "id=" << sess_.id << "\n"
        << "file=" << sess_.file << "\n"
        << "model=" << cfg_.provider.model << "\n"
        << "base_url=" << cfg_.provider.base_url << "\n"
        << "cwd=" << cfg_.cwd << "\n"
        << "nodes=" << sess_.nodes.size() << "\n"
        << "leaf=" << leaf_id_ << "\n";
    if (!name_.empty()) out << "name=" << name_ << "\n";
    return out.str();
  }

  std::string last_assistant_text() const {
    for (auto it = sess_.nodes.rbegin(); it != sess_.nodes.rend(); ++it) {
      if (it->type == "message" && it->msg.role == "assistant" && !it->msg.text.empty())
        return it->msg.text;
    }
    return "";
  }

  std::string last_usage_text() const {
    for (auto it = sess_.nodes.rbegin(); it != sess_.nodes.rend(); ++it) {
      if (it->type == "message" && it->msg.role == "assistant") {
        return "input=" + std::to_string(it->usage.input) +
               " output=" + std::to_string(it->usage.output) +
               " cache_read=" + std::to_string(it->usage.cache_read) +
               " cache_write=" + std::to_string(it->usage.cache_write);
      }
    }
    return "no usage recorded";
  }

  bool export_to(const std::string& path) const {
    std::ifstream in(sess_.file, std::ios::binary);
    std::ofstream out(path, std::ios::binary);
    if (!in || !out) return false;
    out << in.rdbuf();
    return true;
  }

  std::string compact() { return agent_.force_compact(sess_, leaf_id_); }
  void fork(const std::string& node_id) {
    if (sess_.find(node_id)) leaf_id_ = node_id;
  }

  std::string session_file() const { return sess_.file; }
  std::string session_id() const { return sess_.id; }
  const Session& session() const { return sess_; }
  Session& session() { return sess_; }
  Agent& agent() { return agent_; }
  const std::string& active_leaf() const { return leaf_id_; }

  void set_out(std::ostream* o) { agent_.set_out(o); }
  void set_kill_switch(bool on) { agent_.set_kill_switch(on); }

  // Slash-command surface used by main.cpp
  std::string tree_dump() const {
    auto chain = sess_.branch(leaf_id_);
    std::ostringstream out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if ((*it)->type == "diagnostic") continue;   // hide diagnostics in default tree
      size_t depth = chain.size() - (it - chain.rbegin()) - 1;
      out << std::string(depth * 2, ' ') << "* [" << (*it)->type << "] " << (*it)->id;
      if ((*it)->type == "message") {
        std::string t = (*it)->msg.text;
        if (t.size() > 60) t = t.substr(0, 60) + "…";
        out << "  " << (*it)->msg.role << ": " << t;
      }
      out << "\n";
    }
    return out.str();
  }

private:
  Agent agent_;
  Session sess_;
  EventBus& bus_;
  std::string leaf_id_;
  AgentConfig cfg_;
  std::string name_;
};

} // namespace pi
