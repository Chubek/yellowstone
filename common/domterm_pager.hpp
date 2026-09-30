#pragma once

#include <domterm.h>
#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace qobj {

/* A small in-process pager.  DomTERM owns the tty mode and restoration; the
 * rendering is ordinary ANSI so it works in DomTERM, xterm, and ssh. */
class DomtermPager {
 public:
  DomtermPager() = default;
  DomtermPager(const DomtermPager&) = delete;
  ~DomtermPager() { close(); }

  bool run(std::string_view text) {
    if (text.empty()) return true;
    int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0 || !isatty(fd)) { if (fd >= 0) ::close(fd); return false; }
    DT_TTYOptions opts; dt_tty_options_init(&opts); opts.fd = fd;
    DT_Error err{}; session_ = dt_tty_open(&opts, &err);
    if (!session_) { ::close(fd); return false; }
    lines_.clear(); std::string line;
    for (char c : text) { if (c == '\n') { lines_.push_back(line); line.clear(); } else if (c != '\r') line += c; }
    if (!line.empty()) lines_.push_back(line);
    DT_Winsize ws{}; dt_tty_get_winsize(session_, &ws, &err);
    rows_ = ws.rows ? ws.rows : 24; cols_ = ws.columns ? ws.columns : 80;
    dt_tty_set_raw(session_, &err);
    write("\033[?1049h\033[H\033[?25l");
    size_t top = 0; bool done = false;
    while (!done) {
      render(top);
      unsigned char ch = 0; size_t n = 0;
      if (dt_tty_read(session_, &ch, 1, &n, &err) != DT_OK || !n) break;
      if (ch == 'q' || ch == 27) done = true;
      else if (ch == 'j' || ch == '\n' || ch == ' ') top = std::min(top + (ch == ' ' ? rows_ - 1 : 1), lines_.size());
      else if (ch == 'k') top = top ? top - 1 : 0;
      else if (ch == 'g') top = 0;
      else if (ch == 'G') top = lines_.size() > rows_ ? lines_.size() - rows_ : 0;
      else if (ch == '/') { /* search mode: type a literal query, enter */ if (search(top)) {} }
    }
    write("\033[?25h\033[?1049l"); close(); return true;
  }

 private:
  DT_TTYSession* session_ = nullptr; unsigned rows_ = 24, cols_ = 80;
  std::vector<std::string> lines_;
  void write(std::string_view s) { size_t off = 0, n = 0; while (off < s.size() && session_ && dt_tty_write(session_, s.data()+off, s.size()-off, &n, nullptr) == DT_OK) off += n; }
  void render(size_t top) { write("\033[H\033[2J"); unsigned body = rows_ > 1 ? rows_-1 : 1; for (unsigned i=0;i<body;++i) { size_t ix=top+i; if(ix<lines_.size()) { std::string s=lines_[ix]; if(s.size()>cols_) s.resize(cols_); write(s); } write("\033[K\r\n"); } write("\033[7m DomTERM pager  "); write(std::to_string(top+1)); write("/"); write(std::to_string(lines_.size())); write("  q quit  / search\033[0m"); }
  bool search(size_t& top) { std::string q; unsigned char c; size_t n; for (;;) { if(dt_tty_read(session_,&c,1,&n,nullptr)!=DT_OK||!n) return false; if(c=='\n') break; if(c==27) return false; if(c==127) { if(!q.empty()) q.pop_back(); } else if(q.size()<128) q.push_back(char(c)); } if(q.empty()) return false; for(size_t i=top+1;i<lines_.size();++i) if(lines_[i].find(q)!=std::string::npos){top=i;return true;} return false; }
  void close() { if (!session_) return; DT_Error e{}; dt_tty_set_cooked(session_, &e); dt_tty_close(session_); session_=nullptr; }
};
}
