#include "executor.h"
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3 || std::string(argv[1]) != "--pipe") {
        std::cerr << "Usage: " << argv[0] << " --pipe <pipe_name>" << std::endl;
        return 1;
    }
    std::string pipe_name = argv[2];
    try {
        Executor executor;
        executor.run(pipe_name);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
