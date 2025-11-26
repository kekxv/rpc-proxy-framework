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

// New function to demonstrate multiple callbacks
// The callback function is expected to have the signature: void (*callback_fn)(const char* message, int value)
DLLEXPORT void call_multi_callbacks(void (*callback_fn)(const char* message, int value), int count) {
    if (!callback_fn) {
        printf("Callback function is NULL for multi-callbacks.\n");
        return;
    }
    printf("Native code starting multi-callbacks, count: %d\n", count);
    for (int i = 0; i < count; ++i) {
        char message[256];
        snprintf(message, sizeof(message), "Message from native code, call %d", i + 1);
        printf("  Calling back with message: %s, value: %d\n", message, i + 1);
        callback_fn(message, i + 1);
    }
    printf("Native code finished multi-callbacks.\n");
}

// New function to demonstrate writing to an output buffer
// buff: output buffer, size: input buffer capacity, output actual size written
// buff: output buffer, size: input buffer capacity, output actual size written
// New function to demonstrate inout buffer
// buff: inout buffer, size: input buffer capacity, output actual size written
// Returns 0 on success, -1 for invalid arguments, -2 for buffer too small
DLLEXPORT int process_buffer_inout(char* buff, int* size) {
    if (buff == NULL || size == NULL) {
        return -1; // Invalid arguments
    }
    if (*size < 4) {
        *size = 0;
        return -2; // Buffer too small
    }

    // Read input from buffer (e.g., first byte)
    char input_val = buff[0];

    // Write new data to buffer
    buff[0] = 0xAA; // Overwrite a byte to show it's inout
    buff[1] = input_val + 1; // Use input value
    buff[2] = 0xDE;
    buff[3] = 0xAD;

    // Report actual size of the meaningful output data
    *size = 4;

    return 0; // Success
}

typedef void(*ReadCallback)(int type, unsigned char data[], int size, void *that);

DLLEXPORT void trigger_read_callback(ReadCallback cb, int type, const char* input_str, void* context) {
    if (cb) {
        // Cast const char* to unsigned char* for the callback
        // In a real scenario, this might be binary data
        unsigned char* data = (unsigned char*)input_str;
        int size = 0;
        if (input_str) {
            size = (int)strlen(input_str);
        }
        
        printf("Native triggering ReadCallback: type=%d, size=%d, context=%p\n", type, size, context);
        cb(type, data, size, context);
    }
}

typedef void(*FixedReadCallback)(unsigned char data[], void *that);

DLLEXPORT void trigger_fixed_read_callback(FixedReadCallback cb, void* context) {
    if (cb) {
        // Fixed data: 0xDE, 0xAD, 0xBE, 0xEF (4 bytes)
        unsigned char data[] = {0xDE, 0xAD, 0xBE, 0xEF};
        printf("Native triggering FixedReadCallback with 4 bytes\n");
        cb(data, context);
    }
}

