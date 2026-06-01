#include "util/Sha256.h"

#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace autoupdater::util {

namespace {

constexpr std::array<std::uint32_t, 64> k = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL};

std::uint32_t rotr(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

std::uint32_t choose(std::uint32_t e, std::uint32_t f, std::uint32_t g) {
    return (e & f) ^ (~e & g);
}

std::uint32_t majority(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return (a & b) ^ (a & c) ^ (b & c);
}

std::uint32_t bigSigma0(std::uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

std::uint32_t bigSigma1(std::uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

std::uint32_t smallSigma0(std::uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

std::uint32_t smallSigma1(std::uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t length) {
        totalBytes_ += length;
        for (std::size_t i = 0; i < length; ++i) {
            buffer_[bufferSize_++] = data[i];
            if (bufferSize_ == 64) {
                transform(buffer_.data());
                bufferSize_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> final() {
        const std::uint64_t bitLength = static_cast<std::uint64_t>(totalBytes_) * 8ULL;
        buffer_[bufferSize_++] = 0x80;

        if (bufferSize_ > 56) {
            while (bufferSize_ < 64) {
                buffer_[bufferSize_++] = 0;
            }
            transform(buffer_.data());
            bufferSize_ = 0;
        }

        while (bufferSize_ < 56) {
            buffer_[bufferSize_++] = 0;
        }

        for (int i = 7; i >= 0; --i) {
            buffer_[bufferSize_++] = static_cast<std::uint8_t>((bitLength >> (i * 8)) & 0xff);
        }
        transform(buffer_.data());

        std::array<std::uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xff);
            digest[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xff);
            digest[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xff);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xff);
        }
        return digest;
    }

private:
    void transform(const std::uint8_t block[64]) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            w[i] = smallSigma1(w[i - 2]) + w[i - 7] + smallSigma0(w[i - 15]) + w[i - 16];
        }

        auto a = state_[0];
        auto b = state_[1];
        auto c = state_[2];
        auto d = state_[3];
        auto e = state_[4];
        auto f = state_[5];
        auto g = state_[6];
        auto h = state_[7];

        for (std::size_t i = 0; i < 64; ++i) {
            const auto t1 = h + bigSigma1(e) + choose(e, f, g) + k[i] + w[i];
            const auto t2 = bigSigma0(a) + majority(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{
        0x6a09e667UL,
        0xbb67ae85UL,
        0x3c6ef372UL,
        0xa54ff53aUL,
        0x510e527fUL,
        0x9b05688cUL,
        0x1f83d9abUL,
        0x5be0cd19UL};
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t bufferSize_ = 0;
    std::size_t totalBytes_ = 0;
};

std::string toHex(const std::array<std::uint8_t, 32>& digest) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        stream << std::setw(2) << static_cast<int>(byte);
    }
    return stream.str();
}

} // namespace

std::string sha256Bytes(std::string_view data) {
    Sha256 sha;
    sha.update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return toHex(sha.final());
}

Result<std::string> sha256File(const std::filesystem::path& path) noexcept {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return Result<std::string>::fail({ErrorCode::FileSystemError, "Failed to open file for SHA-256"});
        }

        Sha256 sha;
        std::array<char, 64 * 1024> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count > 0) {
                sha.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(count));
            }
        }

        return Result<std::string>::ok(toHex(sha.final()));
    } catch (...) {
        return Result<std::string>::fail({ErrorCode::FileSystemError, "Failed to hash file"});
    }
}

} // namespace autoupdater::util

