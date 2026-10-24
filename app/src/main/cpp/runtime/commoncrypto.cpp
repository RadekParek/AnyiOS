#include "commoncrypto.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "abi.h"
#include "error.h"

namespace podnative::runtime {
namespace {
constexpr int32_t CCSuccess = 0;
constexpr int32_t CCParamError = -4300;
constexpr int32_t CCBufferTooSmall = -4301;
constexpr int32_t CCAlignmentError = -4303;
constexpr int32_t CCDecodeError = -4304;
constexpr int32_t CCUnimplemented = -4305;
constexpr size_t MaxCryptoBytes = 64 * 1024 * 1024;
constexpr uint32_t CCAlgorithmAES = 0;
constexpr uint32_t CCOptionPKCS7Padding = 1;
constexpr uint32_t CCOptionECBMode = 2;

uint32_t rotateLeft(uint32_t value, unsigned amount) {
    return (value << amount) | (value >> (32 - amount));
}

void requireGuestRange(const Memory& memory, uint32_t address, size_t size, uint8_t permission,
                       const char* description) {
    if (size == 0) return;
    if (!address || !memory.permits(address, size, permission))
        throw Error("CC_ARGUMENT", std::string(description) + " is not a valid guest buffer",
                    address);
}
std::vector<uint8_t> copyGuestBytes(Memory& memory, uint32_t address, uint32_t length,
                                    const char* description) {
    if (length > MaxCryptoBytes)
        throw Error("CC_LIMIT", std::string(description) + " exceeds the 64 MiB limit", length);
    requireGuestRange(memory, address, length, Read, description);
    std::vector<uint8_t> bytes(length);
    if (length) memory.read(address, bytes.data(), length);
    return bytes;
}

std::array<uint8_t, 16> md5(const uint8_t* data, size_t size) {
    static constexpr uint32_t constants[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static constexpr unsigned shifts[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    const uint64_t bitLength = uint64_t(size) * 8;
    const size_t paddedSize = ((size + 8) / 64 + 1) * 64;
    std::vector<uint8_t> padded(paddedSize, 0);
    if (size) std::memcpy(padded.data(), data, size);
    padded[size] = 0x80;
    for (unsigned n = 0; n < 8; n++) padded[paddedSize - 8 + n] = uint8_t(bitLength >> (n * 8));

    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476;
    for (size_t offset = 0; offset < padded.size(); offset += 64) {
        uint32_t words[16];
        for (unsigned n = 0; n < 16; n++) {
            const uint8_t* p = padded.data() + offset + n * 4;
            words[n] = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
                       uint32_t(p[3]) << 24;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (unsigned n = 0; n < 64; n++) {
            uint32_t f, g;
            if (n < 16) {
                f = (b & c) | (~b & d);
                g = n;
            } else if (n < 32) {
                f = (d & b) | (~d & c);
                g = (5 * n + 1) & 15;
            } else if (n < 48) {
                f = b ^ c ^ d;
                g = (3 * n + 5) & 15;
            } else {
                f = c ^ (b | ~d);
                g = (7 * n) & 15;
            }
            const uint32_t next = b + rotateLeft(a + f + constants[n] + words[g], shifts[n]);
            a = d;
            d = c;
            c = b;
            b = next;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
    }
    std::array<uint8_t, 16> digest{};
    const uint32_t words[4] = {h0, h1, h2, h3};
    for (unsigned n = 0; n < 4; n++)
        for (unsigned byte = 0; byte < 4; byte++)
            digest[n * 4 + byte] = uint8_t(words[n] >> (byte * 8));
    return digest;
}

std::array<uint8_t, 20> sha1(const uint8_t* data, size_t size) {
    const uint64_t bitLength = uint64_t(size) * 8;
    const size_t paddedSize = ((size + 8) / 64 + 1) * 64;
    std::vector<uint8_t> padded(paddedSize, 0);
    if (size) std::memcpy(padded.data(), data, size);
    padded[size] = 0x80;
    for (unsigned n = 0; n < 8; n++)
        padded[paddedSize - 1 - n] = uint8_t(bitLength >> (n * 8));

    uint32_t h0 = 0x67452301, h1 = 0xefcdab89, h2 = 0x98badcfe, h3 = 0x10325476,
             h4 = 0xc3d2e1f0;
    for (size_t offset = 0; offset < padded.size(); offset += 64) {
        uint32_t words[80];
        for (unsigned n = 0; n < 16; n++) {
            const uint8_t* p = padded.data() + offset + n * 4;
            words[n] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }
        for (unsigned n = 16; n < 80; n++)
            words[n] = rotateLeft(words[n - 3] ^ words[n - 8] ^ words[n - 14] ^ words[n - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (unsigned n = 0; n < 80; n++) {
            uint32_t f, k;
            if (n < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (n < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (n < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t temp = rotateLeft(a, 5) + f + e + k + words[n];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    std::array<uint8_t, 20> digest{};
    const uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (unsigned n = 0; n < 5; n++)
        for (unsigned byte = 0; byte < 4; byte++)
            digest[n * 4 + byte] = uint8_t(words[n] >> (24 - byte * 8));
    return digest;
}

uint8_t gfMultiply(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    for (unsigned n = 0; n < 8; n++) {
        if (b & 1) result ^= a;
        const bool high = (a & 0x80) != 0;
        a = uint8_t(a << 1);
        if (high) a ^= 0x1b;
        b = uint8_t(b >> 1);
    }
    return result;
}
uint8_t gfPower(uint8_t value, unsigned power) {
    uint8_t result = 1;
    while (power) {
        if (power & 1) result = gfMultiply(result, value);
        value = gfMultiply(value, value);
        power >>= 1;
    }
    return result;
}
uint8_t rotateByte(uint8_t value, unsigned amount) {
    return uint8_t((uint32_t(value) << amount) | (uint32_t(value) >> (8 - amount)));
}
struct AesTables {
    std::array<uint8_t, 256> forward{}, inverse{};
    std::array<std::array<uint8_t, 256>, 15> multiply{};
    AesTables() {
        for (unsigned n = 0; n < 256; n++) {
            const uint8_t x = uint8_t(n);
            const uint8_t inv = x ? gfPower(x, 254) : 0;
            const uint8_t s = uint8_t(inv ^ rotateByte(inv, 1) ^ rotateByte(inv, 2) ^
                                      rotateByte(inv, 3) ^ rotateByte(inv, 4) ^ 0x63);
            forward[n] = s;
            inverse[s] = x;
            for (unsigned coefficient = 0; coefficient < multiply.size(); coefficient++)
                multiply[coefficient][n] = gfMultiply(uint8_t(n), uint8_t(coefficient));
        }
    }
};
const AesTables& aesTables() {
    static const AesTables tables;
    return tables;
}
struct AesKey {
    std::vector<uint8_t> rounds;
    unsigned roundCount = 0;
};
AesKey expandKey(const std::vector<uint8_t>& key) {
    const AesTables& tables = aesTables();
    const unsigned keyWords = unsigned(key.size() / 4);
    const unsigned roundCount = keyWords + 6;
    const unsigned wordCount = 4 * (roundCount + 1);
    AesKey expanded;
    expanded.roundCount = roundCount;
    expanded.rounds.resize(wordCount * 4);
    std::copy(key.begin(), key.end(), expanded.rounds.begin());
    uint8_t rcon = 1;
    for (unsigned word = keyWords; word < wordCount; word++) {
        uint8_t temp[4];
        std::copy_n(expanded.rounds.data() + (word - 1) * 4, 4, temp);
        if (word % keyWords == 0) {
            const uint8_t first = temp[0];
            temp[0] = tables.forward[temp[1]];
            temp[1] = tables.forward[temp[2]];
            temp[2] = tables.forward[temp[3]];
            temp[3] = tables.forward[first];
            temp[0] ^= rcon;
            rcon = gfMultiply(rcon, 2);
        } else if (keyWords > 6 && word % keyWords == 4) {
            for (uint8_t& byte : temp) byte = tables.forward[byte];
        }
        for (unsigned byte = 0; byte < 4; byte++)
            expanded.rounds[word * 4 + byte] =
                expanded.rounds[(word - keyWords) * 4 + byte] ^ temp[byte];
    }
    return expanded;
}
void addRoundKey(std::array<uint8_t, 16>& state, const AesKey& key, unsigned round) {
    for (unsigned n = 0; n < 16; n++) state[n] ^= key.rounds[round * 16 + n];
}
void substitute(std::array<uint8_t, 16>& state, bool inverse) {
    const AesTables& tables = aesTables();
    const auto& box = inverse ? tables.inverse : tables.forward;
    for (uint8_t& byte : state) byte = box[byte];
}
void shiftRows(std::array<uint8_t, 16>& state, bool inverse) {
    const auto original = state;
    for (unsigned row = 1; row < 4; row++)
        for (unsigned column = 0; column < 4; column++) {
            const unsigned sourceColumn = inverse ? (column + 4 - row) % 4 : (column + row) % 4;
            state[column * 4 + row] = original[sourceColumn * 4 + row];
        }
}
void mixColumns(std::array<uint8_t, 16>& state, bool inverse) {
    const AesTables& tables = aesTables();
    for (unsigned column = 0; column < 4; column++) {
        uint8_t* s = state.data() + column * 4;
        const uint8_t a = s[0], b = s[1], c = s[2], d = s[3];
        if (!inverse) {
            s[0] = tables.multiply[2][a] ^ tables.multiply[3][b] ^ c ^ d;
            s[1] = a ^ tables.multiply[2][b] ^ tables.multiply[3][c] ^ d;
            s[2] = a ^ b ^ tables.multiply[2][c] ^ tables.multiply[3][d];
            s[3] = tables.multiply[3][a] ^ b ^ c ^ tables.multiply[2][d];
        } else {
            s[0] = tables.multiply[14][a] ^ tables.multiply[11][b] ^
                   tables.multiply[13][c] ^ tables.multiply[9][d];
            s[1] = tables.multiply[9][a] ^ tables.multiply[14][b] ^
                   tables.multiply[11][c] ^ tables.multiply[13][d];
            s[2] = tables.multiply[13][a] ^ tables.multiply[9][b] ^
                   tables.multiply[14][c] ^ tables.multiply[11][d];
            s[3] = tables.multiply[11][a] ^ tables.multiply[13][b] ^
                   tables.multiply[9][c] ^ tables.multiply[14][d];
        }
    }
}
void encryptBlock(std::array<uint8_t, 16>& state, const AesKey& key) {
    addRoundKey(state, key, 0);
    for (unsigned round = 1; round < key.roundCount; round++) {
        substitute(state, false);
        shiftRows(state, false);
        mixColumns(state, false);
        addRoundKey(state, key, round);
    }
    substitute(state, false);
    shiftRows(state, false);
    addRoundKey(state, key, key.roundCount);
}
void decryptBlock(std::array<uint8_t, 16>& state, const AesKey& key) {
    addRoundKey(state, key, key.roundCount);
    for (unsigned round = key.roundCount; round > 1; round--) {
        shiftRows(state, true);
        substitute(state, true);
        addRoundKey(state, key, round - 1);
        mixColumns(state, true);
    }
    shiftRows(state, true);
    substitute(state, true);
    addRoundKey(state, key, 0);
}

std::vector<uint8_t> transformAes(const std::vector<uint8_t>& input, const AesKey& key,
                                  bool encrypt, bool ecb, const std::array<uint8_t, 16>& iv) {
    std::vector<uint8_t> output(input.size());
    std::array<uint8_t, 16> previous = iv;
    for (size_t offset = 0; offset < input.size(); offset += 16) {
        std::array<uint8_t, 16> block{};
        std::copy_n(input.data() + offset, 16, block.data());
        if (encrypt) {
            if (!ecb)
                for (unsigned n = 0; n < 16; n++) block[n] ^= previous[n];
            encryptBlock(block, key);
            std::copy(block.begin(), block.end(), output.begin() + std::ptrdiff_t(offset));
            if (!ecb) previous = block;
        } else {
            const auto ciphertext = block;
            decryptBlock(block, key);
            if (!ecb)
                for (unsigned n = 0; n < 16; n++) block[n] ^= previous[n];
            std::copy(block.begin(), block.end(), output.begin() + std::ptrdiff_t(offset));
            if (!ecb) previous = ciphertext;
        }
    }
    return output;
}
}  // namespace

std::vector<std::string> CommonCrypto::symbols() const { return {"_CCCrypt", "_CC_MD5", "_CC_SHA1"}; }

bool CommonCrypto::invoke(const std::string& symbol, CpuState& cpu, Memory& memory) const {
    if (symbol == "_CC_MD5" || symbol == "_CC_SHA1") {
        Arguments args(cpu, memory);
        const uint32_t inputAddress = args.pointer();
        const uint32_t inputLength = args.word();
        const uint32_t outputAddress = args.pointer();
        const std::vector<uint8_t> input =
            copyGuestBytes(memory, inputAddress, inputLength, "digest input");
        if (!outputAddress)
            throw Error("CC_ARGUMENT", "digest output is a null guest pointer");
        if (symbol == "_CC_MD5") {
            const auto digest = md5(input.data(), input.size());
            requireGuestRange(memory, outputAddress, digest.size(), Write, "MD5 digest output");
            memory.write(outputAddress, digest.data(), digest.size());
        } else {
            const auto digest = sha1(input.data(), input.size());
            requireGuestRange(memory, outputAddress, digest.size(), Write, "SHA-1 digest output");
            memory.write(outputAddress, digest.data(), digest.size());
        }
        cpu.registers[0] = outputAddress;
        return true;
    }
    if (symbol != "_CCCrypt") return false;

    Arguments args(cpu, memory);
    const uint32_t operation = args.word();
    const uint32_t algorithm = args.word();
    const uint32_t options = args.word();
    const uint32_t keyAddress = args.pointer();
    const uint32_t keyLength = args.word();
    const uint32_t ivAddress = args.pointer();
    const uint32_t inputAddress = args.pointer();
    const uint32_t inputLength = args.word();
    const uint32_t outputAddress = args.pointer();
    const uint32_t outputCapacity = args.word();
    const uint32_t movedAddress = args.pointer();
    auto finish = [&](int32_t status, uint32_t moved = 0) {
        if (movedAddress) {
            requireGuestRange(memory, movedAddress, 4, Write, "CCCrypt output-length result");
            memory.write32(movedAddress, moved);
        }
        cpu.registers[0] = uint32_t(status);
    };
    if (operation > 1 || (options & ~(CCOptionPKCS7Padding | CCOptionECBMode)) != 0) {
        finish(CCParamError);
        return true;
    }
    if (algorithm != CCAlgorithmAES) {
        finish(CCUnimplemented);
        return true;
    }
    if (keyLength != 16 && keyLength != 24 && keyLength != 32) {
        finish(CCParamError);
        return true;
    }
    if (inputLength > MaxCryptoBytes || outputCapacity > MaxCryptoBytes) {
        finish(CCParamError);
        return true;
    }
    const bool encrypt = operation == 0;
    const bool usePadding = (options & CCOptionPKCS7Padding) != 0;
    const bool ecb = (options & CCOptionECBMode) != 0;
    std::vector<uint8_t> key = copyGuestBytes(memory, keyAddress, keyLength, "AES key");
    std::vector<uint8_t> input =
        copyGuestBytes(memory, inputAddress, inputLength, "AES input");
    if (!encrypt && (inputLength == 0 || inputLength % 16 != 0)) {
        finish(CCAlignmentError);
        return true;
    }
    if (!usePadding && inputLength % 16 != 0) {
        finish(CCAlignmentError);
        return true;
    }
    std::array<uint8_t, 16> iv{};
    if (!ecb && ivAddress) {
        requireGuestRange(memory, ivAddress, iv.size(), Read, "AES initialization vector");
        memory.read(ivAddress, iv.data(), iv.size());
    }
    if (encrypt && usePadding) {
        const uint8_t padding = uint8_t(16 - input.size() % 16);
        input.insert(input.end(), padding, padding);
    }
    const AesKey expanded = expandKey(key);
    std::vector<uint8_t> output = transformAes(input, expanded, encrypt, ecb, iv);
    if (!encrypt && usePadding) {
        const uint8_t padding = output.back();
        uint8_t bad = uint8_t(padding == 0 || padding > 16);
        for (unsigned n = 0; n < 16; n++) {
            const uint8_t mask = uint8_t(0u - uint8_t(n < padding));
            bad |= uint8_t((output[output.size() - 1 - n] ^ padding) & mask);
        }
        if (bad) {
            finish(CCDecodeError);
            return true;
        }
        output.resize(output.size() - padding);
    }
    if (output.size() > outputCapacity) {
        finish(CCBufferTooSmall);
        return true;
    }
    if (!output.empty()) {
        if (!outputAddress)
            throw Error("CC_ARGUMENT", "CCCrypt output is a null guest pointer");
        requireGuestRange(memory, outputAddress, output.size(), Write, "AES output");
        memory.write(outputAddress, output.data(), output.size());
    }
    finish(CCSuccess, uint32_t(output.size()));
    return true;
}
}  // namespace podnative::runtime
