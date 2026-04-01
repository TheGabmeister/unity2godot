#include "converter/project_writer.h"
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace u2g {

void writeProjectFile(const std::string& outputDir, const std::string& projectName, Log& log) {
    fs::create_directories(outputDir);

    std::string path = outputDir + "/project.godot";
    std::ofstream f(path);
    if (!f) {
        log.error("Failed to write project.godot");
        return;
    }

    f << "; Engine configuration file.\n";
    f << "; It's best edited using the editor, so don't edit it unless you know what you're doing.\n";
    f << "\n";
    f << "config_version=5\n";
    f << "\n";
    f << "[application]\n";
    f << "config/name=\"" << projectName << "\"\n";
    f << "config/features=PackedStringArray(\"4.6\")\n";
    f << "\n";
    f << "[rendering]\n";
    f << "renderer/rendering_method=\"forward_plus\"\n";

    log.info("Generated project.godot");
}

} // namespace u2g
