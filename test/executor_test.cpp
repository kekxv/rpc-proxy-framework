#include "gtest/gtest.h"
#include "struct_manager.h"
#include "ffi_dispatcher.h"
#include "lib_manager.h"
#include <memory>
#include <iostream>

// Forward declarations for test_lib functions (for direct calling if needed, though FfiDispatcher will handle it)
extern "C" {
int add(int a, int b);
char* greet(char* name);
// Add other test_lib functions as needed
}

class ExecutorTest : public ::testing::Test
{
protected:
  StructManager struct_manager;
  FfiDispatcher ffi_dispatcher;
  LibManager lib_manager;

  ExecutorTest() : ffi_dispatcher(struct_manager)
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

    // Load the test library
    std::string lib_path = "../test_lib/my_lib.dylib"; // Adjust path as necessary
    test_lib_id = lib_manager.load_library(lib_path);
    ASSERT_FALSE(test_lib_id.empty());
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
  // FfiDispatcher directly returns the result, not a status wrapper
  ASSERT_EQ(result["type"], "int32");
  ASSERT_EQ(result["value"], 30);
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
  ASSERT_EQ(result["type"], "string");
  ASSERT_EQ(result["value"], "Hello, World");
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
  ASSERT_EQ(result["type"], "int32");
  ASSERT_EQ(result["value"], 30);
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
  ASSERT_EQ(result["type"], "int32");
  ASSERT_EQ(result["value"], 11);
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
  ASSERT_EQ(result["type"], "Point");
  ASSERT_EQ(result["value"]["x"], 100);
  ASSERT_EQ(result["value"]["y"], 200);
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
  ASSERT_EQ(result["type"], "int32");
  ASSERT_EQ(result["value"], 10); // 1+2+3+4 = 10
}

TEST_F(ExecutorTest, SumPoints)
{
  typedef struct
  {
    int32_t x;
    int32_t y;
  } Point;
  // Prepare an array of Point structs in memory
  // This part is tricky as FfiDispatcher expects a pointer to already allocated memory for arrays.
  // For testing purposes, we can simulate this by creating a temporary array.
  // In a real scenario, the controller would manage this memory and pass the pointer.
  // For this test, we'll create a small array of Points and pass its address.
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
  ASSERT_EQ(result["type"], "int32");
  ASSERT_EQ(result["value"], 12); // (1+1) + (2+2) + (3+3) = 2 + 4 + 6 = 12
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
  ASSERT_EQ(result["type"], "Line");
  ASSERT_EQ(result["value"]["p1"]["x"], 10);
  ASSERT_EQ(result["value"]["p1"]["y"], 20);
  ASSERT_EQ(result["value"]["p2"]["x"], 30);
  ASSERT_EQ(result["value"]["p2"]["y"], 40);
}
