#include "ps2/system.hpp"

#ifdef PS2_WINDOW
#include "window.hpp"
#endif
#ifdef PS2_PROFILE
#include "profile.hpp"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

void register_generated_code();
void register_generated_vu();

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ps2run <elf> [--arg text]... [--frames n] [--run n] [--ntsc] [--misses file]\n"
                 "              [--disc image.iso] [--memcard card.ps2] [--pal | --ntsc] [--trace-syscalls] [--trace-io] [--trace-threads]\n"
                 "              [--trace-iop] [--trace-iop-hle] [--frames-dir dir] [--frame-every n]\n"
                 "              [--press frame:button[+button][:frames]]... [--window] [--fullscreen]\n"
                 "              [--save-state frame:file] [--load-state file] [--widescreen] [--patches file]\n"
                 "with --load-state, the frames of --frames, --press, --axis and --save-state count on from the snapshot's\n");
    return 2;
}

std::set<std::string> g_known_entries;
std::size_t g_new_entries = 0;

std::size_t program_count(const std::string &dir) {
    std::size_t count = 0;
    if (dir.empty()) return count;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
        if (entry.path().extension() == ".bin") ++count;
    return count;
}

std::filesystem::path learn_in_game_folder() {
    const std::filesystem::path game = std::filesystem::path(ps2::g_options.disc_path).parent_path();
    const std::filesystem::path entries = game / "entries.txt";
    if (ps2::g_options.disc_path.empty() || !std::filesystem::exists(entries)) return {};
    ps2::g_options.interpret = true;
    if (std::filesystem::is_directory(game / "vu1")) ps2::g_options.vu_record_dir = (game / "vu1").string();
    std::ifstream file(entries);
    for (std::string token; file >> token;) g_known_entries.insert(token);
    ps2::g_options.on_miss = [entries](const std::string &token) {
        if (!g_known_entries.insert(token).second) return;
        std::ofstream(entries, std::ios::app) << token << "\n";
        ++g_new_entries;
    };
    return game;
}

}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    std::vector<std::string> args{argv[1]};
    [[maybe_unused]] bool window = false, fullscreen = false;
    std::string load_path;
    std::uint64_t run_frames = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--arg" && i + 1 < argc) args.push_back(argv[++i]);
        else if (a == "--frames" && i + 1 < argc) ps2::g_options.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--run" && i + 1 < argc) run_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ntsc" || a == "--pal") {
            ps2::g_options.pal = a == "--pal";
            ps2::g_options.rate_forced = true;
        }
        else if (a == "--misses" && i + 1 < argc) ps2::g_options.misses_path = argv[++i];
        else if (a == "--trace-syscalls") ps2::g_options.trace_syscalls = true;
        else if (a == "--widescreen") ps2::g_options.widescreen = true;
        else if (a == "--patches" && i + 1 < argc) {
            if (!ps2::load_patches(argv[++i])) return usage();
        }
        else if (a == "--trace-io") ps2::g_options.trace_io = true;
        else if (a == "--trace-threads") ps2::g_options.trace_threads = true;
        else if (a == "--disc" && i + 1 < argc) ps2::g_options.disc_path = argv[++i];
        else if (a == "--memcard" && i + 1 < argc) ps2::g_options.memcard_path = argv[++i];
        else if (a == "--trace-iop") ps2::g_options.trace_iop_modules = true;
        else if (a == "--trace-iop-hle") ps2::g_options.trace_iop_hle = true;
        else if (a == "--frames-dir" && i + 1 < argc) ps2::g_options.frames_dir = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            ps2::Press press;
            if (!ps2::parse_press(argv[++i], press)) return usage();
            ps2::g_options.presses.push_back(press);
        }
        else if (a == "--axis" && i + 1 < argc) {
            ps2::AxisPress press;
            if (!ps2::parse_axis(argv[++i], press)) return usage();
            ps2::g_options.axis_presses.push_back(press);
        }
#ifdef PS2_WINDOW
        else if (a == "--window") window = true;
        else if (a == "--fullscreen") window = fullscreen = true;
#endif
        else if (a == "--frame-every" && i + 1 < argc) ps2::g_options.frame_every = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--save-state" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto colon = spec.find(':');
            if (colon == std::string::npos) return usage();
            ps2::g_options.save_state_frame = std::strtoull(spec.substr(0, colon).c_str(), nullptr, 10);
            ps2::g_options.save_state_path = spec.substr(colon + 1);
        }
        else if (a == "--load-state" && i + 1 < argc) load_path = argv[++i];
        else return usage();
    }

#ifdef PS2_WINDOW
    if (window && !open_window(fullscreen)) return 1;
#endif
    register_generated_code();
    register_generated_vu();
    const auto emulate = [&]() -> int {
        std::uint32_t entry = 0;
        if (!ps2::load_elf(argv[1], entry)) {
            std::fprintf(stderr, "cannot load %s\n", argv[1]);
            return 1;
        }
        ps2::boot(entry, args);
        if (!load_path.empty()) {
            if (!ps2::load_state(load_path)) return 1;
            // frames given along with a snapshot count on from the frame it was taken at
            const std::uint64_t from = ps2::frame_count();
            if (ps2::g_options.max_frames) ps2::g_options.max_frames += from;
            if (ps2::g_options.save_state_frame) ps2::g_options.save_state_frame += from;
            for (ps2::Press &p : ps2::g_options.presses) p.frame += from;
            for (ps2::AxisPress &p : ps2::g_options.axis_presses) p.frame += from;
        }
        if (run_frames) ps2::g_options.max_frames = ps2::frame_count() + run_frames;
#ifdef PS2_PROFILE
        const char *profile = std::getenv("PS2_PROFILE");
        const char *profile_from = std::getenv("PS2_PROFILE_FROM");
        if (profile) start_profile(profile_from ? std::strtoull(profile_from, nullptr, 10) : 0);
        const int result = ps2::run();
        if (profile) write_profile(profile);
        return result;
#else
        return ps2::run();
#endif
    };
#ifdef PS2_WINDOW
    if (window) {
        const std::filesystem::path game = learn_in_game_folder();
        const std::size_t programs = program_count(ps2::g_options.vu_record_dir);
        return run_window([&] {
            const int result = emulate();
            const std::size_t new_programs = program_count(ps2::g_options.vu_record_dir) - programs;
            if (g_new_entries || new_programs)
                std::fprintf(stderr, "[learn] %zu entry points and %zu VU1 programs this build lacks were written to %s, port.sh translates them on its next run\n",
                             g_new_entries, new_programs, game.string().c_str());
            return result;
        });
    }
#endif
    return emulate();
}
