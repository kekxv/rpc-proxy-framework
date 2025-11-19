#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>

class Executor {
public:
    Executor();
    void run(const std::string& pipe_name);
};

#endif // EXECUTOR_H
