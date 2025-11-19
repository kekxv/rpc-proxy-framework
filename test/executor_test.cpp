#include "gtest/gtest.h"
#include "struct_manager.h"
#include "ffi_dispatcher.h"
#include "lib_manager.h"
#include "callback_manager.h" // Include CallbackManager
#include "ipc_server.h"       // For ClientConnection base class
#include <memory>
#include <iostream>

// Forward declarations for test_lib functions
extern "C" {
int add(int a, int b);
char* greet(char* name);
}

// Dummy ClientConnection for testing CallbackManager in isolation
class DummyClientConnection : public ClientConnection {
public:
    nlohmann::json last_event;

    std::string read() override { return ""; } // No-op
    bool write(const std::string& message) override { return true; } // No-op
    bool sendEvent(const nlohmann::json& event_json) override {
        std::cout << "DummyClientConnection received event: " << event_json.dump() << std::endl;
        last_event = event_json;
        return true;
    }
    bool isOpen() override { return true; } // Always open for test purposes
};

class ExecutorTest : public ::testing::Test
{
protected:
  StructManager struct_manager;
  DummyClientConnection dummy_connection; // Dummy connection for CallbackManager
  CallbackManager callback_manager;
  FfiDispatcher ffi_dispatcher;
  LibManager lib_manager;

  ExecutorTest() : 
    struct_manager(),
    dummy_connection(),
    callback_manager(&dummy_connection, &struct_manager), // Pass dummy connection
    ffi_dispatcher(struct_manager, &callback_manager) // Pass CallbackManager to FfiDispatcher
  {
  }

  void SetUp() override
  {
    // Register Point struct
    json point_def = {
      {{"name", "x"}, {"type", "int32"},},
      {{"name", "y"}, {"type", "int32"},},
    };
    struct_manager.register_struct("Point", point_def);

    // Register Line struct (nested Point)
    json line_def = {
      {{"name", "p1"}, {"type", "Point"},},
      {{"name", "p2"}, {"type", "Point"},},
    };
    struct_manager.register_struct("Line", line_def);

    // Load the test library. The path is relative to the project root,
    // where the test executable is run from.
#ifdef _WIN32
#ifdef CMAKE_BUILD_TYPE
    std::string lib_path = "../../test_lib/" CMAKE_BUILD_TYPE "/my_lib.dll";
#else
    std::string lib_path = "../test_lib/my_lib.dll";
#endif
#elif defined(__linux__)
    std::string lib_path = "../test_lib/my_lib.so";
#else
    std::string lib_path = "../test_lib/my_lib.dylib";
#endif
    test_lib_id = lib_manager.load_library(lib_path);
    if (test_lib_id.empty()) {
        // Fallback for different build configurations if needed
        lib_path = "cmake-build-debug/test_lib/my_lib.dylib";
        test_lib_id = lib_manager.load_library(lib_path);
    }
    ASSERT_FALSE(test_lib_id.empty()) << "Failed to load library from: " << lib_path;
  }

  void TearDown() override
  {
    // Unload the test library
    lib_manager.unload_library(test_lib_id);
  }

  std::string test_lib_id;
};

TEST_F(ExecutorTest, BasicAddFunction)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "add"},
    {"return_type", "int32"},
    {
      "args", {
        {{"type", "int32"}, {"value", 10}},
        {{"type", "int32"}, {"value", 20}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "add"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 30);
}

TEST_F(ExecutorTest, GreetFunction)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "greet"},
    {"return_type", "string"},
    {
      "args", {
        {{"type", "string"}, {"value", "World"}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "greet"), payload);
  ASSERT_EQ(result["return"]["type"], "string");
  ASSERT_EQ(result["return"]["value"], "Hello, World");
}

TEST_F(ExecutorTest, ProcessPointByVal)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "process_point_by_val"},
    {"return_type", "int32"},
    {
      "args", {
        {{"type", "Point"}, {"value", {{"x", 10}, {"y", 20}}}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_point_by_val"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 30);
}

TEST_F(ExecutorTest, ProcessPointByPtr)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "process_point_by_ptr"},
    {"return_type", "int32"},
    {
      "args", {
        {
          {"type", "pointer"},
          {"target_type", "Point"},
          {"value", {{"x", 5}, {"y", 6}}}
        }
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_point_by_ptr"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 11);
}

TEST_F(ExecutorTest, CreatePoint)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "create_point"},
    {"return_type", "Point"},
    {
      "args", {
        {{"type", "int32"}, {"value", 100}},
        {{"type", "int32"}, {"value", 200}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "create_point"), payload);
  ASSERT_EQ(result["return"]["type"], "Point");
  ASSERT_EQ(result["return"]["value"]["x"], 100);
  ASSERT_EQ(result["return"]["value"]["y"], 200);
}

TEST_F(ExecutorTest, GetLineLength)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "get_line_length"},
    {"return_type", "int32"},
    {
      "args", {
        {
          {"type", "Line"}, {
            "value", {
              {"p1", {{"x", 1}, {"y", 2}}},
              {"p2", {{"x", 3}, {"y", 4}}}
            }
          }
        }
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "get_line_length"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 10); // 1+2+3+4 = 10
}

TEST_F(ExecutorTest, SumPoints)
{
  typedef struct
  {
    int32_t x;
    int32_t y;
  } Point;
  Point points_array[] = {{1, 1}, {2, 2}, {3, 3}};
  int count = sizeof(points_array) / sizeof(Point);

  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "sum_points"},
    {"return_type", "int32"},
    {
      "args", {
        {
          {"type", "pointer"},
          {"target_type", "Point[]"},
          {
            "value", {
              {{"x", 1}, {"y", 1}},
              {{"x", 2}, {"y", 2}},
              {{"x", 3}, {"y", 3}}
            }
          }
        },
        {{"type", "int32"}, {"value", count}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "sum_points"), payload);
  ASSERT_EQ(result["return"]["type"], "int32");
  ASSERT_EQ(result["return"]["value"], 12); // (1+1) + (2+2) + (3+3) = 2 + 4 + 6 = 12
}

TEST_F(ExecutorTest, CreateLine)
{
  json payload = {
    {"library_id", test_lib_id},
    {"function_name", "create_line"},
    {"return_type", "Line"},
    {
      "args", {
        {{"type", "int32"}, {"value", 10}},
        {{"type", "int32"}, {"value", 20}},
        {{"type", "int32"}, {"value", 30}},
        {{"type", "int32"}, {"value", 40}}
      }
    }
  };
  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "create_line"), payload);
  ASSERT_EQ(result["return"]["type"], "Line");
  ASSERT_EQ(result["return"]["value"]["p1"]["x"], 10);
  ASSERT_EQ(result["return"]["value"]["p1"]["y"], 20);
  ASSERT_EQ(result["return"]["value"]["p2"]["x"], 30);
  ASSERT_EQ(result["return"]["value"]["p2"]["y"], 40);
}

TEST_F(ExecutorTest, CallbackFunction)
{
    // 1. Register a callback signature
    std::string callback_id = callback_manager.registerCallback("void", {"string", "int32"});
    ASSERT_FALSE(callback_id.empty());

    // 2. Prepare to call the C function that expects a callback
    const char* test_message = "Hello from C++ unit test!";
    json payload = {
        {"library_id", test_lib_id},
        {"function_name", "call_my_callback"},
        {"return_type", "void"},
        {"args", {
            {
                {"type", "callback"},
                {"value", callback_id}
            },
            {
                {"type", "string"},
                {"value", test_message}
            }
        }}
    };

    // 3. Call the function
    ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "call_my_callback"), payload);

    // 4. Verify that the dummy connection received the event
    ASSERT_FALSE(dummy_connection.last_event.is_null());
    ASSERT_EQ(dummy_connection.last_event["event"], "invoke_callback");

    const auto& event_payload = dummy_connection.last_event["payload"];
    ASSERT_EQ(event_payload["callback_id"], callback_id);
    
    const auto& event_args = event_payload["args"];
    ASSERT_EQ(event_args.size(), 2);
    ASSERT_EQ(event_args[0]["type"], "string");
    ASSERT_EQ(event_args[0]["value"], test_message);
    ASSERT_EQ(event_args[1]["type"], "int32");
    ASSERT_EQ(event_args[1]["value"], 123);
}

TEST_F(ExecutorTest, WriteOutBuffFunction)
{
    const int buffer_capacity = 64;
    json payload = {
        {"library_id", test_lib_id},
        {"function_name", "writeOutBuff"},
        {"return_type", "int32"},
        {"args", {
            {
                {"type", "buffer"},
                {"direction", "out"},
                {"size", buffer_capacity}
            },
            {
                {"type", "pointer"},
                {"target_type", "int32"},
                {"direction", "inout"},
                {"value", buffer_capacity}
            }
        }}
    };

    json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "writeOutBuff"), payload);

    // 1. Check direct return value
    ASSERT_EQ(result["return"]["type"], "int32");
    ASSERT_EQ(result["return"]["value"], 0); // Success code

    // 2. Check out_params
    const auto& out_params = result["out_params"];
    ASSERT_EQ(out_params.size(), 2);

    // Find the params by index, as order is not guaranteed
    json buffer_param;
    json size_param;
    for(const auto& param : out_params) {
        if (param["index"] == 0) {
            buffer_param = param;
        } else if (param["index"] == 1) {
            size_param = param;
        }
    }

    ASSERT_FALSE(buffer_param.is_null());
    ASSERT_FALSE(size_param.is_null());

    const std::string expected_string = "Hello from writeOutBuff!";
    ASSERT_EQ(buffer_param["type"], "string");
    ASSERT_EQ(buffer_param["value"], expected_string);

    ASSERT_EQ(size_param["type"], "int32");
    ASSERT_EQ(size_param["value"], expected_string.length());
}
