#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mmdx {

class BinaryReader {
public:
    BinaryReader(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    bool Read(void* dst, size_t n) {
        if (failed_) return false;
        if (pos_ + n > size_) {
            failed_ = true;
            if (dst && n > 0) {
                auto* bytes = static_cast<uint8_t*>(dst);
                for (size_t i = 0; i < n; ++i) bytes[i] = 0;
            }
            return false;
        }
        if (dst && n > 0) {
            auto* bytes = static_cast<uint8_t*>(dst);
            for (size_t i = 0; i < n; ++i) bytes[i] = data_[pos_ + i];
        }
        pos_ += n;
        return true;
    }

    template<class T> bool Read(T& v) { return Read(&v, sizeof(T)); }

    bool Skip(size_t n) { return Read(nullptr, n); }

    size_t Pos() const { return pos_; }
    size_t Size() const { return size_; }
    size_t Remaining() const { return size_ - pos_; }
    bool Failed() const { return failed_; }

    // Signed index of 1/2/4 bytes (int8/int16/int32, sign-extended). Used for
    // bone/texture/material/morph/rigid indices.
    int32_t ReadIndex(uint8_t size) {
        if (size == 1) {
            int8_t v = 0; Read(v); return static_cast<int32_t>(v);
        }
        if (size == 2) {
            int16_t v = 0; Read(v); return static_cast<int32_t>(v);
        }
        int32_t v = 0; Read(v); return v;
    }

    // Unsigned vertex index of 1/2/4 bytes (uint8/uint16/int32).
    int32_t ReadVertexIndex(uint8_t size) {
        if (size == 1) {
            uint8_t v = 0; Read(v); return static_cast<int32_t>(v);
        }
        if (size == 2) {
            uint16_t v = 0; Read(v); return static_cast<int32_t>(v);
        }
        int32_t v = 0; Read(v); return v;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool failed_ = false;
};

// Reads the whole file into `out`. Returns false and fills *error on failure.
inline bool ReadWholeFile(const std::filesystem::path& p, std::vector<uint8_t>& out,
                          std::string* error) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) {
        if (error) *error = "cannot open file";
        return false;
    }
    std::streamsize size = f.tellg();
    if (size < 0) {
        if (error) *error = "cannot determine file size";
        return false;
    }
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        f.read(reinterpret_cast<char*>(out.data()), size);
        if (f.gcount() != size) {
            if (error) *error = "short read";
            return false;
        }
    }
    return true;
}

} // namespace mmdx
