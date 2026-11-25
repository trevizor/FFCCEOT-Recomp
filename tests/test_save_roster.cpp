#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>

#include "save_roster.hpp"

static int failures = 0;
#define CHECK(condition)                                                                          \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);    \
            failures++;                                                                           \
        }                                                                                         \
    } while (0)

namespace {

std::filesystem::path temp_directory() {
    return std::filesystem::temp_directory_path() / "ffcc_test_save_roster";
}

std::vector<uint8_t> make_save() {
    std::vector<uint8_t> save(save_roster::kSaveSize);
    const size_t offsets[]{0, 0x0CA8, 0x3E94};
    const uint16_t lengths[]{3240, 12780, 3216};
    const uint16_t types[]{5, 10, 1};
    for (size_t block = 0; block < 3; block++) {
        save[offsets[block] + 2] = uint8_t(lengths[block]);
        save[offsets[block] + 3] = uint8_t(lengths[block] >> 8);
        save[offsets[block] + 4] = 'X';
        save[offsets[block] + 5] = 'N';
        save[offsets[block] + 6] = 'Y';
        save[offsets[block] + 7] = 'L';
        save[offsets[block] + 8] = uint8_t(types[block]);
    }
    save_roster::repair(save);
    return save;
}

std::vector<uint8_t> character(uint16_t type, char16_t first, uint16_t xp) {
    std::vector<uint8_t> record(save_roster::kCharacterSize);
    record[0x38] = uint8_t(type);
    record[0x39] = uint8_t(type >> 8);
    record[save_roster::kXpOffset] = uint8_t(xp);
    record[save_roster::kXpOffset + 1] = uint8_t(xp >> 8);
    record[0x48] = uint8_t(first);
    record[0x49] = uint8_t(first >> 8);
    return record;
}

void put_character(std::vector<uint8_t>& save, size_t slot, const std::vector<uint8_t>& record) {
    const size_t offset = 0x0CA8 + 0x292C + slot * save_roster::kCharacterSize;
    std::copy(record.begin(), record.end(), save.begin() + offset);
}

std::vector<uint8_t> get_character(const std::vector<uint8_t>& save, size_t slot) {
    const size_t offset = 0x0CA8 + 0x292C + slot * save_roster::kCharacterSize;
    return {save.begin() + offset, save.begin() + offset + save_roster::kCharacterSize};
}

void test_prepare_and_deletion() {
    std::error_code ec;
    std::filesystem::remove_all(temp_directory(), ec);
    auto source = make_save();
    put_character(source, 0, character(0x2A01, u'A', 10));
    put_character(source, 1, character(0x2A05, u'B', 20));
    save_roster::repair(source);
    const auto dir = save_roster::character_directory(temp_directory() / "cfie.sav");
    std::vector<uint8_t> prepared;
    std::string error;
    CHECK(save_roster::prepare(source, prepared, dir, error));
    CHECK(std::filesystem::exists(dir / "slot-0001.ffcc-character"));
    CHECK(std::filesystem::remove(dir / "slot-0002.ffcc-character"));
    CHECK(save_roster::prepare(source, prepared, dir, error));
    CHECK(get_character(prepared, 1)[0x38] == 0);
    std::filesystem::remove_all(temp_directory(), ec);
}

void test_merge_new_characters_money_and_xp() {
    std::error_code ec;
    std::filesystem::remove_all(temp_directory(), ec);
    auto baseline = make_save();
    const auto one = character(0x2A01, u'A', 10);
    put_character(baseline, 0, one);
    save_roster::set_money(baseline, 1000);
    const auto dir = save_roster::character_directory(temp_directory() / "cfie.sav");
    std::vector<uint8_t> prepared;
    std::string error;
    CHECK(save_roster::prepare(baseline, prepared, dir, error));

    auto player1 = baseline;
    put_character(player1, 0, character(0x2A01, u'A', 30));
    save_roster::set_money(player1, 1100);
    auto player2 = baseline;
    put_character(player2, 0, character(0x2A01, u'A', 20));
    put_character(player2, 1, character(0x2A05, u'B', 5));
    put_character(player2, 2, character(0x2A07, u'C', 1));
    save_roster::set_money(player2, 1250);
    std::ofstream extra(dir / "slot-0003.ffcc-character", std::ios::binary);
    extra.write(reinterpret_cast<const char*>(character(0x2A09, u'D', 2).data()), save_roster::kCharacterSize);
    extra.close();

    std::vector<uint8_t> merged;
    CHECK(save_roster::merge(baseline, {player1, player2}, dir, merged, error));
    CHECK(save_roster::valid(merged, error));
    CHECK(save_roster::money(merged) == 1350);
    CHECK(get_character(merged, 0)[save_roster::kXpOffset] == 30);
    CHECK(std::filesystem::exists(dir / "slot-0002.ffcc-character"));
    CHECK(std::filesystem::exists(dir / "slot-0004.ffcc-character"));
    std::filesystem::remove_all(temp_directory(), ec);
}

void test_archive_slots_are_not_loaded() {
    std::error_code ec;
    std::filesystem::remove_all(temp_directory(), ec);
    auto baseline = make_save();
    const auto dir = save_roster::character_directory(temp_directory() / "cfie.sav");
    std::filesystem::create_directories(dir, ec);
    for (size_t slot = 1; slot <= 17; slot++) {
        auto data = character(uint16_t(0x2A00 + slot), char16_t(u'A' + slot), uint16_t(slot));
        std::ofstream file(dir / ("slot-" + std::to_string(10000 + slot).substr(1) + ".ffcc-character"),
                           std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    }
    save_roster::set_money(baseline, 0);
    std::string error;
    std::vector<uint8_t> prepared;
    CHECK(save_roster::prepare(baseline, prepared, dir, error));
    CHECK(get_character(prepared, 15)[0x38] == 0x10);
    CHECK(get_character(prepared, 15)[save_roster::kXpOffset] == 16);

    auto player = prepared;
    put_character(player, 0, character(0x2B00, u'Z', 1));
    save_roster::repair(player);
    std::vector<uint8_t> merged;
    CHECK(save_roster::merge(baseline, {player}, dir, merged, error));
    CHECK(std::filesystem::exists(dir / "slot-0018.ffcc-character"));
    CHECK(std::filesystem::exists(dir / "slot-0017.ffcc-character"));
    CHECK(get_character(merged, 15)[0x38] == 0x10);
    std::filesystem::remove_all(temp_directory(), ec);
}

void test_rotate_active_character() {
    auto save = make_save();
    put_character(save, 0, character(0x2A01, u'A', 10));
    put_character(save, 2, character(0x2A05, u'B', 20));
    put_character(save, 7, character(0x2A07, u'C', 30));
    save_roster::repair(save);
    save_roster::rotate_active_characters(save, 1);
    CHECK(get_character(save, 0)[0x48] == uint8_t(u'B'));
    CHECK(get_character(save, 2)[0x48] == uint8_t(u'C'));
    CHECK(get_character(save, 7)[0x48] == uint8_t(u'A'));
    CHECK(get_character(save, 1)[0x38] == 0);
    std::string error;
    CHECK(save_roster::valid(save, error));
}

}  // namespace

int main() {
    test_prepare_and_deletion();
    test_merge_new_characters_money_and_xp();
    test_archive_slots_are_not_loaded();
    test_rotate_active_character();
    if (failures == 0) std::puts("save roster tests passed");
    return failures == 0 ? 0 : 1;
}
