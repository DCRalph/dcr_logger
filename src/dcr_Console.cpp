#include "dcr_Console.h"

#include "dcr_LoggerSerialMutex.h"

#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>

namespace
{
  ConsoleSession gConsoleSession;
  std::vector<Print *> gStreams;

  String normalizeCrlf(const char *data, size_t len)
  {
    String normalized;
    normalized.reserve(len + 8);

    for (size_t i = 0; i < len; ++i)
    {
      char c = data[i];
      if (c == '\r')
      {
        normalized += '\r';
        if (i + 1 < len && data[i + 1] == '\n')
        {
          normalized += '\n';
          ++i;
        }
        continue;
      }

      if (c == '\n')
      {
        normalized += '\r';
        normalized += '\n';
        continue;
      }

      normalized += c;
    }

    return normalized;
  }
}

std::recursive_mutex &LoggerInternal::serialMutex()
{
  static std::recursive_mutex instance;
  return instance;
}

void ConsoleSession::writeRawLocked(const char *data, size_t len)
{
  if (data == nullptr || len == 0)
    return;

  for (Print *stream : gStreams)
  {
    if (stream != nullptr)
      stream->write(reinterpret_cast<const uint8_t *>(data), len);
  }
}

void ConsoleSession::writeWithCrlfLocked(const char *data, size_t len)
{
  const String normalized = normalizeCrlf(data, len);
  writeRawLocked(normalized.c_str(), normalized.length());
}

void ConsoleSession::writeRaw(const char *data)
{
  if (data == nullptr)
    return;
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  writeRawLocked(data, strlen(data));
}

void ConsoleSession::writeRaw(const String &data)
{
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  writeRawLocked(data.c_str(), data.length());
}

void ConsoleSession::writeText(const char *text)
{
  if (text == nullptr)
    return;
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  writeWithCrlfLocked(text, strlen(text));
}

void ConsoleSession::writeText(const String &text)
{
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  writeWithCrlfLocked(text.c_str(), text.length());
}

void ConsoleSession::writeLine(const char *text)
{
  if (text == nullptr)
  {
    writeText("\n");
    return;
  }

  String line(text);
  line += '\n';
  writeText(line);
}

void ConsoleSession::writeLine(const String &text)
{
  String line(text);
  line += '\n';
  writeText(line);
}

void ConsoleSession::flush()
{
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  for (Print *stream : gStreams)
  {
    if (stream != nullptr)
      stream->flush();
  }
}

void ConsoleSession::prompt()
{
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  writeRawLocked("> ", 2);
  for (Print *stream : gStreams)
  {
    if (stream != nullptr)
      stream->flush();
  }
}

void Console::init()
{
  gConsoleSession.clearLineBuffer();
}

ConsoleSession &Console::session()
{
  return gConsoleSession;
}

void Console::addStream(Print *stream)
{
  if (stream == nullptr)
    return;
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  for (Print *existing : gStreams)
  {
    if (existing == stream)
      return;
  }
  gStreams.push_back(stream);
}

void Console::clearStreams()
{
  std::lock_guard<std::recursive_mutex> lock(LoggerInternal::serialMutex());
  gStreams.clear();
}

String Console::formatV(const char *fmt, va_list ap)
{
  if (fmt == nullptr)
    return String();

  char stackBuf[256];
  va_list apCopy;
  va_copy(apCopy, ap);
  int len = vsnprintf(stackBuf, sizeof(stackBuf), fmt, apCopy);
  va_end(apCopy);
  if (len < 0)
    return String();

  if (static_cast<size_t>(len) < sizeof(stackBuf))
    return String(stackBuf);

  char *heap = static_cast<char *>(malloc(static_cast<size_t>(len) + 1));
  if (heap == nullptr)
    return String();

  va_copy(apCopy, ap);
  vsnprintf(heap, static_cast<size_t>(len) + 1, fmt, apCopy);
  va_end(apCopy);
  String out(heap);
  free(heap);
  return out;
}
