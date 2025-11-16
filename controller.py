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
            pipe_path = f"\\\\.\\pipe\\{self.pipe_name}" # Corrected escaping for Windows path
            # 在Windows上，我们需要使用 pywin32 库来操作命名管道
            # 为简单起见，此示例不支持Windows。可以添加 pywin32 来支持。
            raise NotImplementedError("Windows named pipes are not supported in this example.")
        else:
            # Linux/macOS Unix Domain Socket
            # executor程序创建的socket文件通常在当前工作目录或/tmp下
            # 此处假设在 /tmp 目录下
            socket_path = f"/tmp/{self.pipe_name}"
            if not os.path.exists(socket_path):
                 # 如果不在/tmp下，尝试在当前项目的build目录下
                 script_dir = os.path.dirname(os.path.realpath(__file__))
                 socket_path = os.path.join(script_dir, "build", self.pipe_name)
                 if not os.path.exists(socket_path):
                     raise FileNotFoundError(f"Socket file not found in /tmp or build directory: {self.pipe_name}")

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

def main():
    if len(sys.argv) != 2:
        print("Usage: python controller.py <pipe_name>")
        sys.exit(1)

    pipe_name = sys.argv[1]
    
    # 确定测试库的路径
    # 假设此脚本在项目根目录运行
    lib_ext = {"Linux": ".so", "Darwin": ".dylib", "Windows": ".dll"}[platform.system()]
    lib_path = os.path.abspath(f"test_lib/build/my_lib{lib_ext}")

    if not os.path.exists(lib_path):
        print(f"Error: Test library not found at {lib_path}")
        print("Please build the test library first by running: cd test_lib && cmake . && make")
        sys.exit(1)

    client = RpcProxyClient(pipe_name)

    try:
        client.connect()

        # 1. 加载库
        print(f"\nLoading library: {lib_path}")
        response = client.load_library(lib_path)
        print("Response:", response)
        
        if response.get("status") != "success":
            raise RuntimeError(f"Failed to load library: {response.get('error_message')}")
        
        library_id = response["data"]["library_id"]
        print(f"Library loaded with ID: {library_id}")

        # 2. 调用 add(10, 20)
        print("\nCalling function 'add' with args (10, 20)")
        response = client.call_function(
            library_id,
            "add",
            "int32",
            [
                {"type": "int32", "value": 10},
                {"type": "int32", "value": 20}
            ]
        )
        print("Response:", response)
        if response.get("status") == "success":
            print(f"Result of add(10, 20) is: {response['data']['value']}")

        # 3. 调用 greet("World")
        print("\nCalling function 'greet' with arg ('World')")
        response = client.call_function(
            library_id,
            "greet",
            "string",
            [
                {"type": "string", "value": "World"}
            ]
        )
        print("Response:", response)
        if response.get("status") == "success":
            print(f"Result of greet('World') is: '{response['data']['value']}'")

    except Exception as e:
        print(f"\nAn error occurred: {e}")
    finally:
        client.close()

if __name__ == "__main__":
    main()
