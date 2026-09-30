#include "log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace lad {

namespace {

const auto g_start = std::chrono::steady_clock::now();
FILE* g_log = nullptr;
bool g_log_stderr = false;
FILE* g_hostlog = nullptr;
bool g_hostlog_stderr = false;

void vformat(char* buf, size_t n, const char* fmt, va_list ap) {
  vsnprintf(buf, n, fmt, ap);
}

void emit(FILE* f, const char* prefix, const char* text) {
  if (!f) return;
  fprintf(f, "%s%s\n", prefix, text);
  fflush(f);
}

}  // namespace

double elapsed_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

void log_open(const std::string& path, bool to_stderr) {
  g_log_stderr = to_stderr;
  if (!path.empty() && !g_log) g_log = fopen(path.c_str(), "ae");
}

void hostlog_open(const std::string& path, bool to_stderr) {
  g_hostlog_stderr = to_stderr;
  if (!path.empty() && !g_hostlog) g_hostlog = fopen(path.c_str(), "ae");
}

bool log_enabled() { return g_log || g_log_stderr; }

void log_line(const char* fmt, ...) {
  if (!log_enabled()) return;
  char body[2048];
  va_list ap;
  va_start(ap, fmt);
  vformat(body, sizeof(body), fmt, ap);
  va_end(ap);
  char prefix[48];
  snprintf(prefix, sizeof(prefix), "[%9.3f] ", elapsed_s());
  emit(g_log, prefix, body);
  if (g_log_stderr) emit(stderr, prefix, body);
}

void err_line(const char* fmt, ...) {
  char body[2048];
  va_list ap;
  va_start(ap, fmt);
  vformat(body, sizeof(body), fmt, ap);
  va_end(ap);
  emit(stderr, "longafterdark: ", body);
  if (g_log) {
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "[%9.3f] error: ", elapsed_s());
    emit(g_log, prefix, body);
  }
}

void hostlog_line(std::string_view line) {
  if (!g_hostlog && !g_hostlog_stderr) return;
  std::string text(line);
  if (g_hostlog) emit(g_hostlog, "", text.c_str());
  if (g_hostlog_stderr) emit(stderr, "host: ", text.c_str());
}

}  // namespace lad
