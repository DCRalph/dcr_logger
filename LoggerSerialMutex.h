#pragma once

#include <mutex>

namespace LoggerInternal
{
  std::recursive_mutex &serialMutex();
}
