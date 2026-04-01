#pragma once
#include "util/types.h"
#include "util/log.h"
#include <string>
#include <atomic>

namespace u2g {

void convertTextures(const GuidTable& guids,
                     const std::string& outputDir,
                     Log& log,
                     std::atomic<bool>& cancelled,
                     ProgressCallback progress);

} // namespace u2g
