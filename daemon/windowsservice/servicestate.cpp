/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "servicestate.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace
{
constexpr unsigned maximumDepth = 256;

void validateFlags(std::uint32_t flags)
{
    ServiceState::Header header;
    header.flags = flags;
    if (!ServiceState::validHeader(header)) {
        throw std::runtime_error("Invalid background service state flags");
    }
}

class Writer
{
public:
    void number(std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            byte(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void string(const std::u16string &value)
    {
        if (value.size() > 16383 || value.find(u'\0') != std::u16string::npos) {
            throw std::runtime_error("Invalid registry name in service state");
        }
        number(static_cast<std::uint32_t>(value.size()));
        for (char16_t character : value) {
            byte(static_cast<std::uint8_t>(character));
            byte(static_cast<std::uint8_t>(character >> 8));
        }
    }

    void tree(const ServiceState::RegistryTree &key, unsigned depth)
    {
        if (depth > maximumDepth || key.values.size() > ServiceState::maximumSize / 12 || key.children.size() > ServiceState::maximumSize / 12) {
            throw std::runtime_error("Background service registry snapshot is too large");
        }
        number(static_cast<std::uint32_t>(key.values.size()));
        for (const auto &value : key.values) {
            string(value.name);
            number(value.type);
            if (value.data.size() > ServiceState::maximumSize) {
                throw std::runtime_error("Background service registry value is too large");
            }
            number(static_cast<std::uint32_t>(value.data.size()));
            for (auto data : value.data) {
                byte(data);
            }
        }
        number(static_cast<std::uint32_t>(key.children.size()));
        for (const auto &[name, child] : key.children) {
            if (name.empty() || name.size() > 255 || name.find(u'\\') != std::u16string::npos) {
                throw std::runtime_error("Invalid registry subkey in service state");
            }
            string(name);
            tree(child, depth + 1);
        }
    }

    std::vector<std::uint8_t> data;

private:
    void byte(std::uint8_t value)
    {
        if (data.size() >= ServiceState::maximumSize) {
            throw std::runtime_error("Background service state is too large");
        }
        data.push_back(value);
    }
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes)
        : data(bytes)
    {
    }

    std::uint32_t number()
    {
        const auto bytes = take(4);
        std::uint32_t value = 0;
        for (unsigned index = 0; index < 4; ++index) {
            value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
        }
        return value;
    }

    std::u16string string()
    {
        const auto size = number();
        if (size > 16383) {
            throw std::runtime_error("Invalid registry name length in service state");
        }
        const auto bytes = take(size * 2);
        std::u16string value;
        for (std::uint32_t index = 0; index < size; ++index) {
            const auto character = static_cast<char16_t>(bytes[index * 2] | (bytes[index * 2 + 1] << 8));
            if (!character) {
                throw std::runtime_error("Invalid registry name in service state");
            }
            value.push_back(character);
        }
        return value;
    }

    ServiceState::RegistryTree tree(unsigned depth)
    {
        if (depth > maximumDepth) {
            throw std::runtime_error("Background service registry snapshot is too deep");
        }
        ServiceState::RegistryTree key;
        std::set<std::u16string> names;
        const auto values = count(12);
        for (std::uint32_t index = 0; index < values; ++index) {
            ServiceState::Value value;
            value.name = string();
            if (!names.insert(value.name).second) {
                throw std::runtime_error("Duplicate registry value in service state");
            }
            value.type = number();
            const auto bytes = take(number());
            value.data.assign(bytes.begin(), bytes.end());
            key.values.push_back(std::move(value));
        }
        const auto children = count(12);
        for (std::uint32_t index = 0; index < children; ++index) {
            const auto name = string();
            if (name.empty() || name.size() > 255 || name.find(u'\\') != std::u16string::npos || key.children.contains(name)) {
                throw std::runtime_error("Invalid or duplicate registry subkey in service state");
            }
            key.children.emplace(name, tree(depth + 1));
        }
        return key;
    }

    std::span<const std::uint8_t> take(std::size_t size)
    {
        if (size > data.size()) {
            throw std::runtime_error("Truncated background service state");
        }
        const auto bytes = data.first(size);
        data = data.subspan(size);
        return bytes;
    }

    std::span<const std::uint8_t> data;

private:
    std::uint32_t count(unsigned minimumSize)
    {
        const auto value = number();
        if (value > data.size() / minimumSize) {
            throw std::runtime_error("Invalid entry count in background service state");
        }
        return value;
    }
};
}

std::vector<std::uint8_t> ServiceState::encode(const Snapshot &snapshot)
{
    validateFlags(snapshot.flags);
    Writer writer;
    writer.data.insert(writer.data.end(), magic.begin(), magic.end());
    writer.number(version);
    writer.number(snapshot.flags);
    for (auto attributes : snapshot.attributes) {
        writer.number(attributes);
    }
    for (const auto &key : snapshot.registry) {
        writer.number(key ? 1 : 0);
        if (key) {
            writer.tree(*key, 0);
        }
    }
    return std::move(writer.data);
}

ServiceState::Snapshot ServiceState::decode(std::span<const std::uint8_t> data)
{
    if (data.size() > maximumSize) {
        throw std::runtime_error("Background service state is too large");
    }
    Reader reader(data);
    const auto signature = reader.take(magic.size());
    if (!std::equal(signature.begin(), signature.end(), magic.begin()) || reader.number() != version) {
        throw std::runtime_error("Unsupported background service state format");
    }
    Snapshot snapshot;
    snapshot.flags = reader.number();
    validateFlags(snapshot.flags);
    for (auto &attributes : snapshot.attributes) {
        attributes = reader.number();
    }
    for (auto &key : snapshot.registry) {
        const auto present = reader.number();
        if (present > 1) {
            throw std::runtime_error("Invalid registry presence flag in service state");
        }
        if (present) {
            key = reader.tree(0);
        }
    }
    if (!reader.data.empty()) {
        throw std::runtime_error("Unexpected data after background service state");
    }
    return snapshot;
}
