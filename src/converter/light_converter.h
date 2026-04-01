#pragma once
#include "converter/unity_yaml_parser.h"
#include <string>

namespace u2g {

struct LightData {
    std::string godotType; // "DirectionalLight3D", "OmniLight3D", "SpotLight3D"
    float r = 1, g = 1, b = 1;
    float energy = 1;
    float range = 10;
    float spotAngle = 45;
    bool shadowEnabled = false;
};

LightData convertLight(const YamlNode& lightNode);

} // namespace u2g
