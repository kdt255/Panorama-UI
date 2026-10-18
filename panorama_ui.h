#pragma once

#include "imgui.h"

namespace panorama
{
enum class SettingsTab { Ragebot, Legitbot, Visuals, Misc, Config, Preview };
enum class RagebotSection { Weapon, AntiAim, Misc };
enum class VisualsSection { Enemy, Team, Local, Other };
enum class MiscSection { Movement, Other };
enum class PreviewSection { Combos, Sliders, SliderCheck, Checkboxes, Colors, Buttons };

struct ColorRGBA
{
    int r = 50;
    int g = 250;
    int b = 50;
    int a = 200;
    bool alpha_enabled = true;
};

struct State
{
    // UI-thread owned state; the preview does not write to a game process.
    bool visible = true;
    SettingsTab tab = SettingsTab::Ragebot;
    int section = 0;
    SettingsTab previous_tab = SettingsTab::Ragebot;
    int previous_section = 0;
    float page_transition = 1.0f;
    float previous_offset = 0.0f;
    int page_direction = 0;

    // Ragebot - Weapon
    bool rage_enabled = false;
    int rage_target_selection = 0;
    float rage_fov = 180.0f;
    int rage_hitchance = 65;
    int rage_min_damage = 35;
    bool rage_min_damage_override = true;
    int rage_override_damage = 15;
    bool rage_auto_fire = true;
    bool rage_auto_scope = true;

    // Ragebot - Anti Aim
    bool aa_enabled = false;
    int aa_pitch = 1;
    int aa_yaw_base = 1;
    int aa_yaw = 1;
    float aa_yaw_offset = 0.0f;
    bool aa_jitter_enabled = true;
    float aa_jitter_range = 25.0f;

    // Ragebot - Misc
    bool rage_remove_recoil = true;
    int rage_auto_stop = 1;
    bool rage_double_tap = false;
    bool rage_hide_shots = false;

    // Legitbot
    bool legit_enabled = true;
    int legit_hitbox = 0;
    float legit_fov = 4.5f;
    float legit_smooth = 6.0f;
    int legit_reaction_time = 80;
    bool legit_rcs = true;
    float legit_rcs_scale = 0.8f;
    bool legit_triggerbot = false;

    // Visuals - Enemy
    bool vis_enemy_box = true;
    bool vis_enemy_name = true;
    bool vis_enemy_health = true;
    int vis_enemy_weapon = 1;
    bool vis_enemy_outline = true;
    float vis_enemy_outline_width = 0.5f;
    bool vis_enemy_skeleton = false;
    bool vis_enemy_glow = true;
    int vis_enemy_chams = 1;
    ColorRGBA vis_enemy_color = { 50, 250, 50, 200, true };

    // Visuals - Team
    bool vis_team_enabled = false;
    bool vis_team_box = false;
    bool vis_team_name = true;
    bool vis_team_health = true;
    bool vis_team_outline = true;
    float vis_team_outline_width = 0.5f;
    int vis_team_chams = 0;
    ColorRGBA vis_team_color = { 60, 170, 255, 200, true };

    // Visuals - Local
    bool vis_thirdperson = true;
    float vis_thirdperson_dist = 120.0f;
    float vis_viewmodel_fov = 68.0f;
    float vis_override_fov = 90.0f;
    int vis_local_chams = 0;
    ColorRGBA vis_local_color = { 255, 180, 50, 210, true };

    // Visuals - Other
    bool vis_dropped_weapons = true;
    bool vis_grenade_prediction = true;
    bool vis_bomb_timer = true;
    bool vis_nightmode = true;
    int vis_brightness = 80;
    bool vis_remove_flash = true;
    bool vis_remove_smoke = false;
    ColorRGBA vis_world_color = { 180, 120, 255, 190, true };

    // Misc - Movement
    bool misc_bhop = true;
    int misc_autostrafe = 1;
    bool misc_fast_stop = false;
    bool misc_infinite_duck = false;
    bool misc_edge_jump = false;

    // Misc - Other
    int misc_hitsound = 1;
    bool misc_hitmarker = true;
    bool misc_spectator_list = true;
    bool misc_reveal_ranks = true;
    bool misc_clantag = false;

    // Config
    int cfg_slot = 0;
    int cfg_count = 5;
    char cfg_name[64] = {};
    char cfg_names[16][64] = { "DEFAULT", "RAGE", "LEGIT", "SEMI-RAGE", "HVH" };
    float cfg_menu_scale = 1.0f;
    bool cfg_stream_proof = true;
    bool cfg_safe_mode = true;

    // Preview
    bool prev_combo_bool_text = true;
    int prev_combo_text_1 = 1;
    int prev_combo_text_2 = 2;
    bool prev_combo_bool_filled = true;
    int prev_combo_filled_1 = 2;
    int prev_combo_filled_2 = 0;

    float prev_slider_float_1 = 90.0f;
    float prev_slider_float_2 = 1.25f;
    float prev_slider_float_3 = 75.0f;
    int prev_slider_int_1 = 65;
    int prev_slider_int_2 = 42;
    int prev_slider_int_3 = 120;

    bool prev_schk_1 = true;
    float prev_schk_val_1 = 0.8f;
    bool prev_schk_2 = false;
    float prev_schk_val_2 = 35.0f;
    bool prev_schk_3 = true;
    int prev_schk_val_3 = 25;
    bool prev_schk_4 = false;
    int prev_schk_val_4 = 14;

    bool prev_chk_1 = true;
    bool prev_chk_2 = false;
    bool prev_chk_3 = true;
    bool prev_chk_4 = true;
    bool prev_chk_5 = false;

    ColorRGBA prev_color_1 = { 50, 250, 120, 220, true };
    ColorRGBA prev_color_2 = { 90, 165, 255, 180, false };

    char prev_input_text[64] = "Panorama UI Showcase";
    int prev_btn_clicks = 0;
};

struct Fonts
{
    ImFont* noto_regular = nullptr;
    ImFont* noto_bold = nullptr;
    ImFont* stratum_regular = nullptr;
    ImFont* stratum_bold = nullptr;
};

bool LoadEmbeddedFonts(ImFontAtlas& atlas, Fonts& fonts);
void Render(State& state, const Fonts& fonts);
}
