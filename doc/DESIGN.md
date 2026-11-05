# sysfs-gpio-remote 设计文档

基于 [PLAN.md](./PLAN.md) 细化。参考实现：[sysfs-gpio-shim](/home/cch/github/sysfs-gpio-shim)（FUSE 模拟 /sys/class/gpio）。

## 1. 需求

### 1.1 核心需求

| # | 需求 |
|---|------|
| R1 | 服务端运行在带 GPIO 的 Linux 设备上，客户端运行在本机 Linux |
| R2 | 客户端通过 FUSE 提供与 `/sys/class/gpio` 兼容的虚拟文件系统 |
| R3 | 客户端对 GPIO 文件的任何读/写实时转发到服务端执行 |
| R4 | 通信使用 TCP 套接字 |
| R5 | 支持边沿检测：客户端 `poll()` gpioN/value 的 POLLPRI 机制，服务端边沿事件异步推送 |
| R6 | 命名 GPIO：支持 OpenWrt `gpio-export` 风格命名引脚（内核驱动持有 → 仅 value 透传；gpiod 线名 → 全功能） |
| R7 | 连接时枚举：客户端每次连接查询服务端已导出引脚，补齐本地视图（含服务端残留与预存命名项自动镜像） |

### 1.2 已确认的设计决策

| 决策点 | 结论 |
|--------|------|
| 实现语言 | C（客户端与服务端均为 C，最大化复用参考代码） |
| 边沿检测 | 支持，服务端推送（等待方在服务端） |
| 安全 | 无认证，监听地址/端口可配置（面向可信内网） |
| 断线行为 | 挂起操作立即返回 EIO；自动重连并恢复引脚状态 |
| 挂载点 | 两者都支持：默认自定义目录，可用参数 shadow `/sys/class/gpio` |
| 并发连接 | 服务端单客户端（第二个连接被拒绝） |
| 引脚映射 | 客户端 gpioN 直接映射服务端 GPIO 名 `GPION` |
| 命名引脚（内核持有） | 仅 value 读写（sysfs 透传），direction/edge/active_low 返回 EINVAL |
| 命名引脚（gpiod 线名） | 全功能；本地目录名 = 请求名（如 `pcie_on`） |
| 枚举时机 | 每次连接（含重连）都执行 REQ_LIST；服务端同时重新镜像 `/sys/class/gpio` 预存命名项 |
| 仓库结构 | 单仓库双目标，共享协议头 |

### 1.3 支持的 sysfs 接口（与参考项目一致）

```
/export                    写入引脚号导出（数字编号，或命名导出，见 §4.2.1）
/unexport                  写入引脚号取消导出（数字编号或名字）
/gpioN/                    数字导出后的引脚目录
/gpioN/value               读/写电平；poll() 支持 POLLPRI 边沿唤醒
/gpioN/direction           in / out / low / high
/gpioN/edge                none / rising / falling / both
/gpioN/active_low          0 / 1
/<name>/                   命名导出后的引脚目录（目录名 = 请求名）
/<name>/value              命名引脚的 value；内核持有的引脚仅此文件可用
```

## 2. 总体架构

```
 客户端主机 (Linux)                                GPIO 设备 (Linux)
┌─────────────────────────────┐              ┌─────────────────────────────┐
│  应用程序                     │              │                             │
│   │ read/write/poll          │              │                             │
│   ▼                         │              │                             │
│  FUSE 内核模块                │              │                             │
│   │                         │              │                             │
│   ▼                         │              │                             │
│  gpio-remote-client         │   TCP 双向    │  gpio-remote-server         │
│  ┌───────────────────┐      │  (长连接,     │  ┌───────────────────┐      │
│  │ fuse_ops (FUSE回调)│◄────►│   请求-响应   │  │ connection (请求循环)│      │
│  │ state  (引脚状态)   │ RPC  │   +异步事件)  │  │ gpio_backend(libgpiod)│    │
│  │ rpc    (协议收发)   │      │              │  │ edge_thread(边沿推送)│      │
│  └───────────────────┘      │              │  └───────────────────┘      │
│   ▼                         │              │   ▼                         │
│  FUSE 挂载点                 │              │  libgpiod 2.1 → /dev/gpiochipN
│  (~/gpio 或 /sys/class/gpio)│              │                             │
└─────────────────────────────┘              └─────────────────────────────┘
```

- **客户端** = 参考项目的 FUSE 层 + 把"本地 gpiod 调用"替换为"同步 RPC"
- **服务端** = 参考项目的 gpiod 后端逻辑 + TCP 请求循环 + 边沿事件推送
- 协议为串行请求-响应模型（同一连接上一次只有一个未完成请求），外加服务端异步插队的 `EDGE_EVENT` 消息

## 3. 仓库结构

```
sysfs-gpio-remote/
├── Makefile
├── doc/
│   ├── PLAN.md
│   └── DESIGN.md
├── common/                  # 双方共享
│   ├── protocol.h           # 协议常量、消息类型、帧头结构（纯头文件）
│   ├── proto_io.c/.h        # 帧读写：send_frame / recv_frame（处理分片、超时）
│   └── log.c/.h             # 统一日志（级别、时间戳）
├── server/
│   ├── main.c               # 参数解析、TCP 监听、接受连接（单客户端准入）
│   ├── connection.c/.h      # 客户端连接：请求循环、消息分发、订阅管理
│   ├── gpio_backend.c/.h    # libgpiod 后端（移植自 sysfs-gpio-shim）
│   └── edge_thread.c/.h     # epoll 边沿检测线程 + EDGE_EVENT 推送
└── client/
    ├── main.c               # FUSE 参数解析、挂载点处理、信号处理
    ├── fuse_ops.c/.h        # FUSE 操作实现（getattr/readdir/open/read/write/poll…）
    ├── rpc.c/.h             # RPC 客户端：请求-响应、事件接收线程、重连
    └── state.c/.h           # 引脚状态缓存、断线恢复重放
```

每个源文件控制在 400 行以内；`gpio_backend.c` 若超限，按"chip 管理 / pin 操作 / 边沿"再拆分。

## 4. 线协议

### 4.1 帧格式

所有字段网络字节序（大端）。每帧：

```c
struct frame_header {
    uint32_t magic;        // 0x5247_4F49 ("RGOI")
    uint16_t type;         // 消息类型
    uint16_t status;       // 响应帧：errno 值；请求帧：0
    uint32_t seq;          // 请求/响应配对序号；异步事件帧忽略
    uint32_t payload_len;  // payload 字节数
} __attribute__((packed)); // 16 字节，后跟 payload
```

### 4.2 消息类型

| type | 方向 | 名称 | payload | 响应 payload |
|------|------|------|---------|--------------|
| 1 | C→S | REQ_EXPORT | `u32 pin` | 空 |
| 2 | C→S | REQ_UNEXPORT | `u32 pin` | 空 |
| 3 | C→S | REQ_GET_VALUE | `u32 pin` | `u8 value` |
| 4 | C→S | REQ_SET_VALUE | `u32 pin, u8 value` | 空 |
| 5 | C→S | REQ_SET_DIRECTION | `u32 pin, u8 dir` | 空 |
| 6 | C→S | REQ_SET_EDGE | `u32 pin, u8 edge` | 空 |
| 7 | C→S | REQ_SET_ACTIVE_LOW | `u32 pin, u8 val` | 空 |
| 8 | C→S | REQ_POLL | `u32 pin, u8 flags` | `u8 edge_detected` |
| 9 | C→S | REQ_PING | 空 | 空 |
| 10 | C→S | REQ_EXPORT_NAME | `u8 name_len, name[1..32]` | `u32 slot` |
| 11 | C→S | REQ_LIST | 空 | `u16 count, count × {u32 pin, u8 name_len, name}` |
| 16 | S→C | MSG_EDGE_EVENT | `u32 pin` | —（异步） |

payload 上限 `FRAME_MAX_PAYLOAD = 512`；名称最长 `GPIO_NAME_MAX = 32`
（可打印字符，不含 `/`）。`REQ_LIST` 响应超出 512 字节时截断（服务端打 WARN），
典型设备命名引脚数远低于该容量。

### 4.2.1 命名 GPIO 与槽位模型

服务端每个引脚槽位（0..num_pins-1）有显示名 `gpio%d` 或请求的导出名，以及
两种模式：

- **PB_GPIOD**（全功能）：数字导出按线名 `GPIO%d` 查找；命名导出按任意
  gpiod 线名查找（覆盖 `gpio-line-names` 与 gpio-sim）。
- **PB_SYSFS**（仅 value）：当名字不是 gpiod 线名但
  `<sysfs_dir>/<name>/value` 存在时（OpenWrt `gpio-export` 风格，内核驱动
  持有引脚），直接读写该 sysfs 文件。`direction`/`edge`/`active_low` 设置
  返回 `EINVAL`；`unexport` 仅释放槽位，不解除内核导出。

- **连接时枚举（REQ_LIST）**：客户端每次（重）连接先 `REQ_LIST`，对服务器
  已导出而本地未导出的槽位补齐本地视图（覆盖服务器残留导出与客户端重启后
  恢复），随后再执行本地状态的恢复重放。两个方向合并：LIST 补服务器残留，
  重放恢复服务器重启后的本地引脚。本地已导出的槽位以客户端自身状态为准。
- **连接时镜像**：服务端在每次客户端接入时扫描
  `${GPIO_REMOTE_SYSFS_DIR:-/sys/class/gpio}`（环境变量仅用于测试），把
  预存的命名项（跳过 `export`/`unexport`/`gpiochip*`/`gpioN`）自动导出为
  PB_SYSFS 槽位。
- **幂等性**：重复导出同名（数字或命名）成功返回原槽位；数字导出撞上已被
  命名引脚占用的槽位返回 `EBUSY`。重连重放依赖此幂等性。

枚举值：

```c
// direction (REQ_SET_DIRECTION)
DIR_IN = 0, DIR_OUT = 1, DIR_OUT_LOW = 2, DIR_OUT_HIGH = 3
// edge (REQ_SET_EDGE)
EDGE_NONE = 0, EDGE_RISING = 1, EDGE_FALLING = 2, EDGE_BOTH = 3
// poll flags (REQ_POLL)
MSG_POLL_F_NEW_SESSION = 0x01
```

### 4.3 语义约定

- **错误传播**：服务端操作失败时，响应帧 `status` 填 errno（如 `EINVAL`、`EBUSY`、`EPERM`），客户端原样映射为 FUSE 返回值。语义与参考项目完全一致：
  - export 不存在的引脚 / gpiod 请求失败 → `EBUSY`
  - unexport 未导出的引脚 → `EINVAL`
  - 写 value 到输入方向引脚 → `EPERM`
  - 非法 direction/edge 字符串 → `EINVAL`
  - 命名导出：名字非法（超长/含非可打印或 `/`）→ `EINVAL`；既非 gpiod 线名
    也无对应 sysfs value 文件 → `ENOENT`；无空闲槽位 → `ENOMEM`
  - 命名引脚（PB_SYSFS）：设置 direction/edge/active_low → `EINVAL`；value
    文件只读（输入模式引脚）→ `EACCES`
- **串行模型**：客户端同一时刻只有一个在途请求；`seq` 单调递增，服务端校验（防御性），不匹配视为协议错误断开连接。
- **异步事件**：`MSG_EDGE_EVENT` 可在任意时刻到达，客户端接收线程独立处理，不打断请求-响应配对。
- **REQ_POLL 语义**（对应参考项目 `pin_poll_edge`）：
  1. 客户端 FUSE poll 被调用 → 发送 `REQ_POLL(pin, flags)`；flags bit0
     （`MSG_POLL_F_NEW_SESSION`）表示这是该打开文件上的首次 poll，
     服务端须丢弃该 pin 此前残留的 `edge_detected`（对应 sysfs
     "边沿只在 priming read 之后才对 poll 生效"的语义）；
  2. 服务端：若该连接上此 pin 的 waiter `edge_detected==1` → 清除标志、返回 `edge_detected=1`；否则登记订阅并返回 `0`；
  3. 服务端边沿线程检测到事件 → 置 `edge_detected=1`，向连接推送 `MSG_EDGE_EVENT`；
  4. 客户端收到事件 → 调用 `fuse_notify_poll(ph)` 唤醒内核 poll；
  5. 内核被唤醒后再次进入 FUSE poll → 走步骤 2 得到 `edge_detected=1`，返回 POLLPRI。
- **心跳**：客户端每 30s 发送 `REQ_PING`（响应超时 10s），用于快速探测半开连接；服务端不主动断开空闲连接。

## 5. 服务端设计

### 5.1 main.c

```
gpio-remote-server [-l listen_addr] [-p port] [-n num_pins]
```

- 默认监听 `0.0.0.0:4242`，`num_pins` 默认 54（与参考一致）
- 启动顺序：初始化 gpiod 后端（扫描 `/dev/gpiochip*`、建 epoll）→ 监听 → accept 循环
- **单客户端准入**：同一时刻只接受一个连接。已有客户端时，新连接立即发送一个 `status=EBUSY` 的拒绝帧后关闭。当前客户端断开后回到 accept 循环。

### 5.2 connection.c — 请求循环

每连接一个线程（单客户端，无需线程池）：

```c
while (recv_frame(&req) == OK) {
    lock(pin[req.pin].lock);          // 与边沿线程共享的 per-pin 锁
    ret = dispatch(&req);             // 调 gpio_backend 对应函数
    unlock(...);
    send_frame(RESP, req.seq, ret /*errno*/, resp_payload);
}
cleanup: 注销本连接所有 poll 订阅, 关闭连接
```

- poll 订阅表：每连接维护 `pin → waiter{edge_detected}`（固定数组即可，`num_pins` 已知上限）
- 写 socket 需要独立互斥锁（请求响应线程与边沿推送线程共用一条连接）

### 5.3 gpio_backend.c — gpiod 后端

从 `sysfs-gpio-shim.c` 移植，接口收敛为：

```c
int  backend_init(int num_pins);          // 扫描 gpiochip、初始化 pins[]、启动边沿线程
int  pin_export(int n);                   // 查找 "GPION" → gpiod line request
int  pin_export_named(const char *name, int *slot_out);
                                          // gpiod 线名 → 全功能；否则
                                          // <sysfs_dir>/<name>/value 存在 → PB_SYSFS；
                                          // 否则 ENOENT；幂等（同名返回原槽位）
void backend_mirror_sysfs_named(void);    // 镜像 /sys/class/gpio 预存命名项
int  pin_unexport(int n);                 // PB_SYSFS 仅释放槽位
int  pin_set_direction(int n, uint8_t d); // PB_SYSFS → EINVAL
int  pin_set_edge(int n, uint8_t e);      // PB_SYSFS → EINVAL
int  pin_set_active_low(int n, uint8_t v);// PB_SYSFS → EINVAL
int  pin_set_value(int n, uint8_t v);     // PB_SYSFS 直接写 value 文件
int  pin_get_value(int n, uint8_t *out);  // PB_SYSFS 直接读 value 文件
int  pin_get_display_name(int n, char *buf, size_t size); // 供 REQ_LIST
```

镜像目录可用环境变量 `GPIO_REMOTE_SYSFS_DIR` 覆盖（仅测试用，默认
`/sys/class/gpio`）。

保留参考实现的关键行为：
- `find_gpio_by_name("GPION")` 遍历 chip 查找（支持 BCM 编号）
- 每次配置变更通过 `gpiod_line_request_reconfigure_lines` 重配
- 输入方向固定 300µs 去抖、MONOTONIC 事件时钟
- 方向 out 时按 `low/high` 设初始电平

### 5.4 edge_thread.c — 边沿检测与推送

与参考项目相同：epoll 监听各 pin 的 gpiod request fd，专用线程 `epoll_wait` 循环读取边沿事件。差异：

- 检测到事件后不操作 FUSE pollhandle，而是：
  1. 置对应连接订阅表中的 `edge_detected = 1`（持 pin 锁）
  2. 通过连接的写锁推送 `MSG_EDGE_EVENT{pin}`；连接已断开则跳过

## 6. 客户端设计

### 6.1 main.c

```
gpio-remote-client [-s host] [-p port] [-n num_pins] [挂载点]
# shadow /sys/class/gpio 用法（需 root）：
gpio-remote-client -s 192.168.1.50 /sys/class/gpio -o allow_other,default_permissions
```

- 默认端口 4242；挂载点为必选位置参数（FUSE 惯例）
- 启动顺序：连接服务端 → 初始化 state → `fuse_main` 挂载
- 信号处理：SIGINT/SIGTERM → 优雅卸载并退出

### 6.2 fuse_ops.c — FUSE 操作

结构与参考项目逐项对应，本地后端调用替换为 RPC：

| FUSE 回调 | 行为 |
|-----------|------|
| getattr | `/`、`/export`、`/unexport`、`/gpioN` 或 `/<name>`（已导出才可见）、`/<pin>/{value,direction,edge,active_low}`；权限位与参考一致（export/unexport 0220，属性文件 0660，目录 0755） |
| readdir | 根目录列出 export/unexport/已导出引脚（数字用 `gpioN`，命名用请求名）；引脚目录列出四个属性文件 |
| open | 路径合法性校验（同参考）；`direct_io = 1` |
| read | 按 offset 0 触发 `REQ_GET_VALUE` 并缓存到 file handle 的 16 字节 buf（参考的 `buf_update_maybe` 语义） |
| write | 解析文本 → 对应 REQ_SET_*；export/unexport 写入数字 → REQ_EXPORT/UNEXPORT，写入名字（≤32 字符）→ REQ_EXPORT_NAME / 按名查找后 UNEXPORT |
| release | 释放 file handle，注销 poll 订阅 |
| poll | 仅 `/<pin>/value` 支持；流程见 §4.3；客户端本地维护 `edge_detected` 标志与 waiter 表 |
| flush/fsync 等 | 不需要，返回 0 |

文本解析复用参考项目的 `str_buf_equal`（容忍尾随换行、`direction` 支持 `low/high` 即设置初始电平）。

### 6.3 rpc.c — RPC 客户端

```c
int rpc_connect(host, port);                    // 建立连接
int rpc_call(uint16_t type, void *req, size_t req_len,
             void *resp, size_t *resp_len, uint16_t *status);  // 同步请求-响应
int rpc_is_connected(void);
```

- **接收线程**：`recv_frame` 循环；收到 `MSG_EDGE_EVENT` → 查本地 waiter 表 → `fuse_notify_poll`；收到响应帧 → 唤醒等待的请求线程（条件变量）
- **互斥**：请求线程持 `in_flight` 锁串行化请求；socket 写由请求线程独占（事件帧只读 socket）
- **重连**：接收线程检测 EOF/错误 → `rpc_is_connected()==0`，启动重连线程（1s 起指数退避，上限 10s）；在途请求立即以 EIO 完成

### 6.4 state.c — 引脚状态与断线恢复

客户端缓存每个引脚的已应用配置，用于重连恢复：

```c
typedef struct {
    bool        exported;
    char        name[33];       // "gpio%d" 或命名导出的请求名
    uint8_t     direction;      // 最后一次设置的 direction
    uint8_t     edge;
    uint8_t     active_low;
    int         last_value;     // 仅 direction==out 时维护
} pin_state_t;
```

**重连恢复流程**（每次连接成功后，由 state 层串行执行）：

1. **先 `REQ_LIST`**：对服务端已导出而本地未导出的槽位，按服务端返回的名字
   补齐本地视图（覆盖服务端残留导出、客户端重启后的视图恢复；本地已导出的
   槽位以客户端自身状态为准）
2. **再重放**：对每个本地 `exported` 引脚——数字名（`gpioN`）走
   `REQ_EXPORT`；命名走 `REQ_EXPORT_NAME`（服务端分配的槽位若与本地不同，
   本地条目迁移到新槽位）→ `REQ_SET_DIRECTION` → `REQ_SET_EDGE` →
   `REQ_SET_ACTIVE_LOW` → （若 out）`REQ_SET_VALUE(last_value)`
3. 某一步失败（如引脚已被服务端其他用途占用）→ 该引脚标记 `exported=false` 并向日志告警，后续访问返回 `EIO`
4. 恢复完成前，新到的 RPC 一律排队等待（有 10s 上限，超时返回 EIO）；快速
   重连时旧恢复线程被新一代序号取代，不再改动引脚状态

**断线期间的语义**：

- 未在途的 FUSE 操作：立即返回 `EIO`（不等重连）
- 已挂起的在途请求（如阻塞中的 poll）：立即以 `EIO` 完成，`fuse_notify_poll` 不再触发
- 已打开的文件句柄保持有效，恢复后可直接继续使用

## 7. 并发与锁模型汇总

| 位置 | 保护对象 | 锁 |
|------|----------|-----|
| 服务端 | `pins[n]` 状态 + gpiod request | per-pin `pthread_mutex_t`（请求线程与边沿线程共享，沿用参考设计） |
| 服务端 | 连接 socket 写 | 连接写锁（响应帧与事件帧互斥） |
| 客户端 | RPC 在途请求配对 | `in_flight` 互斥 + 条件变量 |
| 客户端 | waiter 表 / pin 状态 | 全局状态互斥（临界区极短） |

无递归锁、无锁外引用共享数据；引脚数上限固定（默认 54），全部使用静态数组，避免哈希表。

## 8. 错误处理与日志

- 统一日志：`log.h` 提供 `log_error/warn/info/debug`，`-v` 递增级别，输出到 stderr
- 客户端所有 RPC 失败都映射 errno 并透传到 FUSE 返回值，不吞错误
- 协议帧错误（magic 不符、payload 超长、seq 不匹配）：双方立即断开并记录日志
- 客户端 panic 场景（如重连后恢复全部失败）：保持挂载但持续 EIO，不自动 umount，由用户决定

## 9. 依赖与构建

| 依赖 | 客户端 | 服务端 |
|------|--------|--------|
| libfuse3-dev | ✓ | — |
| libgpiod ≥ 2.1（需 v2 API） | — | ✓ |
| gcc/make, pthread | ✓ | ✓ |

```
make            # 构建 gpio-remote-server 与 gpio-remote-client
make test       # 单元测试（协议编解码、状态恢复逻辑）
make clean
```

编译警告：`-Wall -Wextra`，目标零警告。

## 10. 测试策略

1. **单元测试**：帧编解码往返、seq 配对、payload 边界（截断/超长）、state 重放序列生成
2. **协议集成测试**：本地起 server + client，用 `dd`/shell 脚本走完 export → direction → value 读写 → unexport 全流程
3. **无硬件验证**：使用 Linux `gpio-sim` 内核模块（CONFIG_GPIO_SIM）模拟 gpiochip，服务端可完整跑通（含边沿：通过 sysfs 触发 sim 引脚电平变化，验证客户端 poll 被唤醒）
4. **断线测试**：kill -STOP 服务端模拟半开连接（验证 PING 超时）、kill 服务端（验证 EIO + 重连 + 状态重放）
5. **真实硬件**：最终在目标设备上用 LED + 按键做端到端验收

## 11. 任务拆解

| # | 任务 | 产出 | 依赖 |
|---|------|------|------|
| T1 | 协议层：`protocol.h` + `proto_io`（帧编解码、分片处理）+ 单元测试 | common/ 编解码可用 | — |
| T2 | 日志模块 | common/log | — |
| T3 | 服务端 gpiod 后端移植（`gpio_backend.c`，从 shim 移植并收敛接口） | 可独立用测试程序驱动引脚 | — |
| T4 | 服务端边沿检测线程（`edge_thread.c`，移植 epoll 循环，改为推送回调） | 边沿事件产生 | T3 |
| T5 | 服务端 TCP：main + connection（监听、单客户端准入、请求循环、订阅表、事件推送） | 服务端可通过 nc/脚本走通协议 | T1, T3, T4 |
| T6 | 客户端 RPC 层（连接、请求-响应、接收线程、重连） | rpc.c 可用 | T1 |
| T7 | 客户端状态与恢复（`state.c`：pin 状态缓存、重放序列、断线 EIO 语义） | | T6 |
| T8 | 客户端 FUSE 操作（`fuse_ops.c` + `main.c`，从 shim 移植路径/权限/文本解析逻辑，后端换 RPC） | 客户端可挂载并完整读写 | T6, T7 |
| T9 | 客户端 poll/边沿（waiter 表、REQ_POLL 配合、fuse_notify_poll） | 边沿检测端到端可用 | T5, T8 |
| T10 | gpio-sim 环境集成测试 + 断线/重连测试 | 自动化测试脚本 | T5, T8, T9 |
| T11 | README（使用说明、两端部署、gpio-sim 测试方法）+ 参数打磨 | 交付 | T10 |

**里程碑**：
- M1（T1–T4）：协议与后端可独立验证
- M2（T5–T6）：两端进程可通信，非 FUSE 测试程序走通协议
- M3（T7–T9）：客户端可挂载，功能对齐参考项目（含边沿）
- M4（T10–T11）：模拟环境全量测试通过，文档交付

## 12. 已知限制（明确不做）

- 无认证、无加密（内网可信环境前提）
- 服务端不支持多客户端并发
- 不模拟参考项目未实现的属性（如 `/device`、bias 设置）
- 性能以 RTT 为下限（LAN 典型 <1ms/操作），不针对高频率翻转优化
- REQ_LIST 响应受 512 字节负载上限约束，约 70+ 个同时导出引脚时截断
  （服务端打 WARN；典型设备远低于此规模）
- 数字导出（`echo N > export`）撞上已被自动镜像占用的槽位 → `EBUSY`；
  命名引脚请按名字导出
