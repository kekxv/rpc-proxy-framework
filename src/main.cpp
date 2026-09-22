#include "executor.h"
#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#endif

void setup_crash_handler() {
    // 崩溃交由外部 supervisor 处理，避免进程内无限重启。
}

// 读取 token 文件：取第一行并去除行尾空白（兼容 CRLF）。
static std::string read_token_file(const std::string& path)
{
  std::ifstream in(path);
  if (!in.is_open())
  {
    throw std::runtime_error("Cannot open token file: " + path);
  }
  std::string token;
  std::getline(in, token);
  while (!token.empty() &&
         (token.back() == '\n' || token.back() == '\r' || token.back() == ' ' || token.back() == '\t'))
  {
    token.pop_back();
  }
  if (token.empty())
  {
    throw std::runtime_error("Token file is empty: " + path);
  }
  return token;
}

int main(int argc, char* argv[]) {
    setup_crash_handler();

    std::string pipe_name;
    std::string token;

    try
    {
      for (int i = 1; i < argc; ++i)
      {
        std::string arg = argv[i];
        if (arg == "--pipe" && i + 1 < argc)
        {
          pipe_name = argv[++i];
        }
        else if (arg == "--token" && i + 1 < argc)
        {
          token = argv[++i];
        }
        else if (arg == "--token-file" && i + 1 < argc)
        {
          ++i;
          // 优先级: --token > --token-file > RPC_PROXY_TOKEN。
          // --token 已提供时忽略 --token-file（与下方环境变量回退一致）。
          if (token.empty()) token = read_token_file(argv[i]);
        }
        else
        {
          std::cerr << "Unknown or incomplete argument: " << arg << std::endl;
          return 1;
        }
      }
    }
    catch (const std::exception& e)
    {
      std::cerr << "Error: " << e.what() << std::endl;
      return 1;
    }

    if (pipe_name.empty())
    {
      std::cerr << "Usage: " << argv[0]
                << " --pipe <pipe_name> [--token <token> | --token-file <path>]" << std::endl;
      std::cerr << "Token can also be provided via the RPC_PROXY_TOKEN environment variable." << std::endl;
      return 1;
    }

    // Token 解析优先级: --token > --token-file > RPC_PROXY_TOKEN 环境变量
    // 三者都未提供时：保留传统模式（不强制认证），打印警告后继续启动
    if (token.empty())
    {
      const char* env_token = std::getenv("RPC_PROXY_TOKEN");
      if (env_token != nullptr) token = env_token;
    }
    if (token.empty())
    {
      std::cerr << "WARNING: no authentication token configured (--token/--token-file/RPC_PROXY_TOKEN). "
                   "Running in legacy mode: connections will NOT be authenticated." << std::endl;
    }

    try {
        Executor executor;
        executor.run(pipe_name, token);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
