#include <stdio.h>

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

// 定义一个简单的结构体
typedef struct {
    int x;
    int y;
} Point;

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

// 接收一个Point结构体（按值传递），返回其x+y
DLLEXPORT int process_point_by_val(Point p) {
    return p.x + p.y;
}

// 接收一个Point结构体指针（按引用传递），返回其x*y
DLLEXPORT int process_point_by_ptr(const Point* p) {
    if (p == NULL) {
        return -1; // Error
    }
    return p->x * p->y;
}

// 接收一个Point结构体（按值传递），并返回一个新的Point结构体
DLLEXPORT Point create_point(int x, int y) {
    Point p = {x, y};
    return p;
}
