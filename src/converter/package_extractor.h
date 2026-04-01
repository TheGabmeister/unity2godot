#pragma once
#include "util/types.h"
#include "util/log.h"
#include <string>
#include <atomic>

namespace u2g {

bool extractPackage(const std::string& packagePath,
                    const std::string& tempDir,
                    Log& log,
                    std::atomic<bool>& cancelled);

} // namespace u2g
