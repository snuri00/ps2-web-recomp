#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace ps2::state {

class Stream {
public:
    explicit Stream(bool saving) : saving_(saving) {}
    virtual ~Stream() = default;
    bool saving() const { return saving_; }
    bool loading() const { return !saving_; }
    virtual void bytes(void *data, std::size_t size) = 0;

    template <typename T>
    void pod(T &v) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes(&v, sizeof v);
    }

    void size(std::size_t &v) {
        std::uint64_t wide = v;
        pod(wide);
        v = static_cast<std::size_t>(wide);
    }

    void tag(const char *name) {
        char text[8] = {};
        std::strncpy(text, name, sizeof text);
        char got[8];
        std::memcpy(got, text, sizeof got);
        bytes(got, sizeof got);
        if (loading() && std::memcmp(got, text, sizeof got) != 0) throw std::runtime_error(std::string("state section mismatch at ") + name);
    }

    std::uint32_t count(std::size_t n) {
        std::uint32_t v = static_cast<std::uint32_t>(n);
        pod(v);
        return v;
    }

    void str(std::string &s) {
        const std::uint32_t n = count(s.size());
        if (loading()) s.resize(n);
        bytes(s.data(), n);
    }

    template <typename T>
    void vec(std::vector<T> &v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t n = count(v.size());
        if (loading()) v.resize(n);
        bytes(v.data(), static_cast<std::size_t>(n) * sizeof(T));
    }

    template <typename T, typename F>
    void vec(std::vector<T> &v, F each) {
        const std::uint32_t n = count(v.size());
        if (loading()) v.assign(n, T{});
        for (T &item : v) each(item);
    }

    template <typename T>
    void deque(std::deque<T> &d) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t n = count(d.size());
        if (loading()) d.assign(n, T{});
        for (T &item : d) pod(item);
    }

    template <typename T, typename F>
    void deque(std::deque<T> &d, F each) {
        const std::uint32_t n = count(d.size());
        if (loading()) d.assign(n, T{});
        for (T &item : d) each(item);
    }

    template <typename T>
    void set(std::set<T> &s) {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::uint32_t n = count(s.size());
        if (saving()) {
            for (T item : s) pod(item);
            return;
        }
        s.clear();
        for (std::uint32_t i = 0; i < n; ++i) {
            T item{};
            pod(item);
            s.insert(item);
        }
    }

    template <typename K, typename V, typename F>
    void map(std::map<K, V> &m, F each) {
        static_assert(std::is_trivially_copyable_v<K>);
        const std::uint32_t n = count(m.size());
        if (saving()) {
            for (auto &[k, v] : m) {
                K key = k;
                pod(key);
                each(v);
            }
            return;
        }
        m.clear();
        for (std::uint32_t i = 0; i < n; ++i) {
            K key{};
            pod(key);
            each(m[key]);
        }
    }

    template <typename K, typename V>
    void map(std::map<K, V> &m) {
        map(m, [this](V &v) { pod(v); });
    }

    template <typename V, typename F>
    void string_map(std::map<std::string, V> &m, F each) {
        const std::uint32_t n = count(m.size());
        if (saving()) {
            for (auto &[k, v] : m) {
                std::string key = k;
                str(key);
                each(v);
            }
            return;
        }
        m.clear();
        for (std::uint32_t i = 0; i < n; ++i) {
            std::string key;
            str(key);
            each(m[key]);
        }
    }

private:
    bool saving_;
};

class Writer : public Stream {
public:
    Writer() : Stream(true) {}
    void bytes(void *data, std::size_t size) override {
        if (!size) return; // Empty containers may have a null data() pointer.
        const auto *p = static_cast<const std::uint8_t *>(data);
        out.insert(out.end(), p, p + size);
    }
    std::vector<std::uint8_t> out;
};

class Reader : public Stream {
public:
    Reader(const std::uint8_t *data, std::size_t size) : Stream(false), at_(data), end_(data + size) {}
    void bytes(void *data, std::size_t size) override {
        if (!size) return;
        if (static_cast<std::size_t>(end_ - at_) < size) throw std::runtime_error("state file is truncated");
        std::memcpy(data, at_, size);
        at_ += size;
    }

private:
    const std::uint8_t *at_;
    const std::uint8_t *end_;
};

}
