/*
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "servicestate.h"

#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename Action>
void rejects(Action action)
{
    bool rejected = false;
    try {
        action();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "Invalid service state was accepted");
}

std::vector<std::uint8_t> text(std::u16string_view value)
{
    std::vector<std::uint8_t> bytes;
    for (auto character : value) {
        bytes.push_back(static_cast<std::uint8_t>(character));
        bytes.push_back(static_cast<std::uint8_t>(character >> 8));
    }
    bytes.insert(bytes.end(), {0, 0});
    return bytes;
}
}

int main()
{
    try {
        ServiceState::Snapshot snapshot;
        require(ServiceState::encode(snapshot).size() == 48, "Unexpected empty snapshot size");
        require(ServiceState::decode(ServiceState::encode(snapshot)) == snapshot, "Empty snapshot round trip failed");
        snapshot.flags = ServiceState::Complete | ServiceState::ConfigCreated | ServiceState::Removing | ServiceState::Restored;
        snapshot.attributes[0] = 1;
        snapshot.attributes[4] = 128;
        ServiceState::RegistryTree root;
        root.values = {{u"", 1, text(u"Original name")},
                       {u"Expand", 2, text(u"%TEMP%\\original")},
                       {u"Multiple", 7, text(std::u16string_view(u"first\0second\0", 13))},
                       {u"Binary", 3, {0, 1, 127, 255}},
                       {u"Dword", 4, {42, 0, 0, 0}},
                       {u"Qword", 11, {1, 0, 0, 0, 1, 0, 0, 0}},
                       {u"Empty", 0, {}},
                       {u"\u6d4b\u8bd5", 1, text(u"\u6587\u4ef6\U0001f4c1")}};
        root.children[u"Nested"].children[u"Empty"] = {};
        snapshot.registry[0] = root;
        snapshot.registry[2] = ServiceState::RegistryTree{};
        const auto encoded = ServiceState::encode(snapshot);
        require(ServiceState::decode(encoded) == snapshot, "Registry types or UTF-16 names changed during round trip");
        ServiceState::Header header;
        std::memcpy(&header, encoded.data(), sizeof(header));
        require(ServiceState::validHeader(header) && header.flags == snapshot.flags, "Startup gate header is incompatible with the snapshot");
        require(ServiceState::blocksStartup(header), "Pending removal did not block daemon startup");
        header.flags = 0;
        require(ServiceState::blocksStartup(header), "Incomplete capture did not block daemon startup");
        header.flags = ServiceState::Complete;
        require(!ServiceState::blocksStartup(header), "A completed registration blocked daemon startup");
        header.format = 99;
        require(ServiceState::blocksStartup(header), "An unsupported state format did not block daemon startup");
        header.format = ServiceState::version;
        auto unsupported = encoded;
        unsupported[8] = 2;
        rejects([&] {
            ServiceState::decode(unsupported);
        });
        for (std::size_t size = 0; size < encoded.size(); ++size) {
            rejects([&] {
                ServiceState::decode(std::span(encoded).first(size));
            });
        }
        for (std::size_t offset = 0; offset < encoded.size(); ++offset) {
            auto mutation = encoded;
            mutation[offset] ^= 0xff;
            std::optional<ServiceState::Snapshot> decoded;
            try {
                decoded = ServiceState::decode(mutation);
            } catch (const std::runtime_error &) {
                continue;
            }
            require(ServiceState::decode(ServiceState::encode(*decoded)) == *decoded, "A modified valid snapshot did not round trip");
        }
        auto invalid = encoded;
        invalid.push_back(0);
        rejects([&] {
            ServiceState::decode(invalid);
        });
        for (auto offset : {0, 8, 15, 36}) {
            invalid = encoded;
            invalid[offset] = 0xff;
            rejects([&] {
                ServiceState::decode(invalid);
            });
        }
        invalid = encoded;
        invalid[40] = invalid[41] = invalid[42] = invalid[43] = 0xff;
        rejects([&] {
            ServiceState::decode(invalid);
        });
        snapshot.registry[0]->values.push_back(snapshot.registry[0]->values[0]);
        rejects([&] {
            ServiceState::decode(ServiceState::encode(snapshot));
        });
        snapshot.registry[0]->values.pop_back();
        snapshot.registry[0]->children[u"invalid\\path"] = {};
        rejects([&] {
            ServiceState::encode(snapshot);
        });
        snapshot.registry[0]->children.erase(u"invalid\\path");
        snapshot.flags = ServiceState::Restored;
        rejects([&] {
            ServiceState::encode(snapshot);
        });
        header.flags = ServiceState::Restored;
        require(!ServiceState::validHeader(header), "Invalid removal header was accepted");
        std::cout << "Service state round trips, corruption checks, and startup gate validation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
