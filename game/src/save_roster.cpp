#include "save_roster.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <string_view>

namespace save_roster {
namespace {

constexpr size_t kBankSize = 32 * 1024;
constexpr std::array<size_t, 3> kBlockOffsets{0x0000, 0x0CA8, 0x3E94};
constexpr std::array<uint16_t, 3> kBlockTypes{5, 10, 1};
constexpr size_t kRosterOffset = 0x0CA8 + 0x292C;
constexpr size_t kMoneyOffset = 0x0CA8 + 0x0014;
constexpr uint32_t kMagic = 0x4C594E58;

uint16_t get16(std::span<const uint8_t> data, size_t offset) {
    return uint16_t(data[offset]) | uint16_t(data[offset + 1]) << 8;
}

uint32_t get32(std::span<const uint8_t> data, size_t offset) {
    return uint32_t(data[offset]) | uint32_t(data[offset + 1]) << 8 |
           uint32_t(data[offset + 2]) << 16 | uint32_t(data[offset + 3]) << 24;
}

void put32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; i++) data[offset + size_t(i)] = uint8_t(value >> (8 * i));
}

uint16_t checksum(std::span<const uint8_t> data) {
    uint32_t sum = 0;
    for (size_t i = 0; i < data.size(); i += 2) {
        sum += i + 1 < data.size() ? get16(data, i) : data[i];
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return uint16_t(~sum);
}

bool block_valid(std::span<const uint8_t> bank, size_t expected_offset, uint16_t expected_type) {
    if (expected_offset + 10 > bank.size()) return false;
    const size_t length = get16(bank, expected_offset + 2);
    if (length < 10 || expected_offset + length > bank.size() ||
        get32(bank, expected_offset + 4) != kMagic || get16(bank, expected_offset + 8) != expected_type)
        return false;
    return get16(bank, expected_offset) == checksum(bank.subspan(expected_offset + 4, length - 4));
}

bool character_present(std::span<const uint8_t> character) {
    return character.size() == kCharacterSize && get16(character, 0x38) != 0;
}

std::string character_key(std::span<const uint8_t> character) {
    std::string key;
    const uint16_t type = get16(character, 0x38);
    key.append(reinterpret_cast<const char*>(&type), sizeof type);
    key.append(reinterpret_cast<const char*>(character.data() + 0x48), 20);
    return key;
}

uint16_t experience(std::span<const uint8_t> character) {
    return get16(character, kXpOffset);
}

std::filesystem::path slot_path(const std::filesystem::path& directory, size_t slot) {
    std::string number = std::to_string(slot);
    number.insert(number.begin(), 4 - std::min(size_t(4), number.size()), '0');
    return directory / ("slot-" + number + ".ffcc-character");
}

std::optional<std::vector<uint8_t>> read_character(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot read " + path.string();
        return std::nullopt;
    }
    const auto length = file.tellg();
    if (length != std::streamoff(kCharacterSize)) {
        error = path.string() + " is not a 140-byte character file";
        return std::nullopt;
    }
    std::vector<uint8_t> data(kCharacterSize);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) {
        error = "cannot read " + path.string();
        return std::nullopt;
    }
    if (!character_present(data)) {
        error = path.string() + " does not contain a character";
        return std::nullopt;
    }
    return data;
}

bool write_file(const std::filesystem::path& path, std::span<const uint8_t> data) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(file);
}

std::map<size_t, std::vector<uint8_t>> load_characters(const std::filesystem::path& directory,
                                                       std::string& error) {
    std::map<size_t, std::vector<uint8_t>> characters;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        const std::string filename = entry.path().filename().string();
        constexpr std::string_view kSuffix = ".ffcc-character";
        if (!entry.is_regular_file() || filename.size() != 9 + kSuffix.size() ||
            filename.rfind("slot-", 0) != 0 || filename.substr(9) != kSuffix)
            continue;
        size_t consumed = 0;
        size_t slot = 0;
        try {
            slot = std::stoul(filename.substr(5, 4), &consumed);
        } catch (...) {
            continue;
        }
        if (consumed != 4 || slot == 0) continue;
        if (auto data = read_character(entry.path(), error)) characters.emplace(slot, std::move(*data));
        else return {};
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "cannot list " + directory.string() + ": " + ec.message();
        return {};
    }
    return characters;
}

bool save_character(const std::filesystem::path& directory, size_t slot, std::span<const uint8_t> character,
                    std::string& error) {
    const auto path = slot_path(directory, slot);
    if (!write_file(path, character)) {
        error = "cannot write " + path.string();
        return false;
    }
    return true;
}

void write_roster(std::vector<uint8_t>& save, const std::map<size_t, std::vector<uint8_t>>& characters) {
    std::fill(save.begin() + kRosterOffset,
              save.begin() + kRosterOffset + kActiveCharacters * kCharacterSize, 0);
    for (size_t slot = 1; slot <= kActiveCharacters; slot++) {
        const auto it = characters.find(slot);
        if (it != characters.end())
            std::copy(it->second.begin(), it->second.end(),
                      save.begin() + kRosterOffset + (slot - 1) * kCharacterSize);
    }
    repair(save);
}

}  // namespace

std::filesystem::path character_directory(const std::filesystem::path& save_path) {
    return save_path.parent_path() / (save_path.stem().string() + "-characters");
}

bool valid(std::span<const uint8_t> save, std::string& error) {
    if (save.size() != kSaveSize) {
        error = "save is not 64 KiB";
        return false;
    }
    for (size_t bank = 0; bank < 2; bank++) {
        const auto bytes = save.subspan(bank * kBankSize, kBankSize);
        for (size_t i = 0; i < kBlockOffsets.size(); i++) {
            if (!block_valid(bytes, kBlockOffsets[i], kBlockTypes[i])) {
                error = "save block checksum or header is invalid";
                return false;
            }
        }
    }
    return true;
}

void repair(std::vector<uint8_t>& save) {
    if (save.size() != kSaveSize) return;
    for (size_t i = 0; i < kBlockOffsets.size(); i++) {
        const size_t length = get16(save, kBlockOffsets[i] + 2);
        const uint16_t value = checksum(std::span<const uint8_t>(save).subspan(
            kBlockOffsets[i] + 4, length - 4));
        save[kBlockOffsets[i]] = uint8_t(value);
        save[kBlockOffsets[i] + 1] = uint8_t(value >> 8);
    }
    std::copy(save.begin(), save.begin() + kBankSize, save.begin() + kBankSize);
}

uint32_t money(std::span<const uint8_t> save) {
    return save.size() == kSaveSize ? get32(save, kMoneyOffset) : 0;
}

void set_money(std::vector<uint8_t>& save, uint32_t value) {
    if (save.size() != kSaveSize) return;
    put32(save, kMoneyOffset, value);
    repair(save);
}

void rotate_active_characters(std::vector<uint8_t>& save, size_t offset) {
    if (save.size() != kSaveSize || offset == 0) return;
    std::vector<std::pair<size_t, std::array<uint8_t, kCharacterSize>>> records;
    for (size_t slot = 0; slot < kActiveCharacters; slot++) {
        const size_t at = kRosterOffset + slot * kCharacterSize;
        if (!character_present(std::span<const uint8_t>(save).subspan(at, kCharacterSize))) continue;
        std::array<uint8_t, kCharacterSize> record{};
        std::copy(save.begin() + at, save.begin() + at + kCharacterSize, record.begin());
        records.emplace_back(at, record);
    }
    if (records.size() < 2) return;
    offset %= records.size();
    for (size_t i = 0; i < records.size(); i++) {
        const auto& from = records[(i + offset) % records.size()].second;
        std::copy(from.begin(), from.end(), save.begin() + records[i].first);
    }
    repair(save);
}

bool prepare(std::span<const uint8_t> source, std::vector<uint8_t>& save,
             const std::filesystem::path& character_dir, std::string& error) {
    std::error_code ec;
    const bool first_use = !std::filesystem::exists(character_dir);
    std::filesystem::create_directories(character_dir, ec);
    if (ec) {
        error = "cannot create " + character_dir.string() + ": " + ec.message();
        return false;
    }
    if (source.empty()) {
        save.clear();
        return true;
    }
    if (!valid(source, error)) return false;
    save.assign(source.begin(), source.end());

    std::map<size_t, std::vector<uint8_t>> characters;
    if (first_use) {
        for (size_t slot = 1; slot <= kActiveCharacters; slot++) {
            const size_t offset = kRosterOffset + (slot - 1) * kCharacterSize;
            const auto record = source.subspan(offset, kCharacterSize);
            if (!character_present(record)) continue;
            auto& destination = characters[slot];
            destination.assign(record.begin(), record.end());
            if (!save_character(character_dir, slot, destination, error)) return false;
        }
    } else {
        characters = load_characters(character_dir, error);
        if (!error.empty()) return false;
    }
    write_roster(save, characters);
    return true;
}

bool merge(std::span<const uint8_t> baseline, const std::vector<std::vector<uint8_t>>& session_saves,
           const std::filesystem::path& character_dir, std::vector<uint8_t>& save, std::string& error) {
    if (!baseline.empty() && !valid(baseline, error)) return false;
    const auto first_save = std::find_if(session_saves.begin(), session_saves.end(),
                                         [](const auto& item) { return !item.empty(); });
    for (const auto& session : session_saves) {
        if (session.empty()) continue;
        if (!valid(session, error)) return false;
    }

    auto characters = load_characters(character_dir, error);
    if (!error.empty()) return false;
    std::map<std::string, size_t> by_identity;
    for (const auto& [slot, character] : characters) by_identity[character_key(character)] = slot;

    auto first_free = [&] {
        size_t slot = 1;
        while (characters.find(slot) != characters.end()) slot++;
        return slot;
    };
    auto add_character = [&](std::span<const uint8_t> candidate) -> bool {
        if (!character_present(candidate)) return true;
        const std::string key = character_key(candidate);
        auto found = by_identity.find(key);
        if (found == by_identity.end()) {
            const size_t slot = first_free();
            std::vector<uint8_t> record(candidate.begin(), candidate.end());
            by_identity.emplace(key, slot);
            characters.emplace(slot, std::move(record));
            return true;
        }
        auto& current = characters[found->second];
        if (experience(candidate) > experience(current))
            current.assign(candidate.begin(), candidate.end());
        return true;
    };

    if (first_save == session_saves.end()) {
        if (baseline.empty()) {
            error = "no instance produced a save";
            return false;
        }
        save.assign(baseline.begin(), baseline.end());
    } else {
        save.assign(first_save->begin(), first_save->end());
    }
    for (const auto& session : session_saves) {
        if (session.empty()) continue;
        for (size_t slot = 0; slot < kActiveCharacters; slot++) {
            const size_t offset = kRosterOffset + slot * kCharacterSize;
            if (!add_character(std::span<const uint8_t>(session).subspan(offset, kCharacterSize))) {
                error = "cannot merge character roster";
                return false;
            }
        }
    }

    const int64_t initial_money = baseline.empty() ? 0 : int64_t(money(baseline));
    int64_t final_money = initial_money;
    for (const auto& session : session_saves) {
        if (!session.empty()) final_money += int64_t(money(session)) - initial_money;
    }
    final_money = std::clamp<int64_t>(final_money, 0, std::numeric_limits<uint32_t>::max());
    put32(save, kMoneyOffset, uint32_t(final_money));
    for (const auto& [slot, character] : characters)
        if (!save_character(character_dir, slot, character, error)) return false;
    write_roster(save, characters);
    return true;
}

}  // namespace save_roster
