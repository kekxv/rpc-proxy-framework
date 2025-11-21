#include "executor.h"
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

char** g_argv = nullptr;

#ifdef _WIN32
LONG WINAPI UnhandledExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    LPSTR commandLine = GetCommandLineA();

    if (CreateProcessA(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    ExitProcess(1);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void SignalHandler(int signum) {
    if (g_argv != nullptr) {
        execv(g_argv[0], g_argv);
    }
    _exit(1);
}
#endif

void setup_crash_handler() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    struct sigaction sa;
    sa.sa_handler = SignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
#endif
}

int main(int argc, char* argv[]) {
    g_argv = argv;
    setup_crash_handler();

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
