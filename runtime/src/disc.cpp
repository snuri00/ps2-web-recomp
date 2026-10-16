#include "ps2/disc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

namespace ps2::disc {

namespace {

std::FILE *g_file = nullptr;
Reader g_reader;
Probe g_probe;
std::uint64_t g_size = 0;
std::uint32_t g_layer1 = 0;
std::map<std::string, Entry> g_index;

std::uint32_t u32(const std::uint8_t *p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::string upper(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void walk(std::uint32_t base, std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth) {
    if (depth > 16) return;
    std::vector<std::uint8_t> data((size + kSector - 1) / kSector * kSector);
    if (!read(base + lba, static_cast<std::uint32_t>(data.size() / kSector), data.data())) return;
    std::size_t off = 0;
    while (off < size) {
        const std::uint8_t length = data[off];
        if (!length) {
            off = (off / kSector + 1) * kSector;
            continue;
        }
        const std::uint8_t *rec = &data[off];
        off += length;
        const std::uint8_t name_len = rec[32];
        std::string name(reinterpret_cast<const char *>(rec + 33), name_len);
        if (name_len == 1 && (name[0] == 0 || name[0] == 1)) continue;
        const auto semi = name.find(';');
        if (semi != std::string::npos) name = name.substr(0, semi);
        const std::string path = prefix + upper(name);
        const bool dir = rec[25] & 2;
        const Entry entry{base + u32(rec + 2), u32(rec + 10), dir};
        g_index[path] = entry;
        if (dir) walk(base, u32(rec + 2), u32(rec + 10), path + "/", depth + 1);
    }
}

void index_layer(std::uint32_t pvd_sector, std::uint32_t base) {
    std::uint8_t pvd[kSector];
    if (!read(pvd_sector, 1, pvd) || std::memcmp(pvd, "\x01" "CD001", 6) != 0) return;
    walk(base, u32(pvd + 158), u32(pvd + 166), "", 0);
}

constexpr std::uint32_t kChunkSectors = 32;
constexpr std::size_t kMaxCachedChunks = 128;

struct CacheChunk {
    std::uint32_t chunk_idx = 0;
    std::uint64_t access_seq = 0;
    std::vector<std::uint8_t> data;
};

std::map<std::uint32_t, CacheChunk> g_cache;
std::uint64_t g_cache_seq = 0;

void evict_oldest_chunk() {
    if (g_cache.size() < kMaxCachedChunks) return;
    auto oldest = g_cache.begin();
    for (auto it = g_cache.begin(); it != g_cache.end(); ++it) {
        if (it->second.access_seq < oldest->second.access_seq) oldest = it;
    }
    g_cache.erase(oldest);
}

}

bool open(const std::string &path) {
    g_file = std::fopen(path.c_str(), "rb");
    if (!g_file) return false;
#if defined(_WIN32)
    _fseeki64(g_file, 0, SEEK_END);
    const std::uint64_t size = static_cast<std::uint64_t>(_ftelli64(g_file));
#else
    fseeko(g_file, 0, SEEK_END);
    const std::uint64_t size = static_cast<std::uint64_t>(ftello(g_file));
#endif
    std::FILE *file = g_file;
    return open(path, size, [file](std::uint64_t offset, void *out, std::size_t bytes) {
#if defined(_WIN32)
        _fseeki64(file, static_cast<long long>(offset), SEEK_SET);
#else
        fseeko(file, static_cast<off_t>(offset), SEEK_SET);
#endif
        return std::fread(out, 1, bytes, file);
    });
}

bool open(const std::string &name, std::uint64_t size, Reader reader, Probe probe) {
    g_reader = std::move(reader);
    g_probe = std::move(probe);
    g_size = size;
    g_index.clear();
    g_cache.clear();
    g_cache_seq = 0;
    index_layer(16, 0);
    std::uint8_t pvd[kSector];
    read(16, 1, pvd);
    const std::uint32_t layer0 = u32(pvd + 80);
    for (std::uint32_t s = layer0; s < layer0 + 64 && static_cast<std::uint64_t>(s) * kSector < g_size; ++s) {
        std::uint8_t probe[kSector];
        if (read(s, 1, probe) && std::memcmp(probe, "\x01" "CD001", 6) == 0) {
            g_layer1 = s - 16;
            index_layer(s, g_layer1);
            break;
        }
    }
    std::fprintf(stderr, "[disc] %s: %zu files, %llu sectors, layer 1 at %u\n", name.c_str(), g_index.size(),
                 static_cast<unsigned long long>(g_size / kSector), g_layer1);
    return true;
}

bool is_open() { return static_cast<bool>(g_reader); }

bool read(std::uint32_t sector, std::uint32_t count, void *out) {
    if (!g_reader) return false;
    if (!count) return true;

    if (count > kChunkSectors * 2) {
        const std::size_t want = static_cast<std::size_t>(count) * kSector;
        const std::size_t got = g_reader(static_cast<std::uint64_t>(sector) * kSector, out, want);
        if (got < want) std::memset(static_cast<std::uint8_t *>(out) + got, 0, want - got);
        return got > 0;
    }

    std::uint8_t *dst = static_cast<std::uint8_t *>(out);
    std::uint32_t remaining = count;
    std::uint32_t cur_sector = sector;

    while (remaining > 0) {
        const std::uint32_t chunk_idx = cur_sector / kChunkSectors;
        const std::uint32_t offset_in_chunk = cur_sector % kChunkSectors;
        const std::uint32_t can_read = std::min(remaining, kChunkSectors - offset_in_chunk);

        auto it = g_cache.find(chunk_idx);
        if (it == g_cache.end()) {
            CacheChunk chunk;
            chunk.chunk_idx = chunk_idx;
            chunk.access_seq = ++g_cache_seq;
            chunk.data.resize(kChunkSectors * kSector);
            const std::uint64_t chunk_start = static_cast<std::uint64_t>(chunk_idx) * kChunkSectors;
            const std::size_t want_bytes = kChunkSectors * kSector;
            const std::size_t got_bytes = g_reader(chunk_start * kSector, chunk.data.data(), want_bytes);
            if (got_bytes < want_bytes) {
                std::memset(chunk.data.data() + got_bytes, 0, want_bytes - got_bytes);
            }
            evict_oldest_chunk();
            it = g_cache.emplace(chunk_idx, std::move(chunk)).first;
        } else {
            it->second.access_seq = ++g_cache_seq;
        }

        std::memcpy(dst, it->second.data.data() + offset_in_chunk * kSector, can_read * kSector);
        dst += can_read * kSector;
        cur_sector += can_read;
        remaining -= can_read;
    }
    return true;
}

bool ready(std::uint32_t sector, std::uint32_t count) {
    if (!count) return true;
    const std::uint32_t chunk_idx = sector / kChunkSectors;
    if (g_cache.count(chunk_idx)) return true;
    return !g_probe || g_probe(static_cast<std::uint64_t>(sector) * kSector, static_cast<std::size_t>(count) * kSector);
}

std::optional<Entry> find(const std::string &raw) {
    std::string path = raw;
    const auto colon = path.find(':');
    if (colon != std::string::npos) path = path.substr(colon + 1);
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto semi = path.find(';');
    if (semi != std::string::npos) path = path.substr(0, semi);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    auto it = g_index.find(upper(path));
    if (it == g_index.end()) return std::nullopt;
    return it->second;
}

std::uint32_t layer1_start() { return g_layer1; }
std::uint64_t sector_count() { return g_size / kSector; }

}
