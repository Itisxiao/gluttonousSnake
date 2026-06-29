## 项目简介
这是一个贪吃蛇服务器，用来练习C++

入门版：
C++ + WebSocket + JSON + 单进程房间服

进阶版：
C++ + Asio + Protobuf + Redis/MySQL

实时版：
C++ + UDP/KCP + Protobuf + 固定 Tick + 状态同步

大型版：
Gateway + Match + Room/Battle + DB + Redis + RPC

## 当前实现

本仓库已经实现入门版房间服：

- C++17 单进程服务端
- 原生 TCP socket + WebSocket 握手/帧解析
- JSON 文本协议
- 多房间、多玩家
- 固定 tick 推进游戏状态
- 碰墙、撞蛇、抢同一格判死

## 编译运行

```bash
cmake -S . -B build
cmake --build build
./build/snake_server 9002
```

服务默认监听：

```text
ws://127.0.0.1:9002
```

## 打开页面

先启动服务端：

```bash
./build/snake_server 9002
```

再用浏览器打开：

```text
web/index.html
```

页面默认连接 `ws://127.0.0.1:9002`，点击“连接”后会加入 `lobby` 房间。使用方向键或 WASD 控制移动。

## 客户端协议

客户端发送 JSON 文本帧。

加入房间：

```json
{"type":"join","room":"lobby","name":"alice"}
```

转向：

```json
{"type":"turn","dir":"up"}
```

离开房间：

```json
{"type":"leave"}
```

心跳：

```json
{"type":"ping"}
```

服务端返回 `joined`、`state`、`error`、`pong` 等 JSON 文本帧。`state` 示例：

```json
{
  "type": "state",
  "room": "lobby",
  "width": 40,
  "height": 30,
  "food": {"x": 10, "y": 12},
  "players": [
    {
      "id": 1,
      "name": "alice",
      "alive": true,
      "score": 0,
      "snake": [{"x": 5, "y": 5}, {"x": 4, "y": 5}]
    }
  ]
}
```

## 设计说明

这是练习用入门实现，故意不引入第三方库。生产项目建议替换为成熟网络库和 JSON 库，例如 Boost.Beast/WebSocket++、asio、nlohmann/json，并拆分网络层、房间层、协议层和游戏逻辑。
