# Submariner Clock Server

一个 C++17 单文件 HTTP 服务器示例。启动后监听指定端口，浏览器访问根路径会加载一个绿色潜水表风格的实时模拟时钟页面。

## Build

```powershell
cmake -S examples/submariner_clock_server -B build/submariner_clock_server
cmake --build build/submariner_clock_server --config Release
```

## Run

```powershell
.\build\submariner_clock_server\Release\submariner_clock_server.exe -p 8080
```

如果使用单配置生成器，二进制通常位于：

```powershell
.\build\submariner_clock_server\submariner_clock_server.exe -p 8080
```

打开浏览器访问：

```text
http://127.0.0.1:8080/
```

## Options

- `-p <port>`: 配置监听端口，范围 `1..65535`。
- `-h`, `--help`: 显示帮助。
