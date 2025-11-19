#include <stdio.h>
#include <stdint.h> // Required for uint64_t
#include <string.h> // Required for strlen and strcpy

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

// New function to demonstrate writing to an output buffer
// buff: output buffer, size: input buffer capacity, output actual size written
// buff: output buffer, size: input buffer capacity, output actual size written
// Returns 0 on success, -1 for invalid arguments, -2 for buffer too small
DLLEXPORT int writeOutBuff(char* buff, int* size) {
    if (buff == NULL || size == NULL) {
        return -1; // Invalid arguments
    }

    const char* data_to_write = "Hello from writeOutBuff!";
    int data_len = strlen(data_to_write);
    int required_size = data_len + 1; // +1 for null terminator

    if (*size < required_size) {
        // Not enough space, do not write anything, just report error.
        return -2; // Buffer too small
    }

    strcpy(buff, data_to_write);
    *size = data_len; // Update size to actual data length written (excluding null terminator)

    return 0; // Success
}

