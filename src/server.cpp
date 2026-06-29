#include "server.hpp"

#include "json_util.hpp"
#include "net.hpp"
#include "websocket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <sstream>
#include <utility>
#include <vector>

SnakeServer::SnakeServer(uint16_t port) : port_(port), rng_(std::random_device{}()) {}

bool SnakeServer::start() {
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

void SnakeServer::run() {
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
        //select（同步 I/O 多路复用）
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

void SnakeServer::acceptClient() {
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

void SnakeServer::readClient(int fd) {
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

void SnakeServer::handleHandshake(Client& client) {
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

void SnakeServer::handleJson(Client& client, const std::string& payload) {
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

void SnakeServer::joinRoom(Client& client, std::string roomId, std::string name) {
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
    const Position head = randomEmptyCell(room, rng_);
    const Position body1{std::max(0, head.x - 1), head.y};
    const Position body2{std::max(0, head.x - 2), head.y};
    player.snake.push_back(head);
    player.snake.push_back(body1);
    player.snake.push_back(body2);
    room.players[playerId] = std::move(player);
    client.roomId = roomId;
    client.playerId = playerId;
    placeFood(room, rng_);

    std::ostringstream joined;
    joined << "{\"type\":\"joined\",\"room\":\"" << escapeJson(roomId) << "\",\"playerId\":" << playerId
           << ",\"width\":" << kBoardWidth << ",\"height\":" << kBoardHeight << "}";
    sendJson(client.fd, joined.str());
    broadcastState(room);
}

void SnakeServer::leaveRoom(Client& client) {
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

void SnakeServer::turn(Client& client, const std::string& dir) {
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

std::optional<Player*> SnakeServer::findPlayer(Client& client) {
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

void SnakeServer::tickRooms() {
    const auto now = Clock::now();
    for (auto& [_, room] : rooms_) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - room.lastTick).count();
        if (elapsed < kTickMs || !room.running) {
            continue;
        }
        room.lastTick = now;
        updateRoom(room, rng_);
        broadcastState(room);
    }
}

void SnakeServer::broadcastState(const Room& room) {
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

void SnakeServer::sendJson(int fd, const std::string& json) {
    sendAll(fd, makeFrame(json));
}

void SnakeServer::sendError(int fd, const std::string& message) {
    sendJson(fd, "{\"type\":\"error\",\"message\":\"" + escapeJson(message) + "\"}");
}

void SnakeServer::disconnect(int fd) {
    auto it = clients_.find(fd);
    if (it != clients_.end()) {
        leaveRoom(it->second);
        clients_.erase(it);
    }
    close(fd);
}
