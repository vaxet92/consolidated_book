#include <iostream>

#include "md_core/md_core.h"
#include "logger/logger.h"

int main() {
    Logger::Log(LogLevel::kInfo, "Hello, World!");

    MDCore core;
    core.init(MDCoreConfig{});

    return 0;
}