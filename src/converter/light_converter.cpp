#include "converter/light_converter.h"

namespace u2g {

LightData convertLight(const YamlNode& n) {
    LightData ld;

    // m_Type: 0=Spot, 1=Directional, 2=Point, 3=Area (unsupported)
    int lightType = (int)n["m_Type"].toInt(1);
    switch (lightType) {
        case 0:  ld.godotType = "SpotLight3D"; break;
        case 1:  ld.godotType = "DirectionalLight3D"; break;
        case 2:  ld.godotType = "OmniLight3D"; break;
        default: ld.godotType = "OmniLight3D"; break; // fallback
    }

    // Color
    auto& color = n["m_Color"];
    ld.r = color["r"].toFloat(1.0f);
    ld.g = color["g"].toFloat(1.0f);
    ld.b = color["b"].toFloat(1.0f);

    // Intensity
    ld.energy = n["m_Intensity"].toFloat(1.0f);

    // Range (for point/spot)
    ld.range = n["m_Range"].toFloat(10.0f);

    // Spot angle: Unity stores full cone angle, Godot uses half angle
    ld.spotAngle = n["m_SpotAngle"].toFloat(30.0f) / 2.0f;

    // Shadows: m_Shadows.m_Type — 0=none, 1=hard, 2=soft
    auto& shadows = n["m_Shadows"];
    int shadowType = (int)shadows["m_Type"].toInt(0);
    ld.shadowEnabled = (shadowType > 0);

    return ld;
}

} // namespace u2g
