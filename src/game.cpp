#include "game.hpp"

#include <algorithm>
#include <utility>
#include <vector>

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

bool cellOccupied(const Room& room, Position p) {
    for (const auto& [_, player] : room.players) {
        for (const auto& part : player.snake) {
            if (part.x == p.x && part.y == p.y) {
                return true;
            }
        }
    }
    return false;
}

Position randomEmptyCell(const Room& room, std::mt19937& rng) {
    std::uniform_int_distribution<int> xDist(2, kBoardWidth - 3);
    std::uniform_int_distribution<int> yDist(2, kBoardHeight - 3);
    for (int attempt = 0; attempt < 200; ++attempt) {
        Position p{xDist(rng), yDist(rng)};
        if (!cellOccupied(room, p)) {
            return p;
        }
    }
    return Position{2, 2};
}

void placeFood(Room& room, std::mt19937& rng) {
    room.food = randomEmptyCell(room, rng);
}

bool hitsSnake(const Room& room, int movingPlayerId, Position p) {
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

std::vector<GameEvent> updateRoom(Room& room, std::mt19937& rng) {
    std::vector<GameEvent> events;
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
            events.push_back(GameEvent{GameEventType::Died, player.id, player.name, player.score, head});
            continue;
        }
        player.snake.push_front(head);
        if (head.x == room.food.x && head.y == room.food.y) {
            player.score += 1;
            ateFood = true;
            events.push_back(GameEvent{GameEventType::AteFood, player.id, player.name, player.score, head});
        } else {
            player.snake.pop_back();
        }
    }
    if (ateFood) {
        placeFood(room, rng);
    }
    return events;
}
