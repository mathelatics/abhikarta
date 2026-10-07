// pi-agent: CLI entry point (client.ts/main.ts analog): arg parsing, config,
// modes: interactive REPL | print (-p) | rpc server. Slash engine lives here.
#include <cxxopts.hpp>
#include "core/agent.hpp"
#include "core/skills.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>

using namespace pi;

struct Opts {
  std::string base_url, api_key, model, prompt, resume, tools_csv, system_prompt;
  bool rpc = false; long window = 8192; int rpc_port = 8787;
};

int main(int argc, char** argv) {
  Opts o;
  try {
    cxxopts::Options cx("pi", "minimal Pi-style coding agent");
    cx.add_options()
      ("b,base-url","OpenAI-compatible base URL (e.g. https://integrate.api.nvidia.com/v1 or http://localhost:1234/v1)",cxxopts::value<std::string>()->default_value(""))
      ("k,api-key","API key (or env PI_API_KEY / NVIDIA_API_KEY)",cxxopts::value<std::string>()->default_value(""))
      ("m,model","model id",cxxopts::value<std::string>()->default_value(""))
      ("p,prompt","print mode: run one prompt and exit",cxxopts::value<std::string>()->default_value(""))
      ("r,resume","resume session jsonl path",cxxopts::value<std::string>()->default_value(""))
      ("t,tools","comma-separated tool allow-list (e.g. read,grep,find)",cxxopts::value<std::vector<std::string>>()->default_value(""))
      ("system-prompt","override system prompt text",cxxopts::value<std::string>()->default_value(""))
      ("window","context window tokens",cxxopts::value<long>()->default_value("8192"))
      ("rpc","run RPC HTTP server mode")
      ("port","RPC port",cxxopts::value<int>()->default_value("8787"))
      ("h,help","show help");
    auto a = cx.parse(argc, argv);
    if (a.count("help")) { std::cout << cx.help(); return 0; }
    o.base_url = a["base-url"].as<std::string>();
    o.api_key  = a["api-key"].as<std::string>();
    o.model    = a["model"].as<std::string>();
    o.prompt   = a["prompt"].as<std::string>();
    o.resume   = a["resume"].as<std::string>();
    o.tools_csv= "";  for (auto& t : a["tools"].as<std::vector<std::string>>()) o.tools_csv += (o.tools_csv.empty()?"":",")+t;
    o.system_prompt = a["system-prompt"].as<std::string>();
    o.window = a["window"].as<long>();
    o.rpc = a.count("rpc") > 0;
    o.rpc_port = a["port"].as<int>();
  } catch (const cxxopts::exceptions::exception& e) {
    std::cerr << "pi: " << e.what() << "\n"; return 2;
  }

  // resolve configuration
  if (o.base_url.empty()) {
    const char* e = getenv("PI_BASE_URL");
    o.base_url = e ? e : (getenv("NVIDIA_API_KEY") ? "https://integrate.api.nvidia.com/v1"
                                                   : "http://localhost:1234/v1");
  }
  if (o.api_key.empty()) {
    if (const char* e = getenv("PI_API_KEY")) o.api_key = e;
    else if (const char* e = getenv("NVIDIA_API_KEY")) o.api_key = e;
    else if (const char* e = getenv("OPENAI_API_KEY")) o.api_key = e;
  }
  if (o.model.empty()) {
    const char* e = getenv("PI_MODEL");
    o.model = e ? e : (o.base_url.find("nvidia") != std::string::npos
                       ? "openai/gpt-oss-20b" : "local-model");
  }

  // ~/.pi_agent.env — user-local config/key file (chmod 600, never committed).
  // Loaded last so flags still win; env vars already set take precedence.
  if (const char* home = getenv("HOME")) {
    std::string envf = std::string(home) + "/.pi_agent.env";
    std::ifstream ef(envf);
    std::string ln;
    while (std::getline(ef, ln)) {
      if (ln.empty() || ln[0] == '#') continue;
      auto eq = ln.find('='); if (eq == std::string::npos) continue;
      std::string k = ln.substr(0, eq), v = ln.substr(eq + 1);
      if (!getenv(k.c_str())) setenv(k.c_str(), v.c_str(), 0);
    }
    if (o.api_key.empty()) {
      if (const char* e = getenv("PI_API_KEY")) o.api_key = e;
      else if (const char* e = getenv("NVIDIA_API_KEY")) o.api_key = e;
    }
    if (o.base_url.empty() || o.base_url == "http://localhost:1234/v1") {
      if (const char* e = getenv("PI_BASE_URL")) o.base_url = e;
    }
    if (getenv("PI_MODEL") && (o.model == "local-model" || o.model == "meta/llama-3.1-70b-instruct"))
      o.model = getenv("PI_MODEL");
  }

  char cwdbuf[4096]; if (getcwd(cwdbuf, sizeof cwdbuf)); 
  AgentConfig cfg; cfg.provider.base_url = o.base_url; cfg.provider.api_key = o.api_key;
  cfg.provider.model = o.model; cfg.provider.context_window = o.window; cfg.cwd = cwdbuf;

  EventBus bus;
  bus.on("tool_call", [](const std::string& p){ std::cerr << "\x1b[33m⚙ " << p << "\x1b[0m\n"; });
  Agent agent(cfg, bus);
  if (!o.tools_csv.empty()) {
    std::vector<std::string> names; std::stringstream ss(o.tools_csv); std::string t;
    while (std::getline(ss, t, ',')) if (!t.empty()) names.push_back(t);
    agent.registry().set_allowlist(names);
  }
  ToolRegistry::register_all(agent.registry(), cfg.cwd);
  if (!o.system_prompt.empty()) agent.context().base_prompt = o.system_prompt;
  Skills::scan(agent.context(), cfg.cwd);

  Session sess; std::string leaf;
  if (!o.resume.empty()) {
    if (!sess.resume(o.resume)) { std::cerr << "cannot resume " << o.resume << "\n"; return 1; }
    leaf = sess.nodes.back().id;
  } else {
    sess.open_new(cfg.cwd); leaf = sess.id;
  }
  std::cerr << "pi | model=" << o.model << " | " << o.base_url
            << "\nsession: " << sess.file << "\n";

  auto handle_line = [&](const std::string& raw) -> bool {
    std::string line = raw;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (line.empty()) return true;
    if (line[0] == '/') {
      std::string cmd = line.substr(1);
      if (cmd == "exit" || cmd == "quit") return false;
      if (cmd == "compact") { std::cout << agent.force_compact(sess, leaf) << "\n"; return true; }
      if (cmd == "tree") {
        auto chain = sess.branch(leaf);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
          size_t depth = chain.size() - (it - chain.rbegin()) - 1;
          std::cout << std::string(depth * 2, ' ') << "* [" << (*it)->type << "] "
                    << (*it)->id;
          if ((*it)->type == "message") {
            std::string t = (*it)->msg.text;
            if (t.size() > 60) t = t.substr(0, 60) + "…";
            std::cout << "  " << (*it)->msg.role << ": " << t;
          }
          std::cout << "\n";
        }
        return true;
      }
      if (cmd.rfind("fork:", 0) == 0) {
        std::string nid = cmd.substr(5);
        if (sess.find(nid)) { leaf = nid; std::cout << "forked at " << nid << "\n"; }
        else std::cout << "node not found: " << nid << "\n";
        return true;
      }
      if (cmd.rfind("skill:", 0) == 0) {
        std::string sk = cmd.substr(6);
        std::string exp = Skills::expand_skill(agent.context(), sk);
        if (exp.empty()) { std::cout << "unknown skill: " << sk << "\n"; return true; }
        agent.run_turn(sess, leaf, exp); std::cout << "\n"; return true;
      }
      std::cout << "unknown command (try /tree /compact /fork:<id> /skill:<name> /exit)\n";
      return true;
    }
    agent.run_turn(sess, leaf, line);
    std::cout << "\n";
    return true;
  };

  if (o.rpc) {
    httplib::Server svr;
    svr.set_payload_max_length(8u<<20);
    svr.Post("/rpc", [&](const httplib::Request& req, httplib::Response& res) {
      ju::Doc d; if (!d.parse(req.body)) { res.status=400; res.set_content("{\"error\":\"bad json\"}","application/json"); return; }
      std::string method = ju::str(d.root,"method");
      std::string params = ju::str(d.root,"params");
      if (method == "prompt") {
        std::ostringstream cap;
        agent.set_out(&cap);
        std::string reply = agent.run_turn(sess, leaf, params);
        agent.set_out(nullptr);
        res.set_content("{\"reply\":" + ju::q(reply) + ",\"session\":" + ju::q(sess.file) + "}","application/json");
      } else if (method == "sessions") {
        res.set_content("{\"file\":" + ju::q(sess.file) + ",\"nodes\":" + std::to_string(sess.nodes.size()) + "}","application/json");
      } else { res.status = 404; res.set_content("{\"error\":\"unknown method\"}","application/json"); }
    });
    std::cerr << "RPC on http://127.0.0.1:" << o.rpc_port << "/rpc\n";
    svr.listen("127.0.0.1", o.rpc_port);
    return 0;
  }

  if (!o.prompt.empty()) {           // print-to-stdout mode
    handle_line(o.prompt);
    return 0;
  }

  // interactive REPL (simple line reader; TUI module can replace this)
  std::string line;
  while (true) {
    std::cout << "\x1b[36m› \x1b[0m" << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (!handle_line(line)) break;
  }
  return 0;
}
