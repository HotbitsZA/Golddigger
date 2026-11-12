#include "ModelService.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>

#include <dlib/svm.h>

#include "Features/feature_builder.h"
#include "Training/model_types.h"

namespace golddiggerui
{
  namespace fs = std::filesystem;

  namespace
  {
    std::string lowerCopy(std::string value)
    {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
                     { return static_cast<char>(std::tolower(ch)); });
      return value;
    }

    std::string modifiedUtc(const fs::path &path)
    {
      std::error_code ec;
      const auto fileTime = fs::last_write_time(path, ec);
      if (ec)
        return std::string();

      const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
          fileTime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
      const std::time_t seconds = std::chrono::system_clock::to_time_t(systemTime);
      std::tm tm{};
#if defined(_WIN32)
      gmtime_s(&tm, &seconds);
#else
      gmtime_r(&seconds, &tm);
#endif
      std::ostringstream out;
      out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
      return out.str();
    }

    int timeframeOrder(const std::string &timeframe)
    {
      if (timeframe == "m15")
        return 0;
      if (timeframe == "h1")
        return 1;
      if (timeframe == "d1")
        return 2;
      return 3;
    }
  } // namespace

  struct ModelService::LoadedModel
  {
    dlib::decision_function<kernel_type> decisionFunction;
    dlib::vector_normalizer<sample_type> normalizer;
    fs::file_time_type mtime{};
    std::uintmax_t size = 0;
  };

  ModelService::ModelService(AppPaths paths)
      : m_paths(std::move(paths))
  {
  }

  std::string ModelService::timeframeOfModel(const std::string &fileName)
  {
    const std::string normalized = lowerCopy(fs::path(fileName).stem().string());
    for (const char *tag : {"m15", "h1", "d1"})
    {
      if (normalized.find(tag) != std::string::npos)
        return tag;
    }
    return std::string();
  }

  std::vector<ModelInfo> ModelService::list() const
  {
    std::vector<ModelInfo> models;
    std::error_code ec;
    if (!fs::is_directory(m_paths.modelsDir, ec))
      return models;

    for (const auto &entry : fs::directory_iterator(m_paths.modelsDir, ec))
    {
      if (ec)
        break;
      if (!entry.is_regular_file(ec) || entry.path().extension() != ".dat")
        continue;

      const std::string name = entry.path().filename().string();
      const std::string normalized = lowerCopy(name);
      if (normalized.rfind("tuner_", 0) == 0 || normalized.rfind("tunner_", 0) == 0)
        continue;

      ModelInfo info;
      info.timeframe = timeframeOfModel(name);
      info.name = name;
      info.path = entry.path().string();
      info.exists = true;
      info.sizeBytes = fs::file_size(entry.path(), ec);
      info.modifiedUtc = modifiedUtc(entry.path());
      models.push_back(std::move(info));
    }

    std::sort(models.begin(), models.end(), [](const ModelInfo &lhs, const ModelInfo &rhs)
              {
        if (timeframeOrder(lhs.timeframe) != timeframeOrder(rhs.timeframe))
          return timeframeOrder(lhs.timeframe) < timeframeOrder(rhs.timeframe);
        return lhs.name < rhs.name; });

    return models;
  }

  std::string ModelService::resolve(CandleTimeframe timeframe,
                                    const std::string &instrument,
                                    const std::string &overridePath) const
  {
    std::error_code ec;
    if (!overridePath.empty())
      return overridePath;

    const std::string instrumentTag = lowerCopy(instrument);
    const std::string timeframeTag = to_string(timeframe);
    const fs::path modelsDir(m_paths.modelsDir);

    const std::vector<fs::path> candidates = {
        modelsDir / (instrumentTag + "-" + timeframeTag + "-bid.dat"),
        modelsDir / (instrumentTag + "-" + timeframeTag + ".dat"),
        modelsDir / ("gold_digger_" + timeframeTag + ".dat"),
        modelsDir / ("gold_digger_" + instrumentTag + "_" + timeframeTag + ".dat"),
    };

    for (const auto &candidate : candidates)
    {
      if (fs::is_regular_file(candidate, ec))
        return candidate.string();
    }

    // Any other model that carries this timeframe, newest first. This keeps
    // working when a user renames a model, e.g. "xauusd-m15-v2.dat".
    fs::path best;
    fs::file_time_type bestTime{};
    if (fs::is_directory(modelsDir, ec))
    {
      for (const auto &entry : fs::directory_iterator(modelsDir, ec))
      {
        if (ec)
          break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".dat")
          continue;
        const std::string name = lowerCopy(entry.path().filename().string());
        if (name.rfind("tuner_", 0) == 0 || name.rfind("tunner_", 0) == 0)
          continue;
        if (name.find(timeframeTag) == std::string::npos)
          continue;

        const auto writeTime = fs::last_write_time(entry.path(), ec);
        if (best.empty() || (!ec && writeTime > bestTime))
        {
          best = entry.path();
          bestTime = writeTime;
        }
      }
    }

    return best.empty() ? candidates.front().string() : best.string();
  }

  bool ModelService::hasModel(CandleTimeframe timeframe,
                              const std::string &instrument,
                              const std::string &overridePath) const
  {
    std::error_code ec;
    return fs::is_regular_file(resolve(timeframe, instrument, overridePath), ec);
  }

  std::shared_ptr<const ModelService::LoadedModel> ModelService::loadModel(
      const std::string &path) const
  {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
      throw std::runtime_error("model file not found: " + path);

    const auto mtime = fs::last_write_time(path, ec);
    if (ec)
      throw std::runtime_error("cannot stat model file: " + path);
    const auto size = fs::file_size(path, ec);
    if (ec)
      throw std::runtime_error("cannot stat model file: " + path);

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_cache.find(path);
      if (found != m_cache.end() && found->second->mtime == mtime && found->second->size == size)
        return found->second;
    }

    auto loaded = std::make_shared<LoadedModel>();
    try
    {
      dlib::deserialize(path) >> loaded->decisionFunction >> loaded->normalizer;
    }
    catch (const std::exception &ex)
    {
      throw std::runtime_error("failed to load model '" + path + "': " + ex.what());
    }
    loaded->mtime = mtime;
    loaded->size = size;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache[path] = loaded;
    return loaded;
  }

  PredictionResult ModelService::predict(CandleTimeframe timeframe,
                                         const std::string &instrument,
                                         const std::vector<Candle> &candles,
                                         double estimatedSpread,
                                         const std::string &overridePath)
  {
    PredictionResult result;
    result.timeframe = to_string(timeframe);
    result.modelPath = resolve(timeframe, instrument, overridePath);
    result.estimatedSpread = estimatedSpread;
    result.horizonCandles = (timeframe == CandleTimeframe::M15) ? feature_builder::kM15HorizonCandles
                                                                : feature_builder::kDefaultHorizonCandles;

    std::error_code ec;
    if (!fs::is_regular_file(result.modelPath, ec))
    {
      result.error = "no trained model found for " + result.timeframe +
                     "; expected " + result.modelPath;
      return result;
    }

    const std::size_t required = feature_builder::min_history_candles();
    if (candles.size() < required)
    {
      result.error = "not enough candles for the model: need " + std::to_string(required) +
                     ", have " + std::to_string(candles.size());
      return result;
    }

    try
    {
      const auto model = loadModel(result.modelPath);
      sample_type sample = feature_builder::build_feature_sample(candles, candles.size() - 1);
      sample = model->normalizer(sample);

      result.predictedLogReturn = model->decisionFunction(sample);
      result.candle = candles.back();
      result.candlesUsed = candles.size();
      result.targetPrice = result.candle.close * std::exp(result.predictedLogReturn);
      result.change = result.targetPrice - result.candle.close;
      result.changePct = (std::exp(result.predictedLogReturn) - 1.0) * 100.0;
      result.action = (result.change > estimatedSpread)
                          ? "BUY"
                          : ((result.change < -estimatedSpread) ? "SELL" : "HOLD");

      const auto intervalMs = static_cast<std::uint64_t>(candle_interval(timeframe).count());
      result.horizonEndMs = result.candle.timestamp +
                            intervalMs * static_cast<std::uint64_t>(result.horizonCandles + 1);
      result.available = true;
    }
    catch (const std::exception &ex)
    {
      result.error = ex.what();
    }

    if (!std::isfinite(result.predictedLogReturn) || !std::isfinite(result.targetPrice) ||
        !std::isfinite(result.change) || !std::isfinite(result.changePct))
    {
      result.available = false;
      result.error = "the model produced a non finite prediction";
    }
    return result;
  }
} // namespace golddiggerui
