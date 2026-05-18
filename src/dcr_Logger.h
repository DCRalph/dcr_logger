#pragma once

#include <Arduino.h>
#include <FS.h>
#include <cstdarg>
#include <functional>
#include <string>
#include <vector>

#include "esp_debug_helpers.h"

#ifndef DCR_LOGGER_CACHE_PSRAM_BYTES
#define DCR_LOGGER_CACHE_PSRAM_BYTES (2 * 1024 * 1024)
#endif

#ifndef DCR_LOGGER_CACHE_NO_PSRAM_BYTES
#define DCR_LOGGER_CACHE_NO_PSRAM_BYTES (128 * 1024)
#endif

extern int hereCounter;

// print here with hereCount and linenumber and function name from where it was called using define
#define here()                                                            \
  do                                                                      \
  {                                                                       \
    Logger::Logf(LogLevel::Info, "HERE", "[%d] %s:%d %s\n",               \
                 hereCounter++, __FILE__, __LINE__, __PRETTY_FUNCTION__); \
    esp_backtrace_print(10);                                              \
  } while (0)



enum class LogLevel : uint8_t
{
  Fatal = 0,
  Error,
  Warn,
  Info,
  Debug,
  Verbose,
  Trace
};

class Logger
{
public:
  // Callback invoked for every dispatched log line when the secondary sink is
  // enabled. The line is the fully-formatted, ANSI-colored output as written
  // to the console.
  using SecondarySink = std::function<void(LogLevel level, const char *tag, const String &line)>;

  static void SetLevel(LogLevel level);
  static LogLevel GetLevel();
  static bool IsEnabled(LogLevel level);

  static void Logs(LogLevel level, const char *tag, const char *message);
  static void Logf(LogLevel level, const char *tag, const char *fmt, ...);          // logf stands for log format
  static void Logv(LogLevel level, const char *tag, const char *fmt, va_list args); // logv stands for log variable

  static void InstallLogHook();

  // Register an application-provided sink (e.g. for forwarding to MQTT or a
  // file). Pass nullptr / empty std::function to unregister.
  static void SetSecondarySink(SecondarySink sink);

  // In-memory ring buffer capacity (bytes). Grows to the PSRAM size when PSRAM
  // is available at runtime.
  static size_t GetCacheCapacityBytes();

  // Number of bytes currently retained in the ring buffer.
  static size_t GetCachedLogBytes();

  // Write the retained ring-buffer contents to a filesystem path (e.g.
  // "/log.txt"). Returns true when all cached bytes were written.
  static bool WriteCacheToFile(fs::FS &filesystem, const char *path);

private:
  static LogLevel _level;
};

namespace LoggerInternal
{
  template <typename T>
  inline T Unwrap(T val)
  {
    return val;
  }

  inline const char *Unwrap(const String &s)
  {
    return s.c_str();
  }

  inline const char *Unwrap(const std::string &s)
  {
    return s.c_str();
  }

  void Raw(const char *message);
  void Flush();
  void LogText(LogLevel level, const char *tag, const char *message);
  char *GetLogCache();
  size_t GetCachedLogSize();
  size_t GetCacheCapacity();
  bool WriteCacheToFile(fs::FS &filesystem, const char *path);
  std::vector<String> GetLatestLogs();

  template <typename... Args>
  inline void DispatchLog(LogLevel level, const char *tag, const char *fmt, Args... args)
  {
    Logger::Logf(level, tag, fmt, Unwrap(args)...);
  }
}

#ifndef LOG_TAG
#define LOG_TAG "APP"
#endif

#define debugE(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Error, LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define debugW(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Warn, LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define debugI(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Info, LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define debugD(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Debug, LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define debugV(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Verbose, LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define debugF(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Fatal, LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define debugT(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Trace, LOG_TAG, fmt "\n", ##__VA_ARGS__)
#define debugA(fmt, ...) LoggerInternal::DispatchLog(LogLevel::Trace, LOG_TAG, fmt "\n", ##__VA_ARGS__)
