#include "converter/camera_converter.h"

namespace u2g {

CameraData convertCamera(const YamlNode& n) {
    CameraData cd;

    cd.fov       = n["field of view"].toFloat(60.0f);
    cd.nearClip  = n["near clip plane"].toFloat(0.3f);
    cd.farClip   = n["far clip plane"].toFloat(1000.0f);

    // orthographic: 0=perspective, 1=orthographic
    cd.orthographic = (n["orthographic"].toInt(0) == 1);
    cd.orthoSize    = n["orthographic size"].toFloat(5.0f);

    return cd;
}

} // namespace u2g
