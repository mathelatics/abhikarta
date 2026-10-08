// pi-agent: tool registry — read/bash/edit/write (+grep/find, default OFF)
#pragma once
#include "types.hpp"
#include "jsonutil.hpp"
#include <functional>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <array>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>

namespace pi {

struct ToolResult { bool ok; std::string output; };

struct Tool {
  std::string name;
  std::string description;
  std::string schema_json;  // JSON-schema for parameters
  std::function<ToolResult(const std::string& args_json)> run;
};

inline std::string jarg(json_value_s* root, const char* key) {
  return ju::str(root, key);
}

static bool path_in_workspace(const std::string& path, const std::string& workspace_root) {
  namespace fs = std::filesystem;
  try {
    fs::path root = fs::weakly_canonical(fs::path(workspace_root));
    fs::path p(path);
    if (!p.is_absolute()) p = root / p;
    fs::path abs = fs::weakly_canonical(p);
    auto rs = root.generic_string();
    auto as = abs.generic_string();
    if (!rs.empty() && rs.back() != '/') rs += '/';
    return as == root.generic_string() || as.rfind(rs, 0) == 0;
  } catch (const std::exception&) { return false; }
}

class ToolRegistry {
public:
  std::map<std::string, Tool> tools;
  std::set<std::string> enabled;   // allow-list ("" => defaults)
  std::function<bool(const std::string&, const std::string&)> approval_;
  std::string root_;

  void reg(Tool t) { tools[t.name] = std::move(t); }

  void set_approval(std::function<bool(const std::string&, const std::string&)> cb) {
    approval_ = std::move(cb);
  }

  void set_allowlist(const std::vector<std::string>& names) {
    enabled.clear();
    for (auto& n : names) enabled.insert(n);
  }
  bool allowed(const std::string& n) const {
    return enabled.empty() ? builtin_default_on(n) : enabled.count(n) > 0;
  }
  static bool builtin_default_on(const std::string& n) {
    return n == "read" || n == "bash" || n == "edit" || n == "write";
  }

  // ---- OpenAI-compatible tool descriptors ----
  std::string schemas_json() const {
    std::ostringstream o; o << "[";
    bool first = true;
    for (auto& [name, t] : tools) {
      if (!allowed(name)) continue;
      o << (first ? "" : ",") << "{\"type\":\"function\",\"function\":{\"name\":"
        << ju::q(t.name) << ",\"description\":" << ju::q(t.description)
        << ",\"parameters\":" << t.schema_json << "}}";
      first = false;
    }
    o << "]";
    return o.str();
  }

  ToolResult invoke(const std::string& name, const std::string& args_json) {
    if (!allowed(name))
      return {false, "ERROR: tool '" + name + "' is not enabled (allow-list enforced)"};
    if (approval_ && !approval_(name, args_json))
      return {false, "ERROR: tool '" + name + "' denied by approval policy"};
    auto it = tools.find(name);
    if (it == tools.end()) return {false, "ERROR: unknown tool " + name};
    return it->second.run(args_json);
  }

  static void register_all(ToolRegistry& r, const std::string& cwd) {
    r.root_ = cwd;
    // READ
    r.reg({"read", "Read a text file. Args: {path:string, offset?:int, limit?:int}",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"path\"]}",
      [cwd](const std::string& a){
        ju::Doc d; if(!d.parse(a)) return ToolResult{false,"bad args json"};
        std::string p = jarg(d.root,"path");
        if(!path_in_workspace(p, cwd)) return ToolResult{false,"path outside workspace root: "+p};
        long off = ju::num(d.root,"offset",1), lim = ju::num(d.root,"limit",2000);
        std::ifstream f(p); if(!f) return ToolResult{false,"file not found: "+p};
        std::string line; std::ostringstream out; long i=0,n=0;
        while(std::getline(f,line)){ ++i; if(i<off) continue;
          out<<i<<"\t"<<line<<"\n"; if(++n>=lim) break; }
        return ToolResult{true,out.str()}; }});
    // BASH
    r.reg({"bash", "Run a shell command in the working directory. Args: {command:string}",
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},\"required\":[\"command\"]}",
      [cwd](const std::string& a){
        ju::Doc d; if(!d.parse(a)) return ToolResult{false,"bad args json"};
        std::string cmd = jarg(d.root,"command");
        std::string full = "cd " + cwd + " 2>/dev/null; " + cmd + " 2>&1";
        std::array<char,4096> buf; std::string out;
        FILE* pp = popen(full.c_str(),"r");
        if(!pp) return ToolResult{false,"popen failed"};
        while(fgets(buf.data(),buf.size(),pp)) out += buf.data();
        int rc = pclose(pp);
        if(out.size()>64000) out = out.substr(0,64000)+"\n[truncated]";
        std::ostringstream o; o<<out<<"\n[exit "<<WEXITSTATUS(rc)<<"]";
        return ToolResult{WEXITSTATUS(rc)==0,o.str()}; }});
    // WRITE
    r.reg({"write", "Create/overwrite a file with content. Args: {path:string, content:string}",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}",
      [cwd](const std::string& a){
        ju::Doc d; if(!d.parse(a)) return ToolResult{false,"bad args json"};
        std::string p=jarg(d.root,"path"), c=jarg(d.root,"content");
        if(!path_in_workspace(p, cwd)) return ToolResult{false,"path outside workspace root: "+p};
        std::ofstream f(p,std::ios::trunc); if(!f) return ToolResult{false,"cannot open "+p};
        f<<c; return ToolResult{true,"wrote "+std::to_string(c.size())+" bytes to "+p}; }});
    // EDIT
    r.reg({"edit", "Replace EXACT unique oldText with newText in a file. Args: {path,oldText,newText}",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"oldText\":{\"type\":\"string\"},\"newText\":{\"type\":\"string\"}},\"required\":[\"path\",\"oldText\",\"newText\"]}",
      [cwd](const std::string& a){
        ju::Doc d; if(!d.parse(a)) return ToolResult{false,"bad args json"};
        std::string p=jarg(d.root,"path"),o=jarg(d.root,"oldText"),nw=jarg(d.root,"newText");
        if(!path_in_workspace(p, cwd)) return ToolResult{false,"path outside workspace root: "+p};
        std::ifstream in(p); if(!in) return ToolResult{false,"file not found: "+p};
        std::stringstream ss; ss<<in.rdbuf(); std::string s=ss.str();
        size_t pos=s.find(o);
        if(pos==std::string::npos) return ToolResult{false,"oldText not found in "+p};
        if(s.find(o,pos+1)!=std::string::npos) return ToolResult{false,"oldText not unique in "+p};
        s.replace(pos,o.size(),nw);
        std::ofstream f(p,std::ios::trunc); f<<s;
        return ToolResult{true,"edited "+p}; }});
    // GREP / FIND — exist but DISABLED by default (read-only mode helpers)
        r.reg({"grep", "Search files for a literal string. Args: {pattern,path?}. Disabled unless allow-listed.",
      "{\"type\":\"object\",\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"pattern\"]}",
      [cwd](const std::string& a){
        ju::Doc d; if(!d.parse(a)) return ToolResult{false,"bad args json"};
        std::string pat=ju::str(d.root,"pattern"), p=ju::str(d.root,"path",cwd);
        if(!path_in_workspace(p, cwd)) return ToolResult{false,"path outside workspace root: "+p};
        int fd[2]; if(pipe(fd)<0) return ToolResult{false,"pipe failed"};
        pid_t pid=fork(); if(pid<0){ close(fd[0]); close(fd[1]); return ToolResult{false,"fork failed"}; }
        if(pid==0){ close(fd[0]); dup2(fd[1],STDOUT_FILENO); dup2(fd[1],STDERR_FILENO); close(fd[1]); const char* argv[]={"grep","-rn","-F","--",pat.c_str(),p.c_str(),nullptr}; execvp("grep",(char* const*)argv); _exit(127); }
        close(fd[1]); std::string out; char buf[4096]; ssize_t n; while((n=read(fd[0],buf,sizeof(buf)))>0) out+=std::string(buf,n);
        close(fd[0]); int st=0; waitpid(pid,&st,0);
        if(out.size()>64000) out=out.substr(0,64000)+"\n[truncated]";
        return ToolResult{true,out.empty()?"no matches":out}; }});
    r.reg({"find", "Find files by glob. Args: {glob,path?}. Disabled unless allow-listed.",
      "{\"type\":\"object\",\"properties\":{\"glob\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},\"required\":[\"glob\"]}",
      [cwd](const std::string& a){
        ju::Doc d; if(!d.parse(a)) return ToolResult{false,"bad args json"};
        std::string g=ju::str(d.root,"glob"), p=ju::str(d.root,"path",cwd);
        if(!path_in_workspace(p, cwd)) return ToolResult{false,"path outside workspace root: "+p};
        int fd[2]; if(pipe(fd)<0) return ToolResult{false,"pipe failed"};
        pid_t pid=fork(); if(pid<0){ close(fd[0]); close(fd[1]); return ToolResult{false,"fork failed"}; }
        if(pid==0){ close(fd[0]); dup2(fd[1],STDOUT_FILENO); dup2(fd[1],STDERR_FILENO); close(fd[1]); const char* argv[]={"find",p.c_str(),"-name",g.c_str(),nullptr}; execvp("find",(char* const*)argv); _exit(127); }
        close(fd[1]); std::string out; char buf[4096]; ssize_t n; while((n=read(fd[0],buf,sizeof(buf)))>0) out+=std::string(buf,n);
        close(fd[0]); int st=0; waitpid(pid,&st,0);
        if(out.size()>64000) out=out.substr(0,64000)+"\n[truncated]";
        return ToolResult{true,out.empty()?"no matches":out}; }});
  }

  static std::string ju_qsh(const std::string& s) { // shell single-quote escape
    std::string o="'"; for(char c:s){ if(c=='\'') o+="'\''"; else o+=c; } o+="'"; return o;
  }
};

} // namespace pi
