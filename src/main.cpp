// pi-agent: CLI entry point (client.ts/main.ts analog): arg parsing, config,
// modes: interactive REPL | print (-p) | rpc server. Slash engine lives here.
#include <cxxopts.hpp>
#include "core/agent.hpp"
#include "core/console.hpp"
#include "core/skills.hpp"
#include "core/config.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <atomic>
#include <termios.h>
#include <unistd.h>

// this the main application that needed to be the working agent of the our program that we are going to the do 
// so lets not forget to the same thing that are required here ok
using namespace pi;

// Single-owner stdin: the REPL thread is the ONLY reader of std::cin.
// Worker-thread callbacks (tool approval) receive lines through this bus,
// so concurrent reads of cin can never happen.
struct LineBus {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::string> q;
  void push(std::string s) {
    { std::lock_guard<std::mutex> lk(m); q.push_back(std::move(s)); }
    cv.notify_one();
  }
  std::string wait() {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return !q.empty(); });
    std::string s = std::move(q.front()); q.pop_front();
    return s;
  }
};

// Read a line with echo disabled (API keys).

struct Opts {
  std::string base_url, api_key, model, prompt, resume, tools_csv, system_prompt;
  bool rpc = false; long window = 8192; int rpc_port = 8787;
  bool approve_tools = false;
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
      ("approve-tools","Ask approval before bash/write/edit")
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
    o.approve_tools = a.count("approve-tools") > 0;
  } catch (const cxxopts::exceptions::exception& e) {
    std::cerr << "pi: " << e.what() << "\n"; return 2;
  }

  // 1) legacy ~/.pi_agent.env — set missing env vars (flags still win below)
  if (const char* home = getenv("HOME")) {
    std::ifstream ef(std::string(home) + "/.pi_agent.env");
    std::string ln;
    while (std::getline(ef, ln)) {
      if (ln.empty() || ln[0] == '#') continue;
      auto eq = ln.find('='); if (eq == std::string::npos) continue;
      std::string k = ln.substr(0, eq);
      if (!getenv(k.c_str())) setenv(k.c_str(), ln.substr(eq + 1).c_str(), 0);
    }
  }

  // 2) provider profiles (~/.pi/config.json, written by /login)
  PiConfig picfg;
  picfg.load();
  ProviderProfile active_prof;
  if (!picfg.active.empty()) {
    auto it = picfg.profiles.find(picfg.active);
    if (it != picfg.profiles.end()) active_prof = it->second;
  }

  // 3) resolve endpoint: CLI flag > env var > active profile > builtin default
  if (o.base_url.empty()) {
    if (const char* e = getenv("PI_BASE_URL")) o.base_url = e;
    else if (!active_prof.base_url.empty()) o.base_url = active_prof.base_url;
  }
  if (o.base_url.empty())
    o.base_url = (getenv("NVIDIA_API_KEY") || getenv("PI_API_KEY"))
                   ? "https://integrate.api.nvidia.com/v1"
                   : "http://localhost:1234/v1";
  if (o.api_key.empty()) {
    if (const char* e = getenv("PI_API_KEY")) o.api_key = e;
    else if (const char* e = getenv("NVIDIA_API_KEY")) o.api_key = e;
    else if (const char* e = getenv("OPENAI_API_KEY")) o.api_key = e;
    else o.api_key = active_prof.api_key;
  }
  if (o.model.empty()) {
    if (const char* e = getenv("PI_MODEL")) o.model = e;
    else if (!active_prof.model.empty()) o.model = active_prof.model;
    else o.model = o.base_url.find("nvidia") != std::string::npos
                     ? "nvidia/nemotron-3.5-lightning-30b-a3b" : "local-model";
  }

  // TTY mode: pin the input line at the bottom and route every stdout/stderr
  // write through Console rows, so thinking/status render ABOVE the input line
  // and the caret stays still while typing (even mid-stream, for interjections).
  bool tty_mode = ::isatty(STDIN_FILENO) == 1;
  Console::ref().set_tty(tty_mode, "\x1b[36m› \x1b[0m");
  CoutBoard cboard(Console::ref()), eboard(Console::ref());
  std::streambuf *saved_cout = nullptr, *saved_cerr = nullptr;
  if (tty_mode) {
    saved_cout = std::cout.rdbuf(&cboard);
    saved_cerr = std::cerr.rdbuf(&eboard);
  }

  // Log startup config (never print the key)
  std::cerr << "pi: starting agent config\n"
            << "  profile=" << (picfg.active.empty() ? "(none)" : picfg.active) << "\n"
            << "  base_url=" << o.base_url << "\n"
            << "  model=" << o.model << "\n"
            << "  tools=" << o.tools_csv << "\n"
            << "  window=" << o.window << "\n";

  char cwdbuf[4096]; if (getcwd(cwdbuf, sizeof cwdbuf)); 
  AgentConfig cfg; cfg.provider.base_url = o.base_url; cfg.provider.api_key = o.api_key;
  cfg.provider.model = o.model; cfg.provider.context_window = o.window; cfg.cwd = cwdbuf;

  EventBus bus;
  bus.on("tool_call", [](const std::string& p){ std::cerr << "\x1b[33m⚙ " << p << "\x1b[0m\n"; });
  bus.on("tool_result", [](const std::string& p){ std::cerr << "\x1b[32m⚙ result:\x1b[0m\n" << p << "\n"; });
  bus.on("agent_end", [](const std::string&){ std::cerr << "pi: agent finished\n"; });
  bus.on("compaction", [](const std::string& p){ std::cerr << "pi: compaction: " << p << "\n"; });
  bus.on("goal_achieved", [](const std::string& p){ std::cerr << "pi: goal achieved: " << p << "\n"; });
  AgentSession session(cfg, bus, o.resume);
  if (session.session_id().empty()) {
    session.open_new(cfg.cwd);
  }
  if (!o.tools_csv.empty()) {
    std::vector<std::string> names; std::stringstream ss(o.tools_csv); std::string t;
    while (std::getline(ss, t, ',')) if (!t.empty()) names.push_back(t);
    session.agent().registry().set_allowlist(names);
  }
  ToolRegistry::register_all(session.agent().registry(), cfg.cwd);
  if (!o.system_prompt.empty()) session.agent().context().base_prompt = o.system_prompt;
  Skills::scan(session.agent().context(), cfg.cwd);

  // ---- interactivity state (REPL thread owns stdin; workers never read it)
  LineBus linebus;
  std::atomic<bool> running{false};          // a turn is executing on worker
  std::atomic<bool> approval_waiting{false}; // worker awaits an approval answer
  std::thread worker;
  std::vector<std::string> last_models;      // last /model listing (for <n> picks)
  bool interactive = false;                  // true only inside the REPL loop

  // Start a turn on the worker thread so the REPL keeps reading input:
  // plain text typed meanwhile is queued and injected at the next LLM step.
  auto start_turn = [&](const std::string& text) {
    if (worker.joinable()) worker.join();    // previous turn already finished
    running = true;
    worker = std::thread([&session, &running, text] {
      session.prompt(text);
      session.agent().clear_stop();
      Console::ref().message("");          // close the reply's row before the next prompt
      running = false;
    });
    Console::ref().message("(running — type to queue an interjection, /abort to stop)");
  };

  // Optional dangerous-tool approval policy — answer comes from the REPL
  // thread via linebus (the ONLY std::cin reader), never a second cin read.
  if (o.approve_tools) {
    session.agent().registry().set_approval([&](const std::string& name, const std::string& args_json) -> bool {
      static const std::set<std::string> guarded = {"bash", "write", "edit", "grep", "find"};
      if (!guarded.count(name)) return true;
      approval_waiting = true;
      std::cerr << "\n[approve] allow " << name << " " << args_json << "? [y/N]: " << std::flush;
      std::string ans = linebus.wait();
      approval_waiting = false;
      return ans == "y" || ans == "Y" || ans == "yes" || ans == "YES";
    });
  }

  // TTY mode banner (row above the pinned input line).
  Console::ref().message("pi | model=" + o.model + " | " + o.base_url);
  Console::ref().message("session: " + session.session_file());
  Console::ref().message("commands: /help /login /model — while a turn runs, type to queue an interjection, /abort to stop");

  // Apply a /model spec: "<n>" (from last listing), "<profile>",
  // "<profile>:<model>", or a bare "<model-id>" on the current endpoint.
  auto apply_model_spec = [&](std::string spec) {
    if (spec.empty()) {
      std::cout << "usage: /model            list models (pick with /model <n>)\n"
                   "       /model <n>        select from last listing\n"
                   "       /model <id>       set model id on current endpoint\n"
                   "       /model <profile>  switch provider profile\n"
                   "       /model <profile>:<id>  switch provider + model\n";
      return;
    }
    if (spec.find_first_not_of("0123456789") == std::string::npos && !last_models.empty()) {
      int n = std::atoi(spec.c_str());
      if (n >= 1 && n <= (int)last_models.size()) spec = last_models[n - 1];
      else { std::cout << "pick a number 1.." << last_models.size() << "\n"; return; }
    }
    std::string prov, bare;
    auto colon = spec.find(':');
    if (colon != std::string::npos && picfg.profiles.count(spec.substr(0, colon))) {
      prov = spec.substr(0, colon); bare = spec.substr(colon + 1);
    } else if (picfg.profiles.count(spec) && spec.find('/') == std::string::npos) {
      prov = spec;   // plain profile name -> switch provider
    }
    if (!prov.empty()) {
      auto& p = picfg.profiles[prov];
      std::string model = bare.empty() ? p.model : bare;
      if (model.empty()) model = PiConfig::pick_chat_model(p.models, session.agent().provider_cfg().model);
      if (model.empty()) model = session.agent().provider_cfg().model;  // keep current
      p.model = model;
      picfg.active = prov;
      picfg.save();
      session.switch_endpoint(p.base_url, p.api_key, model);
      o.model = model;
      std::cout << "provider=" << prov << "  model=" << model << "\n  base_url=" << p.base_url << "\n";
      return;
    }
    // bare model id on the current endpoint (persist to active profile)
    session.set_model(spec);
    o.model = spec;
    if (!picfg.active.empty() && picfg.profiles.count(picfg.active)) {
      picfg.profiles[picfg.active].model = spec;
      picfg.save();
    }
    std::cout << "model=" << spec << "  base_url=" << session.agent().provider_cfg().base_url << "\n";
  };

  auto handle_line = [&](const std::string& raw) -> bool {
    std::string line = raw;
    if (!line.empty() && line.back() == '\n') line.pop_back();
    if (line.empty()) return true;
    if (line[0] == '/') {
      std::string cmd = line.substr(1);
      if (cmd == "exit" || cmd == "quit") return false;
      if (cmd == "abort" || cmd == "stop") {
        Console::ref().message("nothing running — /abort only stops an active turn");
        return true;
      }
      if (cmd == "help") {
        std::cout << "Slash commands:\n"
                  << "  /help\n"
                  << "  /login               list provider profiles (keys masked)\n"
                  << "  /login <name>        connect: openai|nvidia|hf|ollama|lmstudio|llamacpp|<custom>\n"
                  << "  /logout <name>       remove a saved profile\n"
                  << "  /model               list models of the active endpoint (then /model <n>)\n"
                  << "  /model <id|n|profile>|profile:id   switch model/provider at any time\n"
                  << "  /session\n"
                  << "  /new\n"
                  << "  /resume:<path>\n"
                  << "  /name:<name>\n"
                  << "  /export:<path-or-empty>\n"
                  << "  /copy\n"
                  << "  /usage\n"
                  << "  /tree\n"
                  << "  /fork:<node-id>\n"
                  << "  /compact\n"
                  << "  /skill:<name>\n"
                  << "  /abort               stop the running turn (while running: type to queue, /abort)\n"
                  << "  /exit, /quit\n";
        return true;
      }
      if (cmd == "login" || cmd.rfind("login ", 0) == 0) {
        std::string name = cmd == "login" ? "" : cmd.substr(6);
        if (name.empty()) {
          std::cout << "profiles (* = active), keys masked:\n";
          if (picfg.profiles.empty())
            std::cout << "  (none — try /login nvidia | openai | hf | ollama | lmstudio | llamacpp | custom)\n";
          for (auto& [n, p] : picfg.profiles)
            std::cout << "  " << (n == picfg.active ? "*" : " ") << " " << n
                      << "  " << p.base_url
                      << "  key=" << PiConfig::mask_key(p.api_key)
                      << "  model=" << (p.model.empty() ? "(unset)" : p.model) << "\n";
          return true;
        }
        ProviderProfile p;
        auto it = picfg.profiles.find(name);
        if (it != picfg.profiles.end()) p = it->second;
        else {
          p = PiConfig::preset(name);
          if (p.base_url.empty()) {   // custom provider
            std::string u = Console::ref().ask("base URL (OpenAI-compatible): ");
                if (u.empty()) { Console::ref().message("cancelled"); return true; }
                p.base_url = u;
            }
        }
        if (!PiConfig::is_local(p.base_url)) {
            if (!p.api_key.empty()) {
              std::string k = Console::ref().ask("API key [" + PiConfig::mask_key(p.api_key) + ", Enter=keep, -=clear]: ", true);
              if (k == "-") p.api_key.clear();
              else if (!k.empty()) p.api_key = k;
            } else {
              p.api_key = Console::ref().ask("API key (hidden): ", true);
            }
        } else {
            Console::ref().message("local endpoint — no API key needed");
        }
        std::vector<std::string> models; std::string err;
        std::cout << "probing " << p.base_url << "/models ... " << std::flush;
        bool ok = Provider::list_models(p.base_url, p.api_key, models, err);
        if (ok) { p.models = models; std::cout << models.size() << " models\n"; }
        else {
            std::cout << "failed (" << err << ")\n" << std::flush;
            std::string ans = Console::ref().ask("save profile anyway? [y/N]: ");
            if (ans != "y" && ans != "Y" && ans != "yes") { Console::ref().message("not saved"); return true; }
        }
        picfg.profiles[name] = p;
        picfg.active = name;
        picfg.save();
        std::string model = p.model;
        if (model.empty()) model = PiConfig::pick_chat_model(p.models, session.agent().provider_cfg().model);
        if (model.empty()) model = session.agent().provider_cfg().model;
        p.model = model;
        picfg.profiles[name] = p;   // persist chosen default
        picfg.save();
        session.switch_endpoint(p.base_url, p.api_key, model);
        o.model = model;
        std::cout << "logged in: profile=" << name << " base_url=" << p.base_url
                  << " model=" << model << "\n  (saved to " << picfg.path << ")\n";
        return true;
      }
      if (cmd.rfind("logout ", 0) == 0) {
        std::string name = cmd.substr(7);
        if (!picfg.profiles.erase(name)) { std::cout << "no profile " << name << "\n"; return true; }
        if (picfg.active == name) picfg.active.clear();
        picfg.save();
        std::cout << "removed profile " << name << " (current endpoint unchanged)\n";
        return true;
      }
      if (cmd == "model" || cmd.rfind("model ", 0) == 0) {
        std::string arg = cmd == "model" ? "" : cmd.substr(6);
        if (arg.empty()) {
          auto& pc = session.agent().provider_cfg();
          std::vector<std::string> models; std::string err;
          if (Provider::list_models(pc.base_url, pc.api_key, models, err)) {
            last_models = models;
            if (!picfg.active.empty() && picfg.profiles.count(picfg.active)) {
              picfg.profiles[picfg.active].models = models;
              picfg.save();
            }
          } else {
            auto it2 = picfg.profiles.find(picfg.active);
            if (it2 != picfg.profiles.end() && !it2->second.models.empty()) {
              models = it2->second.models;
              std::cout << "live list failed (" << err << "); using cached list\n";
            } else {
              std::cout << "cannot list models: " << err << "\n";
              return true;
            }
            last_models = models;
          }
          std::cout << "models at " << pc.base_url << " (" << models.size() << "):\n";
          for (size_t i = 0; i < models.size(); ++i)
            std::cout << "  " << (i + 1) << ". " << models[i]
                      << (models[i] == pc.model ? "  *" : "") << "\n";
          std::cout << "(* = current) pick: /model <n>\n";
          return true;
        }
        apply_model_spec(arg);
        return true;
      }
      if (cmd.rfind("model:", 0) == 0) {   // legacy form
        apply_model_spec(cmd.substr(6));
        return true;
      }
      if (cmd == "compact") { std::cout << session.compact() << "\n"; return true; }
      if (cmd == "session") { std::cout << session.session_info() << "\n"; return true; }
      if (cmd == "new") { session.new_session(); std::cerr << "new session: " << session.session_file() << "\n"; return true; }
      if (cmd.rfind("resume:", 0) == 0) {
        std::string path = cmd.substr(7);
        auto out = session.resume_session(path);
        if (!out.empty()) std::cerr << "resumed: " << out << "\n";
        else std::cerr << "cannot resume " << path << "\n";
        return true;
      }
      if (cmd.rfind("name:", 0) == 0) {
        std::string nm = cmd.substr(5);
        session.set_name(nm);
        std::cout << "named session as '" << nm << "'\n";
        return true;
      }
      if (cmd.rfind("export:", 0) == 0) {
        std::string outp = cmd.substr(7);
        if (outp.empty()) { std::cout << "session file: " << session.session_file() << "\n"; return true; }
        bool ok = session.export_to(outp);
        std::cout << (ok ? "exported to " + outp : "export failed") << "\n";
        return true;
      }
      if (cmd == "copy") {
        std::string last = session.last_assistant_text();
        if (last.empty()) { std::cout << "no assistant reply yet\n"; return true; }
        std::cout << last << "\n";
        return true;
      }
      if (cmd == "usage") {
        std::cout << session.last_usage_text() << "\n";
        return true;
      }
      if (cmd == "tree") {
        std::cout << session.tree_dump();
        return true;
      }
      if (cmd.rfind("fork:", 0) == 0) {
        std::string nid = cmd.substr(5);
        if (session.session().find(nid)) { session.fork(nid); std::cout << "forked at " << nid << "\n"; }
        else std::cout << "node not found: " << nid << "\n";
        return true;
      }
      if (cmd.rfind("skill:", 0) == 0) {
        std::string sk = cmd.substr(6);
        std::string exp = Skills::expand_skill(session.agent().context(), sk);
        if (exp.empty()) { std::cout << "unknown skill: " << sk << "\n"; return true; }
        if (interactive) start_turn(exp);
        else { session.prompt(exp); std::cout << "\n"; }
        return true;
      }
      std::cout << "unknown command (try /help)\n";
      return true;
    }
    if (interactive) start_turn(line);
    else { session.prompt(line); std::cout << "\n"; }
    return true;
  };

  if (o.rpc) {
    httplib::Server svr;
    svr.set_payload_max_length(8u<<20);
    svr.Post("/rpc", [&](const httplib::Request& req, httplib::Response& res) {
      ju::Doc d; if (!d.parse(req.body)) { res.status=400; res.set_content("{\"error\":\"bad json\"}","application/json"); return; }
      std::string method = ju::str(d.root,"method");
      std::string params = ju::str(d.root,"params");
      if (method == "prompt" || method == "session.prompt") {
        std::ostringstream cap;
        session.set_out(&cap);
        std::string reply = session.prompt(params);
        session.set_out(nullptr);
        res.set_content("{\"reply\":" + ju::q(reply) + ",\"session\":" + ju::q(session.session_file()) + "}","application/json");
      } else if (method == "sessions" || method == "session.list") {
        res.set_content("{\"file\":" + ju::q(session.session_file()) + ",\"nodes\":" + std::to_string(session.session().nodes.size()) + "}","application/json");
      } else if (method == "compact" || method == "session.compact") {
        std::string reply = session.compact();
        res.set_content("{\"compact\":" + ju::q(reply) + "}","application/json");
      } else if (method == "tree" || method == "session.tree") {
        res.set_content("{\"tree\":" + ju::q(session.tree_dump()) + "}","application/json");
      } else if (method == "fork" || method == "session.fork") {
        if (!params.empty()) {
          session.fork(params);
          res.set_content("{\"forked\":" + ju::q(params) + "}","application/json");
        } else { res.status = 400; res.set_content("{\"error\":\"fork requires params=<node-id>\"}","application/json"); }
      } else if (method == "kill" || method == "agent.abort") {
        session.set_kill_switch(true);
        res.set_content("{\"kill_switch\":true}","application/json");
      } else { res.status = 404; res.set_content("{\"error\":\"unknown method\"}","application/json"); }
    });
    std::cerr << "RPC on http://127.0.0.1:" << o.rpc_port << "/rpc\n";
    svr.listen("127.0.0.1", o.rpc_port);
    if (tty_mode) { std::cout.rdbuf(saved_cout); std::cerr.rdbuf(saved_cerr); }
    return 0;
  }

  if (!o.prompt.empty()) {           // print-to-stdout mode
    handle_line(o.prompt);
    if (worker.joinable()) worker.join();
    return 0;
  }

  // interactive REPL (async: turns run on a worker, stdin stays live for
  // interjections + /abort; TTY mode pins the input line, everything else
  // renders above it)
  interactive = true;
  auto restore_streams = [&]() {
    if (tty_mode) { std::cout.rdbuf(saved_cout); std::cerr.rdbuf(saved_cerr); }
  };
  std::string line;
  while (true) {
    bool got;
    if (tty_mode) {
      got = Console::ref().read_line(line) > 0;    // renders prompt + caret itself
    } else {
      Console::ref().prompt("\x1b[36m› \x1b[0m");
      got = (bool)std::getline(std::cin, line);
    }
    if (!got) {
      if (running.load()) { session.agent().request_stop(); if (worker.joinable()) worker.join(); }
      break;
    }
    if (approval_waiting.load()) { linebus.push(line); continue; }   // answer for approval cb
    if (running.load()) {
      if (line == "/abort" || line == "/stop") {
        session.agent().request_stop();
        Console::ref().message("aborting at the next step...");
        continue;
      }
      if (line == "/exit" || line == "/quit") {
        session.agent().request_stop();
        if (worker.joinable()) worker.join();
        break;
      }
      if (!line.empty() && line[0] == '/') { Console::ref().message("[busy] agent running — /abort or /exit only"); continue; }
      if (line.empty()) continue;
      session.agent().queue_interjection(line);
      Console::ref().message("[queued] injected at the next LLM step: " + line);
      continue;
    }
    if (!handle_line(line)) break;
  }
  if (worker.joinable()) worker.join();
  restore_streams();
  return 0;
}
