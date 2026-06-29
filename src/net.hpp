#pragma once

#include <string>

bool setNonBlocking(int fd);
bool sendAll(int fd, const std::string& data);
