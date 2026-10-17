#pragma once

#include "elf.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace recomp {

enum class Fix : std::uint8_t { Word, Jump, High, Low };

struct Link {
    Fix kind{};
    bool external{};
    std::uint32_t value{};
};

using Links = std::map<std::uint32_t, Link>;

struct Snr2Module {
    std::string name;
    std::vector<std::uint8_t> bytes;
    std::uint32_t size{};
    std::uint32_t base{};
    std::uint32_t code_begin{};
    std::uint32_t code_end{};
    Links links;
    std::vector<std::uint32_t> seeds;
};

std::optional<Snr2Module> read_snr2_module(std::vector<std::uint8_t> bytes);
Links snr2_imports(const Image &image);
Image snr2_image(const Snr2Module &module);

}
