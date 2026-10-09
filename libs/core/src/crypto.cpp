#include "robot_pm/crypto.hpp"

#include <optional>
#include <vector>

namespace robot_pm {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Round{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr std::array<std::uint8_t, 256> kSbox{
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
        0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
        0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
        0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
        0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
        0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
        0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
        0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
        0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
        0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
        0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
        0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
        0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
        0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
        0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
        0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

constexpr std::array<std::uint8_t, 11> kRcon{0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

[[nodiscard]] std::uint32_t rotate_right(std::uint32_t value, std::uint32_t bits) {
    return (value >> bits) | (value << (32U - bits));
}

void sha256_block(std::array<std::uint32_t, 8>& state, const std::uint8_t block[64]) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
        words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24U) |
                       (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16U) |
                       (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8U) |
                       static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < 64; ++index) {
        const std::uint32_t small = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^
                                    (words[index - 15] >> 3U);
        const std::uint32_t large = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^
                                    (words[index - 2] >> 10U);
        words[index] = words[index - 16] + small + words[index - 7] + large;
    }
    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];
    for (std::size_t index = 0; index < 64; ++index) {
        const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choose = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + choose + kSha256Round[index] + words[index];
        const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

[[nodiscard]] std::uint8_t mul(std::uint8_t left, std::uint8_t right) {
    std::uint8_t product = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if ((right & 1U) != 0) {
            product = static_cast<std::uint8_t>(product ^ left);
        }
        const bool high = (left & 0x80U) != 0;
        left = static_cast<std::uint8_t>(left << 1U);
        if (high) {
            left = static_cast<std::uint8_t>(left ^ 0x1bU);
        }
        right = static_cast<std::uint8_t>(right >> 1U);
    }
    return product;
}

[[nodiscard]] std::array<std::uint8_t, 256> inverse_sbox() {
    std::array<std::uint8_t, 256> inverse{};
    for (int index = 0; index < 256; ++index) {
        inverse[kSbox[static_cast<std::size_t>(index)]] = static_cast<std::uint8_t>(index);
    }
    return inverse;
}

void expand_key(const std::uint8_t key[32], std::uint8_t round_key[240]) {
    for (int index = 0; index < 32; ++index) {
        round_key[index] = key[index];
    }
    int produced = 32;
    int rcon_index = 1;
    std::uint8_t temp[4];
    while (produced < 240) {
        for (int index = 0; index < 4; ++index) {
            temp[index] = round_key[produced - 4 + index];
        }
        if (produced % 32 == 0) {
            const std::uint8_t first = temp[0];
            temp[0] = temp[1];
            temp[1] = temp[2];
            temp[2] = temp[3];
            temp[3] = first;
            for (int index = 0; index < 4; ++index) {
                temp[index] = kSbox[temp[index]];
            }
            temp[0] = static_cast<std::uint8_t>(temp[0] ^ kRcon[static_cast<std::size_t>(rcon_index)]);
            ++rcon_index;
        } else if (produced % 32 == 16) {
            for (int index = 0; index < 4; ++index) {
                temp[index] = kSbox[temp[index]];
            }
        }
        for (int index = 0; index < 4; ++index) {
            round_key[produced] = static_cast<std::uint8_t>(round_key[produced - 32] ^ temp[index]);
            ++produced;
        }
    }
}

void add_round_key(std::uint8_t state[16], const std::uint8_t* round_key) {
    for (int index = 0; index < 16; ++index) {
        state[index] = static_cast<std::uint8_t>(state[index] ^ round_key[index]);
    }
}

void sub_bytes(std::uint8_t state[16]) {
    for (int index = 0; index < 16; ++index) {
        state[index] = kSbox[state[index]];
    }
}

void inv_sub_bytes(std::uint8_t state[16], const std::array<std::uint8_t, 256>& inverse) {
    for (int index = 0; index < 16; ++index) {
        state[index] = inverse[state[index]];
    }
}

void shift_rows(std::uint8_t state[16]) {
    std::uint8_t saved = state[1];
    state[1] = state[5];
    state[5] = state[9];
    state[9] = state[13];
    state[13] = saved;
    saved = state[2];
    state[2] = state[10];
    state[10] = saved;
    saved = state[6];
    state[6] = state[14];
    state[14] = saved;
    saved = state[3];
    state[3] = state[15];
    state[15] = state[11];
    state[11] = state[7];
    state[7] = saved;
}

void inv_shift_rows(std::uint8_t state[16]) {
    std::uint8_t saved = state[13];
    state[13] = state[9];
    state[9] = state[5];
    state[5] = state[1];
    state[1] = saved;
    saved = state[2];
    state[2] = state[10];
    state[10] = saved;
    saved = state[6];
    state[6] = state[14];
    state[14] = saved;
    saved = state[3];
    state[3] = state[7];
    state[7] = state[11];
    state[11] = state[15];
    state[15] = saved;
}

void mix_columns(std::uint8_t state[16]) {
    for (int column = 0; column < 4; ++column) {
        const std::uint8_t a = state[column * 4];
        const std::uint8_t b = state[column * 4 + 1];
        const std::uint8_t c = state[column * 4 + 2];
        const std::uint8_t d = state[column * 4 + 3];
        state[column * 4] = static_cast<std::uint8_t>(mul(a, 2) ^ mul(b, 3) ^ c ^ d);
        state[column * 4 + 1] = static_cast<std::uint8_t>(a ^ mul(b, 2) ^ mul(c, 3) ^ d);
        state[column * 4 + 2] = static_cast<std::uint8_t>(a ^ b ^ mul(c, 2) ^ mul(d, 3));
        state[column * 4 + 3] = static_cast<std::uint8_t>(mul(a, 3) ^ b ^ c ^ mul(d, 2));
    }
}

void inv_mix_columns(std::uint8_t state[16]) {
    for (int column = 0; column < 4; ++column) {
        const std::uint8_t a = state[column * 4];
        const std::uint8_t b = state[column * 4 + 1];
        const std::uint8_t c = state[column * 4 + 2];
        const std::uint8_t d = state[column * 4 + 3];
        state[column * 4] = static_cast<std::uint8_t>(mul(a, 14) ^ mul(b, 11) ^ mul(c, 13) ^ mul(d, 9));
        state[column * 4 + 1] = static_cast<std::uint8_t>(mul(a, 9) ^ mul(b, 14) ^ mul(c, 11) ^ mul(d, 13));
        state[column * 4 + 2] = static_cast<std::uint8_t>(mul(a, 13) ^ mul(b, 9) ^ mul(c, 14) ^ mul(d, 11));
        state[column * 4 + 3] = static_cast<std::uint8_t>(mul(a, 11) ^ mul(b, 13) ^ mul(c, 9) ^ mul(d, 14));
    }
}

void encrypt_block(std::uint8_t state[16], const std::uint8_t round_key[240]) {
    add_round_key(state, round_key);
    for (int round = 1; round < 14; ++round) {
        sub_bytes(state);
        shift_rows(state);
        mix_columns(state);
        add_round_key(state, round_key + round * 16);
    }
    sub_bytes(state);
    shift_rows(state);
    add_round_key(state, round_key + 14 * 16);
}

void decrypt_block(std::uint8_t state[16], const std::uint8_t round_key[240]) {
    const std::array<std::uint8_t, 256> inverse = inverse_sbox();
    add_round_key(state, round_key + 14 * 16);
    for (int round = 13; round >= 1; --round) {
        inv_shift_rows(state);
        inv_sub_bytes(state, inverse);
        add_round_key(state, round_key + round * 16);
        inv_mix_columns(state);
    }
    inv_shift_rows(state);
    inv_sub_bytes(state, inverse);
    add_round_key(state, round_key);
}

[[nodiscard]] int base64_value(char character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

[[nodiscard]] std::string base64_encode(const std::uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (std::size_t index = 0; index < size; index += 3) {
        const unsigned group = (static_cast<unsigned>(data[index]) << 16U) |
                               ((index + 1 < size ? static_cast<unsigned>(data[index + 1]) : 0U) << 8U) |
                               (index + 2 < size ? static_cast<unsigned>(data[index + 2]) : 0U);
        out.push_back(kDigits[(group >> 18U) & 63U]);
        out.push_back(kDigits[(group >> 12U) & 63U]);
        out.push_back(index + 1 < size ? kDigits[(group >> 6U) & 63U] : '=');
        out.push_back(index + 2 < size ? kDigits[group & 63U] : '=');
    }
    return out;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    int value = 0;
    int bits = 0;
    for (const char character : text) {
        if (character == '=' || character == '\n' || character == '\r') {
            continue;
        }
        const int digit = base64_value(character);
        if (digit < 0) {
            return std::nullopt;
        }
        value = (value << 6) | digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
        }
    }
    return out;
}

[[nodiscard]] bool pkcs7_unpad(std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return false;
    }
    const std::uint8_t count = bytes.back();
    if (count == 0 || count > 16 || count > bytes.size()) {
        return false;
    }
    for (std::size_t index = bytes.size() - count; index < bytes.size(); ++index) {
        if (bytes[index] != count) {
            return false;
        }
    }
    bytes.resize(bytes.size() - count);
    return true;
}

}  // namespace

std::array<std::uint8_t, 32> sha256_bytes(std::string_view input) {
    std::array<std::uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> padded(input.begin(), input.end());
    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
    padded.push_back(0x80);
    while ((padded.size() % 64) != 56) {
        padded.push_back(0);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>((bit_length >> static_cast<unsigned>(shift)) & 0xffU));
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        sha256_block(state, padded.data() + offset);
    }
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state.size(); ++index) {
        digest[index * 4] = static_cast<std::uint8_t>((state[index] >> 24U) & 0xffU);
        digest[index * 4 + 1] = static_cast<std::uint8_t>((state[index] >> 16U) & 0xffU);
        digest[index * 4 + 2] = static_cast<std::uint8_t>((state[index] >> 8U) & 0xffU);
        digest[index * 4 + 3] = static_cast<std::uint8_t>(state[index] & 0xffU);
    }
    return digest;
}

std::string sha256_hex(std::string_view input) {
    const std::array<std::uint8_t, 32> digest = sha256_bytes(input);
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        text[index * 2] = kDigits[digest[index] >> 4U];
        text[index * 2 + 1] = kDigits[digest[index] & 0x0fU];
    }
    return text;
}

std::array<std::uint8_t, 16> aes256_encrypt_block(const std::uint8_t key[32], const std::uint8_t block[16]) {
    std::uint8_t round_key[240];
    expand_key(key, round_key);
    std::array<std::uint8_t, 16> state{};
    for (int index = 0; index < 16; ++index) {
        state[static_cast<std::size_t>(index)] = block[index];
    }
    encrypt_block(state.data(), round_key);
    return state;
}

std::string encrypt_feishu_payload(std::string_view encrypt_key, std::string_view plain) {
    const std::array<std::uint8_t, 32> key = sha256_bytes(encrypt_key);
    std::uint8_t round_key[240];
    expand_key(key.data(), round_key);
    std::array<std::uint8_t, 16> iv{};
    for (std::size_t index = 0; index < iv.size(); ++index) {
        iv[index] = static_cast<std::uint8_t>(0x30 + (index % 10));
    }
    std::vector<std::uint8_t> padded(plain.begin(), plain.end());
    const std::uint8_t pad = static_cast<std::uint8_t>(16 - (padded.size() % 16));
    padded.insert(padded.end(), pad, pad);
    std::vector<std::uint8_t> out(iv.begin(), iv.end());
    std::uint8_t previous[16];
    for (int index = 0; index < 16; ++index) {
        previous[index] = iv[static_cast<std::size_t>(index)];
    }
    for (std::size_t offset = 0; offset < padded.size(); offset += 16) {
        std::uint8_t block[16];
        for (int index = 0; index < 16; ++index) {
            block[index] = static_cast<std::uint8_t>(padded[offset + static_cast<std::size_t>(index)] ^ previous[index]);
        }
        encrypt_block(block, round_key);
        out.insert(out.end(), block, block + 16);
        for (int index = 0; index < 16; ++index) {
            previous[index] = block[index];
        }
    }
    return base64_encode(out.data(), out.size());
}

std::expected<std::string, Error> decrypt_feishu_payload(std::string_view encrypt_key, std::string_view cipher_b64) {
    const std::optional<std::vector<std::uint8_t>> decoded = base64_decode(cipher_b64);
    if (!decoded || decoded->size() < 32 || (decoded->size() % 16) != 0) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "加密正文解不开"});
    }
    const std::array<std::uint8_t, 32> key = sha256_bytes(encrypt_key);
    std::uint8_t round_key[240];
    expand_key(key.data(), round_key);
    std::vector<std::uint8_t> plain;
    plain.reserve(decoded->size() - 16);
    std::uint8_t previous[16];
    for (int index = 0; index < 16; ++index) {
        previous[index] = (*decoded)[static_cast<std::size_t>(index)];
    }
    for (std::size_t offset = 16; offset < decoded->size(); offset += 16) {
        std::uint8_t block[16];
        for (int index = 0; index < 16; ++index) {
            block[index] = (*decoded)[offset + static_cast<std::size_t>(index)];
        }
        std::uint8_t cipher_copy[16];
        for (int index = 0; index < 16; ++index) {
            cipher_copy[index] = block[index];
        }
        decrypt_block(block, round_key);
        for (int index = 0; index < 16; ++index) {
            plain.push_back(static_cast<std::uint8_t>(block[index] ^ previous[index]));
            previous[index] = cipher_copy[index];
        }
    }
    if (!pkcs7_unpad(plain)) {
        return std::unexpected(Error{ErrorCode::kEditRejected, "加密正文解不开"});
    }
    return std::string(plain.begin(), plain.end());
}

}  // namespace robot_pm
