import json
import os
import socket
import struct
import sys
import platform
import threading
import queue
import time
import base64

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
    self.response_queue = queue.Queue()
    self.event_queue = queue.Queue()
    self.receiver_thread = None
    self.running = False
    self.pending_requests = {} # To store futures for responses

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
      
      self.running = True
      self.receiver_thread = threading.Thread(target=self._receive_messages, daemon=True)
      self.receiver_thread.start()

  def close(self):
    self.running = False
    if self.sock:
      self.sock.shutdown(socket.SHUT_RDWR) # Signal to close both ends
      self.sock.close()
      self.sock = None
    if self.receiver_thread and self.receiver_thread.is_alive():
      self.receiver_thread.join(timeout=1) # Wait for receiver thread to finish
    print(f"{Colors.BRIGHT_CYAN}Connection closed.{Colors.RESET}")

  def _receive_messages(self):
    """在单独的线程中持续接收消息"""
    while self.running:
      try:
        # Read message length
        len_bytes = self.sock.recv(4)
        if not len_bytes:
          print(f"{Colors.YELLOW}Executor disconnected.{Colors.RESET}")
          self.running = False
          break
        
        message_len = struct.unpack('>I', len_bytes)[0]
        
        # Read message data
        message_bytes = b''
        while len(message_bytes) < message_len:
          packet = self.sock.recv(message_len - len(message_bytes))
          if not packet:
            print(f"{Colors.RED}Executor disconnected during message read.{Colors.RESET}")
            self.running = False
            break
          message_bytes += packet
        
        if not self.running: # Check if shutdown was initiated during read
            break

        message_data = json.loads(message_bytes.decode('utf-8'))

        if "request_id" in message_data:
          # This is a response to a request
          req_id = message_data["request_id"]
          if req_id in self.pending_requests:
            self.pending_requests[req_id].set_result(message_data)
            del self.pending_requests[req_id]
          else:
            print(f"{Colors.YELLOW}Received unexpected response for ID {req_id}:{Colors.RESET}")
            print(json.dumps(message_data, indent=None))
        elif "event" in message_data:
          # This is an asynchronous event
          self.event_queue.put(message_data)
          print(f"{Colors.MAGENTA}<-- Received Event [{message_data['event']}]:{Colors.RESET}")
          print(json.dumps(message_data, indent=None))
        else:
          print(f"{Colors.YELLOW}Received unknown message type:{Colors.RESET}")
          print(json.dumps(message_data, indent=None))

      except socket.timeout:
        pass # No data, continue loop
      except (socket.error, json.JSONDecodeError) as e:
        if self.running:
          print(f"{Colors.RED}Error in receiver thread: {e}{Colors.RESET}")
        self.running = False
        break
    print(f"{Colors.BRIGHT_CYAN}Receiver thread stopped.{Colors.RESET}")


  def _send_request(self, request_json):
    """发送请求并等待响应"""
    if not self.sock or not self.running:
      raise ConnectionError("Not connected to the executor or connection closed.")

    req_id = request_json["request_id"]
    future_response = EventWithResult() # Use the custom event class that can hold a result
    self.pending_requests[req_id] = future_response

    # --- 打印发送的请求 ---
    print(f"{Colors.BRIGHT_CYAN}--> Sending Request [{request_json['command']}] id={req_id}:{Colors.RESET}")
    print(json.dumps(request_json, indent=None))

    message = json.dumps(request_json).encode('utf-8')
    try:
      self.sock.sendall(struct.pack('>I', len(message)))
      self.sock.sendall(message)
    except socket.error as e:
      del self.pending_requests[req_id]
      raise ConnectionError(f"Failed to send request: {e}")

    # Wait for the response
    future_response.wait(timeout=10) # Wait for response, with a timeout
    if not future_response.is_set():
        if req_id in self.pending_requests:
            del self.pending_requests[req_id]
        raise TimeoutError(f"Timeout waiting for response for request ID {req_id}")
    
    response_data = future_response.result() # Get the result set by the receiver thread

    # --- 打印接收到的响应 ---
    response_color = Colors.BRIGHT_GREEN if response_data.get("status") == "success" else Colors.BRIGHT_RED
    print(f"{response_color}<-- Received Response for id={req_id}:{Colors.RESET}")
    print(json.dumps(response_data, indent=None))

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

  def register_callback(self, return_type, args_type):
    request = {
      "command": "register_callback",
      "request_id": self._get_next_request_id(),
      "payload": {
        "return_type": return_type,
        "args_type": args_type
      }
    }
    return self._send_request(request)

  def unregister_callback(self, callback_id):
    request = {
      "command": "unregister_callback",
      "request_id": self._get_next_request_id(),
      "payload": {
        "callback_id": callback_id
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

# --- Custom Event class for threading.Event with result ---
class EventWithResult(threading.Event):
    def __init__(self):
        super().__init__()
        self._result = None

    def set_result(self, result):
        self._result = result
        self.set()

    def result(self):
        return self._result

def run_test(client, test_name, test_func, *args):
  print(f"\n{Colors.BOLD}{Colors.BRIGHT_CYAN}--- Running Test: {test_name} ---{Colors.RESET}")
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
  assert response["data"]["return"]["value"] == 30, f"Expected 30, got {response['data']['value']}"

def test_greet_function(client, library_id):
  print(f"{Colors.BLUE}Calling 'greet' function ('World')...{Colors.RESET}")
  args = [
    {"type": "string", "value": "World"}
  ]
  response = client.call_function(library_id, "greet", "string", args)
  assert response["status"] == "success", f"Failed to call 'greet': {response.get('error_message')}"
  assert response["data"]["return"]["value"] == "Hello, World", f"Expected 'Hello, World', got {response['data']['value']}"

def test_process_point_by_val(client, library_id):
  print(f"{Colors.BLUE}Calling 'process_point_by_val' function (Point{{x=5, y=10}})...{Colors.RESET}")
  point_val = {"x": 5, "y": 10}
  args = [
    {"type": "Point", "value": point_val}
  ]
  response = client.call_function(library_id, "process_point_by_val", "int32", args)
  assert response["status"] == "success", f"Failed to call 'process_point_by_val': {response.get('error_message')}"
  assert response["data"]["return"]["value"] == 15, f"Expected 15, got {response['data']['value']}"

def test_process_point_by_ptr(client, library_id):
  print(f"{Colors.BLUE}Calling 'process_point_by_ptr' function (Point{{x=10, y=20}})...{Colors.RESET}")
  point_val = {"x": 10, "y": 20}
  args = [
    {"type": "pointer", "value": point_val, "target_type": "Point"}
  ]
  response = client.call_function(library_id, "process_point_by_ptr", "int32", args)
  assert response["status"] == "success", f"Failed to call 'process_point_by_ptr': {response.get('error_message')}"
  assert response["data"]["return"]["value"] == 30, f"Expected 30, got {response['data']['value']}"

def test_create_point(client, library_id):
  print(f"{Colors.BLUE}Calling 'create_point' function (x=100, y=200)...{Colors.RESET}")
  args = [
    {"type": "int32", "value": 100},
    {"type": "int32", "value": 200}
  ]
  response = client.call_function(library_id, "create_point", "Point", args)
  assert response["status"] == "success", f"Failed to call 'create_point': {response.get('error_message')}"
  assert response["data"]["return"]["value"] == {"x": 100, "y": 200}, f"Expected {{'x': 100, 'y': 200}}, got {response['data']['value']}"

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
  assert response["data"]["return"]["value"] == 10, f"Expected 10, got {response['data']['value']}"

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
  assert response["data"]["return"]["value"] == 12, f"Expected 12, got {response['data']['value']}"

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
  assert response["data"]["return"]["value"] == expected_line, f"Expected {expected_line}, got {response['data']['value']}"

def test_callback_functionality(client, library_id):
  print(f"{Colors.BLUE}Registering callback signature (void, [string, int32])...{Colors.RESET}")
  response = client.register_callback("void", ["string", "int32"])
  assert response["status"] == "success", f"Failed to register callback: {response.get('error_message')}"
  callback_id = response["data"]["callback_id"]
  print(f"{Colors.GREEN}Callback registered with ID: {callback_id}{Colors.RESET}")

  print(f"{Colors.BLUE}Calling 'call_my_callback' function with registered callback...{Colors.RESET}")
  args = [
    {"type": "callback", "value": callback_id},
    {"type": "string", "value": "Hello from Python!"}
  ]
  response = client.call_function(library_id, "call_my_callback", "void", args)
  assert response["status"] == "success", f"Failed to call 'call_my_callback': {response.get('error_message')}"
  print(f"{Colors.GREEN}call_my_callback returned successfully, expecting one event...{Colors.RESET}")

  # Explicitly retrieve the event for this test to ensure it's processed
  try:
    event = client.event_queue.get(timeout=5)
    assert event["event"] == "invoke_callback", f"Expected invoke_callback event, got {event['event']}"
    assert event["payload"]["callback_id"] == callback_id, f"Expected callback_id {callback_id}, got {event['payload']['callback_id']}"
    print(f"{Colors.GREEN}Successfully received and verified the single invoke_callback event.{Colors.RESET}")
  except queue.Empty:
    raise TimeoutError("Did not receive invoke_callback event within timeout in single callback test.")

  # Clear any remaining events that might have buffered up unexpectedly
  while not client.event_queue.empty():
      client.event_queue.get_nowait()
      print(f"{Colors.YELLOW}Warning: Cleared unexpected event from queue.{Colors.RESET}")


  print(f"{Colors.BLUE}Unregistering callback: {callback_id}{Colors.RESET}")
  response = client.unregister_callback(callback_id)
  assert response["status"] == "success", f"Failed to unregister callback: {response.get('error_message')}"
  print(f"{Colors.GREEN}Callback unregistered successfully.{Colors.RESET}")

def test_multi_callback_functionality(client, library_id):
  print(f"{Colors.BLUE}Registering multi-callback signature (void, [string, int32])...{Colors.RESET}")
  response = client.register_callback("void", ["string", "int32"])
  assert response["status"] == "success", f"Failed to register multi-callback: {response.get('error_message')}"
  multi_callback_id = response["data"]["callback_id"]
  print(f"{Colors.GREEN}Multi-callback registered with ID: {multi_callback_id}{Colors.RESET}")

  num_calls = 3 # Number of times the C function will call back
  print(f"{Colors.BLUE}Calling 'call_multi_callbacks' function with registered callback {num_calls} times...{Colors.RESET}")
  args = [
    {"type": "callback", "value": multi_callback_id},
    {"type": "int32", "value": num_calls}
  ]
  response = client.call_function(library_id, "call_multi_callbacks", "void", args)
  assert response["status"] == "success", f"Failed to call 'call_multi_callbacks': {response.get('error_message')}"
  print(f"{Colors.GREEN}call_multi_callbacks returned successfully, expecting {num_calls} events...{Colors.RESET}")

  # Verify multiple callback events
  received_events = []
  for i in range(num_calls):
    try:
      # Wait for each event
      event = client.event_queue.get(timeout=5)
      assert event["event"] == "invoke_callback", f"Expected invoke_callback event, got {event['event']}"
      assert event["payload"]["callback_id"] == multi_callback_id, f"Expected callback_id {multi_callback_id}, got {event['payload']['callback_id']}"
      
      event_args = event["payload"]["args"]
      assert len(event_args) == 2, f"Expected 2 args, got {len(event_args)}"
      
      expected_message = f"Message from native code, call {i + 1}"
      expected_value = i + 1
      
      assert event_args[0]["type"] == "string" and event_args[0]["value"] == expected_message, \
          f"Unexpected first arg for call {i+1}: got '{event_args[0]['value']}', expected '{expected_message}'"
      assert event_args[1]["type"] == "int32" and event_args[1]["value"] == expected_value, \
          f"Unexpected second arg for call {i+1}: got {event_args[1]['value']}, expected {expected_value}"
      
      print(f"{Colors.GREEN}  Received and verified invoke_callback event {i+1}/{num_calls}: msg='{event_args[0]['value']}', val={event_args[1]['value']}{Colors.RESET}")
      received_events.append(event)
    except queue.Empty:
      raise TimeoutError(f"Did not receive invoke_callback event {i+1} within timeout.")

  assert len(received_events) == num_calls, f"Expected {num_calls} events, but received {len(received_events)}"

  print(f"{Colors.BLUE}Unregistering multi-callback: {multi_callback_id}{Colors.RESET}")
  response = client.unregister_callback(multi_callback_id)
  assert response["status"] == "success", f"Failed to unregister multi-callback: {response.get('error_message')}"
  print(f"{Colors.GREEN}Multi-callback unregistered successfully.{Colors.RESET}")


def test_process_buffer_inout(client, library_id):
  print(f"{Colors.BLUE}Calling 'process_buffer_inout' function with INOUT buffer...{Colors.RESET}")
  
  buffer_capacity = 64
  input_raw_data = b'\x05' # Input byte for the C function
  input_base64 = base64.b64encode(input_raw_data).decode('utf-8') # "BQ=="

  # C function writes {0xAA, 0x06, 0xDE, 0xAD} for input 0x05
  expected_raw_output_prefix = b'\xAA\x06\xDE\xAD'

  args = [
      # Arg 0: The inout buffer
      {"type": "buffer", "direction": "inout", "size": buffer_capacity, "value": input_base64},
      # Arg 1: The size, passed as an INOUT pointer
      {"type": "pointer", "target_type": "int32", "direction": "inout", "value": buffer_capacity}
  ]
  
  response = client.call_function(library_id, "process_buffer_inout", "int32", args)
  assert response["status"] == "success", f"Failed to call 'process_buffer_inout': {response.get('error_message')}"
  
  # --- Verify the new complex response format ---
  
  # 1. Verify the direct return value (the status code)
  return_data = response["data"]["return"]
  assert return_data["type"] == "int32", f"Expected return type int32, got {return_data['type']}"
  assert return_data["value"] == 0, f"Expected return value 0 (success), got {return_data['value']}"
  
  # 2. Verify the output parameters
  out_params = response["data"]["out_params"]
  assert len(out_params) == 2, f"Expected 2 output parameters, got {len(out_params)}"
  
  # Find the buffer and the size from the out_params array
  # Their order is not guaranteed, so we check by index.
  out_buffer_param = None
  out_size_val = None
  for param in out_params:
      if param["index"] == 0: # This was our buffer
          out_buffer_param = param
      elif param["index"] == 1: # This was our size
          out_size_val = param["value"]

  assert out_buffer_param is not None, "Did not receive output buffer in response"
  assert out_size_val is not None, "Did not receive output size in response"

  # 3. Assert the values
  assert out_buffer_param["type"] == "buffer", f"Expected output buffer type 'buffer', got '{out_buffer_param['type']}'"
  
  # The buffer value is a base64 encoded string
  output_base64_value = out_buffer_param["value"]
  
  # Decode the base64 output and verify its content
  decoded_output_bytes = base64.b64decode(output_base64_value)
  
  # Check only the prefix that was actually written by the C function
  assert decoded_output_bytes.startswith(expected_raw_output_prefix), \
      f"Expected decoded buffer to start with {expected_raw_output_prefix.hex()}, got {decoded_output_bytes[:len(expected_raw_output_prefix)].hex()}"
  
  # The rest of the buffer should be zeros (due to zero-initialization)
  assert len(decoded_output_bytes) == buffer_capacity, f"Expected buffer length {buffer_capacity}, got {len(decoded_output_bytes)}"
  assert decoded_output_bytes[len(expected_raw_output_prefix):] == b'\x00' * (buffer_capacity - len(expected_raw_output_prefix)), \
      f"Expected remaining buffer to be zeros, but got {decoded_output_bytes[len(expected_raw_output_prefix):].hex()}"

  assert out_size_val == len(expected_raw_output_prefix), f"Expected updated size {len(expected_raw_output_prefix)}, got {out_size_val}"

  print(f"{Colors.GREEN}Buffer content verified (prefix: {expected_raw_output_prefix.hex()}, Size: {out_size_val}){Colors.RESET}")


def main():
  if len(sys.argv) != 2:
    print(f"{Colors.BRIGHT_RED}Usage: python {sys.argv[0]} <pipe_name>{Colors.RESET}")
    sys.exit(1)

  pipe_name = sys.argv[1]

  lib_ext = {"Linux": ".so", "Darwin": ".dylib", "Windows": ".dll"}[platform.system()]
  lib_path = os.path.abspath(f"build/test_lib/my_lib{lib_ext}")

  # Fallback for common build directories
  if not os.path.exists(lib_path):
    lib_path = os.path.abspath(f"cmake-build-debug/test_lib/my_lib{lib_ext}")
  if not os.path.exists(lib_path):
    lib_path = os.path.abspath(f"test_lib/build/my_lib{lib_ext}") # For direct test_lib build
  
  if not os.path.exists(lib_path):
    print(f"{Colors.BRIGHT_RED}Error: Test library not found at {lib_path}{Colors.RESET}")
    print(f"{Colors.YELLOW}Please build the test library first.{Colors.RESET}")
    sys.exit(1)

  client = RpcProxyClient(pipe_name)
  library_id = None
  # callback_id = None # Removed explicit cleanup, handled by unregister_callback in test functions

  try:
    time.sleep(1) # Add a small delay to allow the executor to start
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
    run_test(client, "Single Callback Functionality", test_callback_functionality, library_id) # Renamed
    run_test(client, "Multi-Callback Functionality", test_multi_callback_functionality, library_id) # New test
    run_test(client, "Process Buffer Inout Functionality", test_process_buffer_inout, library_id)

  except Exception as e:
    print(f"\n{Colors.BOLD}{Colors.BRIGHT_RED}An error occurred during tests: {e}{Colors.RESET}")
  finally:
    if library_id:
      print(f"\n{Colors.BRIGHT_CYAN}Unloading library: {library_id}{Colors.RESET}")
      client.unload_library(library_id)

    print(f"{Colors.BRIGHT_CYAN}Unregistering struct 'Line'{Colors.RESET}")
    client.unregister_struct("Line")

    print(f"{Colors.BRIGHT_CYAN}Unregistering struct 'Point'{Colors.RESET}")
    client.unregister_struct("Point")

    client.close()

if __name__ == "__main__":
  main()