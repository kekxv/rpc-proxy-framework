import json
import os
import socket
import struct
import sys
import platform

class RpcProxyClient:
    def __init__(self, pipe_name):
        self.pipe_name = pipe_name
        self.sock = None
        self.request_id_counter = 0

    def connect(self):
        """根据操作系统连接到命名管道或Unix套接字"""
        if platform.system() == "Windows":
            # Windows 命名管道的路径格式
            pipe_path = f"\\\\.\\pipe\\{self.pipe_name}"
            # 在Windows上，我们需要使用 pywin32 库来操作命名管道
            # 为简单起见，此示例不支持Windows。可以添加 pywin32 来支持。
            raise NotImplementedError("Windows named pipes are not supported in this example.")
        else:
            # Linux/macOS Unix Domain Socket
            socket_path = f"/tmp/{self.pipe_name}" # The executor explicitly creates the socket in /tmp
            if not os.path.exists(socket_path):
                 raise FileNotFoundError(f"Socket file not found: {socket_path}")

            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            print(f"Connecting to {socket_path}...")
            self.sock.connect(socket_path)
            print("Connected.")

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None
            print("Connection closed.")

    def _send_request(self, request_json):
        """发送请求并接收响应"""
        if not self.sock:
            raise ConnectionError("Not connected to the executor.")

        message = json.dumps(request_json).encode('utf-8')
        # 首先发送4字节的消息长度头
        self.sock.sendall(struct.pack('>I', len(message)))
        # 然后发送消息体
        self.sock.sendall(message)

        # 首先接收4字节的响应长度头
        response_len_bytes = self.sock.recv(4)
        if not response_len_bytes:
            raise ConnectionError("Connection closed while waiting for response.")
        response_len = struct.unpack('>I', response_len_bytes)[0]

        # 接收完整的响应体
        response_bytes = self.sock.recv(response_len)
            
        return json.loads(response_bytes.decode('utf-8'))

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
        """
        注册一个结构体类型。
        definition: [ {"name": "member_name", "type": "member_type"}, ... ]
        """
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
    print(f"\n--- Running Test: {test_name} ---")
    try:
        result = test_func(client, *args)
        print(f"--- Test '{test_name}' PASSED ---")
        return result
    except Exception as e:
        print(f"--- Test '{test_name}' FAILED: {e} ---")
        raise # Re-raise the exception to stop further tests if one fails

def test_register_point_struct(client):
    point_struct_definition = [
        {"name": "x", "type": "int32"},
        {"name": "y", "type": "int32"}
    ]
    print("Registering struct 'Point'...")
    response = client.register_struct("Point", point_struct_definition)
    print("Register response:", response)
    assert response["status"] == "success", f"Failed to register struct: {response.get('error_message')}"
    return response

def test_load_library(client, lib_path):
    print(f"Loading library from {lib_path}...")
    response = client.load_library(lib_path)
    print("Load response:", response)
    assert response["status"] == "success", f"Failed to load library: {response.get('error_message')}"
    return response["data"]["library_id"]

def test_add_function(client, library_id):
    print("Calling 'add' function (10 + 20)...")
    args = [
        {"type": "int32", "value": 10},
        {"type": "int32", "value": 20}
    ]
    response = client.call_function(library_id, "add", "int32", args)
    print("Add function response:", response)
    assert response["status"] == "success", f"Failed to call 'add': {response.get('error_message')}"
    assert response["data"]["value"] == 30, f"Expected 30, got {response['data']['value']}"

def test_greet_function(client, library_id):
    print("Calling 'greet' function ('World')...")
    args = [
        {"type": "string", "value": "World"}
    ]
    response = client.call_function(library_id, "greet", "string", args)
    print("Greet function response:", response)
    assert response["status"] == "success", f"Failed to call 'greet': {response.get('error_message')}"
    assert response["data"]["value"] == "Hello, World", f"Expected 'Hello, World', got {response['data']['value']}"

def test_process_point_by_val(client, library_id):
    print("Calling 'process_point_by_val' function (Point{x=5, y=10})...")
    point_val = {"x": 5, "y": 10}
    args = [
        {"type": "Point", "value": point_val}
    ]
    response = client.call_function(library_id, "process_point_by_val", "int32", args)
    print("Process Point By Value response:", response)
    assert response["status"] == "success", f"Failed to call 'process_point_by_val': {response.get('error_message')}"
    assert response["data"]["value"] == 15, f"Expected 15, got {response['data']['value']}"

def test_process_point_by_ptr(client, library_id):
    print("Calling 'process_point_by_ptr' function (Point{x=10, y=20})...")
    point_val = {"x": 10, "y": 20}
    args = [
        {"type": "pointer", "value": point_val, "target_type": "Point"}
    ]
    response = client.call_function(library_id, "process_point_by_ptr", "int32", args)
    print("Process Point By Pointer response:", response)
    assert response["status"] == "success", f"Failed to call 'process_point_by_ptr': {response.get('error_message')}"
    assert response["data"]["value"] == 30, f"Expected 30, got {response['data']['value']}"

def test_create_point(client, library_id):
    print("Calling 'create_point' function (x=100, y=200)...")
    args = [
        {"type": "int32", "value": 100},
        {"type": "int32", "value": 200}
    ]
    response = client.call_function(library_id, "create_point", "Point", args)
    print("Create Point response:", response)
    assert response["status"] == "success", f"Failed to call 'create_point': {response.get('error_message')}"
    assert response["data"]["value"] == {"x": 100, "y": 200}, f"Expected {{'x': 100, 'y': 200}}, got {response['data']['value']}"

def test_register_line_struct(client):
    line_struct_definition = [
        {"name": "p1", "type": "Point"},
        {"name": "p2", "type": "Point"}
    ]
    print("Registering struct 'Line'...")
    response = client.register_struct("Line", line_struct_definition)
    print("Register response:", response)
    assert response["status"] == "success", f"Failed to register struct: {response.get('error_message')}"
    return response

def test_get_line_length(client, library_id):
    print("Calling 'get_line_length' function (Line{{p1={{x=1, y=2}}, p2={{x=3, y=4}}}})...")
    line_val = {"p1": {"x": 1, "y": 2}, "p2": {"x": 3, "y": 4}}
    args = [
        {"type": "Line", "value": line_val}
    ]
    response = client.call_function(library_id, "get_line_length", "int32", args)
    print("Get Line Length response:", response)
    assert response["status"] == "success", f"Failed to call 'get_line_length': {response.get('error_message')}"
    assert response["data"]["value"] == 10, f"Expected 10, got {response['data']['value']}"

def test_sum_points(client, library_id):
    print("Calling 'sum_points' function with an array of Points...")
    points_array = [
        {"x": 1, "y": 1},
        {"x": 2, "y": 2},
        {"x": 3, "y": 3}
    ]
    # The C function expects a pointer to the first element and the count
    args = [
        {"type": "pointer", "value": points_array, "target_type": "Point[]"}, # Custom type for array of structs
        {"type": "int32", "value": len(points_array)}
    ]
    response = client.call_function(library_id, "sum_points", "int32", args)
    print("Sum Points response:", response)
    assert response["status"] == "success", f"Failed to call 'sum_points': {response.get('error_message')}"
    assert response["data"]["value"] == (1+1+2+2+3+3), f"Expected {1+1+2+2+3+3}, got {response['data']['value']}"

def test_create_line(client, library_id):
    print("Calling 'create_line' function (p1={{x=10, y=11}}, p2={{x=12, y=13}})...")
    args = [
        {"type": "int32", "value": 10},
        {"type": "int32", "value": 11},
        {"type": "int32", "value": 12},
        {"type": "int32", "value": 13}
    ]
    response = client.call_function(library_id, "create_line", "Line", args)
    print("Create Line response:", response)
    assert response["status"] == "success", f"Failed to call 'create_line': {response.get('error_message')}"
    expected_line = {"p1": {"x": 10, "y": 11}, "p2": {"x": 12, "y": 13}}
    assert response["data"]["value"] == expected_line, f"Expected {expected_line}, got {response['data']['value']}"



def main():
    if len(sys.argv) != 2:
        print("Usage: python controller.py <pipe_name>")
        sys.exit(1)

    pipe_name = sys.argv[1]
    
    lib_ext = {"Linux": ".so", "Darwin": ".dylib", "Windows": ".dll"}[platform.system()]
    lib_path = os.path.abspath(f"build/test_lib/my_lib{lib_ext}")

    if not os.path.exists(lib_path):
        print(f"Error: Test library not found at {lib_path}")
        print("Please build the test library first by running: cd test_lib && cmake . && make")
        sys.exit(1)
        
    client = RpcProxyClient(pipe_name)
    library_id = None
    
    try:
        client.connect()

        # Run tests
        run_test(client, "Register Point Struct", test_register_point_struct)
        run_test(client, "Register Line Struct", test_register_line_struct) # New test
        library_id = run_test(client, "Load Library", test_load_library, lib_path)
        run_test(client, "Add Function", test_add_function, library_id)
        run_test(client, "Greet Function", test_greet_function, library_id)
        run_test(client, "Process Point By Value", test_process_point_by_val, library_id)
        run_test(client, "Process Point By Pointer", test_process_point_by_ptr, library_id)
        run_test(client, "Create Point Function", test_create_point, library_id)
        run_test(client, "Get Line Length Function", test_get_line_length, library_id) # New test
        run_test(client, "Sum Points Function", test_sum_points, library_id) # New test
        run_test(client, "Create Line Function", test_create_line, library_id) # New test

    except Exception as e:
        print(f"\nAn error occurred during tests: {e}")
    finally:
        if library_id:
            print(f"\nUnloading library: {library_id}")
            response = client.unload_library(library_id)
            print("Unload response:", response)
        
        print("Unregistering struct 'Line'") # New unregister
        response = client.unregister_struct("Line")
        print("Unregister response:", response)

        print("Unregistering struct 'Point'")
        response = client.unregister_struct("Point")
        print("Unregister response:", response)
        
        client.close()

if __name__ == "__main__":
    main()
