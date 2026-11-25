#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace save_roster {

constexpr size_t kSaveSize = 64 * 1024;
constexpr size_t kCharacterSize = 140;
constexpr size_t kActiveCharacters = 16;
constexpr size_t kXpOffset = 0x3A;

std::filesystem::path character_directory(const std::filesystem::path& save_path);

bool prepare(std::span<const uint8_t> source, std::vector<uint8_t>& save,
             const std::filesystem::path& character_dir, std::string& error);

bool merge(std::span<const uint8_t> baseline, const std::vector<std::vector<uint8_t>>& session_saves,
           const std::filesystem::path& character_dir, std::vector<uint8_t>& save, std::string& error);
void rotate_active_characters(std::vector<uint8_t>& save, size_t offset);

bool valid(std::span<const uint8_t> save, std::string& error);
uint32_t money(std::span<const uint8_t> save);
void set_money(std::vector<uint8_t>& save, uint32_t value);
void repair(std::vector<uint8_t>& save);

}  // namespace save_roster
