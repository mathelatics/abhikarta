// pi-agent: event bus (extensions subscribe to loop events)
#pragma once
#include "types.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pi {

class EventBus {
public:
  using Handler = std::function<void(const std::string& payload)>;
  void on(const std::string& ev, Handler h) {
    std::lock_guard<std::mutex> g(m_); subs_[ev].push_back(std::move(h));
  }
  void emit(const std::string& ev, const std::string& payload = "") {
    std::vector<Handler> copy;
    { std::lock_guard<std::mutex> g(m_); copy = subs_[ev]; }
    for (auto& h : copy) h(payload);
  }
private:
  std::mutex m_;
  std::unordered_map<std::string, std::vector<Handler>> subs_;
};

} // namespace pi
