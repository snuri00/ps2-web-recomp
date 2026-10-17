#pragma once

#include "analysis.hpp"
#include "elf.hpp"
#include "snr2.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace recomp {

struct CodegenOptions {
    std::string output;
    std::uint32_t unit_bits = 14;
    std::set<std::uint32_t> probes;
};

struct Translation {
    const Image *image{};
    Program *program{};
    const Links *links{};
    std::string prefix;
    bool relocatable{};
    std::string name;
    std::uint32_t size{};
    std::vector<std::pair<std::uint32_t, std::uint32_t>> checks;
    // A translation of a code window the game itself rewrites. It is emitted
    // into the module table like a relocatable module's, but its addresses are
    // absolute: every image of that window is precompiled for the one base it
    // occupies.
    bool fixed_module{};
    std::uint32_t fixed_base{};
};

void generate(const std::vector<Translation> &translations, const CodegenOptions &options);

}
