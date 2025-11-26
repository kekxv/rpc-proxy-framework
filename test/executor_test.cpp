#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <namedpipeapi.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#endif
#include "gtest/gtest.h"
#include "struct_manager.h"
#include "ffi_dispatcher.h"
#include "lib_manager.h"
#include "callback_manager.h"
#include "ipc_server.h"
#include "executor.h"
#include "utils/base64.h"

#include <memory>
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <map>
#include <future>
#include <atomic>
#include <queue>
#include <vector>
#include <json/json.h>

using json = Json::Value;

// -----------------------------------------------------------------------------
// Mock Classes & Setup
// -----------------------------------------------------------------------------

class DummyClientConnection : public ClientConnection
{
public:
  json last_event;
  std::string read() override { return ""; }
  bool write(const std::string& message) override { return true; }

  bool sendEvent(const json& event_json) override
  {
    last_event = event_json;
    return true;
  }

  bool isOpen() override { return true; }
};

class ExecutorTest : public ::testing::Test
{
protected:
  StructManager struct_manager;
  DummyClientConnection dummy_connection;
  CallbackManager callback_manager;
  FfiDispatcher ffi_dispatcher;
  LibManager lib_manager;
  std::string test_lib_id;

  ExecutorTest() :
    struct_manager(),
    dummy_connection(),
    callback_manager(&dummy_connection, &struct_manager),
    ffi_dispatcher(struct_manager, &callback_manager)
  {
  }

  void SetUp() override
  {
    json point_def(Json::arrayValue);
    { json m; m["name"]="x"; m["type"]="int32"; point_def.append(m); }
    { json m; m["name"]="y"; m["type"]="int32"; point_def.append(m); }
    struct_manager.register_struct("Point", point_def);

    json line_def(Json::arrayValue);
    { json m; m["name"]="p1"; m["type"]="Point"; line_def.append(m); }
    { json m; m["name"]="p2"; m["type"]="Point"; line_def.append(m); }
    struct_manager.register_struct("Line", line_def);

#ifdef _WIN32
#ifdef CMAKE_BUILD_TYPE
    std::string lib_path = "../../test_lib/" CMAKE_BUILD_TYPE "/my_lib.dll";
#else
    std::string lib_path = "../test_lib/my_lib.dll";
#endif
#elif defined(__linux__)
    std::string lib_path = "build/test_lib/my_lib.so";
    if (access(lib_path.c_str(), F_OK) != 0) lib_path = "../test_lib/my_lib.so";
#else
    std::string lib_path = "../test_lib/my_lib.dylib";
#endif
    try
    {
      test_lib_id = lib_manager.load_library(lib_path);
    }
    catch (...)
    {
      lib_path = "cmake-build-debug/test_lib/my_lib.dylib";
      try { test_lib_id = lib_manager.load_library(lib_path); }
      catch (...) {}
    }
  }

  void TearDown() override
  {
    if (!test_lib_id.empty()) lib_manager.unload_library(test_lib_id);
  }
};

TEST_F(ExecutorTest, BasicAddFunction)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "add";
  payload["return_type"] = "int32";
  
  json args(Json::arrayValue);
  { json a; a["type"]="int32"; a["value"]=10; args.append(a); }
  { json a; a["type"]="int32"; a["value"]=20; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "add"), payload);
  ASSERT_EQ(result["return"]["value"].asInt(), 30);
}

TEST_F(ExecutorTest, GreetFunction)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "greet";
  payload["return_type"] = "string";
  
  json args(Json::arrayValue);
  { json a; a["type"]="string"; a["value"]="World"; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "greet"), payload);
  ASSERT_EQ(result["return"]["value"].asString(), "Hello, World");
}

TEST_F(ExecutorTest, ProcessPointByVal)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "process_point_by_val";
  payload["return_type"] = "int32";

  json p_val; p_val["x"]=10; p_val["y"]=20;
  json args(Json::arrayValue);
  { json a; a["type"]="Point"; a["value"]=p_val; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_point_by_val"), payload);
  ASSERT_EQ(result["return"]["value"].asInt(), 30);
}

TEST_F(ExecutorTest, ProcessPointByPtr)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "process_point_by_ptr";
  payload["return_type"] = "int32";

  json p_val; p_val["x"]=5; p_val["y"]=6;
  json args(Json::arrayValue);
  { json a; a["type"]="pointer"; a["target_type"]="Point"; a["value"]=p_val; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_point_by_ptr"), payload);
  ASSERT_EQ(result["return"]["value"].asInt(), 11);
}

TEST_F(ExecutorTest, CreatePoint)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "create_point";
  payload["return_type"] = "Point";

  json args(Json::arrayValue);
  { json a; a["type"]="int32"; a["value"]=100; args.append(a); }
  { json a; a["type"]="int32"; a["value"]=200; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "create_point"), payload);
  ASSERT_EQ(result["return"]["value"]["x"].asInt(), 100);
}

TEST_F(ExecutorTest, GetLineLength)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "get_line_length";
  payload["return_type"] = "int32";

  json p1; p1["x"]=1; p1["y"]=2;
  json p2; p2["x"]=3; p2["y"]=4;
  json l_val; l_val["p1"]=p1; l_val["p2"]=p2;

  json args(Json::arrayValue);
  { json a; a["type"]="Line"; a["value"]=l_val; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "get_line_length"), payload);
  ASSERT_EQ(result["return"]["value"].asInt(), 10);
}

TEST_F(ExecutorTest, SumPoints)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "sum_points";
  payload["return_type"] = "int32";

  json points(Json::arrayValue);
  { json p; p["x"]=1; p["y"]=1; points.append(p); }
  { json p; p["x"]=2; p["y"]=2; points.append(p); }
  { json p; p["x"]=3; p["y"]=3; points.append(p); }

  json args(Json::arrayValue);
  { json a; a["type"]="pointer"; a["target_type"]="Point[]"; a["value"]=points; args.append(a); }
  { json a; a["type"]="int32"; a["value"]=3; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "sum_points"), payload);
  ASSERT_EQ(result["return"]["value"].asInt(), 12);
}

TEST_F(ExecutorTest, CreateLine)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "create_line";
  payload["return_type"] = "Line";

  json args(Json::arrayValue);
  { json a; a["type"]="int32"; a["value"]=10; args.append(a); }
  { json a; a["type"]="int32"; a["value"]=20; args.append(a); }
  { json a; a["type"]="int32"; a["value"]=30; args.append(a); }
  { json a; a["type"]="int32"; a["value"]=40; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "create_line"), payload);
  ASSERT_EQ(result["return"]["value"]["p1"]["x"].asInt(), 10);
}

TEST_F(ExecutorTest, CallbackFunction)
{
  if (test_lib_id.empty()) return;
  std::string cb_id = callback_manager.registerCallback("void", {"string", "int32"});
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "call_my_callback";
  payload["return_type"] = "void";

  json args(Json::arrayValue);
  { json a; a["type"]="callback"; a["value"]=cb_id; args.append(a); }
  { json a; a["type"]="string"; a["value"]="Hello"; args.append(a); }
  payload["args"] = args;

  ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "call_my_callback"), payload);
  ASSERT_EQ(dummy_connection.last_event["event"].asString(), "invoke_callback");
}

TEST_F(ExecutorTest, ProcessBufferInout)
{
  if (test_lib_id.empty()) return;
  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "process_buffer_inout";
  payload["return_type"] = "int32";

  json args(Json::arrayValue);
  { json a; a["type"]="buffer"; a["direction"]="inout"; a["size"]=64; a["value"]="BQ=="; args.append(a); }
  { json a; a["type"]="pointer"; a["target_type"]="int32"; a["direction"]="inout"; a["value"]=64; args.append(a); }
  payload["args"] = args;

  json result = ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "process_buffer_inout"), payload);
  ASSERT_EQ(result["return"]["value"].asInt(), 0);
}

TEST_F(ExecutorTest, TriggerReadCallback)
{
  if (test_lib_id.empty()) return;

  // typedef void(*ReadCallback)(int type, unsigned char data[], int size, void *that);
  // Corresponds to: int32, buffer_ptr(size_index=2), int32, pointer
  
  // Construct complex args_type definition
  json args_def(Json::arrayValue);
  args_def.append("int32");
  
  json buffer_arg;
  buffer_arg["type"] = "buffer_ptr";
  buffer_arg["size_arg_index"] = 2;
  args_def.append(buffer_arg);
  
  args_def.append("int32");
  args_def.append("pointer");

  std::string cb_id = callback_manager.registerCallback("void", args_def);

  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "trigger_read_callback";
  payload["return_type"] = "void";

  json args(Json::arrayValue);
  // 1. callback func ptr
  { json a; a["type"]="callback"; a["value"]=cb_id; args.append(a); }
  // 2. int type
  { json a; a["type"]="int32"; a["value"]=99; args.append(a); }
  // 3. const char* input_str (as test data)
  { json a; a["type"]="string"; a["value"]="TestBinaryData"; args.append(a); }
  // 4. void* context
  { json a; a["type"]="pointer"; a["value"]=123456; args.append(a); }
  
  payload["args"] = args;

  ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "trigger_read_callback"), payload);
  
  // Verify callback was invoked
  ASSERT_EQ(dummy_connection.last_event["event"].asString(), "invoke_callback");
  
  json cb_args = dummy_connection.last_event["payload"]["args"];
  ASSERT_EQ(cb_args.size(), 4);

  // Check arg 0: type
  EXPECT_EQ(cb_args[0]["value"].asInt(), 99);

  // Check arg 1: data buffer
  // Should be base64 encoded "TestBinaryData"
  // "TestBinaryData" -> Base64: "VGVzdEJpbmFyeURhdGE="
  EXPECT_EQ(cb_args[1]["type"].asString(), "buffer_ptr");
  EXPECT_EQ(cb_args[1]["value"].asString(), "VGVzdEJpbmFyeURhdGE=");
  EXPECT_EQ(cb_args[1]["size"].asInt(), 14);

  // Check arg 2: size (len of "TestBinaryData" is 14)
  EXPECT_EQ(cb_args[2]["value"].asInt(), 14);

  // Check arg 3: context
  EXPECT_EQ(cb_args[3]["value"].asUInt64(), 123456);
}

TEST_F(ExecutorTest, TriggerFixedReadCallback)
{
  if (test_lib_id.empty()) return;

  // typedef void(*FixedReadCallback)(unsigned char data[], void *that);
  // Corresponds to: buffer_ptr(fixed_size=4), pointer
  
  json args_def(Json::arrayValue);
  
  json buffer_arg;
  buffer_arg["type"] = "buffer_ptr";
  buffer_arg["fixed_size"] = 4;
  args_def.append(buffer_arg);
  
  args_def.append("pointer");

  std::string cb_id = callback_manager.registerCallback("void", args_def);

  json payload;
  payload["library_id"] = test_lib_id;
  payload["function_name"] = "trigger_fixed_read_callback";
  payload["return_type"] = "void";

  json args(Json::arrayValue);
  // 1. callback func ptr
  { json a; a["type"]="callback"; a["value"]=cb_id; args.append(a); }
  // 2. void* context
  { json a; a["type"]="pointer"; a["value"]=987654; args.append(a); }
  
  payload["args"] = args;

  ffi_dispatcher.call_function(lib_manager.get_function(test_lib_id, "trigger_fixed_read_callback"), payload);
  
  // Verify callback was invoked
  ASSERT_EQ(dummy_connection.last_event["event"].asString(), "invoke_callback");
  
  json cb_args = dummy_connection.last_event["payload"]["args"];
  ASSERT_EQ(cb_args.size(), 2);

  // Check arg 0: data buffer (fixed size 4)
  // Data: DE AD BE EF
  // Base64: 3q2+7w==
  EXPECT_EQ(cb_args[0]["type"].asString(), "buffer_ptr");
  EXPECT_EQ(cb_args[0]["value"].asString(), "3q2+7w==");
  EXPECT_EQ(cb_args[0]["size"].asInt(), 4);

  // Check arg 1: context
  EXPECT_EQ(cb_args[1]["value"].asUInt64(), 987654);
}
