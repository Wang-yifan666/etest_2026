# ETest 2026 通信协议 

> 适用系统：树莓派 5 视觉上位机 ↔ MCU 底盘/摆杆控制器

---

## 1. 物理层

| 项目 | 约定 |
|---|---|
| 接口 | UART |
| 波特率 | 115200 bps |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | None |
| 编码 | ASCII |
| 行结束 | `\r\n`（接收端兼容单独 `\n`） |
| 最大行长 | 128 字节（含 CRLF） |

**字段规则：**
- 字段内不得包含逗号 `,`、星号 `*`、CR、LF
- 所有数字使用十进制 ASCII
- 未识别报文只记录，不退出程序
- 超长行整行丢弃

---

## 2. 心跳

树莓派每 **500ms** 发送一次，MCU 立即回复。**2000ms** 无响应标记链路离线。

```text
HOST → PING
MCU  ← OK,PING
```

>  心跳正常 ≠ 视觉有效。BALL 超时或 LOST 时仍需关闭滚球闭环。

---

## 3. 启动与握手

### 3.1 上电报文

MCU 上电后主动发送：

```text
MCU ← BOOT,OK,<boot_id>,<fw_version>
```

示例：

```text
BOOT,OK,84,1.0.1
```

### 3.2 握手流程

树莓派连接后依次执行，三步全部成功即为**握手成功**：

```text
① HOST → PING
   MCU  ← OK,PING

② HOST → PROTO?
   MCU  ← PROTO,5,2

③ HOST → CAPS?
   MCU  ← CAPS,MOTION=4,STATUS=4,TUNE=5,BALL=2,ROD=1,CONTEST=1
```

---

## 4. 比赛题目

### 4.1 题目列表

| MCU 发送 | 题目编号 | 名称 | 说明 |
|---|---|---|---|
| M0001 | H2 | 快速单圈 | 纯循迹，从 A 点出发跑一圈回到 A 点，计时 |
| M0002 | H3 | 滚球清扫 | 滚球 + 循迹，边走边控制球的位置 |
| M0003 | H4 | AB 中心 | 从 A 到 B 并在中心区域完成指定任务 |
| M0004 | H5 | 单圈中心 | 跑一圈并在中心区域停留/完成任务 |
| M0005 | H6 | 单圈目标 | 跑一圈，滚球跟踪动态目标位置 |

### 4.2 进入题目

**握手成功后**，由 **MCU（下位机）** 发送 M000X 编号，进入对应题目：

```text
MCU ← M0001
```

上位机收到 M000X 后，开始标定流程（见第 5 章）。

---

## 5. 标定流程

上位机收到 M000X 后，按以下步骤完成标定：

### 5.1 声明视觉会话

```text
HOST → VSESSION,<session>,MONOTONIC,<fps_x100>,<camera_id>
MCU  ← OK,VSESSION,<session>
```

| 字段 | 类型 | 说明 |
|---|---|---|
| session | uint32 | 视觉会话 ID，随机生成 |
| fps_x100 | uint16 | 标称帧率 × 100，如 30fps → 3000 |
| camera_id | string | 摄像头标识，如 CAM0 |

示例：

```text
HOST → VSESSION,317,MONOTONIC,3000,CAM0
MCU  ← OK,VSESSION,317
```

MCU 收到新 session 后：清空旧 BALL 序号、清空速度估计、清空 PID 积分、视觉状态置为 WARMUP。

### 5.2 发送标定帧

视觉会话声明后，上位机持续发送 `status=CALIB` 的 BALL 帧，表示正在标定：

```text
HOST → BALL,<session>,<seq>,<capture_ms>,<age_ms>,0,0.0,CALIB
```

标定期间 `position_0p1mm=0`、`confidence=0.0`，MCU 不进行位置闭环控制。

### 5.3 标定完成

标定完成后，上位机开始发送 `status=OK` 的 BALL 帧，表示标定已就绪：

```text
HOST → BALL,<session>,<seq>,<capture_ms>,<age_ms>,<position>,<confidence>,OK
```

此时 MCU 不立即运动，等待上位机发送启动命令。

### 5.4 启动运动

标定完成（首条 `status=OK`）后，由**上位机**发送 CONTESTSTART 启动运动：

```text
HOST → CONTESTSTART,<Hx>
MCU  ← OK,CONTESTSTART,<Hx>,ACCEPTED
```

示例：

```text
HOST → CONTESTSTART,H5
MCU  ← OK,CONTESTSTART,H5,ACCEPTED
```

MCU 收到 CONTESTSTART 后正式开始运动（位置闭环、循迹等）。

---

## 6. 滚球位置协议

### 6.1 BALL 报文格式

```text
HOST → BALL,<session>,<seq>,<capture_ms>,<age_ms>,<position_0p1mm>,<confidence>,<status>
```

| 字段 | 类型 | 说明 |
|---|---|---|
| session | uint32 | 视觉会话 ID，与 VSESSION 一致 |
| seq | uint32 | 本会话消息序号，每帧递增（含 LOST/CALIB/ERROR） |
| capture_ms | uint32 | 从会话零点到图像采集时刻的毫秒数 |
| age_ms | uint32 | 图像采集到报文发送前的耗时（ms） |
| position_0p1mm | int32 | 钢球相对摆杆中心 O 的绝对位置，单位 0.1mm |
| confidence | float | 检测置信度 0.0~1.0（0.0=完全不可信，1.0=完全可信） |
| status | enum | `OK` / `LOST` / `CALIB` / `ERROR` |

### 6.2 位置坐标

```text
摆杆中心 O 为原点 (position = 0)
P1 → P2 为正方向

position_0p1mm > 0：钢球在 O 的正方向
position_0p1mm < 0：钢球在 O 的负方向

合法范围：-1250 ~ +1250（即 ±125.0mm）
```

示例：`position_0p1mm = -183` 表示 **-18.3 mm**。

### 6.3 状态语义

| status | 含义 | 位置可用？ | position_0p1mm 值 | confidence 值 |
|---|---|---|---|---|
| OK | 检测有效 | ✅ 是 | 实际值 | 实际值 |
| LOST | 本帧未检测到球 | ❌ 否 | 填 0 | 填 0.0 |
| CALIB | 摄像头/标定中 | ❌ 否 | 填 0 | 填 0.0 |
| ERROR | 摄像头/算法异常 | ❌ 否 | 填 0 | 填 0.0 |

**强制规则：**
1. 仅 `status=OK` 时 MCU 使用位置数据
2. 非 OK 时 `position_0p1mm=0, confidence=0.0`
3. 禁止 LOST 后继续发送上一帧旧位置
4. 每帧 BALL 的 seq 必须递增
5. 发送队列只保留最新一帧，禁止积压

### 6.4 置信度建议阈值

| confidence | 行为 |
|---|---|
| 0.80 ~ 1.00 | 正常使用 |
| 0.50 ~ 0.79 | 降权使用 |
| 0.01 ~ 0.49 | 无效/仅诊断 |
| 0.0 | LOST/CALIB/ERROR |

### 6.5 视觉状态机

MCU 根据 BALL 到达情况维护视觉状态：

```text
OFFLINE  →  握手后进入 HANDSHAKE
         →  VSESSION 后进入 WARMUP
         →  连续收到有效帧后进入 VALID
         →  age_ms 偏大时进入 DEGRADED
         →  超时未收到新帧进入 STALE → LOST
         →  硬超时（默认 300ms）→ 停车
```

### 6.6 BALL 帧示例

```text
HOST → BALL,317,0,0,24,0,0.0,CALIB
HOST → BALL,317,1,34,25,0,0.0,CALIB
HOST → BALL,317,2,67,27,-12,0.91,OK
HOST → BALL,317,3,100,26,-18,0.89,OK
HOST → BALL,317,4,133,29,-15,0.90,OK
HOST → BALL,317,5,166,25,0,0.0,LOST          ← 丢球
HOST → BALL,317,6,200,24,5,0.86,OK           ← 恢复
```

---

## 7. 比赛停止

### 7.1 主动停止

上位机可随时停止比赛：

```text
HOST → CONTESTSTOP
MCU  ← OK,CONTESTSTOP
```

停止效果：中止比赛状态机、关闭底盘运动。

### 7.2 自动完成

比赛完成后 MCU 主动上报：

```text
MCU ← DONE,<Hx>,<elapsed_ms>,<distance_mm>,<result>
```

示例：

```text
MCU ← DONE,H5,15230,2450,PASS
```

### 7.3 状态查询

```text
HOST → CONTESTSTATUS?
MCU  ← CONTEST,<mode>,<phase>,<running>,<elapsed_ms>,<lap_count>,<target_0p1mm>,<result>,<fault>
```

---

## 8. 完整运行流程示例

```text
MCU ← BOOT,OK,84,1.0.1                              ← 上电

HOST → PING
MCU  ← OK,PING                                        ← ① 心跳

HOST → PROTO?
MCU  ← PROTO,5,2                                      ← ② 协议版本

HOST → CAPS?
MCU  ← CAPS,MOTION=4,STATUS=4,TUNE=5,BALL=2,ROD=1,CONTEST=1  ← ③ 能力查询
                                                        ====== 握手成功 ======

MCU ← M0004                                           ← 下位机发送题目编号 ★

HOST → VSESSION,317,MONOTONIC,3000,CAM0              ← 上位机声明视觉会话
MCU  ← OK,VSESSION,317

HOST → BALL,317,0,0,24,0,0.0,CALIB                   ← 标定中...
HOST → BALL,317,1,34,25,0,0.0,CALIB
HOST → BALL,317,2,67,27,-12,0.91,OK                  ← 标定完成
HOST → BALL,317,3,100,26,-18,0.89,OK

HOST → CONTESTSTART,H5                              ← 上位机启动运动 ★
MCU  ← OK,CONTESTSTART,H5,ACCEPTED                  ← ===== 正式开始运动 =====

HOST → BALL,317,4,133,29,-15,0.90,OK                 ← 持续发送位置
HOST → BALL,317,5,166,30,-14,0.91,OK

... BALL 持续发送，MCU 进行位置闭环和循迹 ...

MCU ← DONE,H5,15230,2450,PASS                     ← 比赛完成

HOST → CONTESTSTOP                                    ← 或主动停止
MCU  ← OK,CONTESTSTOP
```

---

## 9. 错误处理

### 9.1 常见错误码

| 错误码 | 含义 |
|---|---|
| BALL_NOT_READY | 视觉数据未就绪（标定未完成） |
| ROD_NOT_HOMED | 摆杆未回零 |
| FAULT_ACTIVE | 存在锁存故障 |
| VISION_TIMEOUT | 视觉数据超时 |
| CALIB_TIMEOUT | 标定超时 |

### 9.2 错误报文格式

```text
MCU ← ERR,<MODULE>,<CODE>,<KEY>,<DETAIL>
```

示例：

```text
MCU ← ERR,VISION,TIMEOUT,,BALL_AGE_OVER_300MS
```

---

## 10. 默认参数

```text
UART 波特率                115200
心跳间隔                    500 ms
心跳超时                    2000 ms
BALL 正常 age_ms 上限       70 ms
BALL 降级 age_ms 上限       120 ms
BALL 接收超时                150 ms
BALL 硬超时停车              300 ms
BALL 恢复需连续有效帧        3 帧
标定超时                    10000 ms
钢球位置合法范围             ±1250 (0.1mm)
```

---

**文档结束**