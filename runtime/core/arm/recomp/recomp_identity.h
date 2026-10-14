// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <openssl/evp.h>
#include "common/common_types.h"

namespace Core {
inline constexpr unsigned RecompAbiVersion = 3;
struct RecompModuleIdentity {
    const char* name;
    std::array<const char*, 3> sha256;
};
inline std::string RecompSha256(std::span<const u8> data) {
    std::array<unsigned char, 32> hash{};
    unsigned length{};
    if (EVP_Digest(data.data(), data.size(), hash.data(), &length, EVP_sha256(), nullptr) != 1 ||
        length != hash.size()) throw std::runtime_error("Cannot hash recompiled module");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : hash) {
        result += hex[byte >> 4]; result += hex[byte & 15];
    }
    return result;
}
void SetRecompInputDump(const std::string& directory);
bool RecompInputDumpEnabled();
void FinishRecompInputDump(u64 program_id);
void SetRecompIdentities(std::span<const RecompModuleIdentity> identities);
bool ValidateRecompModule(std::string_view name, const std::array<std::span<const u8>, 3>& segments,
                          u64 base, const std::array<u64, 3>& offsets);
bool AllRecompModulesValidated();
std::string RecompModuleAt(u64 pc, u64& offset);
} // namespace Core
