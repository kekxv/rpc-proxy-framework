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

// Add more tests here for structs, pointers, arrays, etc.
// For example:
// TEST_F(ExecutorTest, ProcessPointByVal) { ... }
// TEST_F(ExecutorTest, ProcessPointByPtr) { ... }
// TEST_F(ExecutorTest, CreatePoint) { ... }
// TEST_F(ExecutorTest, GetLineLength) { ... }
// TEST_F(ExecutorTest, SumPoints) { ... }
// TEST_F(ExecutorTest, CreateLine) { ... }
