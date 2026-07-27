# 工程通信协议

> **协议版本：V4**  
> 本协议用于上位机与底盘控制器之间的 UART 文本通信，覆盖运动控制、状态查询、在线调参、故障处理、循迹控制和黑匣子导出等功能。

## 目录

- [1. 协议概述](#1-协议概述)
- [2. 基本报文格式](#2-基本报文格式)
- [3. 基础通信命令](#3-基础通信命令)
- [4. 运动控制命令](#4-运动控制命令)
- [5. 状态查询协议](#5-状态查询协议)
- [6. 在线调参协议](#6-在线调参协议)
- [7. 循迹控制协议](#7-循迹控制协议)
- [8. 灰度传感器校准协议](#8-灰度传感器校准协议)
- [9. 故障处理协议](#9-故障处理协议)
- [10. 电机自检协议](#10-电机自检协议)
- [11. 黑匣子协议](#11-黑匣子协议)
- [12. 异步上报消息](#12-异步上报消息)
- [13. 推荐通信流程](#13-推荐通信流程)

---

## 1. 协议概述

本工程采用基于 **UART0** 的 ASCII 文本通信协议，用于上位机与底盘控制器之间的控制、状态查询、在线调参、故障处理、循迹控制和黑匣子导出。

### 1.1 串口配置

| 项目 | 配置 |
|---|---|
| 通信接口 | UART0 |
| 波特率 | 115200 bps |
| 数据位 | 8 |
| 停止位 | 1 |
| 校验位 | 无 |
| 硬件流控 | 无 |
| 协议版本 | 4 |
| 数据格式 | ASCII 文本 |
| 字段分隔符 | `,` |
| 报文结束符 | `\r\n` 或 `\n` |

### 1.2 通信约定

1. 每条报文以换行符结束。
2. 多字段之间使用英文逗号 `,` 分隔。
3. 命令和标签建议统一使用大写字母。
4. 多行数据使用 `BEGIN` 和 `END` 报文包围。
5. 异步消息可能在任意时刻由控制器主动发送。
6. 上位机接收时应按行缓存，不应假设一次串口读取刚好得到一条完整报文。

---

## 2. 基本报文格式

### 2.1 命令格式

```text
COMMAND,param1,param2,...
```

### 2.2 成功响应

```text
OK
OK,<COMMAND>
OK,<COMMAND>,<DETAIL>
```

### 2.3 错误响应

```text
ERR,<OPERATION>,<ERROR_CODE>,<KEY>,<DETAIL>
```

| 字段 | 描述 |
|---|---|
| `OPERATION` | 发生错误的操作或模块 |
| `ERROR_CODE` | 错误代码 |
| `KEY` | 相关参数或对象，可为空 |
| `DETAIL` | 错误详细信息，可为空 |

### 2.4 警告消息

```text
WARN,<MODULE>,<WARNING_CODE>,<KEY>,<DETAIL>
```

### 2.5 数据上报

```text
<TAG>,<VALUE1>,<VALUE2>,...
```

### 2.6 多行数据块

```text
<TAG>_BEGIN,<SEQ>,...
<DATA_TAG>,...
<DATA_TAG>,...
<TAG>_END,<SEQ>,...
```

- `SEQ` 为数据块序号。
- 上位机应检查开始报文和结束报文中的序号是否一致。
- 数据块未完整接收时，不应直接使用其中的数据。

---

## 3. 基础通信命令

| 命令 | 描述 | 返回 |
|---|---|---|
| `PING` | 检查设备是否在线 | `OK,PING` |
| `PROTO?` | 查询协议版本 | `PROTO,4` |
| `CONFIG` | 查询固件配置和功能开关 | `CONFIG_BEGIN...CONFIG_END` |
| `SCHEMA` | 查询遥测数据字段定义 | `SCHEMA_BEGIN...SCHEMA_END` |
| `STATUS` | 查询完整状态 | `STATUS_BEGIN...STATUS_END` |
| `STATUS,FULL` | 查询完整状态 | `STATUS_BEGIN...STATUS_END` |
| `STATUS,LITE` | 查询精简状态 | `STATUS_BEGIN...STATUS_END` |

设备启动后主动发送：

```text
BOOT,OK
```

---

## 4. 运动控制命令

### 4.1 通用响应规则

下位机收到运动命令并通过参数检查后，应立即返回“已接受”响应。该响应只表示命令已经被接收并准备执行，**不表示动作已经完成**。

动作完成后，下位机应再主动发送完成消息。

统一格式：

```text
OK,<COMMAND>,<PARAMETERS>,ACCEPTED
DONE,<COMMAND>,<PARAMETERS>
```

执行失败时返回：

```text
ERR,<COMMAND>,<ERROR_CODE>,<PARAMETERS>,<DETAIL>
```

例如，上位机发送左转 `80°`：

```text
L080
```

下位机确认接收：

```text
OK,L,80,ACCEPTED
```

左转完成后，下位机主动发送：

```text
DONE,L,80
```

如果命令参数非法：

```text
ERR,L,INVALID_ANGLE,80,ANGLE_OUT_OF_RANGE
```

> 上位机只有收到 `DONE` 后，才能认为本次转向动作已经完成。收到 `OK,...,ACCEPTED` 时，只能将任务状态设置为“执行中”。

### 4.2 前进

命令：

```text
F
```

下位机确认接收：

```text
OK,F,ACCEPTED
```

前进命令默认为持续执行，直到收到停止命令、其他运动命令或触发安全保护，因此不主动发送 `DONE,F`。

### 4.3 后退

命令：

```text
B
```

下位机确认接收：

```text
OK,B,ACCEPTED
```

后退命令默认为持续执行，直到收到停止命令、其他运动命令或触发安全保护，因此不主动发送 `DONE,B`。

### 4.4 左转

格式：

```text
L0<angle>
```

其中：

- `angle` 为十进制整数角度。
- 范围建议为 `1~360`。
- 不足三位时前面补零。

示例：

```text
L080
```

表示原地左转 `80°`。

下位机确认接收：

```text
OK,L,80,ACCEPTED
```

左转完成后：

```text
DONE,L,80
```

### 4.5 右转

格式：

```text
R0<angle>
```

其中：

- `angle` 为十进制整数角度。
- 范围建议为 `1~360`。
- 不足三位时前面补零。

示例：

```text
R180
```

表示原地右转 `180°`。

下位机确认接收：

```text
OK,R,180,ACCEPTED
```

右转完成后：

```text
DONE,R,180
```

### 4.6 停止

命令：

```text
STOP
```

或：

```text
S
```

下位机完成停车后返回：

```text
OK,STOP
```

若下位机当前正在执行转向命令，收到停止命令后应中止当前动作，并可主动发送：

```text
ERR,L,ABORTED,<angle>,STOP_COMMAND
```

或：

```text
ERR,R,ABORTED,<angle>,STOP_COMMAND
```

---

## 5. 状态查询协议

### 5.1 查询命令

```text
STATUS
STATUS,FULL
STATUS,LITE
```

### 5.2 返回格式

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

### 5.3 通用字段说明

| 字段 | 描述 |
|---|---|
| `seq` | 状态数据块序号 |
| `fw_ms` | 固件运行时间，单位为毫秒 |
| `partial` | `0` 表示完整，`1` 表示数据不完整 |
| `total_drop` | 累计状态数据丢弃次数 |

### 5.4 数据块完整性检查

1. `STATUS_BEGIN` 与 `STATUS_END` 的 `seq` 必须一致。
2. `partial` 为 `1` 时，应将本次状态标记为不完整。
3. `total_drop` 增加时，应记录通信或缓冲区异常日志。
4. 未收到 `STATUS_END` 前，不应将当前数据块判定为完整状态。

---

## 6. 在线调参协议

### 6.1 查询参数列表

```text
TUNEKEYS
```

返回：

```text
TUNEKEYS_BEGIN
TUNEKEY,<key>,<min>,<max>,<current>,<group>,<safety>,<flags>
TUNEKEYS_END
```

### 6.2 查询单个参数

```text
TUNEGET,<key>
```

返回：

```text
TUNE,<key>,<value>
```

### 6.3 查询全部参数

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

### 6.4 修改单个参数

```text
TUNESET,<key>,<value>
```

示例：

```text
TUNESET,PID_KP,0.250000
```

成功返回：

```text
TUNE,PID_KP,0.250000
```

错误返回：

```text
ERR,TUNESET,<ERROR_CODE>,<key>,<detail>
```

### 6.5 批量调参事务

#### 开始事务

```text
TUNEBEGIN
```

返回：

```text
OK,TUNEBEGIN
```

#### 暂存参数

```text
TUNESET,<key>,<value>
```

返回：

```text
TUNESTAGE,<key>,<value>
```

#### 提交参数

```text
TUNECOMMIT
```

返回：

```text
OK,TUNECOMMIT,<count>
```

#### 取消事务

```text
TUNEABORT
```

返回：

```text
OK,TUNEABORT
```

### 6.6 参数保存

```text
TUNESAVE
```

或：

```text
SAVECONFIG
```

用于将当前参数保存到 Flash。

### 6.7 恢复默认参数

```text
TUNERESET
```

### 6.8 导出参数

```text
TUNEEXPORT
```

---

## 7. 循迹控制协议

### 7.1 开始循迹

```text
LINESTART
```

返回：

```text
OK,LINESTART
```

### 7.2 停止循迹

```text
LINESTOP
```

返回：

```text
OK,LINESTOP
```

### 7.3 查询循迹状态

```text
LINESTATUS
```

### 7.4 设置循迹模式

覆盖模式：

```text
LINEMODE,OVERRIDE
```

排他模式：

```text
LINEMODE,EXCLUSIVE
```

### 7.5 设置循迹参数

```text
LINESET,BASE,<value>
LINESET,KP,<value>
LINESET,KD,<value>
LINESET,MAXW,<value>
```

建议统一使用：

```text
TUNESET,LINE_<PARAMETER>,<value>
```

---

## 8. 灰度传感器校准协议

### 8.1 采集黑线数据

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

### 8.2 采集背景数据

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

### 8.3 应用校准结果

```text
LINECAL,APPLY
```

返回：

```text
OK,LINECAL,APPLY
```

### 8.4 查询灰度数据

```text
GRAYSHOW
```

返回：

```text
GRAY,<gray0>,<gray1>,<gray2>,<gray3>,<gray4>,<gray5>,<gray6>,<gray7>
```

---

## 9. 故障处理协议

### 9.1 清除故障

```text
FAULTCLR
```

或：

```text
CLEARFAULT
```

成功返回：

```text
OK,FAULTCLR
```

失败返回：

```text
ERR,SAFETY,FAULT_ACTIVE,,<detail>
```

### 9.2 常见故障返回

```text
ERR,SAFETY,FAULT_LATCHED,,USE_FAULTCLR
ERR,CLOCK,PLL_UNLOCKED,MOTION_LOCKED,
ERR,IMU,INIT_FAIL,,
ERR,LINE,ANOMALY,,
```

---

## 10. 电机自检协议

### 10.1 启动自检

```text
MTEST,<motor>,<duty>,<duration_ms>
```

其中 `motor` 可为：

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

开始返回：

```text
MTEST,BUSY
```

完成后返回电机测试结果，包括占空比、编码器增量、估算速度和方向。

### 10.2 查询自检状态

```text
MTESTSTATUS
```

### 10.3 查询帮助

```text
MTEST,HELP
```

自检通过后可能返回：

```text
MTEST,IDLE,PASSED,SAFETY_GATE_OPEN
```

---

## 11. 黑匣子协议

### 11.1 查询黑匣子信息

```text
BLACKBOX,INFO
```

或：

```text
BB,INFO
```

### 11.2 清空数据

```text
BLACKBOX,CLEAR
```

### 11.3 冻结记录

```text
BLACKBOX,FREEZE
```

### 11.4 恢复记录

```text
BLACKBOX,RESUME
```

### 11.5 导出数据

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

### 11.6 查询帮助

```text
BLACKBOX,HELP
```

---

## 12. 异步上报消息

设备可能主动发送以下消息：

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

### 12.1 MCU 按键事件

```text
M0001
M0002
M0003
M0004
```

分别表示四个 MCU 按键事件。

上位机不应将这些消息当作普通命令响应，而应按异步事件处理。

其中：

```text
DONE,L,<angle>
DONE,R,<angle>
```

表示对应的转向动作已经真正完成。它们与立即返回的 `OK,...,ACCEPTED` 含义不同。

---

## 13. 推荐通信流程

```text
设备启动
    ↓
BOOT,OK
    ↓
上位机发送 PING
    ↓
OK,PING
    ↓
上位机发送 PROTO?
    ↓
PROTO,4
    ↓
上位机发送 CONFIG
    ↓
CONFIG_BEGIN ... CONFIG_END
    ↓
上位机发送 SCHEMA
    ↓
SCHEMA_BEGIN ... SCHEMA_END
    ↓
上位机发送 TUNEKEYS
    ↓
TUNEKEYS_BEGIN ... TUNEKEYS_END
    ↓
上位机开始发送控制、查询或调参命令
```

---

## 附录 A：上位机接收建议

1. 使用字节缓冲区持续接收串口数据。
2. 查找 `\n`，按行切分报文。
3. 去除行末的 `\r` 和 `\n`。
4. 空行直接忽略。
5. 使用英文逗号分割字段。
6. 根据首字段识别消息类型。
7. 未识别的标签应记录警告日志，但不应终止程序。
8. 单行超过最大长度时应丢弃当前行并记录错误。
9. 多行数据块应设置接收超时，避免永久等待 `_END`。
10. 异步消息和命令响应必须能够同时处理。

## 附录 B：建议的日志等级

| 情况 | 建议等级 |
|---|---|
| 正常命令发送与响应 | `DEBUG` |
| 设备启动、连接成功、协议匹配 | `INFO` |
| 未知标签、状态数据不完整 | `WARN` |
| 命令返回 `ERR`、数据块超时 | `ERROR` |
| 串口无法打开、协议版本不兼容 | `ERROR` |

---

## 附录 C：修订记录

### V4 Revision 1

- 删除尚未使用的云台控制描述。
- 明确转向命令采用两阶段反馈：
  - `OK,...,ACCEPTED`：命令已接收，即将执行；
  - `DONE,...`：动作已经完成。
- 补充左转、右转的参数范围和补零规则。
- 补充转向被停止命令中止时的错误反馈。
