#include "core/tools.hpp"
#include <cassert>
#include <fstream>
#include <sstream>
#include <cstdio>
using namespace pi;
int main() {
  ToolRegistry r; ToolRegistry::register_all(r, ".");
  auto w = r.invoke("write", "{\"path\":\"/tmp/pi_tool_t.txt\",\"content\":\"line1\\nline2\\n\"}");
  assert(w.ok);
  auto rd = r.invoke("read", "{\"path\":\"/tmp/pi_tool_t.txt\"}");
  assert(rd.ok && rd.output.find("line2")!=std::string::npos);
  auto e = r.invoke("edit", "{\"path\":\"/tmp/pi_tool_t.txt\",\"oldText\":\"line2\",\"newText\":\"LINE-TWO\"}");
  assert(e.ok);
  auto rd2 = r.invoke("read", "{\"path\":\"/tmp/pi_tool_t.txt\"}");
  assert(rd2.output.find("LINE-TWO")!=std::string::npos);
  auto bash = r.invoke("bash", "{\"command\":\"echo piworks\"}");
  assert(bash.ok && bash.output.find("piworks")!=std::string::npos);
  assert(!r.allowed("grep"));
  auto g = r.invoke("grep", "{\"pattern\":\"piworks\"}");
  assert(!g.ok && g.output.find("not enabled")!=std::string::npos);
  r.set_allowlist({"read","grep","find"});
  assert(r.allowed("grep") && !r.allowed("write") && !r.allowed("bash"));
  auto wr = r.invoke("write", "{\"path\":\"/tmp/x\",\"content\":\"y\"}");
  assert(!wr.ok);
  auto g2 = r.invoke("grep", "{\"pattern\":\"LINE-TWO\",\"path\":\"/tmp/pi_tool_t.txt\"}");
  assert(g2.ok && g2.output.find("LINE-TWO")!=std::string::npos);
  printf("test_tools OK (r/b/e/w + read-only allowlist)\n");
  return 0;
}
