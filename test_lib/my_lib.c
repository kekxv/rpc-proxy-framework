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

typedef struct {
    Point p1;
    Point p2;
} Line;

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

DLLEXPORT int process_point_by_ptr(Point* p) {
    // const Point* p = (const Point*)p_ptr_val;
    if (p == NULL) {
        return -1; // Error
    }
    return p->x + p->y;
}

DLLEXPORT Point create_point(int32_t x, int32_t y) {
    Point p = {x, y};
    return p;
}

DLLEXPORT int get_line_length(Line line) {
    return line.p1.x + line.p1.y + line.p2.x + line.p2.y;
}

DLLEXPORT int sum_points(const Point* points, int count) {
    if (points == NULL) {
        return -1;
    }
    int total_sum = 0;
    for (int i = 0; i < count; ++i) {
        total_sum += points[i].x + points[i].y;
    }
    return total_sum;
}

DLLEXPORT Line create_line(int32_t p1x, int32_t p1y, int32_t p2x, int32_t p2y) {
    Line l = {{p1x, p1y}, {p2x, p2y}};
    return l;
}

// New function to demonstrate callbacks
// The callback function is expected to have the signature: void (*callback_fn)(const char* message, int value)
DLLEXPORT void call_my_callback(void (*callback_fn)(const char* message, int value), const char* msg) {
    if (callback_fn) {
        printf("Native code calling back with message: %s, value: %d\n", msg, 123);
        callback_fn(msg, 123);
    } else {
        printf("Callback function is NULL.\n");
    }
}

