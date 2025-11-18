#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>

#include "nlohmann/json.hpp" // Make sure nlohmann/json is available

#ifdef _WIN32
#include <windows.h>
#include <namedpipeapi.h>
#include <winsock2.h> // For htonl/ntohl on Windows
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h> // For htonl/ntohl on Unix-like systems
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// Helper function to send JSON request with length prefix
json send_request(
#ifdef _WIN32
    HANDLE pipe,
#else
    int sock,
#endif
    const json& request) {
    std::string request_str = request.dump();
    uint32_t length = request_str.length();
    uint32_t network_order_length = htonl(length); // Convert to network byte order (big-endian)

#ifdef _WIN32
    DWORD bytes_written;
    WriteFile(pipe, &network_order_length, 4, &bytes_written, NULL);
    WriteFile(pipe, request_str.c_str(), length, &bytes_written, NULL);
    FlushFileBuffers(pipe);
#else
    write(sock, &network_order_length, 4);
    write(sock, request_str.c_str(), length);
#endif

    // Read length prefix of response
    uint32_t network_order_response_length;
#ifdef _WIN32
    DWORD bytes_read;
    ReadFile(pipe, &network_order_response_length, 4, &bytes_read, NULL);
#else
    read(sock, &network_order_response_length, 4);
#endif
    uint32_t response_length = ntohl(network_order_response_length); // Convert from network byte order

    // Read response
    std::vector<char> response_buffer(response_length);
#ifdef _WIN32
    ReadFile(pipe, response_buffer.data(), response_length, &bytes_read, NULL);
#else
    read(sock, response_buffer.data(), response_length);
#endif
    std::string response_str(response_buffer.begin(), response_buffer.end());
    return json::parse(response_str);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <pipe_name>" << std::endl;
        return 1;
    }
    std::string pipe_name = argv[1];

#ifdef _WIN32
    std::string pipe_path = "\\\\.\\pipe\\" + pipe_name;
    HANDLE pipe = CreateFileA(pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to connect to named pipe: " << GetLastError() << std::endl;
        return 1;
    }
    std::cout << "Connected to named pipe: " << pipe_path << std::endl;
#else
    std::string socket_path = "/tmp/" + pipe_name;
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1) {
        std::cerr << "Failed to create socket" << std::endl;
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "Failed to connect to Unix domain socket: " << socket_path << std::endl;
        close(sock);
        return 1;
    }
    std::cout << "Connected to Unix domain socket: " << socket_path << std::endl;
#endif

    fs::path current_path = fs::current_path();
    // Adjust lib_path to be relative to the cpp_controller directory
    fs::path lib_path = current_path  / "build" / "test_lib" ;
#ifdef _WIN32
    lib_path /= "my_lib.dll";
#elif __APPLE__
    lib_path /= "my_lib.dylib";
#else
    lib_path /= "my_lib.so";
#endif
    std::string library_full_path = lib_path.string();

    try {
        // 1. Register Struct "Point"
        json register_struct_request = {
            {"command", "register_struct"},
            {"request_id", "req-1"},
            {"payload", {
                {"struct_name", "Point"},
                {"definition", {
                    {{"name", "x"}, {"type", "int32"}},
                    {{"name", "y"}, {"type", "int32"}}
                }}
            }}
        };
        std::cout << "Registering struct 'Point'" << std::endl;
        json register_struct_response = send_request(
#ifdef _WIN32
            pipe,
#else
            sock,
#endif
            register_struct_request);
        std::cout << "Response: " << register_struct_response.dump() << std::endl;

        // 2. Load Library
        json load_library_request = {
            {"command", "load_library"},
            {"request_id", "req-2"},
            {"payload", {{"path", library_full_path}}} // Corrected payload construction
        };
        std::cout << "Loading library: " << library_full_path << std::endl;
        json load_library_response = send_request(
#ifdef _WIN32
            pipe,
#else
            sock,
#endif
            load_library_request);
        std::cout << "Response: " << load_library_response.dump() << std::endl;
        if (load_library_response["status"] == "error") {
            throw std::runtime_error("Failed to load library: " + load_library_response["error_message"].get<std::string>());
        }
        std::string library_id = load_library_response["data"]["library_id"].get<std::string>();
        std::cout << "Library loaded with ID: " << library_id << std::endl;

        // 3. Call function 'add'
        json add_request = {
            {"command", "call_function"},
            {"request_id", "req-3"},
            {"payload", {
                {"library_id", library_id},
                {"function_name", "add"},
                {"return_type", "int32"},
                {"args", {
                    {{"type", "int32"}, {"value", 10}},
                    {{"type", "int32"}, {"value", 20}}
                }}
            }}
        };
        std::cout << "Calling function 'add' with args (10, 20)" << std::endl;
        json add_response = send_request(
#ifdef _WIN32
            pipe,
#else
            sock,
#endif
            add_request);
        std::cout << "Response: " << add_response.dump() << std::endl;
        if (add_response["status"] == "error") {
            throw std::runtime_error("Failed to call 'add': " + add_response["error_message"].get<std::string>());
        }
        std::cout << "Result of add(10, 20) is: " << add_response["data"]["value"].get<int>() << std::endl;

        // 4. Call function 'greet'
        json greet_request = {
            {"command", "call_function"},
            {"request_id", "req-4"},
            {"payload", {
                {"library_id", library_id},
                {"function_name", "greet"},
                {"return_type", "string"},
                {"args", {
                    {{"type", "string"}, {"value", "C++ World"}}
                }}
            }}
        };
        std::cout << "Calling function 'greet' with arg ('C++ World')" << std::endl;
        json greet_response = send_request(
#ifdef _WIN32
            pipe,
#else
            sock,
#endif
            greet_request);
        std::cout << "Response: " << greet_response.dump() << std::endl;
        if (greet_response["status"] == "error") {
            throw std::runtime_error("Failed to call 'greet': " + greet_response["error_message"].get<std::string>());
        }
        std::cout << "Result of greet('C++ World') is: '" << greet_response["data"]["value"].get<std::string>() << "'" << std::endl;

        // 5. Call function 'process_point_by_val'
        json process_point_by_val_request = {
            {"command", "call_function"},
            {"request_id", "req-5"},
            {"payload", {
                {"library_id", library_id},
                {"function_name", "process_point_by_val"},
                {"return_type", "int32"},
                {"args", {
                    {{"type", "Point"}, {"value", {{"x", 10}, {"y", 20}}}}
                }}
            }}
        };
        std::cout << "Calling function 'process_point_by_val' with args (Point {x=10, y=20})" << std::endl;
        json process_point_by_val_response = send_request(
#ifdef _WIN32
            pipe,
#else
            sock,
#endif
            process_point_by_val_request);
        std::cout << "Response: " << process_point_by_val_response.dump() << std::endl;
        if (process_point_by_val_response["status"] == "error") {
            throw std::runtime_error("Failed to call 'process_point_by_val': " + process_point_by_val_response["error_message"].get<std::string>());
        }
        std::cout << "Result of process_point_by_val is: " << process_point_by_val_response["data"]["value"].get<int>() << std::endl;

        // 6. Call function 'process_point_by_ptr'
        json process_point_by_ptr_request = {
            {"command", "call_function"},
            {"request_id", "req-6"},
            {"payload", {
                {"library_id", library_id},
                {"function_name", "process_point_by_ptr"},
                {"return_type", "int32"},
                {"args", {
                    {{"type", "pointer"}, {"value", {{"x", 5}, {"y", 6}}}, {"target_type", "Point"}} // Corrected for pointer
                }}
            }}
        };
        std::cout << "Calling function 'process_point_by_ptr' with args (Point {x=5, y=6})" << std::endl;
        json process_point_by_ptr_response = send_request(
#ifdef _WIN32
            pipe,
#else
            sock,
#endif
            process_point_by_ptr_request);
        std::cout << "Response: " << process_point_by_ptr_response.dump() << std::endl;
        if (process_point_by_ptr_response["status"] == "error") {
            throw std::runtime_error("Failed to call 'process_point_by_ptr': " + process_point_by_ptr_response["error_message"].get<std::string>());
        }
        std::cout << "Result of process_point_by_ptr is: " << process_point_by_ptr_response["data"]["value"].get<int>() << std::endl;

        // 7. Call function 'create_point'
        json create_point_request = {
            {"command", "call_function"},
            {"request_id", "req-7"},
            {"payload", {
                {"library_id", library_id},
                {"function_name", "create_point"},
                {"return_type", "Point"},
                {"args", {
                    {{"type", "int32"}, {"value", 100}},
                    {{"type", "int32"}, {"value", 200}}
                }}
            }}
        };
        std::cout << "Calling function 'create_point' with args (100, 200)" << std::endl;
        json create_point_response = send_request(
#ifdef _WIN32
            pipe,
#else
            sock,
#endif
            create_point_request);
        std::cout << "Response: " << create_point_response.dump() << std::endl;
        if (create_point_response["status"] == "error") {
            throw std::runtime_error("Failed to call 'create_point': " + create_point_response["error_message"].get<std::string>());
        }
        std::cout << "Result of create_point is: " << create_point_response["data"]["value"].dump() << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

#ifdef _WIN32
    CloseHandle(pipe);
#else
    close(sock);
#endif
    std::cout << "Connection closed." << std::endl;

    return 0;
}
