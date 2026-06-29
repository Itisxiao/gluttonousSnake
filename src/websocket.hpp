#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct WsMessage {
    uint8_t opcode = 0;
    std::string payload;
};

std::string websocketAccept(const std::string& key);
std::optional<std::string> headerValue(const std::string& request, const std::string& name);
std::string makeFrame(const std::string& payload, uint8_t opcode = 0x1);
std::optional<WsMessage> parseFrame(std::string& buffer);
