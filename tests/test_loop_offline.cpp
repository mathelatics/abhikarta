// End-to-end agent loop against a MOCK OpenAI-compatible server (httplib),
// exercising: SSE streaming, tool_calls, multi-round loop, JSONL persistence,
// usage-based compaction trigger. No real API key needed.
#include "core/agent.hpp"
#include <httplib.h>
#include <thread>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
using namespace pi;

static std::atomic<int> calls{0};
static httplib::Server* g_svr = nullptr;

void start_mock(const char* port) {
  static httplib::Server svr;
  g_svr = &svr;
  svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
    int n = ++calls;
    ju::Doc d; bool has_tool_result = false;
    if (d.parse(req.body)) {
      auto* msgs = ju::arr(d.root, "messages");
      for (auto* e = msgs ? msgs->start : nullptr; e; e = e->next)
        if (ju::str(e->value, "role") == "tool") has_tool_result = true;
    }
    res.set_header("Content-Type", "text/event-stream");
    std::ostringstream chunks;
    if (!has_tool_result) {
      // round 1: text delta + tool_call deltas split across SSE chunks.
      // args arrive in two fragments: {"path":"/tmp/pi_e2e.txt"  then  , "content":"created by pi"}
      chunks << "data: {\"choices\":[{\"delta\":{\"content\":\"Sure. \",\"role\":\"assistant\"}}]}\n\n";
      chunks << "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"write\",\"arguments\":\"{\\\"path\\\":\\\"/tmp/pi_e2e.txt\\\"}}]}}]}\n\n";
      chunks << "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\", \\\"content\\\":\\\"created by pi\\\"}\"}}]}}]}\n\n";
      chunks << "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":30}}\n\n";
      chunks << "data: [DONE]\n\n";
    } else {
      chunks << "data: {\"choices\":[{\"delta\":{\"content\":\"File created.\"}}]}\n\n";
      chunks << "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":9000,\"completion_tokens\":5}}\n\n";
      chunks << "data: [DONE]\n\n";
    }
    // SSE via content provider. IMPORTANT (verified by live test): httplib's
    // no-length provider keeps re-invoking the callback from offset 0 forever
    // unless we signal completion — returning false on the SECOND call closes
    // the stream cleanly after all bytes are written.
    auto sp = std::make_shared<std::string>(chunks.str());
    auto done = std::make_shared<bool>(false);
    res.set_content_provider(
        "text/event-stream",
        [sp, done](size_t /*offset*/, httplib::DataSink& sink) {
          if (*done) return false;   // stream complete -> server closes chunked body
          *done = true;
          return sink.write(sp->data(), sp->size());
        });
  });
  svr.listen("127.0.0.1", atoi(port));
}

int main() {
  const char* port = "18923";
  std::thread t(start_mock, port);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::string tmp = "/tmp/pi_e2e_" + now_id(); mkdirs(tmp);
  setenv("HOME", tmp.c_str(), 1);  // sessions land under our temp home
  AgentConfig cfg;
  cfg.provider.base_url = std::string("http://127.0.0.1:") + port + "/v1";
  cfg.provider.model = "mock-model";
  cfg.provider.context_window = 8192;
  cfg.cwd = tmp;
  EventBus bus; int tool_events = 0, compact_events = 0;
  bus.on("tool_call", [&](const std::string&){ tool_events++; });
  bus.on("compaction", [&](const std::string&){ compact_events++; });

  Agent agent(cfg, bus);
  Session sess; sess.open_new(cfg.cwd);
  std::string leaf = sess.id;
  std::ostringstream quiet; agent.set_out(&quiet);

  auto reply = agent.run_turn(sess, leaf, "create a file /tmp/pi_e2e.txt with 'created by pi'");
  assert(reply.find("File created.") != std::string::npos);
  assert(tool_events >= 1);
  // tool actually executed:
  std::ifstream f("/tmp/pi_e2e.txt"); std::stringstream ss; ss<<f.rdbuf();
  assert(ss.str() == "created by pi");
  // usage from last call = 9000+5 => exceeds 0.85*8192=6963 => compaction fired
  assert(compact_events >= 1);
  // session persisted as JSONL tree with roles user/assistant/tool + compaction node
  Session reload; assert(reload.resume(sess.file));
  bool saw_tool=false, saw_compact=false;
  for (auto& n : reload.nodes) {
    if (n.type=="message" && n.msg.role=="tool") saw_tool=true;
    if (n.type=="compaction") saw_compact=true;
  }
  assert(saw_tool && saw_compact);
  printf("test_loop_offline OK (SSE stream + tool loop + persist + auto-compaction)\n");
  if (g_svr) g_svr->stop();
  t.join();
  return 0;
}
