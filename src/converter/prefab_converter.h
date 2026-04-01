#pragma once
#include "util/types.h"
#include "util/log.h"
#include <string>
#include <atomic>
#include <map>

namespace u2g {

// guid -> res:// path of the converted prefab .tscn
using PrefabMap = std::map<std::string, std::string>;

PrefabMap convertPrefabs(const GuidTable& guids,
                         const std::map<std::string, std::string>& materialMap,
                         const std::string& outputDir,
                         Log& log,
                         std::atomic<bool>& cancelled,
                         ProgressCallback progress);

} // namespace u2g
