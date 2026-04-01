#pragma once
#include "converter/unity_yaml_parser.h"

namespace u2g {

struct CameraData {
    float fov = 60;
    float nearClip = 0.3f;
    float farClip = 1000;
    bool orthographic = false;
    float orthoSize = 5;
};

CameraData convertCamera(const YamlNode& cameraNode);

} // namespace u2g
