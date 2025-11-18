import json
import os
import socket
import struct
import sys
import platform

# --- 新增：用于彩色输出的类 ---
class Colors:
  """用于在终端输出彩色文本的 ANSI 转义码"""
  RESET = '\033[0m'
  BOLD = '\033[1m'

  # 前景色
  BLACK = '\033[30m'
  RED = '\033[31m'
  GREEN = '\033[32m'
  YELLOW = '\033[33m'
  BLUE = '\033[34m'
  MAGENTA = '\033[35m'
  CYAN = '\033[36m'
  WHITE = '\033[37m'

  # 亮色
  BRIGHT_RED = '\033[91m'
  BRIGHT_GREEN = '\033[92m'
  BRIGHT_YELLOW = '\033[93m'
  BRIGHT_BLUE = '\033[94m'
  BRIGHT_MAGENTA = '\033[95m'
  BRIGHT_CYAN = '\033[96m'

class RpcProxyClient:
  def __init__(self, pipe_name):
    self.pipe_name = pipe_name
    self.sock = None
    self.request_id_counter = 0

  def connect(self):
    """根据操作系统连接到命名管道或Unix套接字"""
    if platform.system() == "Windows":
      pipe_path = f"\\\\.\\pipe\\{self.pipe_name}"
      raise NotImplementedError("Windows named pipes are not supported in this example.")
    else:
      socket_path = f"/tmp/{self.pipe_name}"
      if not os.path.exists(socket_path):
        raise FileNotFoundError(f"Socket file not found: {socket_path}")

      self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
      print(f"{Colors.BRIGHT_BLUE}Connecting to {socket_path}...{Colors.RESET}")
      self.sock.connect(socket_path)
      print(f"{Colors.BRIGHT_GREEN}Connected.{Colors.RESET}")

  def close(self):
    if self.sock:
      self.sock.close()
      self.sock = None
      print(f"{Colors.YELLOW}Connection closed.{Colors.RESET}")

  def _send_request(self, request_json):
    """发送请求并接收响应，并打印彩色日志"""
    if not self.sock:
      raise ConnectionError("Not connected to the executor.")

    # --- 打印发送的请求 ---
    print(f"{Colors.BRIGHT_CYAN}--> Sending Request [{request_json['command']}] id={request_json['request_id']}:{Colors.RESET}")
    print(json.dumps(request_json, indent=2))

    message = json.dumps(request_json).encode('utf-8')
    self.sock.sendall(struct.pack('>I', len(message)))
    self.sock.sendall(message)

    response_len_bytes = self.sock.recv(4)
    if not response_len_bytes:
      raise ConnectionError("Connection closed while waiting for response.")
    response_len = struct.unpack('>I', response_len_bytes)[0]

    response_bytes = self.sock.recv(response_len)
    response_data = json.loads(response_bytes.decode('utf-8'))

    # --- 打印接收到的响应 ---
    response_color = Colors.BRIGHT_GREEN if response_data.get("status") == "success" else Colors.BRIGHT_RED
    print(f"{response_color}<-- Received Response for id={request_json['request_id']}:{Colors.RESET}")
    print(json.dumps(response_data, indent=2))

    return response_data

  def _get_next_request_id(self):
    self.request_id_counter += 1
    return f"req-{self.request_id_counter}"

  def load_library(self, path):
    request = {
      "command": "load_library",
      "request_id": self._get_next_request_id(),
      "payload": {
        "path": path
      }
    }
    return self._send_request(request)

  def unload_library(self, library_id):
    request = {
      "command": "unload_library",
      "request_id": self._get_next_request_id(),
      "payload": {
        "library_id": library_id
      }
    }
    return self._send_request(request)

  def register_struct(self, name, definition):
    request = {
      "command": "register_struct",
      "request_id": self._get_next_request_id(),
      "payload": {
        "struct_name": name,
        "definition": definition
      }
    }
    return self._send_request(request)

  def unregister_struct(self, name):
    request = {
      "command": "unregister_struct",
      "request_id": self._get_next_request_id(),
      "payload": {
        "struct_name": name
      }
    }
    return self._send_request(request)

  def call_function(self, library_id, function_name, return_type, args):
    request = {
      "command": "call_function",
      "request_id": self._get_next_request_id(),
      "payload": {
        "library_id": library_id,
        "function_name": function_name,
        "return_type": return_type,
        "args": args
      }
    }
    return self._send_request(request)

def run_test(client, test_name, test_func, *args):
  print(f"\n{Colors.BOLD}{Colors.BRIGHT_YELLOW}--- Running Test: {test_name} ---{Colors.RESET}")
  try:
    result = test_func(client, *args)
    print(f"{Colors.BOLD}{Colors.BRIGHT_GREEN}--- Test '{test_name}' PASSED ---{Colors.RESET}")
    return result
  except Exception as e:
    print(f"{Colors.BOLD}{Colors.BRIGHT_RED}--- Test '{test_name}' FAILED: {e} ---{Colors.RESET}")
    raise

def test_register_point_struct(client):
  point_struct_definition = [
    {"name": "x", "type": "int32"},
    {"name": "y", "type": "int32"}
  ]
  print(f"{Colors.BLUE}Registering struct 'Point'...{Colors.RESET}")
  response = client.register_struct("Point", point_struct_definition)
  assert response["status"] == "success", f"Failed to register struct: {response.get('error_message')}"
  return response

def test_load_library(client, lib_path):
  print(f"{Colors.BLUE}Loading library from {lib_path}...{Colors.RESET}")
  response = client.load_library(lib_path)
  assert response["status"] == "success", f"Failed to load library: {response.get('error_message')}"
  return response["data"]["library_id"]

def test_add_function(client, library_id):
  print(f"{Colors.BLUE}Calling 'add' function (10 + 20)...{Colors.RESET}")
  args = [
    {"type": "int32", "value": 10},
    {"type": "int32", "value": 20}
  ]
  response = client.call_function(library_id, "add", "int32", args)
  assert response["status"] == "success", f"Failed to call 'add': {response.get('error_message')}"
  assert response["data"]["value"] == 30, f"Expected 30, got {response['data']['value']}"

def test_greet_function(client, library_id):
  print(f"{Colors.BLUE}Calling 'greet' function ('World')...{Colors.RESET}")
  args = [
    {"type": "string", "value": "World"}
  ]
  response = client.call_function(library_id, "greet", "string", args)
  assert response["status"] == "success", f"Failed to call 'greet': {response.get('error_message')}"
  assert response["data"]["value"] == "Hello, World", f"Expected 'Hello, World', got {response['data']['value']}"

def test_process_point_by_val(client, library_id):
  print(f"{Colors.BLUE}Calling 'process_point_by_val' function (Point{{x=5, y=10}})...{Colors.RESET}")
  point_val = {"x": 5, "y": 10}
  args = [
    {"type": "Point", "value": point_val}
  ]
  response = client.call_function(library_id, "process_point_by_val", "int32", args)
  assert response["status"] == "success", f"Failed to call 'process_point_by_val': {response.get('error_message')}"
  assert response["data"]["value"] == 15, f"Expected 15, got {response['data']['value']}"

def test_process_point_by_ptr(client, library_id):
  print(f"{Colors.BLUE}Calling 'process_point_by_ptr' function (Point{{x=10, y=20}})...{Colors.RESET}")
  point_val = {"x": 10, "y": 20}
  args = [
    {"type": "pointer", "value": point_val, "target_type": "Point"}
  ]
  response = client.call_function(library_id, "process_point_by_ptr", "int32", args)
  assert response["status"] == "success", f"Failed to call 'process_point_by_ptr': {response.get('error_message')}"
  assert response["data"]["value"] == 30, f"Expected 30, got {response['data']['value']}"

def test_create_point(client, library_id):
  print(f"{Colors.BLUE}Calling 'create_point' function (x=100, y=200)...{Colors.RESET}")
  args = [
    {"type": "int32", "value": 100},
    {"type": "int32", "value": 200}
  ]
  response = client.call_function(library_id, "create_point", "Point", args)
  assert response["status"] == "success", f"Failed to call 'create_point': {response.get('error_message')}"
  assert response["data"]["value"] == {"x": 100, "y": 200}, f"Expected {{'x': 100, 'y': 200}}, got {response['data']['value']}"

def test_register_line_struct(client):
  line_struct_definition = [
    {"name": "p1", "type": "Point"},
    {"name": "p2", "type": "Point"}
  ]
  print(f"{Colors.BLUE}Registering struct 'Line'...{Colors.RESET}")
  response = client.register_struct("Line", line_struct_definition)
  assert response["status"] == "success", f"Failed to register struct: {response.get('error_message')}"
  return response

def test_get_line_length(client, library_id):
  print(f"{Colors.BLUE}Calling 'get_line_length' function...{Colors.RESET}")
  line_val = {"p1": {"x": 1, "y": 2}, "p2": {"x": 3, "y": 4}}
  args = [
    {"type": "Line", "value": line_val}
  ]
  response = client.call_function(library_id, "get_line_length", "int32", args)
  assert response["status"] == "success", f"Failed to call 'get_line_length': {response.get('error_message')}"
  assert response["data"]["value"] == 10, f"Expected 10, got {response['data']['value']}"

def test_sum_points(client, library_id):
  print(f"{Colors.BLUE}Calling 'sum_points' function with an array of Points...{Colors.RESET}")
  points_array = [
    {"x": 1, "y": 1},
    {"x": 2, "y": 2},
    {"x": 3, "y": 3}
  ]
  args = [
    {"type": "pointer", "value": points_array, "target_type": "Point[]"},
    {"type": "int32", "value": len(points_array)}
  ]
  response = client.call_function(library_id, "sum_points", "int32", args)
  assert response["status"] == "success", f"Failed to call 'sum_points': {response.get('error_message')}"
  assert response["data"]["value"] == 12, f"Expected 12, got {response['data']['value']}"

def test_create_line(client, library_id):
  print(f"{Colors.BLUE}Calling 'create_line' function...{Colors.RESET}")
  args = [
    {"type": "int32", "value": 10},
    {"type": "int32", "value": 11},
    {"type": "int32", "value": 12},
    {"type": "int32", "value": 13}
  ]
  response = client.call_function(library_id, "create_line", "Line", args)
  assert response["status"] == "success", f"Failed to call 'create_line': {response.get('error_message')}"
  expected_line = {"p1": {"x": 10, "y": 11}, "p2": {"x": 12, "y": 13}}
  assert response["data"]["value"] == expected_line, f"Expected {expected_line}, got {response['data']['value']}"

def main():
  if len(sys.argv) != 2:
    print(f"{Colors.BRIGHT_RED}Usage: python {sys.argv[0]} <pipe_name>{Colors.RESET}")
    sys.exit(1)

  pipe_name = sys.argv[1]

  lib_ext = {"Linux": ".so", "Darwin": ".dylib", "Windows": ".dll"}[platform.system()]
  lib_path = os.path.abspath(f"build/test_lib/my_lib{lib_ext}")

  if not os.path.exists(lib_path):
    lib_path = os.path.abspath(f"cmake-build-debug/test_lib/my_lib{lib_ext}")
  if not os.path.exists(lib_path):
    print(f"{Colors.BRIGHT_RED}Error: Test library not found at {lib_path}{Colors.RESET}")
    print(f"{Colors.YELLOW}Please build the test library first.{Colors.RESET}")
    sys.exit(1)

  client = RpcProxyClient(pipe_name)
  library_id = None

  try:
    client.connect()

    # Run tests
    run_test(client, "Register Point Struct", test_register_point_struct)
    run_test(client, "Register Line Struct", test_register_line_struct)
    library_id = run_test(client, "Load Library", test_load_library, lib_path)
    run_test(client, "Add Function", test_add_function, library_id)
    run_test(client, "Greet Function", test_greet_function, library_id)
    run_test(client, "Process Point By Value", test_process_point_by_val, library_id)
    run_test(client, "Process Point By Pointer", test_process_point_by_ptr, library_id)
    run_test(client, "Create Point Function", test_create_point, library_id)
    run_test(client, "Get Line Length Function", test_get_line_length, library_id)
    run_test(client, "Sum Points Function", test_sum_points, library_id)
    run_test(client, "Create Line Function", test_create_line, library_id)

  except Exception as e:
    print(f"\n{Colors.BOLD}{Colors.BRIGHT_RED}An error occurred during tests: {e}{Colors.RESET}")
  finally:
    if library_id:
      print(f"\n{Colors.YELLOW}Unloading library: {library_id}{Colors.RESET}")
      client.unload_library(library_id)

    print(f"{Colors.YELLOW}Unregistering struct 'Line'{Colors.RESET}")
    client.unregister_struct("Line")

    print(f"{Colors.YELLOW}Unregistering struct 'Point'{Colors.RESET}")
    client.unregister_struct("Point")

    client.close()

if __name__ == "__main__":
  main()