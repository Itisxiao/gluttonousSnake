#include "websocket.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <vector>

namespace {

constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

uint32_t leftRotate(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<uint8_t, 20> sha1(const std::string& input) {
    std::vector<uint8_t> data(input.begin(), input.end());
    const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xefcdab89;
    uint32_t h2 = 0x98badcfe;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xc3d2e1f0;

    for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
        std::array<uint32_t, 80> w{};
        for (int i = 0; i < 16; ++i) {
            const size_t j = chunk + static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(data[j]) << 24) |
                   (static_cast<uint32_t>(data[j + 1]) << 16) |
                   (static_cast<uint32_t>(data[j + 2]) << 8) |
                   static_cast<uint32_t>(data[j + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = leftRotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t temp = leftRotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = leftRotate(b, 30);
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
    const std::array<uint32_t, 5> words{h0, h1, h2, h3, h4};
    for (size_t i = 0; i < words.size(); ++i) {
        digest[i * 4] = static_cast<uint8_t>((words[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>(words[i] & 0xff);
    }
    return digest;
}

std::string base64(const uint8_t* data, size_t len) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(table[(triple >> 18) & 0x3f]);
        out.push_back(table[(triple >> 12) & 0x3f]);
        out.push_back((i + 1 < len) ? table[(triple >> 6) & 0x3f] : '=');
        out.push_back((i + 2 < len) ? table[triple & 0x3f] : '=');
    }
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(s.begin());
    }
    return s;
}

}  // namespace

std::string websocketAccept(const std::string& key) {
    const auto digest = sha1(key + kWsGuid);
    return base64(digest.data(), digest.size());
}

std::optional<std::string> headerValue(const std::string& request, const std::string& name) {
    std::istringstream stream(request);
    std::string line;
    const std::string prefix = name + ":";
    while (std::getline(stream, line)) {
        if (line.size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), line.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return trim(line.substr(prefix.size()));
        }
    }
    return std::nullopt;
}

std::string makeFrame(const std::string& payload, uint8_t opcode) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(126);
        // 解释：先右移 8 位，把高位的 0x12 移到低位，然后取出来。所以先 push 进去的是 0x12。
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        // 解释：直接取低 8 位，也就是 0x34。后 push 进去的是 0x34。
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((payload.size() >> (i * 8)) & 0xff));
        }
    }
    frame += payload;
    return frame;
}

std::optional<WsMessage> parseFrame(std::string& buffer) {
    if (buffer.size() < 2) {
        return std::nullopt;
    }
    const auto b0 = static_cast<uint8_t>(buffer[0]);
    const auto b1 = static_cast<uint8_t>(buffer[1]);
    const uint8_t opcode = b0 & 0x0f;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7f;
    size_t offset = 2;
    if (len == 126) {
        if (buffer.size() < offset + 2) {
            return std::nullopt;
        }
        len = (static_cast<uint8_t>(buffer[offset]) << 8) | static_cast<uint8_t>(buffer[offset + 1]);
        offset += 2;
    } else if (len == 127) {
        if (buffer.size() < offset + 8) {
            return std::nullopt;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<uint8_t>(buffer[offset + i]);
        }
        offset += 8;
    }
    if (!masked || len > 1024 * 1024) {
        buffer.clear();
        return WsMessage{0x8, ""};
    }
    if (buffer.size() < offset + 4 + len) {
        return std::nullopt;
    }
    std::array<uint8_t, 4> mask{};
    for (int i = 0; i < 4; ++i) {
        mask[i] = static_cast<uint8_t>(buffer[offset + i]);
    }
    offset += 4;
    std::string payload;
    payload.reserve(static_cast<size_t>(len));
    for (uint64_t i = 0; i < len; ++i) {
        payload.push_back(static_cast<char>(static_cast<uint8_t>(buffer[offset + i]) ^ mask[i % 4]));
    }
    buffer.erase(0, offset + static_cast<size_t>(len));
    return WsMessage{opcode, payload};
}
