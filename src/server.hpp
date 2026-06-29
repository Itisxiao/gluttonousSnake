#pragma once

#include "game.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>

struct Client {
    int fd = -1;
    bool handshaken = false;
    std::string input;
    std::string roomId;
    int playerId = 0;
};

class SnakeServer {
public:
    explicit SnakeServer(uint16_t port);

    bool start();
    void run();

private:
    void acceptClient();
    void readClient(int fd);
    void handleHandshake(Client& client);
    void handleJson(Client& client, const std::string& payload);
    void joinRoom(Client& client, std::string roomId, std::string name);
    void leaveRoom(Client& client);
    void turn(Client& client, const std::string& dir);
    std::optional<Player*> findPlayer(Client& client);
    void tickRooms();
    void broadcastState(const Room& room);
    void sendJson(int fd, const std::string& json);
    void sendError(int fd, const std::string& message);
    void disconnect(int fd);

    uint16_t port_;
    int listenFd_ = -1;
    int nextPlayerId_ = 1;
    std::mt19937 rng_;
    std::unordered_map<int, Client> clients_;
    std::unordered_map<std::string, Room> rooms_;
};
