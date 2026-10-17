#include "analysis.hpp"
#include "codegen.hpp"
#include "elf.hpp"
#include "snr2.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace recomp;

namespace {

struct Entries {
    std::vector<std::uint32_t> main;
    std::map<std::string, std::vector<std::uint32_t>> modules;
};

Entries read_entries(const std::string &path) {
    Entries entries;
    std::ifstream file(path);
    std::string token;
    while (file >> token) {
        if (token[0] == '#') { std::getline(file, token); continue; }
        const auto colon = token.find(':');
        if (colon == std::string::npos)
            entries.main.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 16)));
        else
            entries.modules[token.substr(0, colon)].push_back(static_cast<std::uint32_t>(std::stoul(token.substr(colon + 1), nullptr, 16)));
    }
    return entries;
}

std::set<std::uint32_t> dynamic_sites(const Links &links) {
    std::set<std::uint32_t> sites;
    for (const auto &[at, link] : links)
        if (link.kind == Fix::Jump && link.external) sites.insert(at);
    return sites;
}

std::size_t code_size(const Program &program) {
    std::size_t code = 0;
    for (std::uint32_t pc = program.base; pc < program.end; pc += 4) code += program.is_code(pc);
    return code;
}

std::uint32_t word_at(const Snr2Module &m, std::uint32_t at) {
    std::uint32_t value;
    std::memcpy(&value, m.bytes.data() + at, 4);
    return value;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> checks_for(const Snr2Module &m, const Program &program) {
    std::vector<std::uint32_t> words;
    for (std::uint32_t pc = m.code_begin; pc + 4 <= m.code_end; pc += 4)
        if (program.is_code(pc) && !m.links.count(pc) && word_at(m, pc)) words.push_back(pc);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> checks{{0, 0x32524E53u}};
    for (std::size_t k = 0; k < 8 && !words.empty(); ++k) {
        const std::uint32_t pc = words[k * words.size() / 8];
        if (checks.back().first != pc) checks.emplace_back(pc, word_at(m, pc));
    }
    return checks;
}

std::vector<Snr2Module> read_modules(const std::string &dir) {
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
        if (entry.is_regular_file()) paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    std::vector<Snr2Module> modules;
    std::map<std::string, int> names;
    for (const auto &path : paths) {
        std::ifstream file(path, std::ios::binary);
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        auto module = read_snr2_module(std::move(bytes));
        if (!module) continue;
        if (std::any_of(modules.begin(), modules.end(), [&](const Snr2Module &m) { return m.bytes == module->bytes; })) continue;
        if (const int n = names[module->name]++) module->name += "_" + std::to_string(n + 1);
        modules.push_back(std::move(*module));
    }
    return modules;
}

// A code window the game rewrites at runtime is a segment of the executable
// with no file contents. The pipeline captures what the game unpacks into it
// as files named <base>_<name>.bin, one content per file, and this reads them
// all as images of the one window, each a module to translate for that base.
std::vector<Snr2Module> read_overlays(const std::string &dir) {
    std::vector<Snr2Module> overlays;
    for (const auto &path : std::filesystem::directory_iterator(dir)) {
        if (!path.is_regular_file() || path.path().extension() != ".bin") continue;
        const std::string stem = path.path().stem().string();
        const auto split = stem.find('_');
        if (split == std::string::npos) continue;
        const std::string base = stem.substr(0, split);
        if (base.empty() || base.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
        Snr2Module o;
        std::ifstream file(path.path(), std::ios::binary);
        o.bytes = std::vector<std::uint8_t>{std::istreambuf_iterator<char>(file), {}};
        if (o.bytes.size() < 0x400u || o.bytes.size() & 3u) continue;
        o.size = static_cast<std::uint32_t>(o.bytes.size());
        o.base = static_cast<std::uint32_t>(std::stoul(base, nullptr, 16));
        o.code_begin = 0;
        o.code_end = o.size;
        o.name = stem;
        if (std::any_of(overlays.begin(), overlays.end(), [&](const Snr2Module &m) { return m.base == o.base && m.bytes == o.bytes; })) continue;
        overlays.push_back(std::move(o));
    }
    std::sort(overlays.begin(), overlays.end(), [](const Snr2Module &a, const Snr2Module &b) { return a.name < b.name; });
    return overlays;
}

// Calls the static program makes into the window seed every image of it, so
// each is translated from those entry points whether or not it has been seen.
std::vector<std::uint32_t> window_entries(const Image &image, const Program &program, std::uint32_t base, std::uint32_t size) {
    std::vector<std::uint32_t> seeds;
    for (std::uint32_t pc = program.base; pc < program.end; pc += 4) {
        if (!program.is_code(pc)) continue;
        const Insn i = decode(*image.word(pc), pc);
        const Flow f = flow(i.op);
        if (f != Flow::Jump && f != Flow::Call) continue;
        const std::uint32_t t = i.jump_target();
        if (t - base < size) seeds.push_back(t);
    }
    return seeds;
}

// A window image is served only while every word of it the game runs as code
// is in place, so the checks are all the translated code words: the game
// writes the window's data freely, and a change any translation covers is a
// different image and a call into it must pick the other module.
std::vector<std::pair<std::uint32_t, std::uint32_t>> checks_for_window(const Snr2Module &o, const Program &program) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> checks;
    for (std::uint32_t at = 0; at + 4 <= o.size; at += 4) {
        if (!program.is_code(o.base + at)) continue;
        std::uint32_t word = 0;
        std::memcpy(&word, o.bytes.data() + at, 4);
        checks.emplace_back(at, word);
    }
    return checks;
}

int usage() {
    std::fprintf(stderr, "usage: eerecomp <elf> [--entries file] [--modules dir] [--overlays dir] [--probes file] [--out dir] [--stats]\n");
    return 2;
}

}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::string elf_path = argv[1], entries_path, out, probes_path, modules_dir, overlays_dir;
    bool stats = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--entries" && i + 1 < argc) entries_path = argv[++i];
        else if (arg == "--stats") stats = true;
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else if (arg == "--probes" && i + 1 < argc) probes_path = argv[++i];
        else if (arg == "--modules" && i + 1 < argc) modules_dir = argv[++i];
        else if (arg == "--overlays" && i + 1 < argc) overlays_dir = argv[++i];
        else return usage();
    }

    try {
        const Image image = Image::load(elf_path);
        const Entries entries = entries_path.empty() ? Entries{} : read_entries(entries_path);
        const Links imports = snr2_imports(image);
        Program program = analyse(image, entries.main, dynamic_sites(imports));
        std::map<Op, std::size_t> histogram;
        for (std::uint32_t pc = program.base; pc < program.end; pc += 4)
            if (program.is_code(pc)) ++histogram[decode(*image.word(pc), pc).op];
        const std::size_t code = code_size(program);
        std::printf("code %zu instructions (%zu KiB), functions %zu, entries %zu, rejected seeds %zu\n",
                    code, code * 4 / 1024, program.functions.size(), program.entries.size(), program.rejected.size());
        if (stats)
            for (const auto &[op, count] : histogram) std::printf("%10zu %s\n", count, name(op));

        std::vector<Translation> translations{{&image, &program, imports.empty() ? nullptr : &imports, "", false, "", 0, {}}};
        const std::vector<Snr2Module> modules = modules_dir.empty() ? std::vector<Snr2Module>{} : read_modules(modules_dir);
        std::deque<Image> images;
        std::deque<Program> programs;
        std::size_t module_code = 0;
        for (const Snr2Module &m : modules) {
            images.push_back(snr2_image(m));
            std::vector<std::uint32_t> seeds = m.seeds;
            if (const auto it = entries.modules.find(m.name); it != entries.modules.end()) seeds.insert(seeds.end(), it->second.begin(), it->second.end());
            programs.push_back(analyse(images.back(), seeds, dynamic_sites(m.links)));
            module_code += code_size(programs.back());
            translations.push_back({&images.back(), &programs.back(), &m.links, "m_" + m.name + "_", true, m.name, m.size, checks_for(m, programs.back())});
        }
        if (!modules.empty())
            std::printf("%zu modules, code %zu instructions (%zu KiB), %zu sites in the program linked at run time\n", modules.size(), module_code,
                        module_code * 4 / 1024, imports.size());
        const std::vector<Snr2Module> overlays =
            overlays_dir.empty() || !std::filesystem::exists(overlays_dir) ? std::vector<Snr2Module>{} : read_overlays(overlays_dir);
        // Data in a window lives and dies with the game, so captures of one
        // image differ in bytes it wrote since. The code identifies the image:
        // analyse every capture with the same seeds, group the ones answering
        // to one code, and translate one image per group, so a re-captured
        // image is never served by a translation that has not seen its entry
        // points.
        std::size_t overlay_code = 0;
        std::map<std::uint32_t, std::vector<std::uint32_t>> window_seeds;
        std::map<std::uint32_t, std::set<std::uint32_t>> window_tokens;
        std::map<std::string, std::string> variant_of;
        std::map<std::string, std::set<std::uint32_t>> window_offsets;
        std::map<std::uint32_t, std::vector<std::pair<std::string, std::vector<std::pair<std::uint32_t, std::uint32_t>>>>> code_signatures;
        std::set<std::string> representatives;
        std::set<std::uint32_t> called_out;
        for (const Snr2Module &o : overlays) {
            if (!window_seeds.count(o.base)) window_seeds[o.base] = window_entries(image, program, o.base, o.size);
            if (const auto it = entries.modules.find(o.name); it != entries.modules.end())
                for (const std::uint32_t entry : it->second) window_tokens[o.base].insert(entry);
        }
        for (const Snr2Module &o : overlays) {
            Image candidate = Image::raw(o.base, o.bytes, {0, o.size});
            std::vector<std::uint32_t> seeds = window_seeds[o.base];
            for (const std::uint32_t entry : window_tokens[o.base]) seeds.push_back(o.base + entry);
            const Program signature_program = analyse(candidate, seeds, {});
            std::vector<std::pair<std::uint32_t, std::uint32_t>> signature;
            for (std::uint32_t at = 0; at + 4 <= o.size; at += 4)
                if (signature_program.is_code(o.base + at)) {
                    std::uint32_t word = 0;
                    std::memcpy(&word, o.bytes.data() + at, 4);
                    signature.emplace_back(at, word);
                }
            bool found = false;
            for (const auto &[name, sig] : code_signatures[o.base])
                if (sig == signature) { found = true; variant_of[o.name] = name; break; }
            if (!found) {
                code_signatures[o.base].push_back({o.name, signature});
                variant_of[o.name] = o.name;
                representatives.insert(o.name);
            }
            if (const auto it = entries.modules.find(o.name); it != entries.modules.end())
                for (const std::uint32_t entry : it->second) window_offsets[variant_of[o.name]].insert(entry);
        }
        for (const Snr2Module &o : overlays) {
            if (!representatives.count(o.name)) continue;
            images.push_back(Image::raw(o.base, o.bytes, {0, o.size}));
            std::vector<std::uint32_t> seeds = window_seeds[o.base];
            // Entry tokens name module positions, and unlike a relocatable
            // module this image occupies a real address.
            for (const std::uint32_t entry : window_offsets[o.name]) seeds.push_back(o.base + entry);
            programs.push_back(analyse(images.back(), seeds, {}));
            overlay_code += code_size(programs.back());
            translations.push_back({&images.back(), &programs.back(), nullptr, "m_" + o.name + "_", false, o.name, o.size,
                                    checks_for_window(o, programs.back()), true, o.base});
            for (std::uint32_t pc = o.base; pc < o.base + o.size; pc += 4) {
                if (!programs.back().is_code(pc)) continue;
                const Insn i = decode(*images.back().word(pc), pc);
                const Flow f = flow(i.op);
                const std::uint32_t t = i.jump_target();
                if ((f == Flow::Jump || f == Flow::Call) && t >= program.base && t < program.end && !program.entries.count(t)) called_out.insert(t);
            }
        }
        // Window code jumps into the program at places the program may reach
        // only through pointers, if at all: entry points of it all the same.
        if (!called_out.empty()) {
            std::vector<std::uint32_t> seeds = entries.main;
            seeds.insert(seeds.end(), called_out.begin(), called_out.end());
            program = analyse(image, seeds, dynamic_sites(imports));
            std::printf("%zu entry points of the program called from code windows\n", called_out.size());
        }
        if (!overlays.empty()) {
            std::size_t called_entries = 0;
            for (const auto &[base, seeds] : window_seeds) called_entries += seeds.size();
            std::printf("%zu distinct code window images of %zu captured, code %zu instructions (%zu KiB), %zu entry points called from the program\n",
                        std::count_if(overlays.begin(), overlays.end(), [&](const Snr2Module &o) { return representatives.count(o.name) != 0; }),
                        overlays.size(), overlay_code, overlay_code * 4 / 1024, called_entries);
        }

        if (!out.empty()) {
            CodegenOptions options;
            options.output = out;
            if (!probes_path.empty())
                for (std::uint32_t a : read_entries(probes_path).main) options.probes.insert(a);
            generate(translations, options);
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "eerecomp: %s\n", error.what());
        return 1;
    }
    return 0;
}
