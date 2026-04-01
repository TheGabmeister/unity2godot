#pragma once
#include "converter/converter.h"

namespace u2g {

class ConverterUI {
public:
    void render();

private:
    Converter converter_;
    char packagePath_[1024] = {};
    char outputPath_[1024]  = {};
    bool autoScroll_ = true;
};

} // namespace u2g
