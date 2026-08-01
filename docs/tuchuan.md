# ETEST 2026 车载图传系统

本项目用于 `etest_2026` 车载平衡滚球系统的图像传输、场外实时显示和分段录像。

当前方案采用两块树莓派：

- **发送端树莓派**安装在小车上，负责摄像头采集、视觉算法和 H.264 图传；
- **接收端树莓派**放在比赛环形线路外，连接 HDMI 显示器，负责实时显示和保存每段测试录像；
- 两块树莓派通过两个支持 **Ad-Hoc/IBSS** 的 USB 无线网卡直接通信；
- **发送端不保存视频**；
- 接收端根据下位机测试状态自动开始和结束录像。

---

## 1. 设计目标

图传系统需要完成以下功能：

1. 摄像头画面能够覆盖完整摆杆和钢球运动区域；
2. 接收端能够稳定、实时显示画面；
3. 每次正式测试生成一个独立视频文件；
4. 下位机发送 `DONE` 后，当前录像自动结束；
5. 图传线程不能明显阻塞发送端视觉主循环；
6. 网络短暂抖动时，不允许旧帧不断排队形成数秒延迟；
7. 自动录像控制失败时，接收端仍能手动开始或停止录像。

本方案优先保证：

```text
低延迟 > 不丢帧
录像可靠性 > 文件压缩率
系统可恢复性 > 代码复杂度
```

因此，实时图传允许丢弃过期帧；接收端录像使用 MJPG/AVI，文件较大，但写入和恢复更简单。

---

## 2. 系统架构

```text
┌──────────────────── 车载发送端树莓派 ────────────────────┐
│                                                          │
│  USB/CSI Camera                                          │
│       │                                                  │
│       ├── 原有 OpenCV / YOLO / 钢球位置检测               │
│       │                                                  │
│       └── VideoStreamer 后台线程                          │
│             ├── 缩放到 960×480                           │
│             ├── x264 低延迟编码                           │
│             ├── H.264 → RTP                              │
│             └── UDP 5600                                 │
│                                                          │
│  下位机串口消息                                           │
│       ├── CONTESTSTART ACK → TEST_START                  │
│       └── DONE             → TEST_DONE                   │
│                              UDP 5601                    │
└───────────────────────┬──────────────────────────────────┘
                        │
                Ad-Hoc / IBSS
             192.168.60.0/24
                        │
┌───────────────────────▼──────────────────────────────────┐
│                     接收端树莓派                          │
│                                                          │
│  UDP 5600 → RTP 解包 → H.264 解码 → OpenCV              │
│                                 ├── HDMI 全屏显示          │
│                                 ├── 1 秒内存预录           │
│                                 └── MJPG/AVI 分段录像      │
│                                                          │
│  UDP 5601 → TEST_START / TEST_DONE → 录像状态机           │
└──────────────────────────────────────────────────────────┘
```

---

## 3. 默认参数

| 参数 | 默认值 |
|---|---|
| 网络模式 | Ad-Hoc / IBSS |
| SSID | `ETEST-DIRECT` |
| 无线接口 | `wlan1` |
| 接收端 IP | `192.168.60.1/24` |
| 发送端 IP | `192.168.60.2/24` |
| 默认频段 | 5 GHz |
| 默认信道 | 36 |
| 视频协议 | H.264 + RTP + UDP |
| 视频端口 | `5600` |
| 控制端口 | `5601` |
| 图传分辨率 | `960 × 480` |
| 图传帧率 | `25 FPS` |
| 图传码率 | `2500 kbit/s` |
| GOP | 25 帧 |
| B 帧 | 关闭 |
| 接收抖动缓存 | 80 ms |
| 预录时间 | 1.0 s |
| DONE 后尾录 | 0.5 s |
| 单段最长录像 | 90 s |
| 接收端录像格式 | MJPG / AVI |

所有网络参数必须在发送端、接收端和启动脚本中保持一致。

---

## 4. 文件组成

建议项目目录整理成：

```text
etest_2026/
├── CMakeLists.txt
├── config/
│   └── record.toml
├── include/
│   └── stream/
│       └── video_streamer.hpp
├── src/
│   ├── state/
│   │   └── search.cpp
│   └── stream/
│       └── video_streamer.cpp
└── build/
    └── etest_2026

receiver/
├── receiver.py
└── receiver_adhoc.sh

sender/
└── sender_adhoc.sh
```

配套文件：

```text
video_streamer.hpp       发送端图传类声明
video_streamer.cpp       H.264/RTP/UDP 编码与控制消息发送
receiver.py              接收、显示、预录和录像程序
receiver_adhoc.sh        接收端 Ad-Hoc 配置和启动脚本
sender_adhoc.sh          发送端 Ad-Hoc 配置和启动脚本
```

---

## 5. 硬件准备

### 5.1 发送端

- 一块树莓派；
- 摄像头；
- 一个支持 Ad-Hoc/IBSS 的 USB 无线网卡；
- 稳定的独立降压供电；
- 刚性摄像头支架；
- 足够的散热。

### 5.2 接收端

- 一块树莓派；
- 一个支持 Ad-Hoc/IBSS 的 USB 无线网卡；
- HDMI 显示器；
- 足够空间的存储卡；
- 键盘，至少用于 `Q`、`Esc` 和 `R`；
- 稳定电源。

### 5.3 网卡能力检查

先查看无线接口：

```bash
nmcli device status
```

常见情况：

```text
wlan0    树莓派板载 Wi-Fi
wlan1    USB 无线网卡
```

部分 USB 网卡可能显示为：

```text
wlx001122334455
```

检查 Ad-Hoc 能力：

```bash
nmcli -t -f WIFI-PROPERTIES.ADHOC device show wlan1
```

期望：

```text
WIFI-PROPERTIES.ADHOC:yes
```

也可以检查驱动是否支持 IBSS：

```bash
iw list | sed -n '/Supported interface modes:/,/Band /p'
```

期望包含：

```text
* IBSS
```

芯片宣传支持 Ad-Hoc，不代表当前 Linux 驱动一定支持，必须实机确认。

---

## 6. 软件依赖

以下步骤在发送端和接收端都执行：

```bash
sudo apt update

sudo apt install -y \
    network-manager \
    iw \
    iproute2 \
    iputils-ping \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav
```

接收端额外安装：

```bash
sudo apt install -y \
    python3-opencv \
    python3-numpy
```

发送端需要 OpenCV 开发库；如果原项目已经能够编译，一般无需重复安装。缺失时执行：

```bash
sudo apt install -y libopencv-dev
```

---

## 7. 检查 GStreamer 和 OpenCV

### 7.1 发送端检查编码器

```bash
gst-inspect-1.0 x264enc
gst-inspect-1.0 rtph264pay
```

如果提示找不到 `x264enc`，通常是没有安装：

```bash
gstreamer1.0-plugins-ugly
```

### 7.2 发送端检查 OpenCV

```bash
opencv_version --verbose | grep -i gstreamer
```

期望：

```text
GStreamer: YES
```

### 7.3 接收端检查解码器

```bash
gst-inspect-1.0 rtpjitterbuffer
gst-inspect-1.0 rtph264depay
gst-inspect-1.0 avdec_h264
```

### 7.4 接收端检查 Python OpenCV

```bash
python3 - <<'PY'
import cv2

for line in cv2.getBuildInformation().splitlines():
    if "GStreamer" in line:
        print(line)
PY
```

期望：

```text
GStreamer: YES
```

如果 Python OpenCV 显示 `GStreamer: NO`，`receiver.py` 将无法打开当前接收管线。

---

## 8. 发送端集成

### 8.1 复制图传文件

复制：

```text
video_streamer.hpp
```

到：

```text
etest_2026/include/stream/video_streamer.hpp
```

复制：

```text
video_streamer.cpp
```

到：

```text
etest_2026/src/stream/video_streamer.cpp
```

### 8.2 修改 `CMakeLists.txt`

在 `add_executable()` 的源文件列表中加入：

```cmake
src/stream/video_streamer.cpp
```

示例：

```cmake
add_executable(etest_2026
    src/main.cpp
    src/core/config.cpp
    src/core/context.cpp
    src/core/logger.cpp
    src/state/state.cpp
    src/state/start.cpp
    src/state/search.cpp
    src/state/end.cpp
    src/state/error.cpp
    src/stream/video_streamer.cpp
    src/vision/camera.cpp
    src/vision/latest_frame_capture.cpp
    src/vision/video_recorder.cpp
    src/uart/uart.cpp
    src/uart/protocol.cpp
)
```

### 8.3 在 `search.cpp` 引入图传类

```cpp
#include "stream/video_streamer.hpp"
```

### 8.4 创建图传配置

在进入主循环之前创建：

```cpp
etest::stream::VideoStreamConfig stream_cfg;

stream_cfg.enabled = true;
stream_cfg.receiver_ip = "192.168.60.1";
stream_cfg.video_port = 5600;
stream_cfg.control_port = 5601;
stream_cfg.width = 960;
stream_cfg.height = 480;
stream_cfg.fps = 25;
stream_cfg.bitrate_kbps = 2500;

etest::stream::VideoStreamer video_streamer(stream_cfg);

if(!video_streamer.start())
{
    ETEST_LOG_ERROR(
        "STREAM",
        "video streamer failed to start; vision loop will continue");
}
```

注意：当前使用 Ad-Hoc，接收端地址是：

```cpp
stream_cfg.receiver_ip = "192.168.60.1";
```

不要继续使用旧热点方案中的 `192.168.50.1`。

### 8.5 提交摄像头帧

找到新帧处理位置：

```cpp
if(has_new_frame)
{
    ctx.frame = packet.frame;
```

修改为：

```cpp
if(has_new_frame)
{
    ctx.frame = packet.frame;

    // 发送端只图传，不录像。
    video_streamer.submit(ctx.frame);
```

`submit()` 会：

1. 把图像缩放为目标尺寸；
2. 覆盖上一张尚未编码的旧帧；
3. 唤醒后台编码线程；
4. 立即返回主循环。

这意味着编码速度不足时会丢旧帧，而不是让图像延迟越来越大。

### 8.6 正式测试开始消息

收到下位机 `CONTESTSTART` 确认时：

```cpp
if(uart::protocol::isContestStartAck(msg))
{
    if(!ctx.task.contest_start_acked)
    {
        video_streamer.sendTestStart(
            ctx.task.session_id,
            ctx.task.active_mode.empty()
                ? "UNKNOWN"
                : ctx.task.active_mode);
    }

    ctx.task.contest_start_acked = true;
    ctx.task_phase = TaskPhase::CONTEST;

    ETEST_LOG_INFO(
        "SEARCH",
        "CONTESTSTART confirmed");

    continue;
}
```

发送给接收端：

```text
TEST_START|session_id|mode
```

示例：

```text
TEST_START|103|H5_LAP_CENTER
```

### 8.7 测试结束消息

收到下位机 `DONE` 时：

```cpp
if(auto done = uart::protocol::parseDone(msg))
{
    ETEST_LOG_INFO(
        "SEARCH",
        "DONE received: " + done->mode
            + " result=" + done->result);

    video_streamer.sendTestDone(
        ctx.task.session_id,
        done->mode,
        done->result);

    done_received = true;
    continue;
}
```

发送给接收端：

```text
TEST_DONE|session_id|mode|result
```

示例：

```text
TEST_DONE|103|H5_LAP_CENTER|OK
```

控制消息会重复发送三次。接收端根据 `session_id` 去重，降低 UDP 控制包偶发丢失造成的录像失败概率。

### 8.8 关闭图传

退出状态前执行：

```cpp
video_streamer.stop();
```

即使忘记显式调用，局部对象析构时也会尝试停止线程和关闭 GStreamer 管线。

### 8.9 禁用发送端录像

保持：

```toml
[record]
enabled = false
```

并且不要在视觉主循环中调用：

```cpp
ctx.recorder.writeRaw(...)
ctx.recorder.writeDebug(...)
```

最终视频只保存在接收端。

### 8.10 编译

```bash
cd ~/etest_2026

rm -rf build

cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build -j4
```

生成：

```text
~/etest_2026/build/etest_2026
```

---

## 9. 接收端安装

将 `receiver.py` 放到：

```text
~/receiver.py
```

然后：

```bash
chmod +x ~/receiver.py
```

直接运行：

```bash
python3 ~/receiver.py
```

程序启动后会：

1. 监听 UDP `5600`；
2. 监听 UDP `5601`；
3. 自动尝试打开视频流；
4. 图传断开后自动重连；
5. HDMI 全屏显示；
6. 根据控制消息自动录像。

---

## 10. 接收端录像逻辑

### 10.1 待机状态

即使当前没有正式测试，接收端仍持续：

- 接收图像；
- 显示图像；
- 在内存中保留最近 1 秒画面；
- 不创建录像文件。

### 10.2 收到 `TEST_START`

接收端：

1. 获取当前 `session_id` 和 `mode`；
2. 创建新的 MJPG/AVI 文件；
3. 先写入之前保留的 1 秒预录；
4. 再持续写入实时帧。

预录的目的，是避免下位机已经启动，但 `CONTESTSTART ACK` 和 UDP 控制消息稍晚到达造成开头画面丢失。

### 10.3 收到 `TEST_DONE`

接收端：

1. 检查 `session_id` 是否属于当前录像；
2. 记录 `result`；
3. 设置 0.5 秒结束倒计时；
4. 继续写入最后的停车、稳定画面；
5. 关闭文件；
6. 将 `_RECORDING.avi` 重命名为带结果的正式文件名。

### 10.4 自动超时

如果始终没有收到 `DONE`，单段录像达到 90 秒后自动结束：

```text
..._TIMEOUT.avi
```

这是防止控制链路异常后无限录像。

### 10.5 手动兜底

接收窗口支持：

| 按键 | 功能 |
|---|---|
| `R` | 手动开始或停止录像 |
| `Q` | 退出程序 |
| `Esc` | 退出程序 |

如果自动控制消息失败，可使用 `R` 手动保住本次测试视频。

---

## 11. 默认录像位置

代码中：

```python
RECORD_DIR = Path.home() / "etest_videos"
```

因此默认保存在当前运行用户的主目录。

例如，用户是 `pi`：

```text
/home/pi/etest_videos/
```

用户是 `robot`：

```text
/home/robot/etest_videos/
```

准确查看：

```bash
echo "$HOME/etest_videos"
```

列出录像：

```bash
ls -lh ~/etest_videos
```

查看剩余空间：

```bash
df -h "$HOME"
```

不要使用 `sudo python3 receiver.py` 运行，否则 `Path.home()` 可能变成：

```text
/root/etest_videos/
```

这会造成“录像找不到”的假象。

---

## 12. 录像文件命名

正式文件类似：

```text
20260731_213512_103_H5_LAP_CENTER_session-103_OK.avi
```

组成：

```text
时间戳_模式_session编号_结果.avi
```

临时录制期间文件名带：

```text
_RECORDING.avi
```

正常关闭后会改名。

常见结果：

```text
OK
DONE
TIMEOUT
MANUAL
PROGRAM_EXIT
INTERRUPTED
```

如果发现 `_RECORDING.avi` 残留，通常说明程序异常退出、系统断电或存储写入异常。AVI 可能仍能播放一部分，但不能保证索引完整。

---

## 13. Ad-Hoc 网络配置

### 13.1 默认拓扑

```text
接收端 USB 网卡 wlan1
SSID: ETEST-DIRECT
IP:   192.168.60.1/24
           ▲
           │ Ad-Hoc / IBSS
           ▼
发送端 USB 网卡 wlan1
SSID: ETEST-DIRECT
IP:   192.168.60.2/24
```

没有：

- 无线路由器；
- DHCP；
- 默认网关；
- 热点/AP；
- 互联网依赖。

### 13.2 首次给脚本执行权限

接收端：

```bash
chmod +x receiver_adhoc.sh
```

发送端：

```bash
chmod +x sender_adhoc.sh
```

### 13.3 接收端启动

```bash
./receiver_adhoc.sh
```

默认会：

1. 检查 `wlan1`；
2. 检查 Ad-Hoc 支持；
3. 删除同名旧连接；
4. 创建 `ETEST-DIRECT`；
5. 设置 `192.168.60.1/24`；
6. 关闭无线节能；
7. 启动 `~/receiver.py`。

接收程序不在默认位置时：

```bash
RECEIVER_APP=/home/pi/project/receiver.py \
./receiver_adhoc.sh
```

无线接口不是 `wlan1` 时：

```bash
IFACE=wlx001122334455 \
./receiver_adhoc.sh
```

### 13.4 发送端启动

接收端运行后，在发送端执行：

```bash
./sender_adhoc.sh
```

发送脚本会：

1. 建立相同的 Ad-Hoc 网络；
2. 设置 `192.168.60.2/24`；
3. 关闭无线节能；
4. 检查到 `192.168.60.1` 的路由；
5. 最多等待接收端 30 秒；
6. ping 成功后启动 `~/etest_2026/build/etest_2026`。

程序路径不同：

```bash
SENDER_APP=/home/pi/project/build/etest_2026 \
./sender_adhoc.sh
```

指定网卡：

```bash
IFACE=wlx001122334455 \
./sender_adhoc.sh
```

等待 60 秒：

```bash
WAIT_SECONDS=60 \
./sender_adhoc.sh
```

### 13.5 只建立网络

接收端：

```bash
./receiver_adhoc.sh setup
```

发送端：

```bash
./sender_adhoc.sh setup
```

### 13.6 查看状态

接收端：

```bash
./receiver_adhoc.sh status
```

发送端：

```bash
./sender_adhoc.sh status
```

### 13.7 只测试链路

发送端：

```bash
./sender_adhoc.sh test
```

脚本会建立连接并 ping 接收端 10 次，但不启动 C++ 主程序。

### 13.8 停止连接

接收端：

```bash
./receiver_adhoc.sh stop
```

发送端：

```bash
./sender_adhoc.sh stop
```

---

## 14. 5 GHz 与 2.4 GHz

默认：

```text
BAND=a
CHANNEL=36
```

即 5 GHz 信道 36。

若 USB 网卡或驱动不能在 5 GHz 下工作于 IBSS 模式，两端必须同时切换为 2.4 GHz：

接收端：

```bash
BAND=bg CHANNEL=6 \
./receiver_adhoc.sh
```

发送端：

```bash
BAND=bg CHANNEL=6 \
./sender_adhoc.sh
```

两端必须满足：

```text
SSID 相同
模式相同
频段相同
信道相同
子网相同
IP 不同
```

错误示例：

```text
接收端：5 GHz / channel 36
发送端：2.4 GHz / channel 6
```

这种配置不可能连通。

---

## 15. 正常启动顺序

推荐每次比赛按固定顺序操作。

### 第一步：接收端上电

确认：

```text
HDMI 显示器正常
USB 无线网卡插牢
键盘可用
存储空间充足
```

执行：

```bash
cd ~/scripts
./receiver_adhoc.sh
```

等待接收窗口出现。

### 第二步：发送端上电

执行：

```bash
cd ~/scripts
./sender_adhoc.sh
```

看到：

```text
[TX-ADHOC] 接收端已连通
```

再确认接收显示器已经出现实时画面。

### 第三步：调整摄像头

测试前确认：

- 整根摆杆都在画面中；
- 左右端留有安全余量；
- 钢球清晰；
- 画面没有持续抖动；
- 曝光不会使钢球或刻度严重拖影。

### 第四步：开始正式测试

正常情况下无需操作接收端：

```text
CONTESTSTART ACK
    ↓
自动开始录像
    ↓
DONE
    ↓
自动结束并保存
```

### 第五步：测试后检查文件

```bash
ls -lht ~/etest_videos | head
```

立即播放最新文件确认：

```bash
LATEST="$(find "$HOME/etest_videos" -maxdepth 1 -type f -name '*.avi' -printf '%T@ %p\n' \
    | sort -nr | head -n1 | cut -d' ' -f2-)"

echo "$LATEST"
```

桌面环境可以使用：

```bash
vlc "$LATEST"
```

---

## 16. 单独测试视频链路

先在接收端运行：

```bash
./receiver_adhoc.sh
```

在发送端不运行主程序，使用测试图：

```bash
gst-launch-1.0 -v \
    videotestsrc is-live=true \
    ! video/x-raw,width=960,height=480,framerate=25/1 \
    ! x264enc \
      tune=zerolatency \
      speed-preset=ultrafast \
      bitrate=2500 \
      key-int-max=25 \
      bframes=0 \
      byte-stream=true \
    ! h264parse \
    ! rtph264pay \
      pt=96 \
      config-interval=1 \
      mtu=1200 \
    ! udpsink \
      host=192.168.60.1 \
      port=5600 \
      sync=false \
      async=false
```

接收端应显示 GStreamer 测试图。

这一测试可以独立验证：

```text
Ad-Hoc 网络
UDP 视频端口
RTP 封装
H.264 编码
H.264 解码
OpenCV GStreamer 支持
HDMI 显示
```

---

## 17. 单独测试自动录像控制

接收端需要先收到有效视频流。

发送端发送开始消息：

```bash
echo -n 'TEST_START|123|H5_LAP_CENTER' \
    | nc -u -w1 192.168.60.1 5601
```

等待几秒：

```bash
sleep 5
```

发送结束消息：

```bash
echo -n 'TEST_DONE|123|H5_LAP_CENTER|OK' \
    | nc -u -w1 192.168.60.1 5601
```

预期接收日志：

```text
[CONTROL] Pending start: session=123, mode=H5_LAP_CENTER
[RECORD] Started: ..._RECORDING.avi
[RECORD] DONE received; closing after 0.5s post-roll.
[RECORD] Saved: ..._OK.avi
```

检查：

```bash
ls -lh ~/etest_videos
```

没有视频帧时，即使发送 `TEST_START`，程序也只能进入等待状态，不能创建有效录像。

安装 `nc`：

```bash
sudo apt install -y netcat-openbsd
```

---

## 18. 带宽测试

安装：

```bash
sudo apt install -y iperf3
```

接收端：

```bash
iperf3 -s
```

发送端 TCP 测试：

```bash
iperf3 \
    -c 192.168.60.1 \
    -B 192.168.60.2 \
    -t 30
```

UDP 测试：

```bash
iperf3 \
    -c 192.168.60.1 \
    -B 192.168.60.2 \
    -u \
    -b 10M \
    -t 30
```

当前图传码率约 2.5 Mbit/s。建议无线链路能够稳定通过 10 Mbit/s UDP 测试，并且无持续高丢包。

检查路由：

```bash
ip route get 192.168.60.1
```

发送端正确结果应包含：

```text
dev wlan1 src 192.168.60.2
```

如果显示 `wlan0`，图传没有走预期的 USB 无线网卡。

---

## 19. 接收端状态显示

窗口左上角会显示：

```text
VIDEO ONLINE / VIDEO WAITING
REC mode / session_id
RX FPS
```

含义：

| 状态 | 含义 |
|---|---|
| `VIDEO ONLINE` | 最近约 1 秒内持续收到视频帧 |
| `VIDEO WAITING` | 视频尚未到达或已经中断 |
| `REC OFF` | 当前未录像 |
| `REC H5... / 103` | 当前正在录制对应模式和会话 |
| `RX FPS 25.0` | 当前接收解码帧率 |

显示叠加文字只用于屏幕观察。录像程序写入的是未绘制接收端状态文字的原始接收帧。

---

## 20. 图传参数调整

### 20.1 降低延迟

接收端：

```python
JITTER_LATENCY_MS = 50
```

优点：

- 延迟更低。

缺点：

- 无线抖动时更容易花屏或短暂停顿。

### 20.2 提高稳定性

接收端：

```python
JITTER_LATENCY_MS = 120
```

优点：

- 对乱序和短时抖动容忍度更高。

缺点：

- 延迟增加。

### 20.3 降低 CPU 或带宽压力

发送端：

```cpp
stream_cfg.width = 800;
stream_cfg.height = 400;
stream_cfg.fps = 20;
stream_cfg.bitrate_kbps = 1800;
```

接收端同步修改：

```python
STREAM_WIDTH = 800
STREAM_HEIGHT = 400
STREAM_FPS = 20.0
```

不过接收端 GStreamer 会根据码流解码，`STREAM_WIDTH` 和 `STREAM_HEIGHT` 主要影响无画面占位和默认录像设置。正式录像以收到帧的实际尺寸为准。

### 20.4 提高画质

发送端：

```cpp
stream_cfg.bitrate_kbps = 3500;
```

画质改善有限时，优先检查：

- 光照；
- 快门速度；
- 摄像头支架振动；
- 镜头焦距；
- 图像是否过度缩放。

盲目提高码率不能解决运动模糊。

---

## 21. 常见故障

### 21.1 脚本提示找不到 `wlan1`

查看：

```bash
nmcli device status
ip link
```

然后：

```bash
IFACE=实际接口名 ./receiver_adhoc.sh
```

或：

```bash
IFACE=实际接口名 ./sender_adhoc.sh
```

### 21.2 提示不支持 Ad-Hoc

检查：

```bash
nmcli -t -f WIFI-PROPERTIES.ADHOC device show wlan1
iw list
```

可能原因：

- USB 网卡芯片不支持；
- Linux 驱动不支持 IBSS；
- 网卡被错误驱动接管；
- 当前接口名填错。

不要只看商品页面参数，以实际驱动输出为准。

### 21.3 5 GHz 启动失败

两端同时切换：

```bash
BAND=bg CHANNEL=6
```

如果 2.4 GHz 可以建立，说明硬件可能支持 Ad-Hoc，但不支持 5 GHz IBSS。

### 21.4 能建立网络，但 ping 不通

两端检查：

```bash
ip -4 -brief address
iw dev wlan1 link
ip route
```

应当分别有：

```text
192.168.60.1/24
192.168.60.2/24
```

检查两端：

```text
SSID
band
channel
mode
```

是否完全一致。

### 21.5 ping 通，但没有画面

按顺序检查：

```bash
gst-inspect-1.0 x264enc
gst-inspect-1.0 rtph264pay
gst-inspect-1.0 rtph264depay
gst-inspect-1.0 avdec_h264
```

确认发送端日志没有：

```text
failed to open GStreamer pipeline
```

确认接收端：

```text
GStreamer: YES
```

抓取端口：

```bash
sudo tcpdump -ni wlan1 udp port 5600
```

有数据但无画面，重点检查编码、RTP caps 和解码插件；完全没数据，重点检查发送端、IP 和路由。

### 21.6 有画面但延迟不断增加

原因通常是帧队列没有丢弃旧帧。

当前实现已经：

- 发送端只保留最新帧；
- GStreamer 发送队列为 leaky；
- 接收端有限队列满时删除最旧帧；
- appsink 设置 `drop=true`。

如果自行修改代码，不要改成无限队列。

### 21.7 画面偶尔花屏

尝试：

```python
JITTER_LATENCY_MS = 120
```

或者：

```cpp
stream_cfg.bitrate_kbps = 1800;
```

同时检查：

- 无线信号；
- 网卡温度；
- USB 供电；
- 信道干扰；
- 是否出现持续 UDP 丢包。

### 21.8 自动录像不开始

检查接收日志是否收到：

```text
TEST_START
```

使用：

```bash
sudo tcpdump -ni wlan1 udp port 5601 -A
```

确认发送端是否发出控制消息。

还要确认：

- 已经有视频帧；
- 发送端确实收到 `CONTESTSTART ACK`；
- 没有在发送开始消息前错误覆盖 `session_id`；
- 两端控制端口都是 `5601`。

### 21.9 收到 DONE 但录像不停

检查消息格式：

```text
TEST_DONE|session_id|mode|result
```

接收端只会关闭 `session_id` 与当前录像相同的测试。旧测试延迟到达的 `DONE` 会被忽略，这是正确行为。

### 21.10 录像不知道保存在哪里

不要使用：

```bash
sudo python3 receiver.py
```

正常使用：

```bash
python3 receiver.py
```

查看：

```bash
echo "$HOME"
echo "$HOME/etest_videos"
find "$HOME/etest_videos" -maxdepth 1 -type f -name '*.avi'
```

### 21.11 HDMI 窗口无法出现

本地桌面终端直接运行：

```bash
python3 ~/receiver.py
```

从 SSH 启动时尝试：

```bash
DISPLAY=:0 ./receiver_adhoc.sh
```

若桌面会话属于另一个用户，还需要正确的 `XAUTHORITY`。比赛时建议直接从接收端桌面启动并验证，不要临场依赖未经测试的 SSH 图形转发。

### 21.12 树莓派 CPU 占用过高

发送端先降低：

```cpp
stream_cfg.fps = 20;
stream_cfg.bitrate_kbps = 1800;
```

再考虑降低分辨率。

查看：

```bash
top
vcgencmd measure_temp
vcgencmd get_throttled
```

如果出现降频或欠压，先解决供电和散热，而不是继续优化软件。

---

## 22. 日志与调试命令

### 查看无线状态

```bash
nmcli device status
iw dev wlan1 link
ip -4 -brief address show wlan1
```

### 查看路由

```bash
ip route
ip route get 192.168.60.1
```

### 查看 UDP 视频

```bash
sudo tcpdump -ni wlan1 udp port 5600
```

### 查看 UDP 控制

```bash
sudo tcpdump -ni wlan1 udp port 5601 -A
```

### 查看端口监听

接收端：

```bash
ss -lunp | grep -E '5600|5601'
```

### 查看录像文件

```bash
find ~/etest_videos \
    -maxdepth 1 \
    -type f \
    -name '*.avi' \
    -printf '%TY-%Tm-%Td %TH:%TM:%TS %s %p\n' \
    | sort -r
```

### 查看 OpenCV 版本

```bash
python3 -c 'import cv2; print(cv2.__version__)'
opencv_version
```

---

## 23. 可选：开机自动启动接收端

调试阶段建议手动启动。确认网络、桌面显示和录像都可靠后，再配置 systemd。

创建：

```bash
sudo nano /etc/systemd/system/etest-receiver.service
```

示例：

```ini
[Unit]
Description=ETEST Ad-Hoc Video Receiver
After=NetworkManager.service graphical.target
Wants=NetworkManager.service

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi
Environment=DISPLAY=:0
Environment=XAUTHORITY=/home/pi/.Xauthority
ExecStart=/home/pi/scripts/receiver_adhoc.sh run
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical.target
```

根据实际用户名和脚本路径修改：

```text
User
WorkingDirectory
XAUTHORITY
ExecStart
```

启用：

```bash
sudo systemctl daemon-reload
sudo systemctl enable etest-receiver.service
sudo systemctl start etest-receiver.service
```

查看日志：

```bash
journalctl -u etest-receiver.service -f
```

如果服务能运行但 HDMI 没有窗口，通常是桌面会话环境变量或权限问题。比赛前必须完整上电测试。

---

## 24. 比赛前检查表

### 硬件

- [ ] 摄像头牢固；
- [ ] 视野覆盖整根摆杆；
- [ ] 钢球清晰可见；
- [ ] 发送端供电无欠压；
- [ ] 两个 USB 无线网卡插牢；
- [ ] 接收端 HDMI 稳定；
- [ ] 存储空间足够；
- [ ] 两块树莓派散热正常。

### 网络

- [ ] 两端网卡都支持 Ad-Hoc；
- [ ] SSID 相同；
- [ ] 频段相同；
- [ ] 信道相同；
- [ ] IP 无冲突；
- [ ] 发送端能 ping 接收端；
- [ ] 路由显示走专用 USB 无线网卡；
- [ ] Wi-Fi power save 已关闭；
- [ ] UDP 10 Mbit/s 测试稳定。

### 软件

- [ ] `x264enc` 存在；
- [ ] `avdec_h264` 存在；
- [ ] 两端 OpenCV 支持 GStreamer；
- [ ] C++ 编译为 Release；
- [ ] 发送端 `receiver_ip` 是 `192.168.60.1`；
- [ ] 发送端录像保持关闭；
- [ ] 接收端自动创建录像目录；
- [ ] `R` 手动录像可用；
- [ ] `Q`/`Esc` 能安全退出。

### 自动录像

- [ ] `TEST_START` 能启动新录像；
- [ ] 重复 `TEST_START` 不会生成多个文件；
- [ ] `DONE` 能在 0.5 秒后关闭文件；
- [ ] 不同 `session_id` 不会互相误关录像；
- [ ] 90 秒安全超时有效；
- [ ] 最新 AVI 能正常回放；
- [ ] 文件名包含模式和结果。

---

## 25. 推荐现场操作流程

```text
1. 接收端上电
2. 启动 receiver_adhoc.sh
3. 确认 HDMI 接收窗口出现
4. 发送端上电
5. 启动 sender_adhoc.sh
6. 确认 ping 成功
7. 确认实时图像
8. 调整摄像头
9. 进行一次 TEST_START / TEST_DONE 模拟测试
10. 回放录像
11. 再进入正式测试
```

不要跳过第 9 和第 10 步。图传能显示，不代表自动录像一定正常；自动录像正常，也不代表保存文件一定可以回放。

---

## 26. 工程注意事项

### 26.1 UDP 控制消息不是绝对可靠

当前通过重复发送三次减少丢包，但 UDP 不提供确认机制。

如果后续需要进一步提高可靠性，可以增加：

```text
发送端 TEST_START
接收端 START_ACK
发送端未收到 ACK 时重发
```

不过比赛短周期系统中，当前“三次发送 + 手动 R 键兜底”已经能覆盖大多数情况，改成完整可靠协议会增加调试量。

### 26.2 视频流和控制流分离

视频使用：

```text
UDP 5600
```

控制使用：

```text
UDP 5601
```

好处是：

- 视频丢包不会阻塞控制；
- 控制消息容易抓包；
- 以后可以独立替换视频协议；
- 接收端状态机更清晰。

### 26.3 不建议在发送端同步编码

编码必须放后台线程。如果直接在视觉主循环里调用耗时编码，可能导致：

- 钢球检测帧率下降；
- 串口消息处理延迟；
- 控制周期抖动；
- 图像和控制时间不同步。

当前“最新帧邮箱”结构是合适的折中。

### 26.4 不建议录像到不稳定 U 盘

比赛时优先保存到：

```text
~/etest_videos
```

测试结束后再复制到 U 盘。

USB 无线网卡、摄像头和 U 盘同时占用 USB 总线时，还可能增加供电和带宽压力。

### 26.5 录像文件较大是正常现象

MJPG 会比 H.264 大，但每帧相对独立，实时写入简单。

需要估算时，以实机录制 30 秒为准：

```bash
du -h ~/etest_videos/*.avi
```

不要仅凭理论估算存储空间。

---

## 27. 快速命令汇总

### 接收端

```bash
chmod +x receiver_adhoc.sh
./receiver_adhoc.sh
```

### 发送端

```bash
chmod +x sender_adhoc.sh
./sender_adhoc.sh
```

### 2.4 GHz 备用方案

接收端：

```bash
BAND=bg CHANNEL=6 ./receiver_adhoc.sh
```

发送端：

```bash
BAND=bg CHANNEL=6 ./sender_adhoc.sh
```

### 网络测试

```bash
./sender_adhoc.sh test
```

### 查看录像

```bash
ls -lht ~/etest_videos
```

### 查看路由

```bash
ip route get 192.168.60.1
```

### 抓视频包

```bash
sudo tcpdump -ni wlan1 udp port 5600
```

### 抓控制包

```bash
sudo tcpdump -ni wlan1 udp port 5601 -A
```

---

## 28. 最终方案摘要

```text
网络：
两块 USB 无线网卡建立 Ad-Hoc/IBSS

地址：
接收端 192.168.60.1
发送端 192.168.60.2

视频：
H.264 / RTP / UDP 5600
960×480 @ 25 FPS
2500 kbit/s

控制：
UDP 5601
TEST_START|session_id|mode
TEST_DONE|session_id|mode|result

录像：
只在接收端保存
默认 ~/etest_videos
MJPG / AVI
1 秒预录
DONE 后 0.5 秒尾录
90 秒超时兜底

启动：
先 receiver_adhoc.sh
后 sender_adhoc.sh
```
