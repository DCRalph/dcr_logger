#pragma once

#include <Arduino.h>
#include <Print.h>
#include <cstdarg>

class ConsoleSession
{
public:
  void writeRaw(const char *data);
  void writeRaw(const String &data);
  void writeText(const char *text);
  void writeText(const String &text);
  void writeLine(const char *text);
  void writeLine(const String &text);
  void flush();
  void prompt();

  String &lineBuffer() { return _lineBuffer; }
  void clearLineBuffer() { _lineBuffer = ""; }

private:
  void writeRawLocked(const char *data, size_t len);
  void writeWithCrlfLocked(const char *data, size_t len);

  String _lineBuffer;
};

namespace Console
{
  void init();
  ConsoleSession &session();

  // Register output streams the console writes to. The library does not assume
  // any default stream — the application must add at least one (e.g. &Serial)
  // before the console is used.
  void addStream(Print *stream);
  void clearStreams();

  // Single implementation of printf-style formatting (used by DebugCli and LoggerProxy).
  String formatV(const char *fmt, va_list ap);
}
