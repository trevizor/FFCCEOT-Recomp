// Settings: the settings.ini round trip (every key, including the interface ones and the bindings) and the binding
// swap rules of the controls window.
#include <cstdio>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>

#include "settings.hpp"
#include "ntr/recomp.h"

extern "C" void cfie_camera_distance(NtrCpu* c);

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

namespace {

std::filesystem::path temp_ini() {
    return std::filesystem::temp_directory_path() / "ffcc_test_settings.ini";
}

void test_round_trip() {
    Settings a;
    a.ui_scale = 125;
    a.texture_smoothing = ntr::TextureSmoothing::Sharp;
    a.skip_prelaunch = true;
    a.menu_sounds = false;
    a.preset_chosen = true;
    a.volume = 40;
    a.hud_scale = 150;
    a.camera_distance = 275;
    a.controls.camera_wheel = false;
    a.language = GameLanguage::German;
    a.controls.spell_ring = false;
    a.controls.spell_ring_orientation = ring::Orientation::Top;
    a.controls.spell_ring_size = 90;
    a.controls.spell_ring_x = 20;
    a.controls.spell_ring_y = 80;
    input::assign_key(a.controls, input::Action::OpenMenu, SDL_SCANCODE_F9);
    input::assign_pad(a.controls, input::Action::OpenMenu, input::PadInput::button(SDL_GAMEPAD_BUTTON_GUIDE));
    const auto path = temp_ini();
    CHECK(save_settings(path, a));
    Settings b;
    CHECK(load_settings(path, b));
    CHECK(b.ui_scale == 125);
    CHECK(b.texture_smoothing == ntr::TextureSmoothing::Sharp);
    CHECK(b.skip_prelaunch);
    CHECK(!b.menu_sounds);
    CHECK(b.preset_chosen);
    CHECK(b.volume == 40);
    CHECK(b.hud_scale == 150);
    CHECK(b.camera_distance == 275);
    CHECK(!b.controls.camera_wheel);
    CHECK(b.language == GameLanguage::German);
    CHECK(b.controls == a.controls);
    CHECK(!b.controls.spell_ring);
    CHECK(b.controls.spell_ring_orientation == ring::Orientation::Top);
    CHECK(b.controls.spell_ring_size == 90 && b.controls.spell_ring_x == 20 && b.controls.spell_ring_y == 80);
    CHECK(b.controls.key[int(input::Action::OpenMenu)] == SDL_SCANCODE_F9);
    std::filesystem::remove(path);
}

void test_defaults_survive() {
    const Settings d;
    const auto path = temp_ini();
    CHECK(save_settings(path, d));
    Settings b;
    b.ui_scale = 90;
    b.menu_sounds = false;
    CHECK(load_settings(path, b));
    CHECK(b.ui_scale == d.ui_scale);
    CHECK(b.menu_sounds == d.menu_sounds);
    CHECK(b.controls == d.controls);
    CHECK(d.controls.spell_ring);
    CHECK(d.controls.key[int(input::Action::OpenMenu)] == SDL_SCANCODE_F1);
    std::filesystem::remove(path);
}

void test_alpha_defaults() {
    Settings s;
    CHECK(resolved_console_name(s) == "FFCC");  // (never the PC's name: other players see it)
    s.console_name = "Ana";
    CHECK(resolved_console_name(s) == "Ana");
    CHECK(preset_pending(s));  // (a first run offers the presets)
    s.suppress_preset = true;  // (a test run does not, and never saves that)
    CHECK(!preset_pending(s));
    const auto path = std::filesystem::temp_directory_path() / "ffcc_test_preset.ini";
    CHECK(save_settings(path, s));
    Settings back;
    CHECK(load_settings(path, back));
    CHECK(!back.suppress_preset && !back.preset_chosen);
    std::filesystem::remove(path);
}

void test_set_setting() {
    Settings s;
    CHECK(set_setting(s, "graphics.renderer", "compute"));  // (the compute renderer is switched off: OpenGL instead)
    CHECK(s.renderer == (kComputeRenderer ? ntr::Renderer::Compute : ntr::Renderer::OpenGL));
    CHECK(set_setting(s, "interface.ui_scale", "150"));
    CHECK(s.ui_scale == 150);
    CHECK(!set_setting(s, "interface.ui_scale", "20"));  // (0 or 50-200)
    CHECK(s.ui_scale == 150);
    CHECK(set_setting(s, "interface.skip_prelaunch", "true"));
    CHECK(s.skip_prelaunch);
    CHECK(set_setting(s, "audio.menu_sounds", "false"));
    CHECK(!s.menu_sounds);
    CHECK(set_setting(s, "controls.spell_ring_x", "100"));
    CHECK(set_setting(s, "controls.spell_ring_x", "101"));  // (clamped, like the other numbers)
    CHECK(set_setting(s, "controls.spell_ring_size", "10"));
    CHECK(s.controls.spell_ring_x == 100 && s.controls.spell_ring_size == 30);
    CHECK(set_setting(s, "graphics.texture_smoothing", "smooth"));
    CHECK(s.texture_smoothing == ntr::TextureSmoothing::Smooth);
    CHECK(!set_setting(s, "graphics.texture_smoothing", "blurry"));
    CHECK(s.texture_smoothing == ntr::TextureSmoothing::Smooth);
    CHECK(s.camera_distance == 200);
    CHECK(set_setting(s, "game.camera_distance", "50") && s.camera_distance == 50);
    CHECK(set_setting(s, "game.camera_distance", "400") && s.camera_distance == 400);
    CHECK(set_setting(s, "game.camera_distance", "1000") && s.camera_distance == 400);
    CHECK(set_setting(s, "game.camera_distance", "0") && s.camera_distance == 50);
    CHECK(!set_setting(s, "game.camera_distance", "bad") && s.camera_distance == 50);
    CHECK(s.local_players == 1);
    CHECK(set_setting(s, "game.local_players", "3") && s.local_players == 3);
    CHECK(set_setting(s, "game.local_players", "9") && s.local_players == 4);
    CHECK(set_setting(s, "game.local_players", "0") && s.local_players == 1);
    CHECK(s.npc_control);
    CHECK(set_setting(s, "game.npc_control", "false") && !s.npc_control);
    CHECK(s.dynamic_camera);
    CHECK(set_setting(s, "game.dynamic_camera", "false") && !s.dynamic_camera);
    CHECK(s.npc_exits);
    CHECK(set_setting(s, "game.npc_exits", "false") && !s.npc_exits);
    CHECK(!set_setting(s, "no.such_key", "1"));
}

void test_camera_coordinates() {
    CHECK(cfie::camera_coordinate(-300, 100, 100) == -300);
    CHECK(cfie::camera_coordinate(-300, 100, 50) == -100);
    CHECK(cfie::camera_coordinate(-300, 100, 400) == -1500);
    CHECK(cfie::camera_coordinate(std::numeric_limits<int32_t>::max(), -1, 400) ==
          std::numeric_limits<int32_t>::max());
    CHECK(cfie::camera_coordinate(std::numeric_limits<int32_t>::min(), 1, 400) ==
          std::numeric_limits<int32_t>::min());
}

void test_camera_hook() {
    std::array<uint32_t, 1024> page{};
    uint8_t* pages[] = {reinterpret_cast<uint8_t*>(page.data())};
    NtrCpu cpu{};
    cpu.read_pages = pages;
    cpu.write_pages = pages;
    cpu.r[0] = 0x100;
    cpu.r[2] = 0x200;
    cpu.r[14] = 0x0208468F;
    ntr_wr32(&cpu, 0x100, uint32_t(-300));
    ntr_wr32(&cpu, 0x104, 200);
    ntr_wr32(&cpu, 0x108, 80);
    ntr_wr32(&cpu, 0x200, 100);
    ntr_wr32(&cpu, 0x204, 100);
    ntr_wr32(&cpu, 0x208, 80);

    cfie::camera_distance = 100;
    cfie::dynamic_camera = false;
    cfie_camera_distance(&cpu);
    CHECK(int32_t(ntr_rd32(&cpu, 0x100)) == -300);
    cfie::camera_distance = 200;
    cfie_camera_distance(&cpu);
    CHECK(int32_t(ntr_rd32(&cpu, 0x100)) == -700);
    ntr_wr32(&cpu, 0x100, uint32_t(-300));
    ntr_wr32(&cpu, 0x104, 200);
    cfie::camera_distance = 400;
    cpu.r[14] = 0x02054603;
    cfie_camera_distance(&cpu);
    CHECK(int32_t(ntr_rd32(&cpu, 0x100)) == -300);
    cpu.r[14] = 0x0208468F;
    cfie_camera_distance(&cpu);
    CHECK(int32_t(ntr_rd32(&cpu, 0x100)) == -1500);
    CHECK(ntr_rd32(&cpu, 0x104) == 500 && ntr_rd32(&cpu, 0x108) == 80);
    CHECK(ntr_rd32(&cpu, 0x200) == 100 && ntr_rd32(&cpu, 0x204) == 100);
    cfie::camera_distance = cfie::kCameraDistanceDefault;
    cfie::dynamic_camera = true;
}

void test_frame_players() {
    // The camera looks from the south-east toward the target at about 35 degrees; heights are level.
    const float eye[3] = {500000, 300000, 500000}, target[3] = {0, 0, 0};
    const float tan_half = 0.175f, aspect = 4.0f / 3.0f;
    const float together[2][3] = {{0, 0, 0}, {20000, 0, 10000}};
    cfie::CameraFraming f = cfie::frame_players(eye, target, together, 2, tan_half, aspect);
    CHECK(f.distance == cfie::kDynamicCameraMin);
    CHECK(std::fabs(f.shift_x - 10000) < 1 && std::fabs(f.shift_z - 5000) < 1);  // centered between them
    const float apart[2][3] = {{0, 0, 0}, {-2000000, 0, 0}};
    f = cfie::frame_players(eye, target, apart, 2, tan_half, aspect);
    CHECK(f.distance == cfie::kDynamicCameraMax);
    // Out of reach of the zoom, the view follows player 1 (at 0) instead of the midpoint (-1000000).
    CHECK(f.shift_x > -100000);
    const float mid[2][3] = {{0, 0, 0}, {-1200000, 0, 0}};
    f = cfie::frame_players(eye, target, mid, 2, tan_half, aspect);
    CHECK(f.distance > cfie::kDynamicCameraMin && f.distance < cfie::kDynamicCameraMax);
    // A player on higher ground is higher on the screen, so the view has to be wider to keep both.
    const float high[2][3] = {{0, 0, 0}, {0, 600000, 0}};
    CHECK(cfie::frame_players(eye, target, high, 2, tan_half, aspect).distance > cfie::kDynamicCameraMin);
}

void test_camera_binding_migration() {
    using input::Action;
    using input::PadInput;
    const auto path = temp_ini();
    {
        std::ofstream f(path);
        f << "controls.other_screen.pad = rightstick\ncontrols.swap_screens.pad = leftstick\n";
    }
    Settings s;
    CHECK(load_settings(path, s));
    CHECK(s.controls.pad[int(Action::OtherScreen)].kind == PadInput::Kind::None);
    CHECK(s.controls.pad[int(Action::SwapScreens)].kind == PadInput::Kind::None);
    CHECK(s.controls.pad[int(Action::CameraIn)] == PadInput::button(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
    CHECK(s.controls.pad[int(Action::CameraOut)] == PadInput::button(SDL_GAMEPAD_BUTTON_LEFT_STICK));

    {
        std::ofstream f(path);
        f << "controls.other_screen.pad = south\ncontrols.a.pad = rightstick\n"
             "controls.camera_out.pad = none\ncontrols.swap_screens.pad = leftstick\n";
    }
    Settings customized;
    CHECK(load_settings(path, customized));
    CHECK(customized.controls.pad[int(Action::OtherScreen)] == PadInput::button(SDL_GAMEPAD_BUTTON_SOUTH));
    CHECK(customized.controls.pad[int(Action::A)] == PadInput::button(SDL_GAMEPAD_BUTTON_RIGHT_STICK));
    CHECK(customized.controls.pad[int(Action::CameraIn)].kind == PadInput::Kind::None);
    CHECK(customized.controls.pad[int(Action::CameraOut)].kind == PadInput::Kind::None);
    CHECK(customized.controls.pad[int(Action::SwapScreens)] == PadInput::button(SDL_GAMEPAD_BUTTON_LEFT_STICK));
    std::filesystem::remove(path);
}

void test_assign_key_swaps() {
    using input::Action;
    auto b = input::Bindings::defaults();
    const auto a = b.key[int(Action::A)], bb = b.key[int(Action::B)];
    // B takes A's key: A gets B's old one, so no key does two things.
    input::assign_key(b, Action::B, a);
    CHECK(b.key[int(Action::B)] == a);
    CHECK(b.key[int(Action::A)] == bb);
    // Unbinding.
    input::assign_key(b, Action::X, SDL_SCANCODE_UNKNOWN);
    CHECK(b.key[int(Action::X)] == SDL_SCANCODE_UNKNOWN);
    // Binding a key nobody has takes nothing from anyone.
    const auto before = b;
    input::assign_key(b, Action::Y, SDL_SCANCODE_F7);
    CHECK(b.key[int(Action::Y)] == SDL_SCANCODE_F7);
    for (int i = 0; i < input::kActionCount; i++)
        if (i != int(Action::Y)) CHECK(b.key[i] == before.key[i]);
}

void test_assign_pad_swaps() {
    using input::Action;
    using input::PadInput;
    auto b = input::Bindings::defaults();
    const PadInput a = b.pad[int(Action::A)], bb = b.pad[int(Action::B)];
    input::assign_pad(b, Action::B, a);
    CHECK(b.pad[int(Action::B)] == a);
    CHECK(b.pad[int(Action::A)] == bb);
    input::assign_pad(b, Action::X, PadInput{});
    CHECK(b.pad[int(Action::X)].kind == PadInput::Kind::None);
    // An axis in one direction is not the same input as the other direction.
    input::assign_pad(b, Action::Up, PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, false));
    input::assign_pad(b, Action::Down, PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, true));
    CHECK(b.pad[int(Action::Up)] == PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, false));
    CHECK(b.pad[int(Action::Down)] == PadInput::axis(SDL_GAMEPAD_AXIS_RIGHTY, true));
}

}  // namespace

int main() {
    test_round_trip();
    test_defaults_survive();
    test_set_setting();
    test_camera_coordinates();
    test_camera_hook();
    test_frame_players();
    test_camera_binding_migration();
    test_alpha_defaults();
    test_assign_key_swaps();
    test_assign_pad_swaps();
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("ok");
    return 0;
}
