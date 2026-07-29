# ETest 2026 工程通信协议 V5

> 适用对象：树莓派 5 上位机 ↔ STM32 底盘/摆杆控制器  
> 版本：V5 Revision 1  
> 核心新增：`BALL` 滚球视觉位置报文  
> 兼容性：保留 V4 的 `TARGET`、`LOST` 和其余控制、查询、调参命令

---

## 1. 物理层与串口参数

| 项目 | 约定 |
|---|---|
| 接口 | UART |
| 默认设备 | 树莓派 `/dev/ttyAMA0` |
| 波特率 | 115200 bps |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | None |
| 硬件流控 | None |
| 数据编码 | ASCII |
| 协议版本 | 5 |
| 字段分隔符 | 英文逗号 `,` |
| 报文结束符 | `\r\n` 或 `\n` |
| 默认最大行长 | 512 字节 |
| 默认读超时 | 50 ms |
| 默认写超时 | 200 ms |
| 默认重连间隔 | 1000 ms |
| 默认握手超时 | 1500 ms |
| 默认心跳周期 | 500 ms |
| 默认心跳失联阈值 | 2000 ms |

当前协议不使用二进制帧头、CRC 或转义机制。可靠性依赖参数范围检查、状态字段、序号、超时和心跳。因此：

1. 字段内容不得包含逗号、`\r` 或 `\n`。
2. 所有数值使用十进制 ASCII。
3. 浮点数小数点固定使用 `.`，不能受系统区域设置影响。
4. 未识别报文必须记录日志，但不得导致主程序退出。

---

## 2. 基本报文语法

### 2.1 单行报文

```text
<TAG>[,<FIELD1>,<FIELD2>,...]\r\n
```

例：

```text
PING\r\n
BALL,123,-18,220,OK\r\n
```

解析规则：

1. 持续接收字节并缓存。
2. 找到 `\n` 后切出一行。
3. 删除行末 `\n`，再删除可选的 `\r`。
4. 空行忽略。
5. 使用英文逗号分割。
6. 第一个字段是 `TAG`。
7. 后续字段原样保留，包括空字段。

例如：

```text
ERR,SAFETY,FAULT_ACTIVE,,DETAIL
```

必须解析为：

```text
TAG    = ERR
fields = ["SAFETY", "FAULT_ACTIVE", "", "DETAIL"]
```

### 2.2 成功响应

```text
OK
OK,<COMMAND>
OK,<COMMAND>,<DETAIL>
```

### 2.3 接受与完成分离

适用于转向等需要执行时间的动作：

```text
OK,<COMMAND>,<PARAMETERS>,ACCEPTED
DONE,<COMMAND>,<PARAMETERS>
```

`ACCEPTED` 只表示命令已接收，不表示动作完成。只有收到 `DONE` 才能判定动作完成。

### 2.4 错误响应

```text
ERR,<OPERATION>,<ERROR_CODE>,<KEY>,<DETAIL>
```

| 字段 | 含义 |
|---|---|
| `OPERATION` | 发生错误的命令或模块 |
| `ERROR_CODE` | 稳定、可程序判断的错误代码 |
| `KEY` | 相关参数或对象；允许为空 |
| `DETAIL` | 人类可读说明；允许为空 |

例：

```text
ERR,L,INVALID_ANGLE,400,ANGLE_OUT_OF_RANGE
ERR,SAFETY,FAULT_ACTIVE,,MOTOR_DISABLED
```

### 2.5 警告响应

```text
WARN,<MODULE>,<WARNING_CODE>,<KEY>,<DETAIL>
```

警告不一定要求停止当前动作，但上位机必须记录日志。

### 2.6 多行数据块

```text
<TAG>_BEGIN,<SEQ>,...
<DATA_TAG>,...
<DATA_TAG>,...
<TAG>_END,<SEQ>,...
```

规则：

1. `BEGIN` 与 `END` 的 `SEQ` 必须一致。
2. 数据块接收期间，异步 `ERR/WARN/DONE/BOOT/M000x` 仍可能插入。
3. 异步报文不能被误当作数据块内容。
4. 未收到 `_END` 前，数据块不能视为完整。
5. 数据块必须设置超时。
6. 新的 `_BEGIN` 到来时，旧数据块应中止并记录错误。

---

## 3. 启动、心跳与协议握手

### 3.1 设备启动通知

STM32 启动完成后主动发送：

```text
BOOT,OK
```

### 3.2 心跳

上位机发送：

```text
PING
```

STM32 返回：

```text
OK,PING
```

建议行为：

- 上位机每 500 ms 发送一次 `PING`。
- 连续 2000 ms 未收到有效 `OK,PING`，将下位机标记为离线。
- 离线后停止发送需要闭环执行的视觉控制量。
- 串口恢复后重新执行协议握手。

### 3.3 协议版本

上位机发送：

```text
PROTO?
```

STM32 返回：

```text
PROTO,5
```

版本不是 `5` 时：

1. 上位机记录 `ERROR`。
2. 禁止进入自动控制。
3. 允许继续保持串口连接，用于诊断。
4. 不允许把 V5 的 `BALL` 报文解释成旧协议语义。

---

## 4. 基础查询命令

| 上位机命令 | STM32 返回 |
|---|---|
| `PING` | `OK,PING` |
| `PROTO?` | `PROTO,5` |
| `CONFIG` | `CONFIG_BEGIN...CONFIG_END` |
| `SCHEMA` | `SCHEMA_BEGIN...SCHEMA_END` |
| `STATUS` | `STATUS_BEGIN...STATUS_END` |
| `STATUS,FULL` | 完整状态块 |
| `STATUS,LITE` | 精简状态块 |

### 4.1 CONFIG

请求：

```text
CONFIG
```

返回形式：

```text
CONFIG_BEGIN,<seq>
CONFIG_ITEM,<key>,<value>
CONFIG_ITEM,<key>,<value>
CONFIG_END,<seq>
```

固件若已有其他内容标签，可以保持原标签；上位机应按数据块规则接收，而不是假设固定行数。

### 4.2 SCHEMA

请求：

```text
SCHEMA
```

返回形式：

```text
SCHEMA_BEGIN,<seq>
SCHEMA_ITEM,<tag>,<field1>,<field2>,...
SCHEMA_END,<seq>
```

用于描述状态或黑匣子数据字段。具体内容由固件版本决定。

---

## 5. 运动控制命令

### 5.1 前进

上位机：

```text
F
```

STM32：

```text
OK,F,ACCEPTED
```

前进是持续动作，直到收到：

- `STOP` 或 `S`
- 另一运动命令
- 安全保护触发

默认不发送 `DONE,F`。

### 5.2 后退

上位机：

```text
B
```

STM32：

```text
OK,B,ACCEPTED
```

后退同样是持续动作，默认不发送 `DONE,B`。

### 5.3 左转

格式：

```text
L<angle_3digits>
```

角度建议范围：

```text
1~360
```

不足三位左侧补零。

例：

```text
L080
```

接收确认：

```text
OK,L,80,ACCEPTED
```

执行完成：

```text
DONE,L,80
```

错误示例：

```text
ERR,L,INVALID_ANGLE,400,ANGLE_OUT_OF_RANGE
```

### 5.4 右转

例：

```text
R180
```

接收确认：

```text
OK,R,180,ACCEPTED
```

执行完成：

```text
DONE,R,180
```

### 5.5 停止

上位机：

```text
STOP
```

或：

```text
S
```

停车完成：

```text
OK,STOP
```

若停止命令中止正在执行的转向，可额外上报：

```text
ERR,L,ABORTED,<angle>,STOP_COMMAND
ERR,R,ABORTED,<angle>,STOP_COMMAND
```

---

## 6. 状态查询协议

请求：

```text
STATUS
STATUS,FULL
STATUS,LITE
```

完整返回示例结构：

```text
STATUS_BEGIN,<seq>,<fw_ms>
STATE,<motion_state>,<timer_state>,<ramp_state>,<heading_hold>,<rotation_mode>
FAULT,<latched>,<fault_code>,<fault_position>,<description>
HEALTH,<imu_health>,<mag_health>,<gray_health>
BODY,<linear_speed>,<angular_speed>,<heading>,<slip_index>
IMU,<roll>,<pitch>,<yaw>,<gyro_x>,<gyro_y>,<gyro_z>,<bias_x>,<bias_y>,<bias_z>
MAG,<mag_x>,<mag_y>,<mag_z>,<calibration_state>
M0,<target_speed>,<filtered_speed>,<control_speed>,<duty>,<confidence>
M1,<target_speed>,<filtered_speed>,<control_speed>,<duty>,<confidence>
GRAY,<gray0>,<gray1>,<gray2>,<gray3>,<gray4>,<gray5>,<gray6>,<gray7>
LINE,<state>,<error>,<steering>,<base_speed>,<kp>,<kd>,<intersection_state>
LINEGRAY,<gray0>,<gray1>,<gray2>,<gray3>,<gray4>,<gray5>,<gray6>,<gray7>
STATUS_END,<seq>,<partial>,<total_drop>
```

关键字段：

| 字段 | 含义 |
|---|---|
| `seq` | 状态块序号 |
| `fw_ms` | STM32 固件运行时间，单位 ms |
| `partial` | `0` 完整，`1` 不完整 |
| `total_drop` | 累计状态数据丢弃次数 |

完整性规则：

1. `STATUS_BEGIN.seq == STATUS_END.seq`。
2. `partial=1` 时，本次状态不能作为完整快照。
3. `total_drop` 增加时记录通信或缓冲区警告。
4. 数据块超时后丢弃本次临时数据。

---

## 7. 在线调参协议

### 7.1 查询参数列表

```text
TUNEKEYS
```

返回：

```text
TUNEKEYS_BEGIN
TUNEKEY,<key>,<min>,<max>,<current>,<group>,<safety>,<flags>
TUNEKEYS_END
```

### 7.2 查询单个参数

```text
TUNEGET,<key>
```

返回：

```text
TUNE,<key>,<value>
```

### 7.3 查询全部参数

```text
TUNEGET
```

或：

```text
TUNEGET_ALL
```

返回：

```text
TUNE_BEGIN
TUNE,<key>,<value>
TUNE,<key>,<value>
TUNE_END
```

### 7.4 修改单个参数

```text
TUNESET,<key>,<value>
```

示例：

```text
TUNESET,PID_KP,0.250000
```

成功：

```text
TUNE,PID_KP,0.250000
```

失败：

```text
ERR,TUNESET,<ERROR_CODE>,<key>,<detail>
```

### 7.5 批量事务

开始：

```text
TUNEBEGIN
```

返回：

```text
OK,TUNEBEGIN
```

暂存：

```text
TUNESET,<key>,<value>
```

返回：

```text
TUNESTAGE,<key>,<value>
```

提交：

```text
TUNECOMMIT
```

返回：

```text
OK,TUNECOMMIT,<count>
```

取消：

```text
TUNEABORT
```

返回：

```text
OK,TUNEABORT
```

事务要求：

1. 提交前只暂存，不改变正式运行参数，或由固件明确说明暂存策略。
2. 任一字段非法时，整个提交应失败或返回明确的部分失败信息。
3. 不能静默截断超范围参数。

### 7.6 保存、恢复与导出

保存到 Flash：

```text
TUNESAVE
```

或：

```text
SAVECONFIG
```

恢复默认：

```text
TUNERESET
```

导出：

```text
TUNEEXPORT
```

---

## 8. 循迹控制协议

开始：

```text
LINESTART
```

返回：

```text
OK,LINESTART
```

停止：

```text
LINESTOP
```

返回：

```text
OK,LINESTOP
```

查询状态：

```text
LINESTATUS
```

设置覆盖模式：

```text
LINEMODE,OVERRIDE
```

设置排他模式：

```text
LINEMODE,EXCLUSIVE
```

设置参数：

```text
LINESET,BASE,<value>
LINESET,KP,<value>
LINESET,KD,<value>
LINESET,MAXW,<value>
```

推荐统一走在线调参接口：

```text
TUNESET,LINE_<PARAMETER>,<value>
```

---

## 9. 灰度传感器校准

### 9.1 采集黑线

```text
LINECAL,LINE
```

立即返回：

```text
LINECAL,LINE,BUSY
```

完成返回：

```text
OK,LINECAL,LINE
```

### 9.2 采集背景

```text
LINECAL,BACKGROUND
```

立即返回：

```text
LINECAL,BACKGROUND,BUSY
```

完成返回：

```text
OK,LINECAL,BACKGROUND
```

### 9.3 应用结果

```text
LINECAL,APPLY
```

返回：

```text
OK,LINECAL,APPLY
```

### 9.4 查询原始灰度

```text
GRAYSHOW
```

返回：

```text
GRAY,<gray0>,<gray1>,<gray2>,<gray3>,<gray4>,<gray5>,<gray6>,<gray7>
```

---

## 10. 故障处理协议

清除故障：

```text
FAULTCLR
```

或：

```text
CLEARFAULT
```

成功：

```text
OK,FAULTCLR
```

故障仍然存在：

```text
ERR,SAFETY,FAULT_ACTIVE,,<detail>
```

常见故障示例：

```text
ERR,SAFETY,FAULT_LATCHED,,USE_FAULTCLR
ERR,CLOCK,PLL_UNLOCKED,MOTION_LOCKED,
ERR,IMU,INIT_FAIL,,
ERR,LINE,ANOMALY,,
```

安全规则：

1. 故障锁存时禁止执行危险运动命令。
2. `FAULTCLR` 只能在真实故障条件已经消失时成功。
3. 上位机收到严重 `ERR` 后，不得只清界面状态而继续控制。
4. 错误必须写日志，并保留原始报文。

---

## 11. 电机自检协议

启动：

```text
MTEST,<motor>,<duty>,<duration_ms>
```

`motor` 可取：

```text
L
R
0
1
```

示例：

```text
MTEST,L,0.18,500
```

开始：

```text
MTEST,BUSY
```

查询状态：

```text
MTESTSTATUS
```

帮助：

```text
MTEST,HELP
```

通过示例：

```text
MTEST,IDLE,PASSED,SAFETY_GATE_OPEN
```

自检过程中必须有占空比和持续时间上限，并允许 `STOP` 中止。

---

## 12. 黑匣子协议

查询信息：

```text
BLACKBOX,INFO
```

或：

```text
BB,INFO
```

清空：

```text
BLACKBOX,CLEAR
```

冻结：

```text
BLACKBOX,FREEZE
```

恢复：

```text
BLACKBOX,RESUME
```

导出：

```text
BLACKBOX,DUMP
```

返回：

```text
BLACKBOX_BEGIN,<dump_seq>,<capacity>,<count>,<total_written>
BB_SCHEMA,<field1>,<field2>,...
BB,<data1>,<data2>,...
BB,<data1>,<data2>,...
BLACKBOX_END,<dump_seq>,<count>
```

帮助：

```text
BLACKBOX,HELP
```

上位机必须检查：

1. `dump_seq` 一致。
2. 实际 `BB` 行数与 `count` 一致。
3. 数据块超时或缺行时标记导出失败。
4. 不完整数据可以保存用于诊断，但不得伪装成完整结果。

---

## 13. STM32 异步上报

STM32 可在任何时刻发送：

```text
BOOT,OK
WARN,<MODULE>,<CODE>,<KEY>,<DETAIL>
ERR,<MODULE>,<CODE>,<KEY>,<DETAIL>
GYROCAL,...
MAGCAL,...
OK,INTERSECTION,STOP
DONE,L,<angle>
DONE,R,<angle>
M0001
M0002
M0003
M0004
```

四个按键事件：

```text
M0001
M0002
M0003
M0004
```

要求：

- 异步消息不能当作某个请求的固定“下一条响应”。
- 上位机需要按 `TAG` 分流。
- `DONE` 表示动作真正完成。
- `WARN/ERR` 必须立即记录。
- 按键事件进入事件队列，不阻塞串口接收线程。

---

## 14. 上位机视觉报文

### 14.1 通用目标有效

```text
TARGET,<seq>,<x>,<y>,<angle>,<confidence>
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `seq` | `uint32` | 视觉消息序号 |
| `x` | 浮点 | 横坐标，像素 |
| `y` | 浮点 | 纵坐标，像素 |
| `angle` | 浮点 | 角度，度 |
| `confidence` | 浮点 | `0.0~1.0` |

格式约定：

- `x/y/angle` 固定保留 2 位小数。
- `confidence` 固定保留 3 位小数。
- 拒绝 NaN、Inf 和超范围置信度。

示例：

```text
TARGET,152,320.50,241.20,-3.70,0.920
```

### 14.2 通用目标丢失

```text
LOST,<seq>
```

示例：

```text
LOST,153
```

目标无效时不得继续发送上一帧 `TARGET`。

### 14.3 滚球位置报文

```text
BALL,<seq>,<offset_mm>,<confidence_0_255>,<status>
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `seq` | `uint32` | 滚球视觉消息序号 |
| `offset_mm` | `int32` | 钢球相对当前目标位置的一维偏差，单位 mm |
| `confidence_0_255` | `uint8` 语义 | 置信度，范围 `0~255` |
| `status` | 枚举字符串 | `OK/LOST/CALIB/ERROR` |

偏差方向：

```text
OpenCV 标定轴线 P1 -> P2 为正方向
```

状态语义：

| status | 含义 | 是否允许使用 offset_mm |
|---|---|---|
| `OK` | 检测有效、标定完成 | 是 |
| `LOST` | 没有检测到球 | 否 |
| `CALIB` | 检测到球，但零点仍在标定 | 否 |
| `ERROR` | 配置、相机或计算异常 | 否 |

示例：

```text
BALL,123,-18,220,OK
BALL,124,0,0,LOST
BALL,125,0,0,CALIB
BALL,126,0,0,ERROR
```

强制规则：

1. 仅 `status=OK` 时允许使用 `offset_mm`。
2. `status!=OK` 时，`offset_mm` 和 `confidence_0_255` 必须均为 `0`。
3. 不允许丢球后发送旧位置。
4. `confidence_0_255` 必须在 `0~255`。
5. 非法状态字符串必须拒绝。
6. STM32 不需要逐条回复高频 `BALL`。
7. `seq` 递增，按 `uint32_t` 自然回绕。
8. `seq` 只用于新旧判断和丢帧统计，不是时间戳。
9. 超过视觉超时没有收到新 `BALL` 时，数据自动失效。

推荐视觉超时：

```text
200 ms
```

假设上位机视觉频率约为 20~30 Hz。最终值应放入下位机配置，而不是硬编码散落在控制代码中。

### 14.4 BALL 接收安全状态机

伪代码：

```c
on_ball_message(msg):
    validate_field_count()
    validate_integer_ranges()
    validate_status()

    last_ball_rx_ms = now()

    if msg.status != OK:
        ball_valid = false
        disable_ball_pid()
        enter_safe_output()
        return

    if msg.seq is older or duplicate:
        ignore_and_log()
        return

    ball_offset_mm = msg.offset_mm
    ball_confidence = msg.confidence
    ball_valid = true
```

周期检查：

```c
if now() - last_ball_rx_ms > ball_timeout_ms:
    ball_valid = false
    disable_ball_pid()
    enter_safe_output()
```

---

## 15. 序号规则

`TARGET`、`LOST` 和 `BALL` 使用 `uint32_t seq`。

建议：

1. 每生成一条视觉报文只增加一次。
2. `0xFFFFFFFF -> 0` 允许自然回绕。
3. 下位机使用无符号差判断新旧，不能简单使用 `new_seq > old_seq`。
4. 重连后上位机可重新从 0 开始；下位机应在握手完成或长时间超时后重置序号跟踪。
5. 重复序号可以忽略并记录 `DEBUG/WARN`。
6. 小范围跳号表示丢帧，不应直接停止系统。
7. 大范围反向跳变通常表示上位机重启，应结合心跳和握手判断。

---

## 16. 推荐启动流程

```text
STM32 启动
    ↓
STM32 -> BOOT,OK
    ↓
树莓派 -> PING
    ↓
STM32 -> OK,PING
    ↓
树莓派 -> PROTO?
    ↓
STM32 -> PROTO,5
    ↓
树莓派 -> CONFIG
    ↓
STM32 -> CONFIG_BEGIN ... CONFIG_END
    ↓
树莓派 -> SCHEMA
    ↓
STM32 -> SCHEMA_BEGIN ... SCHEMA_END
    ↓
树莓派 -> TUNEKEYS
    ↓
STM32 -> TUNEKEYS_BEGIN ... TUNEKEYS_END
    ↓
树莓派开始发送 BALL / 查询 / 调参 / 控制命令
```

比赛模式可以简化为：

```text
BOOT,OK
PING / OK,PING
PROTO? / PROTO,5
BALL,...
```

但协议版本校验不能省略。

---

## 17. 超时与安全策略

| 项目 | 建议值 | 超时动作 |
|---|---:|---|
| 串口单次读 | 50 ms | 返回主循环，不退出程序 |
| 串口写 | 200 ms | 记录 ERROR，进入重连 |
| 握手 | 1500 ms | 标记离线并重试 |
| 心跳发送 | 500 ms | 发送 `PING` |
| 心跳失联 | 2000 ms | 禁止自动控制 |
| 多行数据块 | 1000 ms 左右 | 丢弃不完整块 |
| BALL 视觉数据 | 200 ms 建议 | 退出滚球闭环 |
| 串口重连间隔 | 1000 ms | 周期重试 |

关键原则：

- 串口错误不能使上位机进程退出。
- 视觉无效不能沿用旧坐标。
- 心跳在线不等于视觉有效。
- 视觉有效不等于底盘安全状态正常。
- PID 启用条件应同时满足：协议匹配、链路在线、无锁存故障、`BALL status=OK`、视觉数据未超时。

---

## 18. 非法报文处理

遇到以下情况时丢弃报文并写日志：

- 行长超过限制。
- 字段数量不正确。
- 数字解析失败。
- 数值溢出。
- `confidence` 超出范围。
- `BALL status` 非法。
- 非 `OK` 的 `BALL` 携带非零位置。
- 多行块序号不一致。
- 未知标签。
- 行内包含不支持的格式。

建议日志内容至少包含：

```text
方向、原始报文、错误代码、时间、串口状态
```

不得因单条坏报文退出接收线程或主程序。

---

## 19. 日志等级建议

| 情况 | 等级 |
|---|---|
| 正常发送与正常响应 | DEBUG |
| 启动、连接、重连成功、版本匹配 | INFO |
| 未知标签、重复序号、轻微丢帧、状态块不完整 | WARN |
| ERR 报文、解析失败、数据块超时、视觉超时 | ERROR |
| 串口打不开、协议不兼容、持续失联 | ERROR |

高频 `BALL` 正常报文不应逐帧写 INFO。建议周期汇总或 DEBUG 记录。

---

## 20. V5 相对 V4 的变化

1. 协议版本由 `4` 升级为 `5`。
2. 新增：

```text
BALL,<seq>,<offset_mm>,<confidence_0_255>,<status>
```

3. 新增状态：

```text
OK
LOST
CALIB
ERROR
```

4. 规定非 `OK` 状态必须发送零位置、零置信度。
5. 规定下位机只有在 `OK` 且未超时时才允许使用滚球位置。
6. 保留全部 V4 命令和 `TARGET/LOST`，便于回退和其他控制题复用。

---

## 21. 最小比赛通信示例

```text
STM32 -> BOOT,OK
HOST  -> PING
STM32 -> OK,PING
HOST  -> PROTO?
STM32 -> PROTO,5

HOST  -> BALL,1,0,0,CALIB
HOST  -> BALL,2,0,0,CALIB
HOST  -> BALL,3,-3,231,OK
HOST  -> BALL,4,-5,228,OK
HOST  -> BALL,5,0,0,LOST
HOST  -> BALL,6,2,219,OK
```

STM32 行为：

```text
CALIB：不启用滚球 PID
OK：更新偏差并允许闭环
LOST：立即使视觉位置失效
恢复 OK：使用新位置重新进入闭环
```
