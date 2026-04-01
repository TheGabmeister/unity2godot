#include "gui/converter_ui.h"
#include "imgui.h"
#include <nfd.h>
#include <cstring>

namespace u2g {

void ConverterUI::render() {
    // Full-window ImGui panel
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Unity2Godot Converter", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // Title
    ImGui::Text("Unity2Godot Converter");
    ImGui::SameLine(ImGui::GetWindowWidth() - 50);
    ImGui::TextDisabled("v1");
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Package path row ----
    ImGui::Text("Package:");
    ImGui::SameLine(80);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
    ImGui::InputText("##pkg", packagePath_, sizeof(packagePath_));
    ImGui::SameLine();
    if (ImGui::Button("Browse##pkg")) {
        nfdu8char_t* outPath = nullptr;
        nfdu8filteritem_t filter[1] = { {"Unity Package", "unitypackage"} };
        nfdopendialogu8args_t args = {};
        args.filterList  = filter;
        args.filterCount = 1;
        if (NFD_OpenDialogU8_With(&outPath, &args) == NFD_OKAY) {
            strncpy(packagePath_, outPath, sizeof(packagePath_) - 1);
            packagePath_[sizeof(packagePath_) - 1] = '\0';
            NFD_FreePathU8(outPath);
        }
    }

    // ---- Output path row ----
    ImGui::Text("Output:");
    ImGui::SameLine(80);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
    ImGui::InputText("##out", outputPath_, sizeof(outputPath_));
    ImGui::SameLine();
    if (ImGui::Button("Browse##out")) {
        nfdu8char_t* outPath = nullptr;
        nfdpickfolderu8args_t args = {};
        if (NFD_PickFolderU8_With(&outPath, &args) == NFD_OKAY) {
            strncpy(outputPath_, outPath, sizeof(outputPath_) - 1);
            outputPath_[sizeof(outputPath_) - 1] = '\0';
            NFD_FreePathU8(outPath);
        }
    }

    ImGui::Spacing();

    // ---- Convert / Cancel button + progress bar ----
    bool running    = converter_.isRunning();
    bool canConvert = !running && strlen(packagePath_) > 0 && strlen(outputPath_) > 0;

    if (running) {
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
            converter_.cancel();
    } else {
        if (!canConvert) ImGui::BeginDisabled();
        if (ImGui::Button("Convert", ImVec2(80, 0))) {
            converter_.log().clear();
            converter_.start(packagePath_, outputPath_);
        }
        if (!canConvert) ImGui::EndDisabled();
    }

    ImGui::SameLine();
    auto prog = converter_.currentProgress();
    ImGui::Text("%s", prog.phase.c_str());
    ImGui::SameLine();
    ImGui::ProgressBar(prog.progress, ImVec2(-1, 0));

    ImGui::Spacing();

    // ---- Log window ----
    ImGui::Text("Log:");
    ImGui::SameLine();
    auto entries = converter_.log().entries();
    if (ImGui::SmallButton("Copy Log")) {
        std::string logText;
        for (auto& e : entries) {
            const char* prefix;
            switch (e.level) {
                case LogLevel::Info:  prefix = "[INFO]  "; break;
                case LogLevel::Warn:  prefix = "[WARN]  "; break;
                case LogLevel::Error: prefix = "[ERROR] "; break;
                case LogLevel::Fatal: prefix = "[FATAL] "; break;
                default:              prefix = "[INFO]  "; break;
            }
            logText += prefix;
            logText += e.message;
            logText += '\n';
        }
        ImGui::SetClipboardText(logText.c_str());
    }
    float logHeight = converter_.isDone()
        ? ImGui::GetContentRegionAvail().y * 0.6f
        : ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("LogScroll", ImVec2(0, logHeight), ImGuiChildFlags_Borders);

    for (auto& e : entries) {
        ImVec4 color;
        const char* prefix;
        switch (e.level) {
            case LogLevel::Info:  color = ImVec4(1, 1, 1, 1);          prefix = "[INFO]  "; break;
            case LogLevel::Warn:  color = ImVec4(1, 1, 0.2f, 1);      prefix = "[WARN]  "; break;
            case LogLevel::Error: color = ImVec4(1, 0.3f, 0.3f, 1);   prefix = "[ERROR] "; break;
            case LogLevel::Fatal: color = ImVec4(1, 0, 0, 1);         prefix = "[FATAL] "; break;
            default:              color = ImVec4(1, 1, 1, 1);          prefix = "[INFO]  "; break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextWrapped("%s%s", prefix, e.message.c_str());
        ImGui::PopStyleColor();
    }

    if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();

    // ---- Skip report (shown after conversion) ----
    if (converter_.isDone()) {
        ImGui::Text("Skip Report:");
        ImGui::BeginChild("SkipReport", ImVec2(0, 0), ImGuiChildFlags_Borders);

        auto report = converter_.skipReport();
        if (report.empty()) {
            ImGui::Text("No assets were skipped.");
        } else {
            for (auto& cat : report) {
                ImGui::Text("%s: %zu file(s)", cat.name.c_str(), cat.files.size());
                for (auto& f : cat.files) {
                    ImGui::TextWrapped("    %s", f.c_str());
                }
            }
        }

        ImGui::EndChild();
    }

    ImGui::End();
}

} // namespace u2g
