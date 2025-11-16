#include <stdio.h>

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

// 一个简单的加法函数
DLLEXPORT int add(int a, int b) {
    return a + b;
}

// 一个返回字符串的函数
// 注意：在实际应用中，静态缓冲区不是线程安全的。这里仅为演示。
DLLEXPORT const char* greet(const char* name) {
    static char buffer[128];
    snprintf(buffer, sizeof(buffer), "Hello, %s", name);
    return buffer;
}
