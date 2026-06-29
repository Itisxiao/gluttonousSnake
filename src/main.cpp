#include "server.hpp"

#include <cstdlib>

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
