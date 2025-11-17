#include <stdio.h>
#include <stdint.h> // Required for uint64_t

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

typedef struct {
    int32_t x;
    int32_t y;
} Point;

DLLEXPORT int add(int a, int b) {
    return a + b;
}

DLLEXPORT const char* greet(const char* name) {
    static char buffer[256];
    snprintf(buffer, sizeof(buffer), "Hello, %s", name);
    return buffer;
}

DLLEXPORT int process_point_by_val(Point p) {
    return p.x + p.y;
}

DLLEXPORT int process_point_by_ptr(uint64_t p_ptr_val) {
    const Point* p = (const Point*)p_ptr_val;
    if (p == NULL) {
        return -1; // Error
    }
    return p->x + p->y;
}

DLLEXPORT Point create_point(int32_t x, int32_t y) {
    Point p = {x, y};
    return p;
}

