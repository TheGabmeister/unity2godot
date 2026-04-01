#pragma once
#include "util/types.h"
#include "util/log.h"
#include <string>

namespace u2g {

GuidTable buildGuidTable(const std::string& tempDir, Log& log);

} // namespace u2g
