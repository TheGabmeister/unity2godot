#pragma once
#include "util/log.h"
#include <string>

namespace u2g {

void writeProjectFile(const std::string& outputDir,
                      const std::string& projectName,
                      Log& log);

} // namespace u2g
