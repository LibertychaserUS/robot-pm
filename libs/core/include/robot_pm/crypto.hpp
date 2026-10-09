#pragma once

// 这个文件负责飞书回调用的 SHA-256 和 AES-256-CBC。
// 不变量：验签用原文的字节；解密只在验签通过之后。
// 密钥是加密口令的 SHA-256，不是口令本身。

#include "robot_pm/error.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace robot_pm {

[[nodiscard]] std::array<std::uint8_t, 32> sha256_bytes(std::string_view input);

[[nodiscard]] std::string sha256_hex(std::string_view input);

// 前置条件：key 是 32 字节，in 是 16 字节。
// 失败：不失败。结果是一个 AES-256 分组。
[[nodiscard]] std::array<std::uint8_t, 16> aes256_encrypt_block(const std::uint8_t key[32],
                                                               const std::uint8_t block[16]);

// 前置条件：encrypt_key 是飞书加密口令。plain 是解密后的 JSON 文本。
// 失败：不失败。结果是放进 encrypt 字段的 base64。
[[nodiscard]] std::string encrypt_feishu_payload(std::string_view encrypt_key, std::string_view plain);

// 前置条件：cipher_b64 是回调里的 encrypt 字段，验签已经通过。
// 失败：kEditRejected，填充、长度或 base64 不对。
[[nodiscard]] std::expected<std::string, Error> decrypt_feishu_payload(std::string_view encrypt_key,
                                                                       std::string_view cipher_b64);

}  // namespace robot_pm
