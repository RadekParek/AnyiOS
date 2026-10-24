#include "runtime/commoncrypto.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace podnative::runtime;
namespace {
constexpr uint32_t Input = 0x1100;
constexpr uint32_t Key = 0x1200;
constexpr uint32_t Iv = 0x1300;
constexpr uint32_t Output = 0x1400;
constexpr uint32_t Moved = 0x1500;
constexpr uint32_t Stack = 0x1800;
void hex(Memory& memory, uint32_t address, const std::string& text) {
    assert(text.size() % 2 == 0);
    std::vector<uint8_t> data;
    for (size_t n = 0; n < text.size(); n += 2)
        data.push_back(uint8_t(std::stoul(text.substr(n, 2), nullptr, 16)));
    memory.write(address, data.data(), data.size());
}
std::vector<uint8_t> read(Memory& memory, uint32_t address, size_t size) {
    std::vector<uint8_t> result(size);
    memory.read(address, result.data(), size);
    return result;
}
CpuState crypt(CommonCrypto& crypto, Memory& memory, uint32_t op, uint32_t alg, uint32_t options,
               uint32_t keyAddress, uint32_t keyLength, uint32_t ivAddress, uint32_t inputAddress,
               uint32_t inputLength, uint32_t outputAddress, uint32_t outputCapacity,
               uint32_t movedAddress = Moved) {
    CpuState state;
    state.registers[0] = op;
    state.registers[1] = alg;
    state.registers[2] = options;
    state.registers[3] = keyAddress;
    state.registers[13] = Stack;
    const uint32_t stack[] = {keyLength,     ivAddress,      inputAddress, inputLength,
                              outputAddress, outputCapacity, movedAddress};
    for (size_t n = 0; n < 7; n++) memory.write32(Stack + uint32_t(n * 4), stack[n]);
    assert(crypto.invoke("_CCCrypt", state, memory));
    return state;
}
void checkEcbVector(CommonCrypto& crypto, Memory& memory, const std::string& key,
                    const std::string& expected) {
    hex(memory, Key, key);
    hex(memory, Input, "00112233445566778899aabbccddeeff");
    const size_t keyBytes = key.size() / 2;
    CpuState result =
        crypt(crypto, memory, 0, 0, 2, Key, uint32_t(keyBytes), 0, Input, 16, Output, 16);
    assert(int32_t(result.registers[0]) == 0 && memory.read32(Moved) == 16);
    std::vector<uint8_t> actual = read(memory, Output, 16);
    std::vector<uint8_t> wanted;
    for (size_t n = 0; n < expected.size(); n += 2)
        wanted.push_back(uint8_t(std::stoul(expected.substr(n, 2), nullptr, 16)));
    if (actual != wanted) {
        std::cerr << "AES ECB mismatch for " << key << ": ";
        for (uint8_t byte : actual) {
            static constexpr char digits[] = "0123456789abcdef";
            std::cerr << digits[byte >> 4] << digits[byte & 15];
        }
        std::cerr << " expected " << expected << std::endl;
        std::abort();
    }
}
}  // namespace

int main() {
    Memory memory;
    std::vector<uint8_t> page(4096, 0);
    memory.map({0x1000, uint32_t(page.size()), uint8_t(Read | Write), "CommonCrypto test buffers"},
               page.data(), page.size());
    CommonCrypto crypto;
    const auto names = crypto.symbols();
    assert(names.size() == 3);
    assert(std::find(names.begin(), names.end(), "_CCCrypt") != names.end());

    const std::string abc = "abc";
    memory.write(Input, abc.data(), abc.size());
    CpuState md5;
    md5.registers[0] = Input;
    md5.registers[1] = uint32_t(abc.size());
    md5.registers[2] = Output;
    assert(crypto.invoke("_CC_MD5", md5, memory));
    assert(md5.registers[0] == Output);
    assert(read(memory, Output, 16) ==
           std::vector<uint8_t>({0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f,
                                 0x7d, 0x28, 0xe1, 0x7f, 0x72}));
    CpuState sha1;
    sha1.registers[0] = Input;
    sha1.registers[1] = uint32_t(abc.size());
    sha1.registers[2] = Output;
    assert(crypto.invoke("_CC_SHA1", sha1, memory));
    assert(sha1.registers[0] == Output);
    assert(read(memory, Output, 20) ==
           std::vector<uint8_t>({0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                 0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d}));

    // NIST SP 800-38A AES-CBC vector.
    hex(memory, Key, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(memory, Iv, "000102030405060708090a0b0c0d0e0f");
    hex(memory, Input, "6bc1bee22e409f96e93d7e117393172a");
    CpuState encrypted = crypt(crypto, memory, 0, 0, 0, Key, 16, Iv, Input, 16, Output, 16);
    assert(int32_t(encrypted.registers[0]) == 0 && memory.read32(Moved) == 16);
    const std::vector<uint8_t> cbc = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46,
                                      0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};
    assert(read(memory, Output, 16) == cbc);
    std::vector<uint8_t> ciphertext = read(memory, Output, 16);
    memory.write(Input, ciphertext.data(), ciphertext.size());
    CpuState decrypted = crypt(crypto, memory, 1, 0, 0, Key, 16, Iv, Input, 16, Output, 16);
    assert(int32_t(decrypted.registers[0]) == 0 && memory.read32(Moved) == 16);
    assert(read(memory, Output, 16) ==
           std::vector<uint8_t>({0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e,
                                 0x11, 0x73, 0x93, 0x17, 0x2a}));

    // FIPS-197 known-answer vectors exercise all three AES key schedules and ECB mode.
    checkEcbVector(crypto, memory, "000102030405060708090a0b0c0d0e0f",
                   "69c4e0d86a7b0430d8cdb78070b4c55a");
    checkEcbVector(crypto, memory, "000102030405060708090a0b0c0d0e0f1011121314151617",
                   "dda97ca4864cdfe06eaf70a0ec0d7191");
    checkEcbVector(crypto, memory,
                   "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
                   "8ea2b7ca516745bfeafc49904b496089");

    // PKCS#7 adds a complete block for already aligned input and strips it on decrypt.
    hex(memory, Key, "2b7e151628aed2a6abf7158809cf4f3c");
    memory.write(Input, abc.data(), abc.size());
    CpuState padded = crypt(crypto, memory, 0, 0, 1, Key, 16, Iv, Input, 3, Output, 16);
    assert(int32_t(padded.registers[0]) == 0 && memory.read32(Moved) == 16);
    std::vector<uint8_t> paddedCipher = read(memory, Output, 16);
    memory.write(Input, paddedCipher.data(), paddedCipher.size());
    CpuState unpadded = crypt(crypto, memory, 1, 0, 1, Key, 16, Iv, Input, 16, Output, 16);
    assert(int32_t(unpadded.registers[0]) == 0 && memory.read32(Moved) == 3);
    assert(read(memory, Output, 3) == std::vector<uint8_t>({'a', 'b', 'c'}));

    hex(memory, Key, "000102030405060708090a0b0c0d0e0f");
    hex(memory, Input, "69c4e0d86a7b0430d8cdb78070b4c55a");
    CpuState badPadding = crypt(crypto, memory, 1, 0, 1 | 2, Key, 16, 0, Input, 16, Output, 16);
    assert(int32_t(badPadding.registers[0]) == -4304 && memory.read32(Moved) == 0);

    memory.write32(Moved, 0xfeedface);
    hex(memory, Input, "00112233445566778899aabbccddeeff");
    CpuState misaligned = crypt(crypto, memory, 0, 0, 0, Key, 16, 0, Input, 15, Output, 32);
    assert(int32_t(misaligned.registers[0]) == -4303 && memory.read32(Moved) == 0);
    CpuState tooSmall = crypt(crypto, memory, 0, 0, 2, Key, 16, 0, Input, 16, Output, 15);
    assert(int32_t(tooSmall.registers[0]) == -4301 && memory.read32(Moved) == 0);
    CpuState unsupported = crypt(crypto, memory, 0, 1, 0, Key, 16, 0, Input, 16, Output, 16);
    assert(int32_t(unsupported.registers[0]) == -4305 && memory.read32(Moved) == 0);

    std::cout << "CommonCrypto MD5, SHA-1, AES CBC/ECB, padding, and error tests passed\n";
}
