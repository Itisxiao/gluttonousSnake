#pragma once

#include <chrono>
#include <deque>
#include <map>
#include <random>
#include <string>

using Clock = std::chrono::steady_clock;

constexpr int kBoardWidth = 40;
constexpr int kBoardHeight = 30;
constexpr int kTickMs = 150;
constexpr int kMaxPlayersPerRoom = 8;

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

bool isOpposite(const std::string& a, const std::string& b);
Position step(Position p, const std::string& dir);
bool cellOccupied(const Room& room, Position p);
Position randomEmptyCell(const Room& room, std::mt19937& rng);
void placeFood(Room& room, std::mt19937& rng);
bool hitsSnake(const Room& room, int movingPlayerId, Position p);
void updateRoom(Room& room, std::mt19937& rng);
