#include "core/session.hpp"
#include <cassert>
#include <cstdio>
using namespace pi;
int main() {
  std::string tmp = "/tmp/pi_test_" + now_id(); mkdirs(tmp);
  Session s; s.file = tmp + "/t.jsonl"; s.id = "t";
  Node h; h.id="100"; h.type="session"; h.timestamp=epoch(); s.append(h);
  Node a; a.id="101"; a.parent_id="100"; a.type="message"; a.msg.role="user"; a.msg.text="hello world"; s.append(a);
  Node b; b.id="102"; b.parent_id="101"; b.type="message"; b.msg.role="assistant"; b.msg.text="hi"; b.usage.input=10;b.usage.output=5; s.append(b);
  Node c; c.id="103"; c.parent_id="101"; c.type="message"; c.msg.role="user"; c.msg.text="fork branch!"; s.append(c); // bifurcation at 101
  Session r; assert(r.resume(s.file));
  int tree_nodes = 0; for (auto& n : r.nodes) if (n.type != "diagnostic") ++tree_nodes;
  assert(tree_nodes==4);
  auto kids = r.children_of("101"); assert(kids.size()==2);   // tree fork verified
  auto br = r.branch("103"); assert(br.size()==3);            // 103->101->100
  assert(br[2]->id=="100");
  assert(r.find("102")->usage.total()==15);
  assert(r.find("103")->msg.text=="fork branch!");
  printf("test_session OK (tree/fork/branch/usage roundtrip)\n");
  return 0;
}
