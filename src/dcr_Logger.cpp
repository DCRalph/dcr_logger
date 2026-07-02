#include "dcr_Logger.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "dcr_Console.h"
#include "dcr_LoggerSerialMutex.h"

namespace
{
  constexpr size_t LOG_CACHE_MAX_SIZE_WITH_PSRAM = DCR_LOGGER_CACHE_PSRAM_BYTES;
  constexpr size_t LOG_CACHE_MAX_SIZE_NO_PSRAM = DCR_LOGGER_CACHE_NO_PSRAM_BYTES;
  constexpr size_t MAX_LATEST_LOGS = 20;
  bool gInitialized = false;
  bool gSecondarySinkEnabled = false;
  bool gLastSecondarySinkState = false;
  char *gLogCache = nullptr;
  size_t gCacheLogMaxSize = 0;
  size_t gCachedLogSize = 0;
  // Cumulative bytes ever appended to the cache. The cache always holds the
  // stream range [gStreamTotal - gCachedLogSize, gStreamTotal); this lets the
  // incremental file sync find "bytes since last sync" even after evictions.
  // Guarded by serialMutex like the cache itself.
  uint64_t gStreamTotal = 0;
  // Stream position already written to the sync file (see SyncCacheToFile).
  uint64_t gSyncedStreamPos = 0;
  std::vector<String> gLatestLogs;
  Logger::SecondarySink gSecondarySink;

  std::mutex &fileSyncMutex()
  {
    static std::mutex m;
    return m;
  }

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
    {
      discardCount = (cachedSize + bytesToAppend) - cacheCapacity;

      // Evict in large chunks. Discarding only the exact overflow meant that
      // once the cache filled, EVERY append shifted the entire buffer with a
      // multi-MB memmove — on whichever task logged, while holding
      // serialMutex. Dropping a chunk at a time amortizes that shift across
      // hundreds of appends.
      const size_t discardChunk = std::min<size_t>(64 * 1024, cacheCapacity / 8);
      if (discardCount < discardChunk)
        discardCount = discardChunk;
      if (discardCount > cachedSize)
        discardCount = cachedSize;

      // Extend the cut to the next line boundary (bounded scan) so the
      // retained cache still starts on a whole log line.
      const size_t scanLimit = std::min(cachedSize, discardCount + 256);
      while (discardCount < scanLimit && cache[discardCount - 1] != '\n')
        ++discardCount;
    }

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
    gStreamTotal += bytesToAppend;
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

size_t Logger::GetCacheCapacityBytes()
{
  return LoggerInternal::GetCacheCapacity();
}

size_t Logger::GetCachedLogBytes()
{
  return LoggerInternal::GetCachedLogSize();
}

bool Logger::WriteCacheToFile(fs::FS &filesystem, const char *path)
{
  return LoggerInternal::WriteCacheToFile(filesystem, path);
}

bool Logger::SyncCacheToFile(fs::FS &filesystem, const char *path, size_t maxFileBytes)
{
  return LoggerInternal::SyncCacheToFile(filesystem, path, maxFileBytes);
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

size_t LoggerInternal::GetCacheCapacity()
{
  ensureInitialized();
  return gCacheLogMaxSize;
}

bool LoggerInternal::WriteCacheToFile(fs::FS &filesystem, const char *path)
{
  if (path == nullptr || path[0] == '\0')
    return false;

  ensureInitialized();

  // Snapshot the cache under the mutex, then write the file with the mutex
  // released. serialMutex is shared with every log call in the system;
  // holding it across a multi-second flash write blocked all logging tasks
  // (and flash writes additionally suspend the flash/PSRAM cache), which
  // showed up as UI stalls. The snapshot memcpy is bounded and fast.
  char *snapshot = nullptr;
  size_t snapshotSize = 0;
  uint64_t snapshotStreamEnd = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
    if (gLogCache == nullptr || gCachedLogSize == 0)
      return false;
    snapshot = static_cast<char *>(ps_malloc(gCachedLogSize));
    if (snapshot != nullptr)
    {
      snapshotSize = gCachedLogSize;
      memcpy(snapshot, gLogCache, snapshotSize);
      snapshotStreamEnd = gStreamTotal;
    }
  }

  std::lock_guard<std::mutex> fileLock(fileSyncMutex());

  if (snapshot == nullptr)
  {
    // No PSRAM headroom for a snapshot: fall back to writing under the mutex
    // (previous behavior) rather than dropping the flush entirely.
    std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
    if (gLogCache == nullptr || gCachedLogSize == 0)
      return false;
    File file = filesystem.open(path, FILE_WRITE);
    if (!file)
      return false;
    const size_t written = file.write(reinterpret_cast<const uint8_t *>(gLogCache), gCachedLogSize);
    file.close();
    if (written != gCachedLogSize)
      return false;
    gSyncedStreamPos = gStreamTotal;
    return true;
  }

  File file = filesystem.open(path, FILE_WRITE);
  if (!file)
  {
    free(snapshot);
    return false;
  }
  const size_t written = file.write(reinterpret_cast<const uint8_t *>(snapshot), snapshotSize);
  file.close();
  free(snapshot);

  if (written != snapshotSize)
    return false;

  {
    // The file now equals the cache as of the snapshot; record that so the
    // next incremental sync doesn't re-append the same bytes.
    std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
    if (snapshotStreamEnd > gSyncedStreamPos)
      gSyncedStreamPos = snapshotStreamEnd;
  }
  return true;
}

bool LoggerInternal::SyncCacheToFile(fs::FS &filesystem, const char *path, size_t maxFileBytes)
{
  if (path == nullptr || path[0] == '\0' || maxFileBytes == 0)
    return false;

  ensureInitialized();

  // Chunk staging buffer, allocated once. Each chunk is copied out of the
  // cache under serialMutex (bounded memcpy) and written to flash with the
  // mutex released, so log calls on other tasks never block behind flash I/O.
  static uint8_t *chunkBuf = nullptr;
  static size_t chunkBufSize = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
    if (chunkBuf == nullptr)
    {
      chunkBufSize = 16 * 1024;
      chunkBuf = static_cast<uint8_t *>(ps_malloc(chunkBufSize));
      if (chunkBuf == nullptr)
      {
        chunkBufSize = 4 * 1024;
        chunkBuf = static_cast<uint8_t *>(malloc(chunkBufSize));
      }
      if (chunkBuf == nullptr)
      {
        chunkBufSize = 0;
        return false;
      }
    }
  }

  std::lock_guard<std::mutex> fileLock(fileSyncMutex());

  uint64_t droppedBytes = 0;
  size_t totalWritten = 0;
  File file;

  // Cap one sync pass at a full cache's worth of data so a task logging
  // faster than flash can drain cannot pin us in this loop; the remainder
  // goes out on the next sync.
  const size_t maxBytesPerSync = gCacheLogMaxSize;

  while (totalWritten < maxBytesPerSync)
  {
    size_t chunkLen = 0;
    {
      std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
      if (gLogCache == nullptr)
        break;

      const uint64_t streamBase = gStreamTotal - gCachedLogSize;
      if (gSyncedStreamPos < streamBase)
      {
        // Bytes were evicted from the cache before we could sync them.
        droppedBytes += streamBase - gSyncedStreamPos;
        gSyncedStreamPos = streamBase;
      }

      const uint64_t avail = gStreamTotal - gSyncedStreamPos;
      if (avail == 0)
        break;

      chunkLen = static_cast<size_t>(std::min<uint64_t>(avail, chunkBufSize));
      const size_t offset = static_cast<size_t>(gSyncedStreamPos - streamBase);
      memcpy(chunkBuf, gLogCache + offset, chunkLen);
    }

    if (!file)
    {
      file = filesystem.open(path, FILE_APPEND);
      if (!file)
        return false;
    }

    if (file.size() + chunkLen > maxFileBytes)
    {
      file.close();
      file = filesystem.open(path, FILE_WRITE);
      if (!file)
        return false;
    }

    if (droppedBytes > 0)
    {
      const String marker =
          "[LOGGER] " + String(static_cast<unsigned long>(droppedBytes)) + " bytes dropped before sync\n";
      file.write(reinterpret_cast<const uint8_t *>(marker.c_str()), marker.length());
      droppedBytes = 0;
    }

    const size_t written = file.write(chunkBuf, chunkLen);
    if (written != chunkLen)
    {
      file.close();
      return false;
    }
    totalWritten += chunkLen;

    {
      std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
      gSyncedStreamPos += chunkLen;
    }
  }

  if (file)
    file.close();
  return true;
}

std::vector<String> LoggerInternal::GetLatestLogs()
{
  ensureInitialized();
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  return gLatestLogs;
}
