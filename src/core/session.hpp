// pi-agent: JSONL append-only session TREE (id + parentId per line)
#pragma once
#include "types.hpp"
#include "jsonutil.hpp"
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <ctime>
#include <random>
#include <algorithm>

namespace pi {

inline std::string now_id() {
  static std::mt19937_64 rng{std::random_device{}()};
  char b[32]; auto t = (uint64_t)time(nullptr);
  snprintf(b, sizeof b, "%llx%06llx", (unsigned long long)t,
           (unsigned long long)(rng() & 0xffffff));
  return b;
}
inline int64_t epoch() { return (int64_t)time(nullptr); }

inline std::string sanitize_path(const std::string& p) {
  std::string o; for (unsigned char c : p) o += (isalnum(c) ? (char)c : '-');
  return o;
}
inline void mkdirs(const std::string& path) {
  std::string cur; size_t i = 0;
  if (!path.empty() && path[0] == '/') cur = "/";
  for (i = cur.size(); i < path.size(); ++i) {
    cur += path[i];
    if (path[i] == '/' || i + 1 == path.size())
      ::mkdir(cur.c_str(), 0755);   // ignore EEXIST
  }
}
inline std::string home_dir() {
  const char* h = getenv("HOME");
  if (h) return h;
  struct passwd* pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "/tmp";
}

class Session {
public:
  std::string id;            // file stem
  std::string file;          // full jsonl path
  std::vector<Node> nodes;   // in file order (append order)

  static std::string root_for(const std::string& cwd) {
    return home_dir() + "/.pi/agent/sessions/" + sanitize_path(cwd);
  }

  void open_new(const std::string& cwd) {
    id = now_id();
    std::string dir = root_for(cwd);
    mkdirs(dir);
    file = dir + "/" + id + ".jsonl";
    Node hdr; hdr.id = id; hdr.parent_id = ""; hdr.timestamp = epoch();
    hdr.type = "session";
    append(hdr);
    Node diag; diag.id = now_id(); diag.parent_id = id; diag.timestamp = epoch();
    diag.type = "diagnostic";
    diag.summary_json = "{\"event\":\"session_start\",\"cwd\":" + ju::q(cwd) + "}";
    append(diag);
  }
  bool resume(const std::string& path) {
    file = path;
    auto pos = path.find_last_of('/');
    std::string stem = path.substr(pos + 1);
    auto dot = stem.find(".jsonl");
    id = stem.substr(0, dot);
    if (!load()) return false;
    Node diag; diag.id = now_id(); diag.parent_id = nodes.empty() ? "" : nodes.back().id;
    diag.timestamp = epoch(); diag.type = "diagnostic";
    diag.summary_json = "{\"event\":\"session_resume\",\"path\":" + ju::q(path) + "}";
    append(diag);
    return true;
  }

  void append(const Node& n) {
    nodes.push_back(n);
    std::ofstream f(file, std::ios::app);
    f << to_jsonl(n) << "\n";
    f.flush();
  }

  bool load() {
    nodes.clear();
    std::ifstream f(file);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty()) continue;
      ju::Doc d;
      if (!d.parse(line)) continue;         // tolerate corrupt lines
      nodes.push_back(from_jsonl(d.root));
    }
    return true;
  }

  const Node* find(const std::string& nid) const {
    for (auto& n : nodes) if (n.id == nid) return &n;
    return nullptr;
  }
  std::vector<const Node*> children_of(const std::string& nid) const {
    std::vector<const Node*> out;
    for (auto& n : nodes) if (n.parent_id == nid) out.push_back(&n);
    return out;
  }
  // ancestor chain from node to root (inclusive), leaf-first
  std::vector<const Node*> branch(const std::string& nid) const {
    std::vector<const Node*> out;
    const std::string* cur = &nid;
    std::string tmp;
    while (cur) {
      const Node* n = find(*cur);
      if (!n) break;
      out.push_back(n);
      if (n->parent_id.empty()) break;
      tmp = n->parent_id; cur = &tmp;
    }
    return out;
  }

  static std::string msg_to_json(const Message& m) {
    std::ostringstream o;
    o << "{\"role\":" << ju::q(m.role);
    if (!m.text.empty()) o << ",\"text\":" << ju::q(m.text);
    if (!m.tool_calls.empty()) {
      o << ",\"toolCalls\":[";
      for (size_t i = 0; i < m.tool_calls.size(); ++i) {
        auto& tc = m.tool_calls[i];
        o << (i ? "," : "") << "{\"id\":" << ju::q(tc.id)
          << ",\"name\":" << ju::q(tc.name)
          << ",\"args\":" << (tc.args_json.empty() ? "{}" : tc.args_json) << "}";
      }
      o << "]";
    }
    if (!m.tool_call_id.empty())
      o << ",\"toolCallId\":" << ju::q(m.tool_call_id)
        << ",\"name\":" << ju::q(m.name);
    o << "}";
    return o.str();
  }

  static std::string to_jsonl(const Node& n) {
    std::ostringstream o;
    o << "{\"id\":" << ju::q(n.id)
      << ",\"parentId\":" << (n.parent_id.empty() ? "null" : ju::q(n.parent_id))
      << ",\"timestamp\":" << n.timestamp
      << ",\"type\":" << ju::q(n.type);
    if (n.type == "message") {
      o << ",\"message\":" << msg_to_json(n.msg);
      if (!n.summary_json.empty()) o << ",\"summary\":" << n.summary_json;  // e.g. interjection marker
    }
    else if (!n.summary_json.empty()) o << ",\"summary\":" << n.summary_json;
    o << ",\"usage\":{\"input\":" << n.usage.input
      << ",\"output\":" << n.usage.output
      << ",\"cacheRead\":" << n.usage.cache_read
      << ",\"cacheWrite\":" << n.usage.cache_write << "}}";
    return o.str();
  }

  static Message msg_from_json(json_value_s* v) {
    Message m;
    if (!v) return m;
    m.role = ju::str(v, "role");
    m.text = ju::str(v, "text");
    m.tool_call_id = ju::str(v, "toolCallId");
    m.name = ju::str(v, "name");
    auto* a = ju::arr(v, "toolCalls");
    for (auto* e = a ? a->start : nullptr; e; e = e->next) {
      ToolCall tc;
      tc.id = ju::str(e->value, "id");
      tc.name = ju::str(e->value, "name");
      auto* args = ju::member(e->value, "args");
      if (args) {
        size_t sz = 0; void* w = json_write_minified(args, &sz);
        if (w) { tc.args_json.assign(static_cast<char*>(w), sz); free(w); }
      }
      m.tool_calls.push_back(std::move(tc));
    }
    return m;
  }

  static Node from_jsonl(json_value_s* root) {
    Node n;
    n.id = ju::str(root, "id");
    n.parent_id = ju::str(root, "parentId");
    n.timestamp = ju::num(root, "timestamp");
    n.type = ju::str(root, "type");
    if (auto* mv = ju::member(root, "message")) n.msg = msg_from_json(mv);
    if (auto* sv = ju::member(root, "summary")) {
      size_t sz = 0; void* w = json_write_minified(sv, &sz);
      if (w) { n.summary_json.assign(static_cast<char*>(w), sz); free(w); }
    }
    if (auto* uv = ju::member(root, "usage")) {
      n.usage.input = ju::num(uv, "input");
      n.usage.output = ju::num(uv, "output");
      n.usage.cache_read = ju::num(uv, "cacheRead");
      n.usage.cache_write = ju::num(uv, "cacheWrite");
    }
    return n;
  }
};

} // namespace pi
