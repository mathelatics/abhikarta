// pi-agent: skills discovery + slash-command expansion (interactive layer)
#pragma once
#include "context.hpp"
#include <dirent.h>

namespace pi {

inline std::string read_file(const std::string& p) {
  std::ifstream f(p); if (!f) return "";
  std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Parse "---\nname: x\ndescription: y\n---" front matter
inline SkillInfo parse_skill_md(const std::string& path) {
  SkillInfo s; s.location = path;
  std::string c = read_file(path);
  if (c.rfind("---", 0) == 0) {
    auto end = c.find("\n---", 3);
    std::string fm = c.substr(3, end == std::string::npos ? 0 : end - 3);
    std::istringstream in(fm); std::string line;
    while (std::getline(in, line)) {
      auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string k = line.substr(0, colon), v = line.substr(colon + 1);
      while (!v.empty() && v[0] == ' ') v.erase(0, 1);
      if (k == "name") s.name = v;
      else if (k == "description") s.description = v;
    }
  }
  if (s.name.empty()) { // fallback: dir/file name
    auto sl = path.find_last_of('/');
    std::string stem = path.substr(sl + 1);
    auto dot = stem.find(".md");
    s.name = dot == std::string::npos ? stem : stem.substr(0, dot);
    if (s.name == "SKILL") { s.name = path.substr(0, sl).substr(path.substr(0,sl).find_last_of('/')+1); }
  }
  return s;
}

class Skills {
public:
  static void scan(ContextBuilder& ctx, const std::string& cwd) {
    for (auto& root : {home_dir() + "/.pi/agent/skills", cwd + "/.agents/skills"}) {
      DIR* d = opendir(root.c_str());
      if (!d) continue;
      while (auto* de = readdir(d)) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        std::string p = root + "/" + n;
        if (n.size() > 3 && n.substr(n.size()-3) == ".md") {
          ctx.skills.push_back(parse_skill_md(p));
        } else {
          std::string inner = p + "/SKILL.md";
          if (!read_file(inner).empty()) ctx.skills.push_back(parse_skill_md(inner));
        }
      }
      closedir(d);
    }
  }

  // /skill:name -> markup block with name/description/location (lazy-load design)
  static std::string expand_skill(const ContextBuilder& ctx, const std::string& name) {
    for (auto& s : ctx.skills) {
      if (s.name == name) {
        return "<skill name=\"" + s.name + "\" description=\"" + s.description +
               "\" location=\"" + s.location + "\">\n"
               "A skill is available. Use the read tool to read the file at the "
               "location attribute and follow its instructions.\n</skill>";
      }
    }
    return "";
  }
};

} // namespace pi
