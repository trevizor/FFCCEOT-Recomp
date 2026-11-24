// State the game hooks (CFIE, CFIP) share with the frontend.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace cfie {

struct GameEvents {
    // Background music started for the first time: the title screen (nothing before it plays BGM).
    bool title_bgm_started = false;
};

extern GameEvents events;

// Online play: how much the host buffers a guest's inputs (see cfie_set_online_timing).
enum class OnlineBuffer { Low, Medium, Original };
extern OnlineBuffer online_buffer;

// Save reads and writes at once instead of at the DS's pace (see cfie_card_backup_request).
extern bool fast_saving;

// Analog movement (see cfie_pack_input): when this console sends its stick in its input records. Every console of
// this build moves a character by the stick its records carry, whatever its own setting, so all stay in step.
// The 3D projection's aspect ratio (width over height, 4.12 fixed point): 0x1555 is the DS's 4:3.
extern uint16_t aspect;
inline constexpr int kCameraDistanceMin = 50;
inline constexpr int kCameraDistanceMax = 400;
inline constexpr int kCameraDistanceDefault = 200;
inline constexpr int kCameraDistanceStep = 25;
extern int camera_distance;  // percent of the game's distance; set and read on the game thread
extern bool npc_exits;  // controlled party members trigger map exits and entrances
extern bool npc_interact;  // WIP, does not work yet: controlled party members talk to characters, open shops and use crystals
extern bool pads_use_menus;  // any connected controller operates menus, dialogue and cutscenes
extern bool dynamic_camera;  // with several players the camera frames them all, 2x to 4x the game's distance
inline constexpr int kDynamicCameraMin = 200;
inline constexpr int kDynamicCameraMax = 400;

// Where the camera must be so every player is in view. Positions are (x, y, z) in the game's world, y up; the camera
// looks from `eye` to `target`. `tan_half_fov` and `aspect` give the view's width at a distance.
struct CameraFraming {
    int distance = kDynamicCameraMax;  // percent of the game's distance
    float shift_x = 0, shift_z = 0;    // how far the target moves, in world units
};
CameraFraming frame_players(const float eye[3], const float target[3], const float (*players)[3], int count,
                            float tan_half_fov, float aspect);

inline int32_t camera_coordinate(int32_t eye, int32_t target, int distance) {
    const int64_t value = int64_t(target) + (int64_t(eye) - target) * distance / 100;
    return int32_t(std::clamp(value, int64_t(std::numeric_limits<int32_t>::min()),
                              int64_t(std::numeric_limits<int32_t>::max())));
}
enum class AnalogMovement { Off, SinglePlayer, Multiplayer };
extern AnalogMovement analog_movement;

// The left stick as the frontend last read it, x to the right and y down, its length how far it is pushed past the
// deadzone (0 to 1); zero when it is not.
struct Stick {
    float x = 0, y = 0;
};
extern Stick stick;

// Party members a controller has taken (see cfie_npc_input), by player slot (1 to 3): the controller's stick, as for
// `stick`. npc_idle_ticks[n] counts the frontend's input ticks since the game last moved the party member in slot n,
// which is how the frontend knows who is in the party and on the field. Both are read and written on the game thread.
struct NpcInput {
    bool active = false;
    float x = 0, y = 0;
    uint16_t buttons = 0;  // DS actions, R and directions
    int spell = 0;
};
extern NpcInput npc_input[4];
extern bool npc_buttons_read[4];
extern int npc_idle_ticks[4];
extern bool npc_control;  // extra controllers take party members (Start); only with one window
extern bool npc_offline;

// Menu sounds: asks the game to play its sound effect `id` (its own number for it, as the game's menus pass it). The
// effect starts at the game's next sound update, on the game's own thread; it is dropped if the game doesn't get there
// within a third of a second. Safe to call from any thread.
void queue_sound(uint16_t id);

// Logs the id of every sound effect the game starts (to stderr), to learn the numbers (--log-sounds).
extern bool log_sounds;

// The spell element (the six gems of the bottom screen, top to bottom: 0 to 5). request_spell asks for one; the game's
// own pad handler applies it at its next tick, the way R + D-pad does, so it only happens while the field's HUD is
// live and the game decides what is allowed. Safe to call from any thread.
void request_spell(int element);
// The element the game has now (-1 until its HUD has run once), and how many ticks the HUD's pad handler has run:
// a counter that moves means the player is on the field with the spell bar.
int spell_element();
uint64_t spell_ticks();

// The cutscene the game is playing (its movie object, movie/movie%03d.mods): which one (the number in the file name) and the
// number of the frame the game has decoded last (0 is the first). The picture on the DS's screen is that frame. Safe to
// call from any thread; `playing` is false between movies.
struct MovieState {
    bool playing = false;
    int id = 0;
    int frame = 0;
};
MovieState movie();

// Logs each frame of a cutscene the game decodes (to stderr), to learn the numbers (--log-movies).
extern bool log_movies;
// Debugging: every cutscene the game opens is this movie instead (0: as the game asks).
extern int force_movie;

// Picks the release the hooks' addresses are for: "CFIE" (USA, the default) or "CFIP" (Europe). False for any other
// code. Call it before the game starts.
bool select_release(const std::string& game_code);

// The hooks of game/config/<release>/hooks.txt by name, for the game module (null for a name that is not one).
void* find_hook(const char* name);

}  // namespace cfie
