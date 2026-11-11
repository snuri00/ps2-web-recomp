#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace recomp {

struct Segment {
    std::uint32_t address{};
    std::vector<std::uint8_t> bytes;
    bool executable{};
    std::uint32_t initialized_size{};
};

class Image {
public:
    static Image load(const std::string &path);
    static Image raw(std::uint32_t address, std::vector<std::uint8_t> bytes, std::pair<std::uint32_t, std::uint32_t> text);
    static Image split(std::uint32_t address, const std::vector<std::uint8_t> &bytes, std::uint32_t code_begin, std::uint32_t code_end);

    std::uint32_t entry() const { return entry_; }
    const std::vector<Segment> &segments() const { return segments_; }
    const std::map<std::uint32_t, std::string> &symbols() const { return symbols_; }
    std::pair<std::uint32_t, std::uint32_t> text() const { return text_; }

    std::optional<std::uint32_t> word(std::uint32_t address) const;
    bool executable(std::uint32_t address) const;
    void add_symbol(std::uint32_t address, std::string name) { symbols_[address] = std::move(name); }

private:
    std::uint32_t entry_{};
    std::vector<Segment> segments_;
    std::map<std::uint32_t, std::string> symbols_;
    std::pair<std::uint32_t, std::uint32_t> text_{};
};

}
