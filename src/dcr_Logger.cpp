#include "dcr_Logger.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <mutex>

#include "dcr_Console.h"
#include "dcr_LoggerSerialMutex.h"

namespace
{
  constexpr size_t LOG_CACHE_MAX_SIZE_WITH_PSRAM = 1024 * 1024;
  constexpr size_t LOG_CACHE_MAX_SIZE_NO_PSRAM = 128 * 1024;
  constexpr size_t MAX_LATEST_LOGS = 20;
  bool gInitialized = false;
  bool gSaveToFile = false;
  bool gSecondarySinkEnabled = false;
  bool gLastSecondarySinkState = false;
  char *gLogCache = nullptr;
  size_t gCacheLogMaxSize = 0;
  size_t gCachedLogSize = 0;
  std::vector<String> gLatestLogs;
  Logger::SecondarySink gSecondarySink;

  char levelLetter(LogLevel level)
  {
    switch (level)
    {
    case LogLevel::Fatal:
      return 'F';
    case LogLevel::Error:
      return 'E';
    case LogLevel::Warn:
      return 'W';
    case LogLevel::Info:
      return 'I';
    case LogLevel::Debug:
      return 'D';
    case LogLevel::Verbose:
      return 'V';
    case LogLevel::Trace:
      return 'T';
    default:
      return '?';
    }
  }

  const char *levelColor(LogLevel level)
  {
    switch (level)
    {
    case LogLevel::Fatal:
      return "\x1B[1;31m"; // Red bold
    case LogLevel::Error:
      return "\x1B[31m"; // Red
    case LogLevel::Warn:
      return "\x1B[33m"; // Yellow
    case LogLevel::Info:
      return "\x1B[32m"; // Green
    case LogLevel::Debug:
      return "\x1B[36m"; // Cyan
    case LogLevel::Verbose:
      return "\x1B[39m"; // Default color
    case LogLevel::Trace:
      return "\x1B[90m"; // Gray
    default:
      return "";
    }
  }

  esp_log_level_t toEspLevel(LogLevel level)
  {
    switch (level)
    {
    case LogLevel::Fatal:
    case LogLevel::Error:
      return ESP_LOG_ERROR;
    case LogLevel::Warn:
      return ESP_LOG_WARN;
    case LogLevel::Info:
      return ESP_LOG_INFO;
    case LogLevel::Debug:
      return ESP_LOG_DEBUG;
    case LogLevel::Verbose:
    case LogLevel::Trace:
      return ESP_LOG_VERBOSE;
    default:
      return ESP_LOG_INFO;
    }
  }

  void ensureInitialized()
  {
    if (gInitialized)
      return;

    std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
    if (gInitialized)
      return;

    gCacheLogMaxSize = psramFound() ? LOG_CACHE_MAX_SIZE_WITH_PSRAM
                                    : LOG_CACHE_MAX_SIZE_NO_PSRAM;
    if (gLogCache == nullptr)
    {
      gLogCache = static_cast<char *>(ps_calloc(gCacheLogMaxSize + 1, sizeof(char)));
      if (gLogCache != nullptr)
        gLogCache[0] = '\0';
    }
    gCachedLogSize = 0;
    gLatestLogs.reserve(MAX_LATEST_LOGS);
    gInitialized = true;
    esp_log_level_set("*", toEspLevel(LogLevel::Info));
  }

  String ensureTrailingNewline(const String &message)
  {
    if (message.endsWith("\n") || message.endsWith("\r"))
      return message;
    return message + "\n";
  }

  String composeStructuredLine(LogLevel level, const char *tag, const char *message)
  {
    String line;
    line += levelColor(level);

    line += "[";
    line += levelLetter(level);
    line += "][";
    line += (tag != nullptr && tag[0] != '\0') ? tag : "APP";
    line += "] ";
    line += message != nullptr ? message : "";
    line = ensureTrailingNewline(line);

    line += "\x1B[0m";

    return line;
  }

  void appendToCircularTextBuffer(char *cache, size_t cacheCapacity, size_t &cachedSize,
                                  const String &message)
  {
    if (cache == nullptr || cacheCapacity == 0)
    {
      cachedSize = 0;
      return;
    }

    const size_t messageLength = message.length();
    const size_t bytesToAppend = messageLength > cacheCapacity ? cacheCapacity : messageLength;
    const size_t messageOffset = messageLength - bytesToAppend;

    size_t discardCount = 0;
    if (cachedSize + bytesToAppend > cacheCapacity)
      discardCount = (cachedSize + bytesToAppend) - cacheCapacity;

    if (discardCount >= cachedSize)
    {
      cachedSize = 0;
    }
    else if (discardCount > 0)
    {
      memmove(cache, cache + discardCount, cachedSize - discardCount);
      cachedSize -= discardCount;
    }

    if (bytesToAppend > 0)
    {
      memcpy(cache + cachedSize, message.c_str() + messageOffset, bytesToAppend);
      cachedSize += bytesToAppend;
    }

    if (cachedSize > cacheCapacity)
      cachedSize = cacheCapacity;

    cache[cachedSize] = '\0';
  }

  void appendLatestLog(const String &message)
  {
    gLatestLogs.push_back(message);
    while (gLatestLogs.size() > MAX_LATEST_LOGS)
      gLatestLogs.erase(gLatestLogs.begin());
  }

  void publishIfEnabled(LogLevel level, const char *tag, const String &line)
  {
    if (!gSecondarySinkEnabled || !gSecondarySink)
      return;
    gSecondarySink(level, tag, line);
  }

  void dispatchStructured(LogLevel level, const char *tag, const char *message)
  {
    if (!Logger::IsEnabled(level))
      return;

    ensureInitialized();
    std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
    const String line = composeStructuredLine(level, tag, message);
    appendLatestLog(line);
    appendToCircularTextBuffer(gLogCache, gCacheLogMaxSize, gCachedLogSize, line);
    publishIfEnabled(level, tag, line);
    Console::session().writeText(line);
  }

  void dispatchRaw(const char *message)
  {
    if (message == nullptr)
      return;

    ensureInitialized();
    std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
    const String line(message);
    appendLatestLog(line);
    appendToCircularTextBuffer(gLogCache, gCacheLogMaxSize, gCachedLogSize, line);
    publishIfEnabled(LogLevel::Info, "APP", line);
    Console::session().writeText(line);
  }

  int logHookVprintf(const char *fmt, va_list args)
  {
    if (xPortInIsrContext())
      return vprintf(fmt, args);

    va_list apCopy;
    va_copy(apCopy, args);
    String line = Console::formatV(fmt, apCopy);
    va_end(apCopy);

    if (line.length() >= 2)
    {
      const size_t n = line.length();
      if (line[n - 1] == '\n' && line[n - 2] == '\n')
        line.remove(static_cast<unsigned int>(n - 1), 1);
    }

    dispatchStructured(LogLevel::Info, "ESP", line.c_str());
    return static_cast<int>(line.length());
  }
}

int hereCounter = 0;
LogLevel Logger::_level = LogLevel::Info;

void Logger::SetLevel(LogLevel level)
{
  ensureInitialized();
  _level = level;
  esp_log_level_set("*", toEspLevel(level));
}

LogLevel Logger::GetLevel()
{
  return _level;
}

bool Logger::IsEnabled(LogLevel level)
{
  return static_cast<uint8_t>(level) <= static_cast<uint8_t>(_level);
}

void Logger::Logs(LogLevel level, const char *tag, const char *message)
{
  if (!IsEnabled(level))
    return;
  dispatchStructured(level, tag, message);
}

void Logger::Logf(LogLevel level, const char *tag, const char *fmt, ...)
{
  if (!IsEnabled(level))
    return;

  va_list args;
  va_start(args, fmt);
  Logv(level, tag, fmt, args);
  va_end(args);
}

void Logger::Logv(LogLevel level, const char *tag, const char *fmt, va_list args)
{
  if (!IsEnabled(level))
    return;

  va_list apCopy;
  va_copy(apCopy, args);
  const String text = Console::formatV(fmt, apCopy);
  va_end(apCopy);

  dispatchStructured(level, tag, text.c_str());
}

void Logger::InstallLogHook()
{
  ensureInitialized();
  if (esp_log_set_vprintf(logHookVprintf) == nullptr)
    Logs(LogLevel::Error, "LOGGER", "Failed to install log hook");
  else
    Logs(LogLevel::Info, "LOGGER", "Log hook installed successfully");
}

void Logger::SetSecondarySink(SecondarySink sink)
{
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  gSecondarySink = std::move(sink);
}

void LoggerInternal::Raw(const char *message)
{
  dispatchRaw(message);
}

void LoggerInternal::Flush()
{
  Console::session().flush();
}

void LoggerInternal::LogText(LogLevel level, const char *tag, const char *message)
{
  dispatchStructured(level, tag, message);
}

char *LoggerInternal::GetLogCache()
{
  ensureInitialized();
  return gLogCache;
}

size_t LoggerInternal::GetCachedLogSize()
{
  ensureInitialized();
  return gCachedLogSize;
}

std::vector<String> LoggerInternal::GetLatestLogs()
{
  ensureInitialized();
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  return gLatestLogs;
}
