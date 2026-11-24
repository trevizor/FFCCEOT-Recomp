#include <array>
#include <cstdio>
#include <vector>

#include "cfie_game.hpp"
#include "ntr/recomp.h"

extern "C" void cfie_npc_input(NtrCpu* c);
extern "C" void cfie_npc_buttons(NtrCpu* c);
extern "C" int cfie_npc_ai(NtrCpu* c);
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

uint32_t yaw_character = 0;
void movement_yaw(NtrCpu* c, uint32_t target) {
    CHECK(target == 0x02057221);
    yaw_character = c->r[0];
    c->r[0] = 0x4000;
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
    cfie::npc_input[1].buttons = 11;
    cpu.r[0] = 0x021218AC + 32 + 12;
    cpu.r[2] = 0;
    cfie_npc_buttons(&cpu);
    CHECK(cpu.r[2] == 11);
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
}
}  // namespace

int main() {
    test_npc_input();
    return failures ? 1 : 0;
}
