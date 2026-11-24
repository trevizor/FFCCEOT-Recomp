#include <array>
#include <cstdio>
#include <vector>

#include "cfie_game.hpp"
#include "ntr/recomp.h"

extern "C" void cfie_npc_input(NtrCpu* c);
extern "C" void cfie_npc_buttons(NtrCpu* c);
extern "C" int cfie_npc_ai(NtrCpu* c);
extern "C" int cfie_exit_check_player(NtrCpu* c);
extern "C" int cfie_analog_move(NtrCpu* c);
NtrHostApi ntr_host{};

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

constexpr uint32_t kBase = 0x02000000;
constexpr uint32_t kPlayers = 0x02122E98;
constexpr uint32_t kNetwork = 0x0212F700;
constexpr uint32_t kSines = 0x020CD780;
constexpr uint32_t kLeader = 0x023078CC;
constexpr uint32_t kNpc = 0x02307D18;
constexpr uint32_t kController = 0x02307EAC;
constexpr uint32_t kInput = 0x02308000;
constexpr uint32_t kSpellTable = 0x02308100;
constexpr uint32_t kMagic = 0x02308200;

uint32_t yaw_character = 0;
int cast_calls = 0;
bool spell_available = true;
uint32_t expected_spell = 0;
uint32_t expected_field = 0x0211677C;
void movement_yaw(NtrCpu* c, uint32_t target) {
    CHECK(target == 0x02057221);
    yaw_character = c->r[0];
    c->r[0] = 0x4000;
}

void cast_game_call(NtrCpu* c, uint32_t target) {
    switch (target) {
    case 0x02057221:
        movement_yaw(c, target);
        break;
    case 0x02057B21:
        CHECK(c->r[0] == kNpc);
        c->r[0] = 0x02308300;
        break;
    case 0x02090D69:
        CHECK(c->r[0] == 0x02308300 + 184 && c->r[1] == expected_spell);
        c->r[0] = spell_available;
        break;
    case 0x02061BC9:
        CHECK(c->r[0] == expected_spell);
        c->r[0] = kSpellTable;
        break;
    case 0x0209113D:
        CHECK(c->r[0] == 123);
        c->r[0] = kMagic;
        break;
    case 0x0205697D:
        CHECK(c->r[0] == kNpc && c->r[1] == kMagic);
        c->r[0] = 1;
        break;
    case 0x020578C9:
        c->r[0] = 0;
        break;
    case 0x02059B2D:
        c->r[0] = 1;
        break;
    case 0x0206C81D:
        CHECK(c->r[0] == expected_field);
        c->r[0] = 1;
        break;
    case 0x02058C95:
        CHECK(c->r[0] == kNpc && c->r[1] == kMagic && c->r[2] == expected_spell && c->r[3] == 0);
        CHECK(ntr_rd32(c, c->r[13]) == 0);
        ++cast_calls;
        break;
    default:
        CHECK(false);
    }
}

void test_npc_input() {
    std::vector<uint32_t> memory(0x400000 / 4);
    std::vector<uint8_t*> pages(0x02400000 >> NTR_PAGE_SHIFT);
    for (uint32_t i = 0; i < 0x400000 / 4096; ++i)
        pages[(kBase >> NTR_PAGE_SHIFT) + i] = reinterpret_cast<uint8_t*>(memory.data()) + i * 4096;
    NtrCpu cpu{};
    cpu.read_pages = pages.data();
    cpu.write_pages = pages.data();
    cpu.r[0] = kController;
    cpu.r[1] = kInput;
    cpu.r[2] = 0x1234;
    cpu.r[3] = 0x5678;
    cpu.r[14] = 0x2468;
    ntr_wr32(&cpu, kController + 20, kNpc);
    ntr_wr32(&cpu, kNpc + 320, kController);
    ntr_wr8(&cpu, kNpc + 172, 1);
    ntr_wr32(&cpu, kPlayers + 328, kLeader);
    ntr_wr32(&cpu, kPlayers + 572 + 328, kNpc);
    ntr_wr16(&cpu, kSines + 2, 4096);
    ntr_wr16(&cpu, kSines + 1024 * 4, 4096);
    ntr_wr32(&cpu, kInput, 0x02060002);
    ntr_host.call = movement_yaw;
    cfie::npc_input[1] = {true, 0, 1};
    cfie_npc_input(&cpu);
    CHECK(yaw_character == kLeader);
    CHECK(ntr_rd32(&cpu, kInput) == 0x02060001);
    CHECK(int32_t(ntr_rd32(&cpu, kInput + 4)) == 8960);
    CHECK(ntr_rd32(&cpu, kInput + 8) == 0);
    CHECK(ntr_rd32(&cpu, kInput + 12) == 0);
    CHECK(cpu.r[0] == kController && cpu.r[1] == kInput && cpu.r[2] == 0x1234 &&
          cpu.r[3] == 0x5678 && cpu.r[14] == 0x2468);

    ntr_wr32(&cpu, kInput, 0x02060002);
    yaw_character = 0;
    cfie::npc_input[1] = {};
    cfie_npc_input(&cpu);
    CHECK(ntr_rd32(&cpu, kInput) == 0x02060002 && yaw_character == 0);
    cfie::npc_input[1] = {true, 0, 1};
    ntr_wr8(&cpu, kNetwork + 2, 2);
    cfie_npc_input(&cpu);
    CHECK(ntr_rd32(&cpu, kInput) == 0x02060002 && yaw_character == 0);

    ntr_wr8(&cpu, kNetwork + 2, 0);
    cfie::npc_input[1].buttons = 0xD33;
    cpu.r[0] = 0x021218AC + 32 + 12;
    cpu.r[2] = 0;
    cfie_npc_buttons(&cpu);
    CHECK(cpu.r[2] == 0xC03);
    CHECK(cfie::npc_buttons_read[1]);
    cpu.r[0] = 0x021218AC + 12;
    cpu.r[2] = 7;
    cfie_npc_buttons(&cpu);
    CHECK(cpu.r[2] == 7);
    cpu.r[0] += 32;
    ntr_wr8(&cpu, kNetwork + 2, 2);
    cfie_npc_buttons(&cpu);
    CHECK(cpu.r[2] == 7);
    cpu.r[0] = kController;
    ntr_wr32(&cpu, kController + 28, kNpc);
    CHECK(cfie_npc_ai(&cpu) == 0);
    ntr_wr8(&cpu, kNetwork + 2, 0);
    CHECK(cfie_npc_ai(&cpu) == 1);
    ntr_wr32(&cpu, kController + 28, kLeader);
    ntr_wr8(&cpu, kLeader + 172, 0);
    CHECK(cfie_npc_ai(&cpu) == 0);
    ntr_wr32(&cpu, kController + 28, kNpc);
    cfie::npc_input[1] = {};
    CHECK(cfie_npc_ai(&cpu) == 0);

    cpu.r[0] = kController;
    cpu.r[1] = kInput;
    cpu.r[2] = 0x1234;
    cpu.r[3] = 0x5678;
    cpu.r[13] = 0x02309000;
    cpu.r[14] = 0x2468;
    ntr_wr16(&cpu, kSpellTable, 123);
    ntr_wr32(&cpu, 0x021218AC + 32 + 20, 0x400);
    ntr_host.call = cast_game_call;
    expected_spell = 2;
    cfie::npc_input[1] = {true, 0, 0, 0x400, 2};
    cast_calls = 0;
    spell_available = true;
    cfie_npc_input(&cpu);
    CHECK(cast_calls == 1);
    CHECK(cpu.r[0] == kController && cpu.r[1] == kInput && cpu.r[2] == 0x1234 &&
          cpu.r[3] == 0x5678 && cpu.r[13] == 0x02309000 && cpu.r[14] == 0x2468);
    spell_available = false;
    cfie_npc_input(&cpu);
    CHECK(cast_calls == 1);
    spell_available = true;
    cfie::npc_input[1].spell = 6;
    cfie_npc_input(&cpu);
    CHECK(cast_calls == 1);
    cfie::npc_input[1].spell = 2;
    ntr_wr8(&cpu, kNetwork + 2, 2);
    cfie_npc_input(&cpu);
    CHECK(cast_calls == 1);
    ntr_wr8(&cpu, kNetwork + 2, 0);
    ntr_wr32(&cpu, 0x021218AC + 32 + 20, 0);
    cfie_npc_input(&cpu);
    CHECK(cast_calls == 1);
    cfie::npc_input[1] = {true, 1, 0, 0x100, 2};
    cfie_npc_input(&cpu);
    CHECK(ntr_rd32(&cpu, kInput + 4) == 0 && ntr_rd32(&cpu, kInput + 12) == 0);

    ntr_wr32(&cpu, kController + 8, 23);
    cfie::npc_input[1] = {true, 0, 1, 0x400, 2};
    cfie_npc_input(&cpu);
    CHECK(ntr_rd32(&cpu, kInput + 4) == 0 && ntr_rd32(&cpu, kInput + 12) == 0);
    cpu.r[0] = 0x021218AC + 32 + 12;
    cfie_npc_buttons(&cpu);
    CHECK(cpu.r[2] == 0x480);
    cpu.r[0] = kInput + 4;
    cpu.r[1] = 0x80;
    cpu.r[2] = 4096;
    cpu.r[3] = 1;
    CHECK(cfie_analog_move(&cpu) == 1);
    CHECK(int32_t(ntr_rd32(&cpu, kInput + 4)) == 4096);
    CHECK(yaw_character == kLeader);
    cpu.r[0] = 0x021218AC + 32 + 12;
    cfie::npc_input[1].buttons |= 0x100;
    cfie_npc_buttons(&cpu);
    CHECK(cpu.r[2] == 0x400);
    ntr_wr32(&cpu, kController + 8, 2);
    cpu.r[0] = kController;
    cpu.r[1] = kInput;
    cfie::npc_input[1] = {true, 0, 1, 0, 2};
    cfie_npc_input(&cpu);
    CHECK(int32_t(ntr_rd32(&cpu, kInput + 4)) == 8960);

    CHECK(cfie::select_release("CFIP"));
    expected_field = 0x0211679C;
    ntr_wr32(&cpu, kPlayers + 0x20 + 328, kLeader);
    ntr_wr32(&cpu, kPlayers + 0x20 + 572 + 328, kNpc);
    ntr_wr32(&cpu, 0x021218CC + 32 + 20, 0x400);
    cfie::npc_input[1] = {true, 0, 0, 0x400, 2};
    cfie_npc_input(&cpu);
    CHECK(cast_calls == 2);
    ntr_wr8(&cpu, kNetwork + 0x20 + 2, 1);
    cfie_npc_input(&cpu);
    CHECK(cast_calls == 2);
    CHECK(cfie::select_release("CFIE"));
    cfie::npc_input[1] = {};
}

void test_exit_check_player() {
    std::vector<uint32_t> memory(0x400000 / 4);
    std::vector<uint8_t*> pages(0x02400000 >> NTR_PAGE_SHIFT);
    for (uint32_t i = 0; i < 0x400000 / 4096; ++i)
        pages[(kBase >> NTR_PAGE_SHIFT) + i] = reinterpret_cast<uint8_t*>(memory.data()) + i * 4096;
    NtrCpu cpu{};
    cpu.read_pages = pages.data();
    cpu.write_pages = pages.data();
    cfie::npc_input[1] = {true, 0, 0};
    cfie::npc_exits = true;
    auto ask = [&](uint32_t lr, uint32_t slot) {
        cpu.r[0] = 0;
        cpu.r[4] = slot;
        cpu.r[14] = lr;
        return cfie_exit_check_player(&cpu);
    };
    CHECK(ask(0x0206B60D, 1) == 1 && cpu.r[0] == 1);  // a controlled party member counts as in a multiplayer game
    CHECK(ask(0x0206B60D, 0) == 0 && ask(0x0206B60D, 2) == 0);  // the main player and AI members: unchanged
    CHECK(ask(0x0206C8EF, 1) == 0);  // other callers of the check
    cfie::npc_exits = false;
    CHECK(ask(0x0206B60D, 1) == 0);
    cfie::npc_exits = true;
    ntr_wr8(&cpu, kNetwork + 2, 2);
    CHECK(ask(0x0206B60D, 1) == 0);
    cfie::npc_input[1] = {};
}
}  // namespace

int main() {
    test_npc_input();
    test_exit_check_player();
    return failures ? 1 : 0;
}
