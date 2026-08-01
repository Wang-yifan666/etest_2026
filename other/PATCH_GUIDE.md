# etest_2026 图传修改说明（发送端不录像）

## 目标时序

- 图传视频始终发送，便于测试前检查画面。
- 下位机确认 `CONTESTSTART` 后，发送端向接收端发：
  `TEST_START|session_id|mode`
- 下位机发来 `DONE` 后，发送端向接收端发：
  `TEST_DONE|session_id|mode|result`
- 接收端保存 1 秒预录；DONE 后再保存 0.5 秒尾帧，然后关闭 AVI。
- `config/record.toml` 保持 `enabled = false`，发送端不保存任何视频。

## 1. 复制文件

将：

- `sender/include/stream/video_streamer.hpp`
  复制到项目 `include/stream/video_streamer.hpp`
- `sender/src/stream/video_streamer.cpp`
  复制到项目 `src/stream/video_streamer.cpp`
- `receiver/receiver.py`
  复制到接收树莓派，例如 `/home/pi/receiver.py`

## 2. 修改 CMakeLists.txt

在 `add_executable(etest_2026` 的源文件列表中加入：

```cmake
    src/stream/video_streamer.cpp
```

建议放在 `src/state/error.cpp` 后面。

## 3. 修改 src/state/search.cpp

### 3.1 增加头文件

在文件顶部加入：

```cpp
#include "stream/video_streamer.hpp"
```

### 3.2 在 while(ctx.running) 之前创建图传器

在：

```cpp
std::uint64_t last_processed_sequence = 0;
```

后面加入：

```cpp
etest::stream::VideoStreamConfig stream_cfg;
stream_cfg.enabled = true;
stream_cfg.receiver_ip = "192.168.50.1";
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

接收树莓派地址不是 `192.168.50.1` 时，只改这里。

### 3.3 在 CONTESTSTART ACK 时通知接收端开始录像

将原来的：

```cpp
if(uart::protocol::isContestStartAck(msg))
{
    ctx.task.contest_start_acked = true;
    ctx.task_phase = TaskPhase::CONTEST;
    ETEST_LOG_INFO("SEARCH", "CONTESTSTART confirmed");
    continue;
}
```

替换为：

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
    ETEST_LOG_INFO("SEARCH", "CONTESTSTART confirmed");
    continue;
}
```

### 3.4 在 DONE 时通知接收端停止录像

将原来的：

```cpp
if(auto done = uart::protocol::parseDone(msg))
{
    ETEST_LOG_INFO("SEARCH",
                   "DONE received: " + done->mode
                       + " result=" + done->result);
    done_received = true;
    continue;
}
```

替换为：

```cpp
if(auto done = uart::protocol::parseDone(msg))
{
    ETEST_LOG_INFO("SEARCH",
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

### 3.5 每个新帧送入图传线程

找到：

```cpp
if(has_new_frame)
{
    ctx.frame = packet.frame;
```

改为：

```cpp
if(has_new_frame)
{
    ctx.frame = packet.frame;

    // 始终发送原始摄像头画面；发送端不录像。
    video_streamer.submit(ctx.frame);
```

### 3.6 删除错误的模式覆盖

删除这一行：

```cpp
ctx.task.active_mode = "H5_LAP_CENTER";
```

它会把 H2/H3/H4/H6 都强行改成 H5，导致后续控制消息和
CONTESTSTART 重发使用错误模式。

### 3.7 正常退出前停止图传

在：

```cpp
if(can_show_preview && preview_open)
    cv::destroyAllWindows();
```

前加入：

```cpp
video_streamer.stop();
```

即使存在提前 `return State::ERROR`，局部对象析构时也会自动 stop。

## 4. 发送端安装依赖

```bash
sudo apt update
sudo apt install -y \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav
```

检查：

```bash
gst-inspect-1.0 x264enc
opencv_version --verbose | grep -i gstreamer
```

第二条应显示 `GStreamer: YES`。

重新编译：

```bash
cd etest_2026
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

## 5. 接收端安装并运行

```bash
sudo apt update
sudo apt install -y \
    python3-opencv \
    python3-numpy \
    gstreamer1.0-tools \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-libav
```

检查 Python OpenCV：

```bash
python3 - <<'PY'
import cv2
for line in cv2.getBuildInformation().splitlines():
    if "GStreamer" in line:
        print(line)
PY
```

运行：

```bash
python3 /home/pi/receiver.py
```

录像目录：

```text
/home/pi/etest_videos/
```

按键：

- `Q` 或 `Esc`：退出
- `R`：手动开始/停止录像（自动协议失效时兜底）

## 6. 网络

建议接收端固定为：

```text
192.168.50.1
```

发送端固定为：

```text
192.168.50.2
```

两端关闭 Wi-Fi 省电：

```bash
sudo iw dev wlan0 set power_save off
```

先测试：

```bash
ping -c 100 192.168.50.1
```

再启动接收端，最后启动发送端程序。

## 7. 单独测试图传链路

接收端先运行 `receiver.py`。

发送端可临时用测试源检查网络和解码，不经过 C++：

```bash
gst-launch-1.0 -v \
    videotestsrc is-live=true \
    ! video/x-raw,width=960,height=480,framerate=25/1 \
    ! x264enc tune=zerolatency speed-preset=ultrafast \
      bitrate=2500 key-int-max=25 bframes=0 byte-stream=true \
    ! h264parse \
    ! rtph264pay pt=96 config-interval=1 mtu=1200 \
    ! udpsink host=192.168.50.1 port=5600 sync=false async=false
```

控制消息测试：

```bash
echo -n 'TEST_START|123|H5_LAP_CENTER' \
  | nc -u -w1 192.168.50.1 5601

sleep 5

echo -n 'TEST_DONE|123|H5_LAP_CENTER|OK' \
  | nc -u -w1 192.168.50.1 5601
```

预期结果：接收端开始录像，5 秒后收到 DONE，再保留 0.5 秒，
最后在 `~/etest_videos` 生成一个 AVI 文件。
