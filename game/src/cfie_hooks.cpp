// Game-specific glue for the hooks listed in game/config/<release>/hooks.txt (CFIE: USA, CFIP: Europe).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

#include "cfie_game.hpp"
#include "ntr/nitro.h"
#include "ntr/recomp.h"

namespace cfie {
GameEvents events;
OnlineBuffer online_buffer = OnlineBuffer::Low;
bool fast_saving = true;
AnalogMovement analog_movement = AnalogMovement::SinglePlayer;
uint16_t aspect = 0x1555;
int camera_distance = kCameraDistanceDefault;
bool dynamic_camera = true;
bool npc_exits = true;

CameraFraming frame_players(const float eye[3], const float target[3], const float (*players)[3], int count,
                            float tan_half_fov, float aspect) {
    CameraFraming out;
    const float fx = target[0] - eye[0], fy = target[1] - eye[1], fz = target[2] - eye[2];
    const float dist = std::sqrt(fx * fx + fy * fy + fz * fz), horizontal = std::hypot(fx, fz);
    if (count < 1 || dist < 1 || horizontal < 1e-3f || tan_half_fov <= 0 || aspect <= 0) return out;
    // Screen-aligned axes: a to the camera's right, b up the screen. Ground away from the camera rises by the sine of
    // the view's pitch and height by its cosine; heights count from the first player's feet (the target follows them).
    const float gx = fx / horizontal, gz = fz / horizontal, rx = -gz, rz = gx;
    const float pitch = std::clamp(std::fabs(fy) / dist, 0.2f, 1.0f), lift = std::sqrt(1 - pitch * pitch);
    float a[4], b[4], min_a = 1e30f, max_a = -1e30f, min_b = 1e30f, max_b = -1e30f;
    count = std::min(count, 4);
    for (int i = 0; i < count; i++) {
        const float qx = players[i][0] - target[0], qz = players[i][2] - target[2];
        a[i] = qx * rx + qz * rz;
        b[i] = (qx * gx + qz * gz) * pitch + (players[i][1] - players[0][1]) * lift;
        min_a = std::min(min_a, a[i]), max_a = std::max(max_a, a[i]);
        min_b = std::min(min_b, b[i]), max_b = std::max(max_b, b[i]);
    }
    constexpr float kFill = 0.75f;  // the players stay inside this share of the view's half size
    const float half_w = dist * tan_half_fov * aspect, half_h = dist * tan_half_fov;
    const float need = std::max((max_a - min_a) / 2 / (kFill * half_w), (max_b - min_b) / 2 / (kFill * half_h));
    const float scale = std::clamp(need, kDynamicCameraMin / 100.0f, kDynamicCameraMax / 100.0f);
    out.distance = int(std::lround(scale * 100));
    float center_a = (min_a + max_a) / 2, center_b = (min_b + max_b) / 2;
    // Player 1 is the one that must not be lost: the closer they are to the view's edge, the more it follows them.
    const float edge = std::max(std::fabs(a[0] - center_a) / (half_w * scale), std::fabs(b[0] - center_b) / (half_h * scale));
    const float t = std::clamp((edge - 0.5f) / 0.3f, 0.0f, 1.0f), weight = t * t * (3 - 2 * t);
    center_a += (a[0] - center_a) * weight;
    center_b += (b[0] - center_b) * weight;
    out.shift_x = rx * center_a + gx * center_b / pitch;
    out.shift_z = rz * center_a + gz * center_b / pitch;
    return out;
}
Stick stick;
NpcInput npc_input[4];
bool npc_buttons_read[4] = {};
int npc_idle_ticks[4] = {1000, 1000, 1000, 1000};
bool npc_control = true;
bool npc_offline = true;
bool log_sounds = false;
bool log_movies = false;
int force_movie = 0;
}  // namespace cfie

namespace {
// The game's addresses. The European release is the USA build with some code added in the middle of the program: its
// code after about 0x020A3800 sits 0x1C higher and everything from the end of the program (the data, then the
// overlays) 0x20 higher. Addresses in the comments below are the USA release's (CFIE).
struct Addresses {
    uint32_t os_irq_table;  // OS_IRQTable (DTCM)
    uint32_t os_irq_handler_thread_switch;  // OS_IrqHandler_ThreadSwitch (ITCM)
    uint32_t errno_;  // the C library's errno
    uint32_t guest_send_allowance;  // how many more inputs the host lets a guest send, less one
    uint32_t online_timing;  // the online timing table (see cfie_set_online_timing)
    uint32_t network;  // the game's network state (+2: 0 alone, 2 online)
    uint32_t network_is_online;  // (network): the Wi-Fi Connection is up
    uint32_t card_common;  // the card library's common state (cardi_common)
    uint32_t players;  // the players (572 bytes each; +328: their character)
    uint32_t input_slots;  // per-slot input records (32 bytes each)
    uint32_t player_present;  // (players, n): player n is in the game
    uint32_t move_yaw;  // (character): the camera's yaw its movement turns by
    uint32_t sin_cos_table;  // FX_SinCosTable_: sin, cos (4.12) per 16 angle units
    uint32_t animation_step;  // (character): 1, or 1.5 and 0.5 under Haste and Slow
    uint32_t run_animation;  // 'RUN ', at a character's +364 while it plays
    uint32_t start_sound;  // (_, id): the sound manager starts sound effect `id`
    uint32_t spell_element;  // the selected spell element (a byte, 0 to 5)
    uint32_t spell_actions;  // the field HUD's actions: (actions, element + 1) selects one
    uint32_t trigger_action;  // (actions, id): runs the handler of an action
    uint32_t blank_input_call;  // return address: a slot nobody's records came in for
    uint32_t field_state;
};
constexpr Addresses kUsa = {
    0x027E0000,  // os_irq_table
    0x01FF8190,  // os_irq_handler_thread_switch
    0x02103AFC,  // errno_
    0x0212F8A8 + 68,  // guest_send_allowance
    0x0212FD28,  // online_timing
    0x0212F700,  // network
    0x020AD998,  // network_is_online
    0x020FEE40,  // card_common
    0x02122E98,  // players
    0x021218AC,  // input_slots
    0x02099780,  // player_present
    0x02057220,  // move_yaw
    0x020CD780,  // sin_cos_table
    0x0206F1C8,  // animation_step
    0x52554E20,  // run_animation
    0x020B2B10,  // start_sound
    0x02135148,  // spell_element
    0x02136D6C,  // spell_actions
    0x020C68A0,  // trigger_action
    0x020ADF81,  // blank_input_call
    0x0211677C  // field_state
};
constexpr Addresses kEur = {
    0x027E0000,  // os_irq_table
    0x01FF8190,  // os_irq_handler_thread_switch
    0x02103B1C,  // errno_
    0x0212F8C8 + 68,  // guest_send_allowance
    0x0212FD48,  // online_timing
    0x0212F720,  // network
    0x020AD9B4,  // network_is_online
    0x020FEE60,  // card_common
    0x02122EB8,  // players
    0x021218CC,  // input_slots
    0x02099780,  // player_present
    0x02057220,  // move_yaw
    0x020CD79C,  // sin_cos_table
    0x0206F1C8,  // animation_step
    0x52554E20,  // run_animation
    0x020B2B2C,  // start_sound
    0x02135168,  // spell_element
    0x02136D8C,  // spell_actions
    0x020C68BC,  // trigger_action
    0x020ADF9D,  // blank_input_call
    0x0211679C  // field_state
};
const Addresses* A = &kUsa;  // the release being played (cfie::select_release)

// Menu sounds: a small ring of requests from the frontend, started by cfie_sound_update.
struct SoundRequest {
    std::atomic<uint16_t> id{0};
    std::atomic<int64_t> at{0};  // steady-clock milliseconds
};
constexpr uint32_t kSoundQueue = 16;
constexpr int64_t kSoundMaxAgeMs = 300;
SoundRequest sound_queue[kSoundQueue];
std::atomic<uint32_t> sound_head{0}, sound_tail{0};

std::atomic<int> spell_request{-1};
std::atomic<int> spell_now{-1};
std::atomic<uint64_t> spell_tick_count{0};

// The cutscene being played, packed for one atomic read: bit 63 playing, bits 32..46 the movie's number, the low 32 bits the
// frame last decoded.
std::atomic<uint64_t> movie_now{0};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Analog movement: the stick each player slot's input records carry (see cfie_pack_input), the same on every console.
struct SlotStick {
    bool marked = false;  // the records come from this build
    uint8_t angle = 0;    // 256 steps, 0 = down, 64 = right
    uint8_t push = 0;     // how far it is pushed, 0 (at rest) to kFullPush
};
constexpr uint8_t kFullPush = 63;
constexpr uint16_t kNpcActionButtons = 0x0C03;
constexpr uint16_t kDsDirections = 0x00F0;
constexpr uint16_t kDsA = 0x0001;
constexpr uint16_t kDsR = 0x0100;
constexpr uint16_t kDsX = 0x0400;
constexpr uint32_t kCastState = 23;
SlotStick slot_sticks[4];

// The stick of player slot `player` (a character's +172), or null if the D-pad moves it as on the DS.
const SlotStick* moving_stick(int32_t player) {
    if (player < 0 || player >= 4 || slot_sticks[player].push == 0) return nullptr;
    return &slot_sticks[player];
}

// Whether this console sends its stick: as set, and in a network session only while every player's console reads it
// (a DS or another emulator would move the character by the D-pad bits and fall out of step).
bool send_stick(NtrCpu* c) {
    if (cfie::analog_movement == cfie::AnalogMovement::Off) return false;
    if (ntr_rd8(c, A->network + 2) == 0) return true;
    if (cfie::analog_movement != cfie::AnalogMovement::Multiplayer) return false;
    const uint32_t lr = c->r[14], r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3];
    bool all = true;
    for (uint32_t n = 0; n < 4 && all; n++) {
        c->r[0] = A->players;
        c->r[1] = n;
        ntr_call(c, A->player_present | 1);
        all = c->r[0] == 0 || slot_sticks[n].marked;
    }
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, c->r[14] = lr;
    return all;
}
}  // namespace

extern "C" void cfie_os_irq_handler(NtrCpu* c) { ntr::nitro::irq_handler(c, A->os_irq_table, A->os_irq_handler_thread_switch); }

namespace cfie {
void queue_sound(uint16_t id) {
    const uint32_t head = sound_head.load(std::memory_order_relaxed);
    if (head - sound_tail.load(std::memory_order_acquire) >= kSoundQueue) return;  // full: this one is dropped
    sound_queue[head % kSoundQueue].id.store(id, std::memory_order_relaxed);
    sound_queue[head % kSoundQueue].at.store(now_ms(), std::memory_order_relaxed);
    sound_head.store(head + 1, std::memory_order_release);
}
}  // namespace cfie

// 0x02076A94 (sound manager): its update, once per pass of the main loop. Starts the queued menu sounds.
extern "C" void cfie_sound_update(NtrCpu* c) {
    uint32_t tail = sound_tail.load(std::memory_order_relaxed);
    if (tail == sound_head.load(std::memory_order_acquire)) return;
    const uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], lr = c->r[14];
    const int64_t now = now_ms();
    while (tail != sound_head.load(std::memory_order_acquire)) {
        const SoundRequest& q = sound_queue[tail % kSoundQueue];
        const uint16_t id = q.id.load(std::memory_order_relaxed);
        const bool fresh = now - q.at.load(std::memory_order_relaxed) <= kSoundMaxAgeMs;
        sound_tail.store(++tail, std::memory_order_release);
        if (!fresh || id == 0) continue;
        c->r[0] = 0;
        c->r[1] = id;
        ntr_call(c, A->start_sound | 1);
    }
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, c->r[14] = lr;
}

namespace cfie {
void request_spell(int element) { spell_request.store(element, std::memory_order_relaxed); }
int spell_element() { return spell_now.load(std::memory_order_relaxed); }
uint64_t spell_ticks() { return spell_tick_count.load(std::memory_order_relaxed); }
}  // namespace cfie

// 0x020C6C3C (the field HUD's pad updater, once per tick: r0 = its state): applies a requested spell element through the
// game's action dispatcher, the call its R + D-pad handler (0x020C6CCC) makes.
extern "C" void cfie_spell_pad(NtrCpu* c) {
    cfie::npc_offline = ntr_rd8(c, A->network + 2) == 0;
    const int current = int(int8_t(ntr_rd8(c, A->spell_element)));
    spell_now.store(current, std::memory_order_relaxed);
    spell_tick_count.fetch_add(1, std::memory_order_relaxed);
    const int want = spell_request.exchange(-1, std::memory_order_relaxed);
    if (want < 0 || want > 5 || want == current) return;
    const uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], lr = c->r[14];
    c->r[0] = A->spell_actions;
    c->r[1] = uint32_t(want + 1);
    ntr_call(c, A->trigger_action | 1);
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, c->r[14] = lr;
}

namespace cfie {
MovieState movie() {
    const uint64_t v = movie_now.load(std::memory_order_relaxed);
    return {(v >> 63) != 0, int((v >> 32) & 0x7FFF), int(uint32_t(v))};
}
}  // namespace cfie

// 0x020ACE48 (the movie object's open: r0 = the object, r1 = the movie's number).
extern "C" void cfie_on_movie_open(NtrCpu* c) {
    if (cfie::force_movie > 0) c->r[1] = uint32_t(cfie::force_movie);
}

// 0x020ACED4 (the movie object's frame count): the movie's update calls it right after it has decoded a frame and counted
// it (+0x50 is the number of frames decoded, +0x58 the movie's number: its file is movie/movie%03d.mods).
extern "C" void cfie_on_movie_frame(NtrCpu* c) {
    const uint32_t decoded = ntr_rd32(c, c->r[0] + 0x50), id = ntr_rd32(c, c->r[0] + 0x58);
    if (decoded == 0) return;
    if (cfie::log_movies) std::fprintf(stderr, "movie %u: frame %u\n", id, decoded - 1);
    movie_now.store((uint64_t(1) << 63) | (uint64_t(id & 0x7FFF) << 32) | (decoded - 1), std::memory_order_relaxed);
}

// 0x020ACEB4 (the movie object's close): the movie is over, or the object is about to open another.
extern "C" void cfie_on_movie_close(NtrCpu*) {
    if (cfie::log_movies && (movie_now.load(std::memory_order_relaxed) >> 63)) std::fprintf(stderr, "movie: closed\n");
    movie_now.store(0, std::memory_order_relaxed);
}

// 0x02076FD0: the sound manager starts a sound effect (r1 = the manager, r2 = its id).
extern "C" void cfie_on_sound_start(NtrCpu* c) {
    if (cfie::log_sounds) std::fprintf(stderr, "sound: start %u\n", c->r[2]);
}

// 0x0207713C: start background music (r1 = track id, 0 = stop).
extern "C" void cfie_on_bgm_play(NtrCpu* c) {
    if (c->r[1] != 0) cfie::events.title_bgm_started = true;
}

// 0x02082A40: pack a tick's input record (out, buttons, touch: x, y, touching): buttons (bit 13: touching), then the
// touch x and y. While the screen isn't touched no console reads x and y (0x02077CB4 keeps the last touched point),
// and a DS never sends y over 191 (TP_GetCalibratedPoint clamps it), so this build sends its stick there: x = its
// angle, y = 192 + how far it is pushed. A DS still sees the D-pad bits; y = 192 alone marks a console that reads them.
extern "C" int cfie_pack_input(NtrCpu* c) {
    const uint32_t out = c->r[0], touch = c->r[2];
    const bool touching = ntr_rd8(c, touch + 2) != 0;
    const uint32_t buttons = ntr_rd16(c, c->r[1]) | (touching ? 0x2000 : 0);
    uint8_t x = ntr_rd8(c, touch), y = ntr_rd8(c, touch + 1);
    if (!touching) {
        x = 0;
        y = 192;
        const cfie::Stick s = cfie::stick;
        if (const float length = std::hypot(s.x, s.y); length > 0 && send_stick(c)) {
            const long push = std::lround(std::min(length, 1.0f) * kFullPush);
            x = uint8_t(std::lround(std::atan2(s.x, s.y) * (128 / 3.14159265358979)) & 0xFF);
            y = uint8_t(192 + push);
        }
    }
    ntr_wr8(c, out, uint8_t(buttons));
    ntr_wr8(c, out + 1, uint8_t(buttons >> 8));
    ntr_wr8(c, out + 2, x);
    ntr_wr8(c, out + 3, y);
    return 1;
}

// 0x020AF120: apply player slot r1's input record (r0) for this tick, on every console for every slot. Keeps the
// stick it carries; while the player touches the screen, the last one.
extern "C" void cfie_apply_input(NtrCpu* c) {
    const uint32_t slot = c->r[1], record = c->r[0];
    if (slot >= 4) return;
    SlotStick& s = slot_sticks[slot];
    if (c->r[14] == A->blank_input_call) {
        s = {};
        return;
    }
    if (ntr_rd8(c, record + 1) & 0x20) return;
    const uint8_t y = ntr_rd8(c, record + 3);
    s.marked = y >= 192;
    s.angle = s.marked ? ntr_rd8(c, record + 2) : 0;
    s.push = s.marked ? uint8_t(std::min(y - 192, int(kFullPush))) : 0;
}

namespace {
// Writes the movement vector (out) of player slot `player`'s character for a stick at `angle` (256 steps, 0 = down) and
// `speed`: the direction turned by the camera's yaw, times the speed.
void write_stick_vector(NtrCpu* c, uint32_t out, int32_t player, uint8_t angle, int64_t speed) {
    // The stick's direction (x = sin, z = cos of its angle), z towards the bottom of the screen (the D-pad's Down).
    const uint32_t a = uint32_t(angle) << 4;
    const int64_t x = int16_t(ntr_rd16(c, A->sin_cos_table + a * 4)), z = int16_t(ntr_rd16(c, A->sin_cos_table + a * 4 + 2));

    const uint32_t lr = c->r[14];
    c->r[0] = ntr_rd32(c, A->players + 572 * uint32_t(player) + 328);
    ntr_call(c, A->move_yaw | 1);
    c->r[14] = lr;
    const uint32_t i = (c->r[0] & 0xFFFF) >> 4;
    const int64_t sin = int16_t(ntr_rd16(c, A->sin_cos_table + i * 4)), cos = int16_t(ntr_rd16(c, A->sin_cos_table + i * 4 + 2));
    // As MTX_RotY33 and MTX_MultVec33, then FX_Mul by the speed.
    const int64_t vx = (x * cos + z * sin) >> 12, vz = (z * cos - x * sin) >> 12;
    ntr_wr32(c, out, uint32_t((vx * speed + 0x800) >> 12));
    ntr_wr32(c, out + 4, 0);
    ntr_wr32(c, out + 8, uint32_t((vz * speed + 0x800) >> 12));
}
// A party member's top speed under a controller (the player's run).
constexpr int64_t kNpcSpeed = 8960;
}  // namespace

// 0x02056498: a character's movement (velocity out, buttons, speed, player slot): the D-pad's direction (one of
// eight, length 1), turned by the camera's yaw, times the speed. Before it, the game speeds the character up over 8
// ticks while the D-pad is held. With a stick in the player's records the character goes the stick's way instead, at
// the speed times how far it is pushed. Fixed point only, so every console moves it the same.
extern "C" int cfie_analog_move(NtrCpu* c) {
    const int32_t player = int8_t(c->r[3]);
    if (player >= 1 && player <= 3 && cfie::npc_input[player].active &&
        ntr_rd8(c, A->network + 2) == 0 && (c->r[1] & kDsDirections)) {
        const uint32_t character = ntr_rd32(c, A->players + 572 * uint32_t(player) + 328);
        const uint32_t controller = character ? ntr_rd32(c, character + 320) : 0;
        if (controller && ntr_rd32(c, controller + 8) == kCastState) {
            const auto& in = cfie::npc_input[player];
            float x = in.x, y = in.y;
            float length = std::min(std::hypot(x, y), 1.0f);
            if (length == 0) {
                x = float(bool(c->r[1] & 0x10)) - float(bool(c->r[1] & 0x20));
                y = float(bool(c->r[1] & 0x80)) - float(bool(c->r[1] & 0x40));
                length = 1;
            }
            const uint8_t angle = uint8_t(std::lround(std::atan2(x, y) * (128 / 3.14159265358979)) & 0xFF);
            write_stick_vector(c, c->r[0], 0, angle, int64_t(int32_t(c->r[2])) * std::lround(length * kFullPush) / kFullPush);
            c->r[0] = 1;
            return 1;
        }
    }
    const SlotStick* s = moving_stick(player);
    if (!s || !(c->r[1] & 0xF0)) return 0;
    write_stick_vector(c, c->r[0], player, s->angle, int64_t(int32_t(c->r[2])) * s->push / kFullPush);
    c->r[0] = 1;
    return 1;
}

// 0x02093DE8 converts a slot's DS buttons to held, pressed and released action bits.
extern "C" void cfie_npc_buttons(NtrCpu* c) {
    const uint32_t base = A->input_slots + 12;
    if (c->r[0] < base) return;
    const uint32_t offset = c->r[0] - base;
    if (offset % 32 != 0) return;
    const uint32_t slot = offset / 32;
    if (slot < 1 || slot > 3 || !cfie::npc_input[slot].active || ntr_rd8(c, A->network + 2) != 0) return;
    const auto& in = cfie::npc_input[slot];
    c->r[2] = in.buttons & kNpcActionButtons;
    const uint32_t character = ntr_rd32(c, A->players + 572 * slot + 328);
    const uint32_t controller = character ? ntr_rd32(c, character + 320) : 0;
    if (controller && ntr_rd32(c, controller + 8) == kCastState && !(in.buttons & kDsR)) {
        c->r[2] |= in.buttons & kDsDirections;
        const float diagonal = 0.38268343f * std::hypot(in.x, in.y);
        if (in.x < -diagonal) c->r[2] |= 0x20;
        if (in.x > diagonal) c->r[2] |= 0x10;
        if (in.y < -diagonal) c->r[2] |= 0x40;
        if (in.y > diagonal) c->r[2] |= 0x80;
    }
    cfie::npc_buttons_read[slot] = true;
}

// 0x02073670 runs the companion's AI decisions before the character state update.
extern "C" int cfie_npc_ai(NtrCpu* c) {
    const uint32_t character = ntr_rd32(c, c->r[0] + 28);
    const int32_t slot = int8_t(ntr_rd8(c, character + 172));
    if (slot < 1 || slot > 3 || ntr_rd32(c, A->players + 572 * uint32_t(slot) + 328) != character) return 0;
    return cfie::npc_input[slot].active && ntr_rd8(c, A->network + 2) == 0;
}

// 0x0206C8DC (whether this is a multiplayer game), called by the field's exit check (0x0206B5CC) at 0x0206B60D with
// r4 = the player slot it looks at. In a one-player game the check only looks at the main player; a party member a
// controller has taken is let through as in a multiplayer game, so it can use map exits and entrances too.
extern "C" int cfie_exit_check_player(NtrCpu* c) {
    constexpr uint32_t kExitCheckCall = 0x0206B60D;
    const uint32_t slot = c->r[4];
    if (c->r[14] != kExitCheckCall || !cfie::npc_exits || slot < 1 || slot > 3 || !cfie::npc_input[slot].active ||
        ntr_rd8(c, A->network + 2) != 0)
        return 0;
    c->r[0] = 1;
    return 1;
}

// 0x0206DE94 (a character's state machine runs its current state: r0 = the character's controller, r1 = the tick's input,
// whose +4 vector says where it is asked to go): a party member a controller has taken (cfie::npc_input) gets that
// controller's stick there instead of the game's AI's, turned by the camera as the player's own. The state does the rest
// (it runs, turns, animates and collides as the player does). Only offline: in a session every console must agree.
extern "C" void cfie_npc_input(NtrCpu* c) {
    const uint32_t character = ntr_rd32(c, c->r[0] + 20);
    const int32_t slot = int8_t(ntr_rd8(c, character + 172));
    if (slot < 1 || slot > 3 || ntr_rd32(c, A->players + 572 * uint32_t(slot) + 328) != character) return;
    cfie::npc_idle_ticks[slot] = 0;
    const cfie::NpcInput& in = cfie::npc_input[slot];
    if (!in.active || ntr_rd8(c, A->network + 2) != 0) return;
    // The usual X path only permits the main character. Validate as the player's cast does, then start the
    // character-specific cast state used by companion AI, without changing the player's selected spell.
    if (in.spell >= 0 && in.spell < 6 && (ntr_rd32(c, A->input_slots + 32 * uint32_t(slot) + 20) & kDsX)) {
        const uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], lr = c->r[14];
        c->r[0] = character;
        ntr_call(c, 0x02057B21);
        c->r[0] += 184;
        c->r[1] = uint32_t(in.spell);
        ntr_call(c, 0x02090D69);
        const bool available = c->r[0] != 0;
        c->r[0] = uint32_t(in.spell);
        ntr_call(c, 0x02061BC9);
        const uint32_t spell_id = ntr_rd16(c, c->r[0]);
        c->r[0] = spell_id;
        ntr_call(c, 0x0209113D);
        const uint32_t magic = c->r[0];
        bool valid = false;
        if (magic) {
            c->r[0] = character;
            c->r[1] = magic;
            ntr_call(c, 0x0205697D);
            valid = c->r[0] != 0;
        }
        c->r[0] = character;
        ntr_call(c, 0x020578C9);
        const bool blocked = c->r[0] != 0;
        c->r[0] = character;
        ntr_call(c, 0x02059B2D);
        const bool can_cast = c->r[0] != 0;
        c->r[0] = A->field_state;
        ntr_call(c, 0x0206C81D);
        if (available && valid && !blocked && can_cast && c->r[0] != 0 &&
            !(ntr_rd32(c, character + 132) & 8)) {
            c->r[0] = character;
            c->r[1] = magic;
            c->r[2] = uint32_t(in.spell);
            c->r[3] = 0;
            c->r[13] -= 8;
            ntr_wr32(c, c->r[13], 0);
            ntr_call(c, 0x02058C95);
            c->r[13] += 8;
        }
        c->r[0] = r0; c->r[1] = r1; c->r[2] = r2; c->r[3] = r3; c->r[14] = lr;
    }
    // A player's attack stands still (states 6146, 24 and 536 are its start, swing and recovery), so a movement vector
    // during them would cancel it back into the run before it connects.
    const uint32_t state = ntr_rd32(c, c->r[0] + 8);
    const bool attacking = (in.buttons & kDsA) || state == 6146 || state == 24 || state == 536;
    const float length = (in.buttons & kDsR) || state == kCastState || attacking
        ? 0.0f : std::min(std::hypot(in.x, in.y), 1.0f);
    uint8_t angle = 0;
    int64_t speed = 0;
    if (length > 0) {
        angle = uint8_t(std::lround(std::atan2(in.x, in.y) * (128 / 3.14159265358979)) & 0xFF);
        speed = int64_t(std::lround(length * kFullPush)) * kNpcSpeed / kFullPush;
    }
    const uint32_t lr = c->r[14], r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3];
    // Mode 1 faces the movement vector; the AI's mode 2 faces its target instead.
    ntr_wr8(c, r1, 1);
    // All controlled party members share the main player's view, not their AI camera.
    write_stick_vector(c, r1 + 4, 0, angle, speed);
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, c->r[14] = lr;
}

// 0x0206F1BC: how far a player character's animation (r0, +8 +20: the character) goes in a tick (4.12). The run has
// one speed on the DS, so with the stick pushed part way its feet would slide: it plays slower, as much as the
// character moves slower.
// The perspective setup (G3 perspective: r0 = sin and r1 = cos of the half field of view, r2 = the aspect ratio):
// widescreen is the aspect ratio. Its only callers pass the DS's 4:3.
// The geometry engine (third_party/CMakeLists.txt) widens a viewport only while this is counting down (per VBlank), so
// scenes that do not use this projection (the title) are not stretched.
// A scene of orthographic geometry only (the title) stays 4:3 (ffcc_wide_block counts frames).
extern "C" int ffcc_wide_scene;
extern "C" int ffcc_wide_block;
namespace {
// The field's projection, as last set up (cfie_perspective_aspect): tan of the half field of view, and the aspect ratio.
float view_tan_half_fov = 0.5f, view_aspect = 4.0f / 3.0f;
}  // namespace
extern "C" void cfie_perspective_aspect(NtrCpu* c) {
    if (c->r[2] == 0x1555 && cfie::aspect != 0x1555 && ffcc_wide_block == 0) {
        c->r[2] = cfie::aspect;
        ffcc_wide_scene = 8;
    }
    if (c->r[1] != 0 && int32_t(c->r[1]) > 0 && c->r[2] != 0) {
        view_tan_half_fov = float(int32_t(c->r[0])) / float(int32_t(c->r[1]));
        view_aspect = float(c->r[2]) / 4096.0f;
    }
}

namespace {
// With several players the camera's distance and target follow a framing of them (cfie::frame_players); the camera
// glides there instead of jumping.
struct CameraGlide {
    bool active = false;
    float distance = 0, shift_x = 0, shift_z = 0;
};
CameraGlide glide;

// Positions (x, z) of the players the camera must keep in view, player 1 first: the main player and the party
// members controllers have taken, or everyone in the game when it is networked.
int camera_players(NtrCpu* c, float (*out)[3]) {
    const uint32_t lr = c->r[14], r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3];
    const bool network = ntr_rd8(c, A->network + 2) != 0;
    int count = 0;
    for (uint32_t slot = 0; slot < 4; slot++) {
        const uint32_t character = ntr_rd32(c, A->players + 572 * slot + 328);
        if (!character) continue;
        bool wanted = slot == 0 || cfie::npc_input[slot].active;
        if (network && slot != 0) {
            c->r[0] = A->players;
            c->r[1] = slot;
            ntr_call(c, A->player_present | 1);
            wanted = c->r[0] != 0;
        }
        if (!wanted) continue;
        out[count][0] = float(int32_t(ntr_rd32(c, character + 0x50)));
        out[count][1] = float(int32_t(ntr_rd32(c, character + 0x54)));
        out[count][2] = float(int32_t(ntr_rd32(c, character + 0x58)));
        count++;
    }
    c->r[0] = r0, c->r[1] = r1, c->r[2] = r2, c->r[3] = r3, c->r[14] = lr;
    return count;
}
}  // namespace

// Only the field camera's look-at call: other callers include the title and character creation.
extern "C" void cfie_camera_distance(NtrCpu* c) {
    if (c->r[14] != 0x0208468F) return;
    const uint32_t eye = c->r[0], target = c->r[2];
    int32_t e[3], t[3];
    for (int axis = 0; axis < 3; axis++) {
        e[axis] = int32_t(ntr_rd32(c, eye + 4 * axis));
        t[axis] = int32_t(ntr_rd32(c, target + 4 * axis));
    }
    float want_distance = float(cfie::camera_distance), want_x = 0, want_z = 0;
    float positions[4][3];
    const int players = cfie::dynamic_camera ? camera_players(c, positions) : 0;
    if (players >= 2) {
        const float ef[3] = {float(e[0]), float(e[1]), float(e[2])}, tf[3] = {float(t[0]), float(t[1]), float(t[2])};
        const cfie::CameraFraming f =
            cfie::frame_players(ef, tf, positions, players, view_tan_half_fov, view_aspect);
        want_distance = float(f.distance), want_x = f.shift_x, want_z = f.shift_z;
        if (!glide.active) glide = {true, float(cfie::camera_distance), 0, 0};
    } else if (!glide.active) {
        if (cfie::camera_distance == 100) return;
        glide.distance = want_distance;
    }
    if (glide.active) {
        constexpr float kGlide = 0.08f;
        glide.distance += (want_distance - glide.distance) * kGlide;
        glide.shift_x += (want_x - glide.shift_x) * kGlide;
        glide.shift_z += (want_z - glide.shift_z) * kGlide;
        // Back to a single player and settled: hand the camera back to the plain setting.
        if (players < 2 && std::fabs(glide.distance - want_distance) < 1 && std::fabs(glide.shift_x) < 64 &&
            std::fabs(glide.shift_z) < 64)
            glide.active = false;
    }
    const int distance = glide.active ? int(std::lround(glide.distance)) : cfie::camera_distance;
    const int32_t dx = glide.active ? int32_t(std::lround(glide.shift_x)) : 0;
    const int32_t dz = glide.active ? int32_t(std::lround(glide.shift_z)) : 0;
    e[0] += dx, t[0] += dx, e[2] += dz, t[2] += dz;
    for (int axis = 0; axis < 3; axis++)
        ntr_wr32(c, eye + 4 * axis, uint32_t(cfie::camera_coordinate(e[axis], t[axis], distance)));
    ntr_wr32(c, target, uint32_t(t[0]));
    ntr_wr32(c, target + 8, uint32_t(t[2]));
}

extern "C" int cfie_player_animation_step(NtrCpu* c) {
    const uint32_t character = ntr_rd32(c, ntr_rd32(c, c->r[0] + 8) + 20);
    if (ntr_rd32(c, character + 364) != A->run_animation) return 0;
    const int32_t player = int8_t(ntr_rd8(c, character + 172));
    const SlotStick* s = moving_stick(player);
    if (!s || s->push >= kFullPush || character != ntr_rd32(c, A->players + 572 * uint32_t(player) + 328)) return 0;
    const uint32_t lr = c->r[14];
    c->r[0] = character;
    ntr_call(c, A->animation_step | 1);
    c->r[14] = lr;
    c->r[0] = uint32_t(int64_t(int32_t(c->r[0])) * s->push / kFullPush);
    return 1;
}

// 0x02025260: CARDi_RequestStreamCommand. The game's save thread reads and writes the save 1 KB at a time, and the
// card library hands each 256-byte page to the ARM7: a save of 25 KB took 3.2 s. Fast saving does it at once.
extern "C" int cfie_card_backup_request(NtrCpu* c) {
    return cfie::fast_saving && ntr::nitro::card_backup_request(c, A->card_common);
}

// 0x02005BBC: the Wi-Fi Connection test (DWC netcheck). It converts the HTTP status with atoi and fails if errno is
// ERANGE, but errno is never cleared: the stats server's session key (\sesskey\, read with atoi at 0x02040D02)
// overflows on today's revival servers, and every connection after the first then fails with error 52200.
extern "C" void cfie_on_netcheck(NtrCpu* c) { ntr_wr32(c, A->errno_, 0); }

// 0x020ADDBC: how many of the guest's queued inputs (one per game frame, 30 a second) to send to the host now: at most
// 3, and no more than the host allows. Online (Wi-Fi Connection) the game sends nothing until it has 3, so an input
// waits up to 100 ms before it leaves, and the guest sees the result only with the host's reply to it. Over local
// wireless it sends whatever it has every frame; this does the same online (unless the game's own timing is chosen).
extern "C" void cfie_guest_inputs_to_send(NtrCpu* c) {
    int32_t n = std::min(int32_t(ntr_rd32(c, c->r[0] + 108)), 3);
    n = std::min(n, int32_t(ntr_rd32(c, A->guest_send_allowance)) + 1);
    if (cfie::online_buffer == cfie::OnlineBuffer::Original && ntr_rd8(c, A->network + 2) == 2 && n != 3) {
        const uint32_t lr = c->r[14];
        c->r[0] = A->network;
        ntr_call(c, A->network_is_online | 1);
        c->r[14] = lr;
        if (c->r[0] != 0) n = 0;
    }
    c->r[0] = uint32_t(n);
}

// 0x020AE314: fills the timing table for online play (r1: one of two presets, 0 or 1). Over local wireless the host
// holds each guest's inputs 1-2 game frames and runs one frame at a time. Online it holds them (ping / 30 ms) + 5
// frames, at least 5 (3 in preset 1), trimmed only 2 past that, and runs and replies 3 frames at a time, so a guest sees
// its own moves some 200 ms late. Low: ping + 1 frame, trimmed 1 past it, one frame at a time. The buffer still grows
// with the ping, as on a slow connection it must; between two instances on a PC it holds 0-1 inputs. Medium: twice the
// margins, for connections whose packets come late. Original: the game's own table.
extern "C" void cfie_set_online_timing(NtrCpu* c) {
    const uint32_t preset = c->r[1];
    const bool second = preset == 1;
    const int32_t game = second ? 3 : 5;
    const int32_t margin = cfie::online_buffer == cfie::OnlineBuffer::Low ? 1 : 2;
    const bool original = cfie::online_buffer == cfie::OnlineBuffer::Original;
    const int32_t fields[] = {
        original ? 2 : margin,     // +84: the host trims a guest's queued inputs once they are this many over the target
        original ? -2 : -margin,   // +88: down to this many under it
        second ? 2 : 0,            // +92, +96: when to tell guests to wait (unchanged)
        second ? 0 : -2,           //
        original ? 3 : 1,          // +100: frames run (and replied) at a time
        original ? game : margin,  // +104: the smallest target
        20,                        // +108: the largest target
        original ? game : margin,  // +112: the target over the ping
    };
    for (int i = 0; i < 8; i++) ntr_wr32(c, A->online_timing + 84 + 4 * i, uint32_t(fields[i]));
    ntr_wr32(c, A->online_timing + 116, preset);
}

bool cfie::select_release(const std::string& game_code) {
    if (game_code == "CFIE") A = &kUsa;
    else if (game_code == "CFIP") A = &kEur;
    else return false;
    return true;
}

void* cfie::find_hook(const char* name) {
    static const struct {
        const char* name;
        void* fn;
    } kHooks[] = {
        {"cfie_os_irq_handler", reinterpret_cast<void*>(&cfie_os_irq_handler)},
        {"cfie_on_bgm_play", reinterpret_cast<void*>(&cfie_on_bgm_play)},
        {"cfie_sound_update", reinterpret_cast<void*>(&cfie_sound_update)},
        {"cfie_spell_pad", reinterpret_cast<void*>(&cfie_spell_pad)},
        {"cfie_on_sound_start", reinterpret_cast<void*>(&cfie_on_sound_start)},
        {"cfie_pack_input", reinterpret_cast<void*>(&cfie_pack_input)},
        {"cfie_apply_input", reinterpret_cast<void*>(&cfie_apply_input)},
        {"cfie_analog_move", reinterpret_cast<void*>(&cfie_analog_move)},
        {"cfie_npc_buttons", reinterpret_cast<void*>(&cfie_npc_buttons)},
        {"cfie_npc_ai", reinterpret_cast<void*>(&cfie_npc_ai)},
        {"cfie_npc_input", reinterpret_cast<void*>(&cfie_npc_input)},
        {"cfie_exit_check_player", reinterpret_cast<void*>(&cfie_exit_check_player)},
        {"cfie_perspective_aspect", reinterpret_cast<void*>(&cfie_perspective_aspect)},
        {"cfie_camera_distance", reinterpret_cast<void*>(&cfie_camera_distance)},
        {"cfie_player_animation_step", reinterpret_cast<void*>(&cfie_player_animation_step)},
        {"cfie_card_backup_request", reinterpret_cast<void*>(&cfie_card_backup_request)},
        {"cfie_on_movie_open", reinterpret_cast<void*>(&cfie_on_movie_open)},
        {"cfie_on_movie_frame", reinterpret_cast<void*>(&cfie_on_movie_frame)},
        {"cfie_on_movie_close", reinterpret_cast<void*>(&cfie_on_movie_close)},
        {"cfie_on_netcheck", reinterpret_cast<void*>(&cfie_on_netcheck)},
        {"cfie_guest_inputs_to_send", reinterpret_cast<void*>(&cfie_guest_inputs_to_send)},
        {"cfie_set_online_timing", reinterpret_cast<void*>(&cfie_set_online_timing)},
        {"ntr_nitro_save_context", reinterpret_cast<void*>(&ntr_nitro_save_context)},
        {"ntr_nitro_load_context", reinterpret_cast<void*>(&ntr_nitro_load_context)},
        {"ntr_nitro_init_context", reinterpret_cast<void*>(&ntr_nitro_init_context)},
        {"ntr_dsprot_not_detected", reinterpret_cast<void*>(&ntr_dsprot_not_detected)},
    };
    for (const auto& h : kHooks)
        if (std::strcmp(h.name, name) == 0) return h.fn;
    return nullptr;
}
