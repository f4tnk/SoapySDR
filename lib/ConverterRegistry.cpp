// Copyright (c) 2017-2017 Coburn Wightman
// Copyright (c) 2018-2018 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include <SoapySDR/ConverterRegistry.hpp>
#include <algorithm>
#include <stdexcept>
#include <atomic>

void lateLoadDefaultConverters(void);

static SoapySDR::ConverterRegistry::FormatConverters formatConverters;

// Generation counter: incremented on every registration, checked by thread-local cache
static std::atomic<unsigned> converterGeneration(0);

SoapySDR::ConverterRegistry::ConverterRegistry(const std::string &sourceFormat, const std::string &targetFormat, const FunctionPriority &priority, ConverterFunction converterFunction)
{
  if (formatConverters.count(sourceFormat) == 0)
    ;
  else if (formatConverters[sourceFormat].count(targetFormat) == 0)
    ;
  else if (formatConverters[sourceFormat][targetFormat].count(priority) != 0)
    {
      SoapySDR::logf(SOAPY_SDR_ERROR, "SoapySDR::ConverterRegistry(%s, %s, %s) duplicate registration", sourceFormat.c_str(), targetFormat.c_str(), std::to_string(priority).c_str());
      return;
    }
  
  formatConverters[sourceFormat][targetFormat][priority] = converterFunction;
  converterGeneration.fetch_add(1, std::memory_order_release);

  return;
}

std::vector<std::string> SoapySDR::ConverterRegistry::listTargetFormats(const std::string &sourceFormat)
{
  lateLoadDefaultConverters();

  std::vector<std::string> targets;

  if (formatConverters.count(sourceFormat) == 0)
    return targets;

  for(const auto &it:formatConverters[sourceFormat])
    {
      std::string targetFormat = it.first;
      targets.push_back(targetFormat);
    }
  
  std::sort(targets.begin(), targets.end());
  return targets;
}

std::vector<std::string> SoapySDR::ConverterRegistry::listSourceFormats(const std::string &targetFormat)
{
  lateLoadDefaultConverters();

  std::vector<std::string> sources;

  for(const auto &it:formatConverters)
    {
      std::string sourceFormat = it.first;
      if (formatConverters[sourceFormat].count(targetFormat) > 0)
        sources.push_back(sourceFormat);
    }
  
  std::sort(sources.begin(), sources.end());
  return sources;
}

std::vector<SoapySDR::ConverterRegistry::FunctionPriority> SoapySDR::ConverterRegistry::listPriorities(const std::string &sourceFormat, const std::string &targetFormat)
{
  lateLoadDefaultConverters();

  std::vector<FunctionPriority> priorities;
  
  if (formatConverters.count(sourceFormat) == 0)
    ;
  else if (formatConverters[sourceFormat].count(targetFormat) == 0)
    ;
  else if (formatConverters[sourceFormat][targetFormat].size() == 0)
    ;
  else
    {
      for(const auto &it:formatConverters[sourceFormat][targetFormat])
        {
          FunctionPriority priority = it.first;
          priorities.push_back(priority);
        }
    }
  
  return priorities;
  
}

SoapySDR::ConverterRegistry::ConverterFunction SoapySDR::ConverterRegistry::getFunction(const std::string &sourceFormat, const std::string &targetFormat)
{
  lateLoadDefaultConverters();

  // Thread-local cache for hot-path lookups during streaming
  // Avoids triple nested map lookup on every buffer (called thousands of times/sec)
  // Generation counter ensures cache is invalidated when new converters are registered
  static thread_local std::string cachedSrc;
  static thread_local std::string cachedTgt;
  static thread_local ConverterFunction cachedFunc = nullptr;
  static thread_local unsigned cachedGen = 0;

  const unsigned currentGen = converterGeneration.load(std::memory_order_acquire);
  if (cachedFunc != nullptr && cachedGen == currentGen &&
      sourceFormat == cachedSrc && targetFormat == cachedTgt)
  {
      return cachedFunc;
  }

  if (formatConverters.count(sourceFormat) == 0)
    {
      throw std::runtime_error("ConverterRegistry::getFunction() conversion source not registered; "
                               "sourceFormat="+sourceFormat+", targetFormat="+targetFormat);
    }
  
  if (formatConverters[sourceFormat].count(targetFormat) == 0)
    {
      throw std::runtime_error("ConverterRegistry::getFunction() conversion target not registered; "
                               "sourceFormat="+sourceFormat+", targetFormat="+targetFormat);
    }

  if (formatConverters[sourceFormat][targetFormat].size() == 0)
    {
      throw std::runtime_error("ConverterRegistry::getFunction() no functions found for registered conversion; "
                               "sourceFormat="+sourceFormat+", targetFormat="+targetFormat);
    }

  // Populate cache for next hot-path lookup
  cachedFunc = formatConverters[sourceFormat][targetFormat].rbegin()->second;
  cachedSrc = sourceFormat;
  cachedTgt = targetFormat;
  cachedGen = currentGen;
  return cachedFunc;
}

SoapySDR::ConverterRegistry::ConverterFunction SoapySDR::ConverterRegistry::getFunction(const std::string &sourceFormat, const std::string &targetFormat, const FunctionPriority &priority)
{
  lateLoadDefaultConverters();

  if (formatConverters.count(sourceFormat) == 0)
    {
      throw std::runtime_error("ConverterRegistry::getFunction() conversion source not registered; "
                               "sourceFormat="+sourceFormat+", targetFormat="+targetFormat+", priority="+std::to_string(priority));
    }

  if (formatConverters[sourceFormat].count(targetFormat) == 0)
    {
      throw std::runtime_error("ConverterRegistry::getFunction() conversion target not registered; "
                               "sourceFormat="+sourceFormat+", targetFormat="+targetFormat+", priority="+std::to_string(priority));
    }

  if (formatConverters[sourceFormat][targetFormat].count(priority) == 0)
    {
      throw std::runtime_error("ConverterRegistry::getFunction() conversion priority not registered; "
                               "sourceFormat="+sourceFormat+", targetFormat="+targetFormat+", priority="+std::to_string(priority));
    }

  return formatConverters[sourceFormat][targetFormat][priority];
}

std::vector<std::string> SoapySDR::ConverterRegistry::listAvailableSourceFormats(void)
{
    lateLoadDefaultConverters();

    std::vector<std::string> sources;
    for (const auto &it : formatConverters)
    {
        if (std::find(sources.begin(), sources.end(), it.first) == sources.end())
        {
            sources.push_back(it.first);
        }
    }
    std::sort(sources.begin(), sources.end());
    return sources;
}
