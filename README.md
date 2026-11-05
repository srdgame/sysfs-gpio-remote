# sysfs-gpio-remote

远程 GPIO 映射工具。在有 GPIO 的 Linux 设备上运行**服务端**，在本地 Linux
主机上运行**客户端**，客户端通过 FUSE 挂载一个与 `/sys/class/gpio` 完全兼容
的虚拟文件系统，所有 GPIO 操作实时通过 TCP 转发到服务端执行。

任何操作 sysfs GPIO 的现有程序（脚本、`libgpiod` sysfs 兼容层、旧式应用）
无需修改即可操作远程 GPIO。

## 特性

- **sysfs 兼容**：`export`/`unexport`/`direction`/`value`/`edge`/`active_low`
  语义与内核 `/sys/class/gpio` 一致
- **边沿检测**：对 `value` 文件 `poll(POLLPRI)` 即可等待远程边沿事件，
  服务端通过 epoll 监听 gpiod 事件并主动推送
- **断线恢复**：客户端断线后自动指数退避重连（1s→10s），重连后自动重放
  所有已导出引脚的完整状态（direction/edge/active_low/value）
- **实时性**：读写、写值均为同步 RPC；边沿事件为服务端异步推送
- **单客户端**：服务端同一时刻只允许一个客户端连接，重复连接被拒绝（EBUSY）

## 构建

依赖（Debian/Ubuntu）：

```bash
sudo apt install build-essential pkg-config \
    libfuse3-dev libgpiod-dev
```

> 服务端需要 libgpiod v2（>= 2.1）；客户端仅编译时依赖 fuse3。

```bash
make            # 构建 gpio-remote-server、gpio-remote-client、测试工具
make test       # 运行协议单元测试
```

## 运行

### 服务端（GPIO 设备上，需 root 访问 /dev/gpiochip*）

```bash
gpio-remote-server [-l ADDR] [-p PORT] [-n PINS] [-v]
  -l ADDR   监听地址（默认 0.0.0.0）
  -p PORT   监听端口（默认 4242）
  -n PINS   引脚数量上限（默认 54）
  -v        增加日志输出（可重复）
```

引脚编号 `gpioN` 映射到设备上名为 `GPION` 的线路（`gpiod` 线名查找）。
例如 `gpio17` 需要 `gpioinfo` 能找到名为 `GPIO17` 的线。

### 客户端（本地主机）

```bash
gpio-remote-client [-s HOST] [-p PORT] [-n PINS] [-o OPTS] [-v] MOUNTPOINT
  -s HOST   服务端地址（默认 127.0.0.1）
  -p PORT   服务端端口（默认 4242）
  -n PINS   引脚数量上限（默认 54）
  -o OPTS   额外 FUSE 挂载选项（如 allow_other）
  -v        增加日志输出（可重复）
```

示例：

```bash
# 独立挂载点
gpio-remote-client -s 192.168.1.50 ~/remote-gpio

# 遮蔽 /sys/class/gpio（需 root，让现有程序无感切换）
sudo gpio-remote-client -s 192.168.1.50 -o allow_other,default_permissions \
    /sys/class/gpio
```

卸载：`fusermount3 -u ~/remote-gpio`。客户端正常退出时（含 SIGTERM）会自动
卸载挂载点；异常退出（SIGKILL/崩溃）由默认的 `auto_unmount` 选项兜底清理。

## 使用（与 sysfs 完全一致）

```bash
cd ~/remote-gpio

echo 17 > export          # 导出远程 GPIO17
echo out  > gpio17/direction
echo 1    > gpio17/value  # 远程引脚输出高电平

echo in   > gpio17/direction
cat gpio17/value          # 从远程实时读取

echo both > gpio17/edge   # 使能边沿检测
# 等待边沿（任意使用 poll/POLLPRI 的程序均可）
python3 - <<'EOF'
import select
f = open('gpio17/value')
f.read()                      # priming read（sysfs 语义）
p = select.poll()
p.register(f, select.POLLPRI)
print(p.poll())               # 边沿发生时返回
EOF

echo 17 > unexport        # 释放
```

## 测试

```bash
make test                          # 协议单元测试（无需特权）
sudo ./test/integration_test.sh    # gpio-sim 端到端测试（需 root）
```

集成测试使用内核 `gpio-sim` 模块模拟 gpiochip（含 GPIO17/27 两条命名线），
覆盖 export、方向切换、读写、active_low、POLLPRI 边沿、断线重连与状态重放。
要求内核启用 `CONFIG_GPIO_SIM=m`。

## 协议

自定义二进制协议，16 字节帧头
`[u32 magic "RGOI"][u16 type][u16 status][u32 seq][u32 payload_len]`，
网络字节序，最大负载 64 字节。详见 `doc/DESIGN.md`。

## 已知限制

- 无认证/加密，仅适合可信网络（可配合 ssh 隧道）
- 单客户端
- `direction`/`edge`/`active_low` 的读由客户端状态缓存返回（单客户端语义下
  与服务端状态一致）
