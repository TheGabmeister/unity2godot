#pragma once
#include "util/types.h"
#include "util/log.h"
#include <string>
#include <atomic>
#include <map>

namespace u2g {

// guid -> Godot res:// path of the converted .tres
using MaterialMap = std::map<std::string, std::string>;

MaterialMap convertMaterials(const GuidTable& guids,
                             const std::string& outputDir,
                             Log& log,
                             std::atomic<bool>& cancelled,
                             ProgressCallback progress);

} // namespace u2g
