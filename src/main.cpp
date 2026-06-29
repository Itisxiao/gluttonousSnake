#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBoardWidth = 40;
constexpr int kBoardHeight = 30;
constexpr int kTickMs = 150;
constexpr int kMaxPlayersPerRoom = 8;
constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct Position {
    int x = 0;
    int y = 0;
};

struct Player {
    int clientFd = -1;
    int id = 0;
    std::string name;
    std::deque<Position> snake;
    std::string dir = "right";
    std::string nextDir = "right";
    bool alive = true;
    int score = 0;
};

struct Room {
    std::string id;
    std::map<int, Player> players;
    Position food{10, 10};
    bool running = true;
    Clock::time_point lastTick = Clock::now();
};

struct Client {
    int fd = -1;
    bool handshaken = false;
    std::string input;
    std::string roomId;
    int playerId = 0;
};

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

std::string websocketAccept(const std::string& key) {
    const auto digest = sha1(key + kWsGuid);
    return base64(digest.data(), digest.size());
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
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

std::string makeFrame(const std::string& payload, uint8_t opcode = 0x1) {
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 65535) {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
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

struct WsMessage {
    uint8_t opcode = 0;
    std::string payload;
};

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

std::string escapeJson(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::optional<std::string> jsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::string out;
    bool escaped = false;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        const char ch = json[i];
        if (escaped) {
            out.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            return out;
        } else {
            out.push_back(ch);
        }
    }
    return std::nullopt;
}

bool isOpposite(const std::string& a, const std::string& b) {
    return (a == "up" && b == "down") || (a == "down" && b == "up") ||
           (a == "left" && b == "right") || (a == "right" && b == "left");
}

Position step(Position p, const std::string& dir) {
    if (dir == "up") {
        --p.y;
    } else if (dir == "down") {
        ++p.y;
    } else if (dir == "left") {
        --p.x;
    } else {
        ++p.x;
    }
    return p;
}

class SnakeServer {
public:
    explicit SnakeServer(uint16_t port) : port_(port), rng_(std::random_device{}()) {}

    bool start() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            perror("socket");
            return false;
        }

        int yes = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port_);
        if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            perror("bind");
            return false;
        }
        if (listen(listenFd_, SOMAXCONN) != 0) {
            perror("listen");
            return false;
        }
        setNonBlocking(listenFd_);
        std::cout << "snake_server listening on ws://127.0.0.1:" << port_ << "\n";
        return true;
    }

    void run() {
        while (true) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listenFd_, &readSet);
            int maxFd = listenFd_;
            for (const auto& [fd, _] : clients_) {
                FD_SET(fd, &readSet);
                maxFd = std::max(maxFd, fd);
            }

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 50 * 1000;
            const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("select");
                break;
            }

            if (FD_ISSET(listenFd_, &readSet)) {
                acceptClient();
            }

            std::vector<int> readable;
            for (const auto& [fd, _] : clients_) {
                if (FD_ISSET(fd, &readSet)) {
                    readable.push_back(fd);
                }
            }
            for (int fd : readable) {
                readClient(fd);
            }

            tickRooms();
        }
    }

private:
    void acceptClient() {
        while (true) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            const int fd = accept(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
            if (fd < 0) {
                break;
            }
            setNonBlocking(fd);
            Client client;
            client.fd = fd;
            clients_[fd] = client;
        }
    }

    void readClient(int fd) {
        std::array<char, 4096> buf{};
        const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            disconnect(fd);
            return;
        }
        auto it = clients_.find(fd);
        if (it == clients_.end()) {
            return;
        }
        Client& client = it->second;
        client.input.append(buf.data(), static_cast<size_t>(n));
        if (!client.handshaken) {
            handleHandshake(client);
            return;
        }

        while (true) {
            auto message = parseFrame(client.input);
            if (!message) {
                break;
            }
            if (message->opcode == 0x8) {
                disconnect(fd);
                break;
            }
            if (message->opcode == 0x9) {
                sendAll(fd, makeFrame(message->payload, 0xA));
                continue;
            }
            if (message->opcode == 0x1) {
                handleJson(client, message->payload);
            }
        }
    }

    void handleHandshake(Client& client) {
        const size_t end = client.input.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (client.input.size() > 8192) {
                disconnect(client.fd);
            }
            return;
        }
        const std::string request = client.input.substr(0, end + 4);
        auto key = headerValue(request, "Sec-WebSocket-Key");
        if (!key) {
            disconnect(client.fd);
            return;
        }
        std::ostringstream response;
        response << "HTTP/1.1 101 Switching Protocols\r\n"
                 << "Upgrade: websocket\r\n"
                 << "Connection: Upgrade\r\n"
                 << "Sec-WebSocket-Accept: " << websocketAccept(*key) << "\r\n\r\n";
        if (!sendAll(client.fd, response.str())) {
            disconnect(client.fd);
            return;
        }
        client.handshaken = true;
        client.input.erase(0, end + 4);
        sendJson(client.fd, R"({"type":"hello","message":"join with {\"type\":\"join\",\"room\":\"lobby\",\"name\":\"player\"}"})");
    }

    void handleJson(Client& client, const std::string& payload) {
        const auto type = jsonString(payload, "type");
        if (!type) {
            sendError(client.fd, "missing type");
            return;
        }
        if (*type == "join") {
            joinRoom(client, jsonString(payload, "room").value_or("lobby"),
                     jsonString(payload, "name").value_or("player"));
        } else if (*type == "turn") {
            turn(client, jsonString(payload, "dir").value_or(""));
        } else if (*type == "leave") {
            leaveRoom(client);
        } else if (*type == "ping") {
            sendJson(client.fd, R"({"type":"pong"})");
        } else {
            sendError(client.fd, "unknown type");
        }
    }

    void joinRoom(Client& client, std::string roomId, std::string name) {
        if (roomId.empty()) {
            roomId = "lobby";
        }
        if (name.empty()) {
            name = "player";
        }
        leaveRoom(client);

        Room& room = rooms_[roomId];
        room.id = roomId;
        if (static_cast<int>(room.players.size()) >= kMaxPlayersPerRoom) {
            sendError(client.fd, "room full");
            return;
        }

        const int playerId = nextPlayerId_++;
        Player player;
        player.clientFd = client.fd;
        player.id = playerId;
        player.name = name.substr(0, 32);
        const Position head = randomEmptyCell(room);
        const Position body1{std::max(0, head.x - 1), head.y};
        const Position body2{std::max(0, head.x - 2), head.y};
        player.snake.push_back(head);
        player.snake.push_back(body1);
        player.snake.push_back(body2);
        room.players[playerId] = std::move(player);
        client.roomId = roomId;
        client.playerId = playerId;
        placeFood(room);

        std::ostringstream joined;
        joined << "{\"type\":\"joined\",\"room\":\"" << escapeJson(roomId) << "\",\"playerId\":" << playerId
               << ",\"width\":" << kBoardWidth << ",\"height\":" << kBoardHeight << "}";
        sendJson(client.fd, joined.str());
        broadcastState(room);
    }

    void leaveRoom(Client& client) {
        if (client.roomId.empty()) {
            return;
        }
        auto roomIt = rooms_.find(client.roomId);
        if (roomIt != rooms_.end()) {
            roomIt->second.players.erase(client.playerId);
            if (roomIt->second.players.empty()) {
                rooms_.erase(roomIt);
            } else {
                broadcastState(roomIt->second);
            }
        }
        client.roomId.clear();
        client.playerId = 0;
    }

    void turn(Client& client, const std::string& dir) {
        if (dir != "up" && dir != "down" && dir != "left" && dir != "right") {
            sendError(client.fd, "invalid dir");
            return;
        }
        auto player = findPlayer(client);
        if (!player || !(*player)->alive || isOpposite((*player)->dir, dir)) {
            return;
        }
        (*player)->nextDir = dir;
    }

    std::optional<Player*> findPlayer(Client& client) {
        auto roomIt = rooms_.find(client.roomId);
        if (roomIt == rooms_.end()) {
            return std::nullopt;
        }
        auto playerIt = roomIt->second.players.find(client.playerId);
        if (playerIt == roomIt->second.players.end()) {
            return std::nullopt;
        }
        return &playerIt->second;
    }

    void tickRooms() {
        const auto now = Clock::now();
        for (auto& [_, room] : rooms_) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - room.lastTick).count();
            if (elapsed < kTickMs || !room.running) {
                continue;
            }
            room.lastTick = now;
            updateRoom(room);
            broadcastState(room);
        }
    }

    void updateRoom(Room& room) {
        std::vector<std::pair<int, Position>> nextHeads;
        for (auto& [id, player] : room.players) {
            if (!player.alive || player.snake.empty()) {
                continue;
            }
            player.dir = player.nextDir;
            nextHeads.push_back({id, step(player.snake.front(), player.dir)});
        }

        std::map<std::pair<int, int>, int> headCounts;
        for (const auto& [_, head] : nextHeads) {
            ++headCounts[{head.x, head.y}];
        }

        bool ateFood = false;
        for (const auto& [id, head] : nextHeads) {
            auto& player = room.players[id];
            if (head.x < 0 || head.y < 0 || head.x >= kBoardWidth || head.y >= kBoardHeight ||
                headCounts[{head.x, head.y}] > 1 || hitsSnake(room, id, head)) {
                player.alive = false;
                continue;
            }
            player.snake.push_front(head);
            if (head.x == room.food.x && head.y == room.food.y) {
                player.score += 1;
                ateFood = true;
            } else {
                player.snake.pop_back();
            }
        }
        if (ateFood) {
            placeFood(room);
        }
    }

    bool hitsSnake(const Room& room, int movingPlayerId, Position p) const {
        for (const auto& [id, player] : room.players) {
            for (size_t i = 0; i < player.snake.size(); ++i) {
                const bool ownTail = (id == movingPlayerId && i + 1 == player.snake.size());
                if (ownTail) {
                    continue;
                }
                if (player.snake[i].x == p.x && player.snake[i].y == p.y) {
                    return true;
                }
            }
        }
        return false;
    }

    Position randomEmptyCell(const Room& room) {
        std::uniform_int_distribution<int> xDist(2, kBoardWidth - 3);
        std::uniform_int_distribution<int> yDist(2, kBoardHeight - 3);
        for (int attempt = 0; attempt < 200; ++attempt) {
            Position p{xDist(rng_), yDist(rng_)};
            if (!cellOccupied(room, p)) {
                return p;
            }
        }
        return Position{2, 2};
    }

    void placeFood(Room& room) {
        room.food = randomEmptyCell(room);
    }

    bool cellOccupied(const Room& room, Position p) const {
        for (const auto& [_, player] : room.players) {
            for (const auto& part : player.snake) {
                if (part.x == p.x && part.y == p.y) {
                    return true;
                }
            }
        }
        return false;
    }

    void broadcastState(const Room& room) {
        std::ostringstream json;
        json << "{\"type\":\"state\",\"room\":\"" << escapeJson(room.id) << "\",\"width\":" << kBoardWidth
             << ",\"height\":" << kBoardHeight << ",\"food\":{\"x\":" << room.food.x << ",\"y\":" << room.food.y
             << "},\"players\":[";
        bool firstPlayer = true;
        for (const auto& [_, player] : room.players) {
            if (!firstPlayer) {
                json << ",";
            }
            firstPlayer = false;
            json << "{\"id\":" << player.id << ",\"name\":\"" << escapeJson(player.name)
                 << "\",\"alive\":" << (player.alive ? "true" : "false")
                 << ",\"score\":" << player.score << ",\"snake\":[";
            for (size_t i = 0; i < player.snake.size(); ++i) {
                if (i > 0) {
                    json << ",";
                }
                json << "{\"x\":" << player.snake[i].x << ",\"y\":" << player.snake[i].y << "}";
            }
            json << "]}";
        }
        json << "]}";
        const std::string frame = makeFrame(json.str());
        for (const auto& [_, player] : room.players) {
            sendAll(player.clientFd, frame);
        }
    }

    void sendJson(int fd, const std::string& json) {
        sendAll(fd, makeFrame(json));
    }

    void sendError(int fd, const std::string& message) {
        sendJson(fd, "{\"type\":\"error\",\"message\":\"" + escapeJson(message) + "\"}");
    }

    void disconnect(int fd) {
        auto it = clients_.find(fd);
        if (it != clients_.end()) {
            leaveRoom(it->second);
            clients_.erase(it);
        }
        close(fd);
    }

    uint16_t port_;
    int listenFd_ = -1;
    int nextPlayerId_ = 1;
    std::mt19937 rng_;
    std::unordered_map<int, Client> clients_;
    std::unordered_map<std::string, Room> rooms_;
};

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 9002;
    if (argc >= 2) {
        const int parsed = std::atoi(argv[1]);
        if (parsed > 0 && parsed <= 65535) {
            port = static_cast<uint16_t>(parsed);
        }
    }

    SnakeServer server(port);
    if (!server.start()) {
        return 1;
    }
    server.run();
    return 0;
}
