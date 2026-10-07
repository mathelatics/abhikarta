// pi-agent: context initialization + transformation (compaction)
#pragma once
#include "types.hpp"
#include "session.hpp"
#include "tools.hpp"
#include "provider.hpp"
#include "eventbus.hpp"
#include <ctime>

namespace pi {

struct SkillInfo { std::string name, description, location; };

class ContextBuilder {
public:
  std::string base_prompt =
    "You are Pi, a helpful minimal coding agent running in a terminal.\n"
    "Rules:\n"
    "- Prefer small, precise edits. Read files before editing them.\n"
    "- Never fabricate file contents; use tools to inspect.\n"
    "- Keep answers concise unless detail is requested.\n"
    "- When a <skill .../> block appears in a user message, use the read tool on its\n"
    "  location attribute before following its instructions.\n";

  std::vector<SkillInfo> skills;

  // system.md override / append-system.md support (~/.pi/)
  std::string build_system_prompt(const std::string& cwd) const {
    std::string sp;
    std::string sys_override = home_dir() + "/.pi/system.md";
    std::ifstream fo(sys_override);
    if (fo) { std::stringstream ss; ss << fo.rdbuf(); sp = ss.str(); }
    else sp = base_prompt;

    std::string app = home_dir() + "/.pi/append-system.md";
    std::ifstream fa(app);
    if (fa) { std::stringstream ss; ss << fa.rdbuf(); sp += "\n" + ss.str(); }

    // AGENTS.md from home and cwd
    for (auto& p : {home_dir() + "/AGENTS.md", cwd + "/AGENTS.md"}) {
      std::ifstream f(p);
      if (f) { std::stringstream ss; ss << f.rdbuf();
               sp += "\n## " + std::string(p) + "\n" + ss.str(); }
    }

    // Skills descriptions (markup — also parsed by TUI)
    if (!skills.empty()) {
      sp += "\n<available_skills>\n";
      for (auto& s : skills)
        sp += "<skill name=\"" + s.name + "\" description=\"" + s.description +
              "\" location=\"" + s.location + "\"/>\n";
      sp += "</available_skills>\n";
    }

    char date[32]; time_t t = time(nullptr); struct tm lt; localtime_r(&t, &lt);
    strftime(date, sizeof date, "%Y-%m-%d", &lt);
    sp += "\nDate: " + std::string(date) + "\nWorking directory: " + cwd + "\n";
    return sp;
  }

  // Reconstruct OpenAI messages from session branch (tree walk), with optional
  // compaction summary replacing older history.
  static std::vector<Message> history_messages(const Session& sess,
                                               const std::string& leaf_id) {
    auto chain = sess.branch(leaf_id);           // leaf-first
    std::reverse(chain.begin(), chain.end());    // root-first
    std::vector<Message> msgs;
    for (auto* n : chain) {
      if (n->type == "message") msgs.push_back(n->msg);
      else if (n->type == "compaction" || n->type == "summary") {
        Message m; m.role = "user";
        m.text = "[Context summary]\n" + n->summary_json;
        msgs.push_back(m);
      }
    }
    return msgs;
  }
};

// ---- Compaction: usage-based accounting + structured checkpoint prompt ----
inline const char* SUMMARIZE_SYSTEM_PROMPT =
  "You are a context summarization assistant. Your task is to read a conversation "
  "between a user and an AI assistant. The messages above are a conversation to "
  "summarize. Create a structured context checkpoint summary that another LLM will "
  "use to continue the work, in exactly this format:\n"
  "Goal:\nConstraints and preferences:\nProgress - done:\nProgress - in progress:\n"
  "Blocked:\nKey decisions:\nNext steps:\nCritical context (preserve exact file "
  "paths, function names, and error messages):\nKeep each section concise.";

class Compactor {
public:
  double threshold = 0.85;   // fraction of context window triggering compaction

  // Pi's rule: rely on provider-reported usage when available:
  // context_tokens = input + output + cache_read + cache_write
  static long estimate_from_usage(const Usage& u) { return u.total(); }
  // bootstrap fallback ONLY before first response exists: chars/4
  static long bootstrap_estimate(const std::vector<Message>& msgs) {
    long chars = 0; for (auto& m : msgs) { chars += (long)m.text.size(); }
    return chars / 4;
  }

  bool needs_compaction(long ctx_tokens, long window) const {
    return ctx_tokens > (long)(threshold * window);
  }

  // Run one compaction turn: ask model to summarize current history.
  std::string compact(Provider& prov, const std::vector<Message>& history) {
    std::vector<Message> msgs = history;
    Message req; req.role = "user";
    req.text = SUMMARIZE_SYSTEM_PROMPT;
    msgs.push_back(req);
    auto r = prov.chat(msgs, "[]");
    return r.ok ? r.text : std::string("{\"error\":\"") + ju::esc(r.error) + "\"}";
  }
};

} // namespace pi
