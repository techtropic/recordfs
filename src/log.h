// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Marc Micalizzi
#pragma once
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace rfs {

// stderr logger, one line per event: "HH:MM:SS level message"
inline void log_line(const char* level, const std::string& msg) {
  auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_s(&tm, &now);
  std::fprintf(stderr, "%02d:%02d:%02d %s %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
               level, msg.c_str());
}
inline void info(const std::string& msg) { log_line("info", msg); }
inline void warn(const std::string& msg) { log_line("WARN", msg); }
inline void error(const std::string& msg) { log_line("ERROR", msg); }

}  // namespace rfs
