// pi-agent: cooperative terminal with a bottom-pinned input line.
//
// The agent streams long output from a worker thread while the REPL reads
// keys, and every LLM wait shows a `\r` "Thinking" spinner. On a real TTY a
// naive design interleaves badly: `\r` rewrites the current cursor line every
// 120ms, so the prompt, the text being typed, busy hints and interjection
// confirmations get overwritten and "never render". 
//
// This Console owns the terminal:
//  - raw-mode key reader with its own line buffer (canonical getline echoes via
//    the kernel and cannot keep the cursor still),
//  - the input line is ALWAYS the last row (prompt + typed text + caret), while
//    the spinner / thinking / answer / status notes render as rows above it,
//  - sliding window capped at screen height so the input line can never scroll
//    away on long sessions,
//  - one mutex serializes every write; the REPL edits the buffer under the lock
//    but blocks in read() without holding it, so the worker keeps rendering.
// In non-TTY mode (pipes, -p, RPC, tests) it falls back to plain streaming.
#pragma once
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <sstream>
#include <cstddef>
#include <cctype>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>

namespace pi {

class Console {
public:
  static Console& ref() { static Console c; return c; }

  // --- mode ----------------------------------------------------------------
  void set_tty(bool on, std::string prompt) {
    std::lock_guard<std::mutex> lk(m_);
    tty_ = on;
    prompt_ = std::move(prompt);
    if (tty_) enable_raw_locked();
  }
  bool tty() const { return tty_; }

  // Non-TTY "Thinking" spinner frame (fallback path keeps \r line discipline).
  void live(const std::string& frame) {
    std::lock_guard<std::mutex> lk(m_);
    if (tty_) {
      if (claimed_) return;                 // real output owns the screen now
      if (spin_row_ == SIZE_MAX) { rows_.push_back(frame); spin_row_ = rows_.size() - 1; }
      else rows_[spin_row_] = frame;
      render_locked(false);
    } else {
      if (claimed_) return;
      live_ = true;
      std::cerr << "\r" << frame << "\x1b[K" << std::flush;
    }
  }

  // Turn the spinner down (worker finished this LLM call).
  void release() {
    std::lock_guard<std::mutex> lk(m_);
    if (tty_) { spin_row_ = SIZE_MAX; claimed_ = true; render_locked(false); }
    else { if (live_) { std::cerr << "\r\x1b[K" << std::flush; live_ = false; } claimed_ = true; }
  }

  // Call at the top of each provider request: fresh stream state.
  void reset_request() {
    std::lock_guard<std::mutex> lk(m_);
    if (tty_) { commit_open_locked(); }
    spin_row_ = SIZE_MAX;
    live_ = false; claimed_ = false; thinking_open_ = false;
  }

  // One-line message (busy hint, [queued], guard text, banners) on its own row,
  // yielding any in-flight stream tail first so it reads as a status line.
  void message(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (tty_) { commit_open_locked(); rows_.push_back(s); render_locked(true); }
    else { yield_non_tty_locked(); std::cout << s << "\n" << std::flush; }
  }

  // Non-TTY REPL prompt (no trailing newline). In TTY the input line is drawn
  // by the editor itself.
  void prompt(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (!tty_) { yield_non_tty_locked(); std::cout << s << std::flush; }
  }

  // Reasoning/thinking fragment: dim; in TTY it replaces the spinner row and
  // stays open, so the answer will start on a fresh row below it.
  void thinking(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (tty_) {
      if (spin_row_ != SIZE_MAX) {
        rows_[spin_row_] = "\x1b[2m" + s + "\x1b[0m";
        spin_row_ = SIZE_MAX; thinking_open_ = true;
      } else if (thinking_open_) {
        open_.append("\x1b[2m").append(s).append("\x1b[0m");
      } else { commit_open_locked(); open_ = "\x1b[2m" + s + "\x1b[0m"; thinking_open_ = true; }
      render_locked(false);
    } else {
      yield_non_tty_locked();
      std::cerr << "\x1b[2m" << s << "\x1b[0m" << std::flush;
      thinking_open_ = true;
    }
  }

  // Streamed answer chunk: closes the thinking line first, then appends.
  void content(std::ostream& os, const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    if (tty_) {
      if (thinking_open_) { commit_open_locked(); thinking_open_ = false; }
      for (char c : s) {
        if (c == '\n') { rows_.push_back(open_); open_.clear(); }
        else open_.push_back(c);
      }
      render_locked(false);
    } else {
      yield_non_tty_locked();
      if (thinking_open_) { os << "\n"; thinking_open_ = false; }
      os << s << std::flush;
    }
  }

  // ---- input editor (TTY only): read one submitted line -------------------
  // Returns: 1 = full line submitted (in `out`), 0 = EOF/Ctrl-D, -1 = quit
  // (Ctrl-C on an empty line). Editing never blocks output: the caller's fd
  // read handles keys while the worker keeps rendering.
  int read_line(std::string& out) { return read_line_impl(out, false); }

  // Read a line under the editor; hidden=true does not reflect keypresses
  // (API keys) and, in non-TTY mode, temporarily disables echo.
  std::string ask(const std::string& question, bool hidden = false) {
    std::string out;
    if (!tty_) {
      std::cout << question << std::flush;
      if (hidden && isatty(STDIN_FILENO)) {
        termios t{}; if (tcgetattr(STDIN_FILENO, &t) == 0) {
          termios saved = t; t.c_lflag &= ~static_cast<tcflag_t>(ECHO);
          tcsetattr(STDIN_FILENO, TCSANOW, &t);
          std::getline(std::cin, out);
          tcsetattr(STDIN_FILENO, TCSANOW, &saved);
          std::cout << "\n";
          return out;
        }
      }
      std::getline(std::cin, out);
      return out;
    }
    message(question);                              // status row above input
    hidden_ = hidden;
    out.clear();
    read_line_impl(out, hidden);
    hidden_ = false;
    return out;
  }

private:
  Console() = default;

  void enable_raw_locked() {
    if (raw_ready_) return;
    if (tcgetattr(STDIN_FILENO, &orig_) == 0) {
      termios raw = orig_;
      raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
      raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL | IXON);
      tcsetattr(STDIN_FILENO, TCSANOW, &raw);
      raw_ready_ = true;
      ::write(STDOUT_FILENO, "\x1b[?2004h", 8);     // bracketed paste markers
      ::write(STDOUT_FILENO, "\x1b[?25h", 6);       // show cursor (block)
    }
  }
  void restore_raw_locked() {
    if (raw_ready_) {
      ::write(STDOUT_FILENO, "\x1b[?2004l", 8);     // stop bracketed paste
      tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
      raw_ready_ = false;
    }
  }

  ~Console() { std::lock_guard<std::mutex> lk(m_); restore_raw_locked(); }

  int read_line_impl(std::string& out, bool hidden) {
    if (!tty_) { if (!std::getline(std::cin, out)) return 0; return 1; }
    buf_.clear(); cur_ = 0; hist_pos_ = -1; draft_.clear();
    in_paste_ = false; paste_cap_.clear();
    render_locked(true);
    while (true) {
      char c; ssize_t n = read(STDIN_FILENO, &c, 1);
      if (n <= 0) { restore_raw_locked(); return 0; }   // EOF / raw loss
      unsigned char u = static_cast<unsigned char>(c);
      { std::lock_guard<std::mutex> lk(m_);
        // ---- bracketed paste: insert raw text verbatim until the end marker
        if (in_paste_) {
          if (u == '\r') u = '\n';                    // normalize CR/CRLF
          paste_cap_.push_back(static_cast<char>(u));
          if (paste_cap_.size() >= 6 &&
              paste_cap_.compare(paste_cap_.size() - 6, 6, "\x1b[201~") == 0) {
            paste_cap_.erase(paste_cap_.size() - 6);  // strip end marker
            buf_.insert(buf_.begin() + cur_, paste_cap_.begin(), paste_cap_.end());
            cur_ += paste_cap_.size();
            in_paste_ = false; paste_cap_.clear();
            render_locked(true);
          } else {
            render_locked(false);                     // throttled live preview
          }
          continue;
        }
        if (u == '\r') {                              // Enter submits the buffer
          { struct pollfd p{STDIN_FILENO, POLLIN, 0};  // swallow queued LF after CR
            char nl; int rd;
            while ((rd = poll(&p, 1, 1)) == 1 && read(STDIN_FILENO, &nl, 1) == 1 && nl == '\n') {}
          }
          out = buf_;
          remember_history(out);
          buf_.clear(); cur_ = 0; hist_pos_ = -1; draft_.clear();
          render_locked(true); return 1;
        }
        else if (u == 10) {                           // Ctrl-J: newline in buffer
          buf_.insert(buf_.begin() + cur_, '\n'); ++cur_; render_locked(true);
        }
        else if (u == 127 || u == 8) {                // backspace
          if (cur_ > 0) { buf_.erase(cur_ - 1, 1); --cur_; } render_locked(true);
        }
        else if (u == 3) {                            // Ctrl-C: clear, or exit when empty
          if (!buf_.empty()) { buf_.clear(); cur_ = 0; render_locked(true); }
          else { restore_raw_locked(); return -1; }
        }
        else if (u == 4) {                            // Ctrl-D: delete char / EOF
          if (cur_ < buf_.size()) { buf_.erase(cur_, 1); render_locked(true); }
          else { restore_raw_locked(); return 0; }
        }
        else if (u == 1) { cur_ = 0; render_locked(true); }                    // Ctrl-A home
        else if (u == 5) { cur_ = buf_.size(); render_locked(true); }          // Ctrl-E end
        else if (u == 2) { cur_ = cur_ > 0 ? cur_ - 1 : 0; render_locked(true); }// Ctrl-B
        else if (u == 6) { cur_ = cur_ < buf_.size() ? cur_ + 1 : buf_.size(); render_locked(true); } // Ctrl-F
        else if (u == 11) { buf_.erase(cur_); render_locked(true); }           // Ctrl-K kill to end
        else if (u == 21) { buf_.erase(0, cur_); cur_ = 0; render_locked(true); }// Ctrl-U kill to start
        else if (u == 23) { size_t s = word_start(cur_); buf_.erase(s, cur_ - s); cur_ = s; render_locked(true); } // Ctrl-W
        else if (u == 12) { render_locked(true); }                             // Ctrl-L repaint
        else if (u == 20) { if (cur_ > 0 && cur_ < buf_.size()) { std::swap(buf_[cur_ - 1], buf_[cur_]); ++cur_; } render_locked(true); } // Ctrl-T
        else if (u == 27) { handle_escape_locked(); render_locked(true); }     // arrows / alt / paste-start
        else if (u == 9) { buf_.insert(cur_, 2, ' '); cur_ += 2; render_locked(true); }   // Tab → 2 spaces
        else if (u >= 32 && u <= 126) { buf_.insert(buf_.begin() + cur_, static_cast<char>(u)); ++cur_; render_locked(true); }
        else render_locked(true);                   // other control bytes: ignore
      }
    }
  }

  // Arrow/Home/End/Delete, CSI-u (Shift+Enter), and meta/alt keys (ESC b/f/d).
  // Bracketed-paste start (ESC[200~) flips in_paste_ so the reader pastes raw.
  void handle_escape_locked() {
    struct pollfd p{STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, 60) != 1) return;
    char a; if (read(STDIN_FILENO, &a, 1) != 1) return;
    if (a != '[') {                                 // meta/alt key: ESC <char>
      if (a == 'b') cur_ = word_start(cur_);                          // word back
      else if (a == 'f') cur_ = word_end(cur_);                       // word forward
      else if (a == 'd') buf_.erase(cur_, word_end(cur_) - cur_);     // kill word fwd
      else if (a == 'a') cur_ = 0;
      else if (a == 'e') cur_ = buf_.size();
      else if (a == 'u') { buf_.erase(0, cur_); cur_ = 0; }
      else if (a == 'k') buf_.erase(cur_);
      return;
    }
    if (poll(&p, 1, 60) != 1) return;
    char first; if (read(STDIN_FILENO, &first, 1) != 1) return;

    auto read_more = [&](std::string& code, char& last, int timeout) -> bool {
      while (code.size() < 8) {
        if (poll(&p, 1, timeout) != 1) return false;
        char x; if (read(STDIN_FILENO, &x, 1) != 1) return false;
        code.push_back(x);
        if (x == '~' || x == 'u' || isalpha(static_cast<unsigned char>(x))) { last = x; return true; }
        if (x != ';' && !isdigit(static_cast<unsigned char>(x))) { last = x; return true; }
      }
      return true;
    };

    if (first == 'A') { if (!hidden_) history_up(); return; }
    if (first == 'B') { if (!hidden_) history_down(); return; }
    if (first == 'C') { cur_ = std::min<size_t>(buf_.size(), cur_ + 1); return; }
    if (first == 'D') { cur_ = cur_ > 0 ? cur_ - 1 : 0; return; }
    if (first == 'H') { cur_ = 0; return; }
    if (first == 'F') { cur_ = buf_.size(); return; }

    std::string code{first};   char last = 0;
    if (!read_more(code, last, 30)) return;

    if (last == '~') {
      if (code == "3~")      { if (cur_ < buf_.size()) buf_.erase(cur_, 1); }        // Delete
      else if (code == "1~") { cur_ = 0; }                                            // Home
      else if (code == "4~") { cur_ = buf_.size(); }                                  // End
      else if (code == "200~") { in_paste_ = true; paste_cap_.clear(); }              // paste start
      else if (code == "201~") { }                                                    // stray end marker
      return;
    }
    if (last == 'u') {                                // CSI-u: <num>;modu
      int num = 0;
      for (char ch : code) { if (ch == ';') break; if (isdigit(static_cast<unsigned char>(ch))) num = num * 10 + (ch - '0'); }
      if (num == 13) { buf_.insert(buf_.begin() + cur_, '\n'); ++cur_; }              // Shift+Enter
      return;
    }
    // letters beyond the simple cases (F1.., AUP arrows, etc.) are ignored.
  }

  // ---- history -----------------------------------------------------------
  void remember_history(const std::string& s) {
    if (s.empty()) return;
    if (!history_.empty() && history_.back() == s) return;
    history_.push_back(s);
    if (history_.size() > 200) history_.erase(history_.begin());
  }
  void history_up() {
    if (history_.empty()) return;
    if (hist_pos_ < 0) { draft_ = buf_; hist_pos_ = static_cast<int>(history_.size()) - 1; }
    else if (hist_pos_ > 0) { --hist_pos_; }
    buf_ = history_[static_cast<size_t>(hist_pos_)]; cur_ = buf_.size();
  }
  void history_down() {
    if (hist_pos_ < 0 || history_.empty()) return;
    ++hist_pos_;
    if (hist_pos_ >= static_cast<int>(history_.size())) { hist_pos_ = -1; buf_ = draft_; draft_.clear(); }
    else buf_ = history_[static_cast<size_t>(hist_pos_)];
    cur_ = buf_.size();
  }

  // ---- word motion (word chars: alnum, '_', '-') -------------------------
  static bool is_word(char ch) {
    unsigned char u = static_cast<unsigned char>(ch);
    return isalnum(u) || u == '_' || u == '-' || u == '/';
  }
  size_t word_start(size_t pos) const {
    size_t i = pos;
    while (i > 0 && !is_word(buf_[i - 1])) --i;
    while (i > 0 && is_word(buf_[i - 1])) --i;
    return i;
  }
  size_t word_end(size_t pos) const {
    size_t i = pos;
    while (i < buf_.size() && !is_word(buf_[i])) ++i;
    while (i < buf_.size() && is_word(buf_[i])) ++i;
    return i;
  }

  void commit_open_locked() {
    if (!open_.empty()) { rows_.push_back(open_); open_.clear(); }
    thinking_open_ = false;
  }

  void yield_non_tty_locked() {
    if (live_) { std::cerr << "\r\x1b[K" << std::flush; live_ = false; }
    claimed_ = true;
  }

  // ---- TTY rendering ------------------------------------------------------
  static int screen_rows() {
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) return w.ws_row;
    return 24;
  }

  void render_locked(bool force) {
    if (!tty_) return;
    auto now = std::chrono::steady_clock::now();
    if (!force && std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render_).count() < 30)
      return;                                   // throttled: next keystroke/event forces it
    last_render_ = now;

    int total = screen_rows();
    std::vector<std::string> ilines = input_lines();      // buf_ split on '\n'

    // The input block renders as up to `max_in` rows anchored to the bottom;
    // any earlier prompt lines collapse into one dim "N more" row (data is
    // kept in buf_, only display is trimmed).
    size_t max_in = total > 2 ? static_cast<size_t>(total) - 2 : 1;
    size_t rendered = std::min(ilines.size(), max_in);
    if (rendered < 1) rendered = 1;
    size_t dropped = ilines.size() - rendered;

    int cap = total - static_cast<int>(rendered); // status rows above the input
    if (dropped > 0) --cap;                       // collapse row takes one
    if (cap < 1) cap = 1;

    std::vector<std::string> show;                // window: last `cap` status rows
    show.reserve(rows_.size() + 1);
    for (auto& r : rows_) show.push_back(r);
    if (!open_.empty()) show.push_back(open_);
    size_t start = show.size() > static_cast<size_t>(cap) ? show.size() - static_cast<size_t>(cap) : 0;

    std::ostringstream o;
    o << "\x1b[2J\x1b[H";
    for (size_t i = start; i < show.size(); ++i) o << show[i] << "\r\n";
    if (dropped > 0)
      o << "\x1b[2m… " << dropped << " more line(s)\x1b[0m\x1b[K\r\n";
    for (size_t i = dropped; i + 1 < ilines.size(); ++i)          // continuations
      o << "\x1b[2m│ " << ilines[i] << "\x1b[0m\x1b[K\r\n";
    o << prompt_;                                                 // active line
    if (!hidden_) o << ilines.back();
    o << "\x1b[K";
    if (!hidden_) {
      size_t r = 0, c = 0; caret_pos(r, c);
      if (r < dropped) { r = dropped; c = ilines[r].size(); }     // clamp into view
      size_t up = ilines.size() - 1 - r;                          // rows above active
      if (up > 0) o << "\x1b[" << up << "A";
      size_t left = ilines[r].size() > c ? ilines[r].size() - c : 0;
      if (left > 0) o << "\x1b[" << left << "D";
    }
    ::write(STDOUT_FILENO, o.str().data(), o.str().size());       // straight to fd1
  }

  std::vector<std::string> input_lines() const {
    std::vector<std::string> out;
    if (buf_.empty()) { out.emplace_back(); return out; }
    size_t b = 0;
    while (true) {
      size_t nl = buf_.find('\n', b);
      out.push_back(buf_.substr(b, nl == std::string::npos ? std::string::npos : nl - b));
      if (nl == std::string::npos) break;
      b = nl + 1;
    }
    return out;
  }
  void caret_pos(size_t& r, size_t& c) const {
    r = 0; c = 0;
    for (size_t i = 0; i < cur_ && i < buf_.size(); ++i) {
      if (buf_[i] == '\n') { ++r; c = 0; } else { ++c; }
    }
  }

  std::mutex m_;
  bool tty_ = false;
  bool hidden_ = false;

  // stream model (TTY)
  std::vector<std::string> rows_;
  std::string open_;                            // in-flight streamed row (tail)
  size_t spin_row_ = SIZE_MAX;                  // index of the live spinner row
  bool thinking_open_ = false;
  bool live_ = false;                           // non-TTY \r line active
  bool claimed_ = false;                        // a write owns the line: spinner stays quiet

  // editor
  std::string prompt_ = "\x1b[36m› \x1b[0m";
  std::string buf_;
  size_t cur_ = 0;
  std::vector<std::string> history_;            // submitted lines (ring, cap 200)
  int hist_pos_ = -1;                           // -1 = editing fresh / draft
  std::string draft_;                           // buffer before ↑ walked into history
  bool in_paste_ = false;                       // inside ESC[200~ .. ESC[201~
  std::string paste_cap_ = "";                  // accumulating pasted text

  // raw mode
  termios orig_{};
  bool raw_ready_ = false;

  std::chrono::steady_clock::time_point last_render_{};
};

// Redirect std::cout into Console rows (TTY mode) so existing REPL prints —
// slash-command output — render above the pinned input line instead of being
// written at the caret and clobbered by the next redraw. Install ONLY together
// with set_tty(true): Console's non-TTY message() itself writes to std::cout,
// so without TTY this board would echo forever.
class CoutBoard : public std::streambuf {
public:
  explicit CoutBoard(Console& c) : c_(c) {}
protected:
  int overflow(int ch) override {
    if (ch == traits_type::eof()) return traits_type::not_eof(ch);
    if (ch == '\n') {
      if (!buf_.empty()) { c_.message(buf_); buf_.clear(); }
    } else {
      buf_.push_back(static_cast<char>(ch));
    }
    return ch;
  }
  int sync() override {
    if (!buf_.empty()) { c_.message(buf_); buf_.clear(); }
    return 0;
  }
private:
  Console& c_;
  std::string buf_;
};

} // namespace pi