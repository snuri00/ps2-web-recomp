#pragma once

#include "elf.hpp"
#include "r5900.hpp"

#include <cstdint>
#include <set>
#include <vector>

namespace recomp {

struct Program {
    std::uint32_t base{};
    std::uint32_t end{};
    std::vector<std::uint8_t> code;
    std::set<std::uint32_t> functions;
    std::set<std::uint32_t> entries;
    std::set<std::uint32_t> rejected;
    std::set<std::uint32_t> dynamic;

    bool is_code(std::uint32_t address) const {
        return address >= base && address < end && (address & 3u) == 0 && code[(address - base) >> 2];
    }
};

// weak entries become code only where they decode as code, like the
// candidates the analysis finds itself
Program analyse(const Image &image, const std::vector<std::uint32_t> &extra_entries, const std::set<std::uint32_t> &dynamic = {},
                const std::vector<std::uint32_t> &weak = {});

}
