// eetemplates: build position independent code templates from many prelinked images of one code area
//
// usage: eetemplates <load address hex> <out dir> <image>...
//
// Every image holds the same modules placed at other addresses. An image is cut into pieces that
// end after "jr ra; <delay slot>". Pieces equal once address fields are masked (j/jal targets,
// immediates of lui/addiu/ori/loads/stores) are one piece. Pieces that always follow one another
// form a template; each template is written as T<n>.bin with a T<n>.txt telling which words vary
// between the places it occupies:
//   size <bytes>
//   piece <offset> <words> <hash>        a piece start, for finding the template at run time
//   field <offset>                       an immediate that differs: read from memory at run time
//   dyn <offset>                         a j/jal whose target differs: read from memory at run time
//   int <offset>                         a j/jal into the template itself, rewritten to the template base
//   weak <offset>                        a piece start or a word of some image points here: code if it decodes as such
//   at <image> <address>                 a place the template occupies
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kJrRa = 0x03E00008u;
constexpr std::uint32_t kTemplateBase = 0x08000000u;

bool masked_op(std::uint32_t op) {
    return op == 0x0f || op == 0x08 || op == 0x09 || op == 0x0d || op == 0x18 || op == 0x19 || op == 0x1a || op == 0x1b || op == 0x1e || op == 0x1f ||
           (op >= 0x20 && op <= 0x3f);
}

std::uint32_t mask(std::uint32_t w) {
    const std::uint32_t op = w >> 26;
    if (op == 2 || op == 3) return w & 0xFC000000u;
    if (masked_op(op)) return w & 0xFFFF0000u;
    return w;
}

std::uint64_t piece_hash(const std::uint32_t *w, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull ^ n;
    h *= 1099511628211ull;
    for (std::size_t k = 0; k < n; ++k) {
        h ^= mask(w[k]);
        h *= 1099511628211ull;
    }
    return h;
}

struct Piece {
    std::uint32_t start, words;  // word index in the image
    std::uint64_t key;
};

struct Blob {
    std::string name;
    std::vector<std::uint32_t> w;
    std::vector<Piece> pieces;
};

}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: eetemplates <load address hex> <out dir> <image>...\n");
        return 2;
    }
    const std::uint32_t load = std::strtoul(argv[1], nullptr, 16);
    const std::string out = argv[2];
    std::vector<Blob> blobs;
    for (int a = 3; a < argc; ++a) {
        std::ifstream f(argv[a], std::ios::binary);
        std::vector<char> bytes{std::istreambuf_iterator<char>(f), {}};
        Blob b;
        b.name = argv[a];
        b.name = b.name.substr(b.name.find_last_of('/') + 1);
        b.w.resize(bytes.size() / 4);
        std::memcpy(b.w.data(), bytes.data(), b.w.size() * 4);
        std::uint32_t start = 0;
        for (std::uint32_t i = 2; i <= b.w.size(); ++i)
            if (i == b.w.size() || b.w[i - 2] == kJrRa) {
                if (i > start) b.pieces.push_back({start, i - start, piece_hash(&b.w[start], i - start)});
                start = i;
            }
        blobs.push_back(std::move(b));
    }

    // successor and predecessor of every piece key over all its occurrences (~0 = none)
    constexpr std::uint64_t kNone = ~0ull, kMany = ~1ull;
    std::unordered_map<std::uint64_t, std::uint64_t> succ, pred;
    std::unordered_map<std::uint64_t, std::uint32_t> count;
    auto note = [](std::unordered_map<std::uint64_t, std::uint64_t> &m, std::uint64_t k, std::uint64_t v) {
        auto [it, fresh] = m.emplace(k, v);
        if (!fresh && it->second != v) it->second = kMany;
    };
    for (const Blob &b : blobs)
        for (std::size_t k = 0; k < b.pieces.size(); ++k) {
            const std::uint64_t key = b.pieces[k].key;
            ++count[key];
            note(succ, key, k + 1 < b.pieces.size() ? b.pieces[k + 1].key : kNone);
            note(pred, key, k ? b.pieces[k - 1].key : kNone);
        }
    auto linked = [&](std::uint64_t a, std::uint64_t c) {
        return a != c && succ[a] == c && pred[c] == a && count[a] == count[c];
    };

    // templates: chains of always-linked pieces, numbered by first appearance
    struct Occurrence { std::uint32_t blob, piece; };
    struct Template { std::uint32_t pieces; std::vector<Occurrence> at; };
    std::vector<Template> templates;
    std::unordered_map<std::uint64_t, std::uint32_t> template_of_head;
    std::size_t total = 0;
    for (std::uint32_t bi = 0; bi < blobs.size(); ++bi) {
        const Blob &b = blobs[bi];
        total += b.w.size();
        for (std::uint32_t k = 0; k < b.pieces.size();) {
            std::uint32_t n = 1;
            while (k + n < b.pieces.size() && linked(b.pieces[k + n - 1].key, b.pieces[k + n].key)) ++n;
            const std::uint64_t head = b.pieces[k].key;
            auto [it, fresh] = template_of_head.emplace(head, static_cast<std::uint32_t>(templates.size()));
            if (fresh) templates.push_back({n, {}});
            Template &t = templates[it->second];
            if (t.pieces != n) {
                std::fprintf(stderr, "chain of %llx is %u pieces here, %u elsewhere\n", static_cast<unsigned long long>(head), n, t.pieces);
                return 1;
            }
            t.at.push_back({bi, k});
            k += n;
        }
    }

    // weak seeds (code only if they decode as such): every piece start, and every
    // word some image holds that points at a piece start or just past its padding
    std::vector<std::set<std::uint32_t>> weak(templates.size());
    std::size_t pointers = 0;
    for (std::uint32_t ti = 0; ti < templates.size(); ++ti) {
        const Template &t = templates[ti];
        const Blob &first = blobs[t.at[0].blob];
        const std::uint32_t s0 = first.pieces[t.at[0].piece].start;
        for (std::uint32_t p = 0; p < t.pieces; ++p) weak[ti].insert((first.pieces[t.at[0].piece + p].start - s0) * 4);
    }
    for (std::uint32_t bi = 0; bi < blobs.size(); ++bi) {
        const Blob &b = blobs[bi];
        // piece -> template and the offset of the piece in it
        std::vector<std::pair<std::uint32_t, std::uint32_t>> where(b.pieces.size());
        for (std::uint32_t ti = 0; ti < templates.size(); ++ti)
            for (const Occurrence &o : templates[ti].at)
                if (o.blob == bi)
                    for (std::uint32_t p = 0; p < templates[ti].pieces; ++p)
                        where[o.piece + p] = {ti, (b.pieces[o.piece + p].start - b.pieces[o.piece].start) * 4};
        for (const std::uint32_t v : b.w) {
            if (v < load || v - load >= b.w.size() * 4 || (v & 3u)) continue;
            const std::uint32_t idx = (v - load) / 4;
            auto it = std::upper_bound(b.pieces.begin(), b.pieces.end(), idx, [](std::uint32_t x, const Piece &p) { return x < p.start; });
            const std::uint32_t k = static_cast<std::uint32_t>(it - b.pieces.begin()) - 1;
            bool padding = true;
            for (std::uint32_t j = b.pieces[k].start; j < idx; ++j) padding = padding && b.w[j] == 0;
            if (!padding) continue;
            weak[where[k].first].insert(where[k].second + (idx - b.pieces[k].start) * 4);
            ++pointers;
        }
    }

    std::size_t shared_words = 0, written = 0, fields = 0, dyns = 0, ints = 0;
    std::FILE *index = std::fopen((out + "/templates.txt").c_str(), "w");
    for (std::uint32_t ti = 0; ti < templates.size(); ++ti) {
        const Template &t = templates[ti];
        const Blob &first = blobs[t.at[0].blob];
        const std::uint32_t s0 = first.pieces[t.at[0].piece].start;
        const Piece &last = first.pieces[t.at[0].piece + t.pieces - 1];
        const std::uint32_t words = last.start + last.words - s0;
        std::vector<std::uint32_t> body(first.w.begin() + s0, first.w.begin() + s0 + words);
        written += words;
        if (t.at.size() > 1) shared_words += words * (t.at.size() - 1);
        char name[32];
        std::snprintf(name, sizeof name, "T%05u", ti);
        std::FILE *meta = std::fopen((out + "/" + name + ".txt").c_str(), "w");
        std::fprintf(meta, "size %x\n", words * 4);
        for (std::uint32_t p = 0; p < t.pieces; ++p) {
            const Piece &pc = first.pieces[t.at[0].piece + p];
            std::fprintf(meta, "piece %x %x %016llx\n", (pc.start - s0) * 4, pc.words, static_cast<unsigned long long>(pc.key));
        }
        for (std::uint32_t j = 0; j < words; ++j) {
            const std::uint32_t w0 = body[j], op = w0 >> 26;
            bool varies = false, internal = true;
            std::int64_t delta = 0;
            for (std::size_t o = 0; o < t.at.size(); ++o) {
                const Blob &b = blobs[t.at[o].blob];
                const std::uint32_t start = b.pieces[t.at[o].piece].start;
                const std::uint32_t w = b.w[start + j];
                if (w != w0) varies = true;
                if (op == 2 || op == 3) {
                    const std::uint32_t target = (w & 0x03FFFFFFu) << 2;
                    const std::int64_t d = static_cast<std::int64_t>(target) - (load + start * 4);
                    if (o == 0) delta = d;
                    else if (d != delta) internal = false;
                }
            }
            if (op == 2 || op == 3) {
                if (internal && delta >= 0 && delta < words * 4 && (delta & 3) == 0) {
                    body[j] = (op << 26) | (((kTemplateBase + static_cast<std::uint32_t>(delta)) >> 2) & 0x03FFFFFFu);
                    std::fprintf(meta, "int %x\n", j * 4);
                    ++ints;
                } else if (varies) {
                    std::fprintf(meta, "dyn %x\n", j * 4);
                    ++dyns;
                }
            } else if (varies) {
                if (!masked_op(op)) {
                    std::fprintf(stderr, "%s word %x varies but is no address field\n", name, j * 4);
                    return 1;
                }
                std::fprintf(meta, "field %x\n", j * 4);
                ++fields;
            }
        }
        for (const std::uint32_t at : weak[ti]) std::fprintf(meta, "weak %x\n", at);
        for (const Occurrence &o : t.at)
            std::fprintf(meta, "at %s %x\n", blobs[o.blob].name.c_str(), load + blobs[o.blob].pieces[o.piece].start * 4);
        std::fclose(meta);
        std::FILE *bin = std::fopen((out + "/" + name + ".bin").c_str(), "wb");
        std::fwrite(body.data(), 4, body.size(), bin);
        std::fclose(bin);
        std::fprintf(index, "%s %u %zu\n", name, words, t.at.size());
    }
    std::fclose(index);
    std::printf("%zu images, %zu words; %zu templates, %zu words written (%zu shared occurrences words); fields %zu, dynamic jumps %zu, internal jumps %zu, %zu pointers to code\n",
                blobs.size(), total, templates.size(), written, shared_words, fields, dyns, ints, pointers);
    return 0;
}
