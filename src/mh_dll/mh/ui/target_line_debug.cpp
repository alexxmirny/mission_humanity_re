#include "ui/target_line_debug.h"
#include "imgui.h"
#include "ui/target_line_painter.h"

namespace mh::ui::selected_targets::detail {

void draw_target_line_controls() {
    if (!ImGui::CollapsingHeader("Selected target lines")) return;
    ImGui::PushID("selected_target_lines");
    ImGui::PushItemWidth(150.0f); // leave the full join-radius labels visible in the340px panel
    auto &o = options();
    ImGui::TextDisabled("Local presentation only; no simulation changes");
    ImGui::SliderFloat("Opacity", &o.opacity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Width", &o.width, 0.5f, 3.0f, "%.2f 1x px", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Min distance", &o.minimum_tiles, 0.0f, 64.0f, "%.1f tiles", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Total remaining wrapped ground length, including approach and one patrol circuit.\nReveal-role held or any off-screen waypoint restores every connector; markers remain below cutoff.");
    ImGui::SliderFloat("Dash speed", &o.speed, 0.0f, 128.0f, "%.1f 1x px/s", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Dash length", &o.dash_length, 1.0f, 64.0f, "%.1f 1x px", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Dash gap", &o.gap_length, 1.0f, 64.0f, "%.1f 1x px", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextDisabled("Artwork: 1x map px; outline: +1 px");
    ImGui::Separator();
    if (ImGui::RadioButton("Subgroups", o.grouping == grouping_mode::subgroups)) o.grouping = grouping_mode::subgroups;
    ImGui::SameLine();
    if (ImGui::RadioButton("Shared command", o.grouping == grouping_mode::shared_command)) o.grouping = grouping_mode::shared_command;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Merge only a proven common prefix; split at divergence.\nMatching later coordinates never reconnect different route stages.");
    ImGui::SliderFloat("Source join radius", &o.source_radius_tiles, 0.0f, 8.0f, "%.1f tiles", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Goal join radius", &o.goal_radius_tiles, 0.0f, 4.0f, "%.1f tiles", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TextWrapped("Subgroups join compatible nearby sources and goals. Shared command joins only proven common issued intent. Entity target identity stays exact in both modes.");
    ImGui::Separator();
    constexpr auto colors = ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_Uint8;
    ImGui::ColorEdit3("Movement color", o.movement_color, colors);
    ImGui::ColorEdit3("Attack color", o.attack_color, colors);
    ImGui::ColorEdit3("Rally color", o.rally_color, colors);
    ImGui::ColorEdit3("Patrol color", o.patrol_color, colors);
    ImGui::PopItemWidth();
    ImGui::PopID();
}

} // namespace mh::ui::selected_targets::detail
