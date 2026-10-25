/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ServiceState
{
inline constexpr std::array<char, 8> magic{'K', 'D', 'C', 'S', 'T', 'A', 'T', 'E'};
inline constexpr std::uint32_t version = 2;
inline constexpr std::size_t maximumSize = 64 * 1024 * 1024;
inline constexpr wchar_t fileName[] = L"state.bin";
inline constexpr wchar_t directoryName[] = L"service-state";

enum Flag : std::uint32_t {
    Complete = 1,
    Legacy = 2,
    ConfigCreated = 4,
    Removing = 8,
    Restored = 16,
};
inline constexpr std::uint32_t knownFlags = Complete | Legacy | ConfigCreated | Removing | Restored;

struct Header {
    std::array<char, 8> signature = magic;
    std::uint32_t format = version;
    std::uint32_t flags = 0;
};
static_assert(sizeof(Header) == 16);
inline constexpr std::size_t minimumSize = sizeof(Header) + 6 * sizeof(std::uint32_t) + 3 * sizeof(std::uint32_t);

struct Value {
    std::u16string name;
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
    bool operator==(const Value &) const = default;
};

struct RegistryTree {
    std::vector<Value> values;
    std::map<std::u16string, RegistryTree> children;
    bool operator==(const RegistryTree &) const = default;
};

struct Snapshot {
    std::uint32_t flags = 0;
    std::array<std::uint32_t, 6> attributes{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    std::array<std::optional<RegistryTree>, 3> registry;
    bool operator==(const Snapshot &) const = default;
};

inline bool validHeader(const Header &header)
{
    return header.signature == magic && header.format == version && !(header.flags & ~knownFlags)
        && (!(header.flags & Restored) || ((header.flags & Removing) && (header.flags & Complete)));
}
inline bool blocksStartup(const Header &header)
{
    return !validHeader(header) || !(header.flags & Complete) || (header.flags & Removing);
}
std::vector<std::uint8_t> encode(const Snapshot &snapshot);
Snapshot decode(std::span<const std::uint8_t> data);
}
