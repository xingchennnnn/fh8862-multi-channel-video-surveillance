# RTSPserver_V6 创新设计 PPT 内容

## 封面

题目：RTSPserver_V6 创新设计

主题：

- Web 前端低延迟可视化监控平台
- 四路摄像头设备同时预览与统一控制

适用场景：

- 多路嵌入式摄像头视频监控
- 局域网低延迟预览
- 远程参数调节、OSD、遮罩、录像、LED 和时间同步控制

---

## 目录

1. 系统整体架构
2. 创新点一：Web 前端实现思路
3. 创新点一：关键实现细节
4. 创新点二：四路摄像头同时控制
5. 创新点二：关键实现细节
6. 创新设计价值总结

---

## 1. 系统整体架构

### 架构图

```text
FH8862 开发板 1   rtsp://192.168.1.11:8554/main/sub
FH8862 开发板 2   rtsp://192.168.1.12:8554/main/sub
FH8862 开发板 3   rtsp://192.168.1.13:8554/main/sub
FH8862 开发板 4   rtsp://192.168.1.14:8554/main/sub
        │
        │ Live555 RTSP
        ▼
Ubuntu 虚拟机 MediaMTX
RTSP → WebRTC/WHEP 转换，端口 8889
        │
        │ WebRTC
        ▼
Windows 浏览器
2×2 视频墙 + 控制面板

Windows 浏览器
        │ HTTP API
        ▼
Ubuntu Node.js 服务，端口 8080
        │ TCP 控制命令
        ▼
FH8862 板端控制服务，端口 9999
```

### 设计目标

- 将传统 RTSP 视频流转换成浏览器可直接播放的 WebRTC 流。
- 在一个网页中同时管理 4 路摄像头。
- 将视频预览和设备控制放在同一个 Web 操作界面中。
- 将复杂的板端 TCP 命令封装成前端易调用的 HTTP API。

---

## 2. 创新点一：Web 前端实现思路

### 创新描述

V6 版本不是简单地用 VLC 或播放器直接拉 RTSP，而是设计了一个完整的浏览器端监控平台：

- 浏览器打开 `http://192.168.1.1:8080` 即可使用。
- 页面自动生成 2×2 四路视频墙。
- 每路摄像头支持主码流、子码流切换。
- 前端同时提供旋转、OSD、遮罩、录像、LED、时间同步等控制能力。
- 浏览器端不需要安装 RTSP 插件，通过 WebRTC 实现实时播放。

### 实现路线

```text
板端 H.264 编码
    ↓
Live555 输出 RTSP
    ↓
MediaMTX 拉取 RTSP 并转换为 WebRTC/WHEP
    ↓
浏览器使用 RTCPeerConnection 建立 WebRTC 接收链路
    ↓
video 标签播放实时画面
```

### 为什么这样设计

- 浏览器原生不支持直接播放 RTSP。
- WebRTC 支持低延迟实时视频，适合监控预览。
- MediaMTX 可以作为轻量中继，将 RTSP 转换为浏览器友好的 WHEP 接口。
- 前端只需要通过 HTTP POST 发送 SDP offer，即可建立 WebRTC 播放链路。

---

## 3. 创新点一：Web 前端关键实现细节

### 3.1 2×2 视频墙动态生成

前端在 `web/js/webrtc-player.js` 中定义四路摄像头：

```javascript
var cameras = [
    { id: 1, name: 'Camera 1', ip: '192.168.1.11' },
    { id: 2, name: 'Camera 2', ip: '192.168.1.12' },
    { id: 3, name: 'Camera 3', ip: '192.168.1.13' },
    { id: 4, name: 'Camera 4', ip: '192.168.1.14' }
];
```

页面加载后执行：

```text
DOMContentLoaded
    → initDOMElements()
    → detectLocalIP()
    → buildVideoWall()
    → buildCameraTabs()
    → initDefaultMaskRegions()
```

`buildVideoWall()` 根据 `cameras` 数组动态创建四个摄像头面板，每个面板包含：

- 摄像头名称
- 摄像头 IP
- 连接状态
- 主码流/子码流切换按钮
- video 播放区域
- 等待视频流提示层

这种设计避免为四路摄像头重复写四份 HTML，后续扩展更多摄像头也更清晰。

### 3.2 WebRTC 播放链路

每路视频使用统一路径规则：

```text
cam1_main
cam1_sub
cam2_main
cam2_sub
cam3_main
cam3_sub
cam4_main
cam4_sub
```

前端根据摄像头编号和码流类型拼出 WHEP 地址：

```text
http://<Ubuntu_IP>:8889/cam2_sub/whep
```

连接流程：

```text
connectStream(camId, streamType)
    → 创建 RTCPeerConnection
    → addTransceiver('video', recvonly)
    → createOffer()
    → setLocalDescription()
    → 等待 ICE 收集完成
    → POST SDP 到 MediaMTX WHEP 地址
    → 接收 answer SDP
    → setRemoteDescription()
    → ontrack 中把视频流绑定给 video.srcObject
```

前端使用 `peerConnections` 保存每一路连接：

```text
peerConnections['cam2_sub'] = { pc: RTCPeerConnection, connected: false }
```

这样可以做到：

- 每路视频独立连接、独立断开。
- 主码流和子码流切换时只关闭当前摄像头的旧连接。
- 某一路失败不会影响其他路。
- 断线后可以自动重连。

### 3.3 主码流/子码流切换

板端输出两路 RTSP：

```text
rtsp://<board_ip>:8554/main
rtsp://<board_ip>:8554/sub
```

前端通过 `switchStream(camId, streamType)` 实现切换：

```text
切换到 main
    → 断开 sub
    → 更新按钮状态
    → 连接 camX_main

切换到 sub
    → 断开 main
    → 更新按钮状态
    → 连接 camX_sub
```

设计价值：

- 主码流用于清晰预览。
- 子码流用于四路同时观看，降低带宽压力。
- 用户可以按需切换，不需要重启板端程序。

### 3.4 前端控制面板

Web 页面将常用板端功能做成可视化控件：

| 功能 | 前端控件 | 后端命令 |
|---|---|---|
| 旋转 180 度 | checkbox | `ROTATE 0/1` |
| OSD 颜色 | RGB 滑块 | `OSD_COLOR R G B` |
| OSD 文字 | 输入框 | `OSD_TEXT_HEX <hex>` |
| OSD 显示开关 | checkbox | `OSD_TEXT_EN 0/1` |
| OSD 反色 | select | `OSD_INVERT 0/1` |
| 遮罩颜色 | RGB 滑块 | `MASK_COLOR R G B` |
| 遮罩开关 | checkbox | `MASK_ENABLE 0/1` |
| 遮罩区域 | 坐标输入 | `MASK_REGION_SET ...` |
| 设备录像 | 秒数输入 | `RECORD_START <秒>` |
| PC 录像 | 浏览器 MediaRecorder | 本地保存 WebM |
| LED | 按钮 | `LED 0/1/2` |
| 时间同步 | 按钮 | `TIME_SYNC <毫秒时间戳>` |

前端不是只做视频展示，而是把图像效果、设备功能和状态管理统一集成到一个操作台。

### 3.5 中文 OSD 的编码处理

浏览器和 Node.js 默认使用 UTF-8，但板端 OSD 字库使用 GB2312 字节。

实现方式：

```text
浏览器输入中文
    ↓
Node.js 使用 iconv-lite 转 GB2312
    ↓
转成十六进制字符串
    ↓
发送 OSD_TEXT_HEX <hex>
    ↓
板端解码为 GB2312 原始字节
    ↓
OSD 叠加中文
```

这个设计解决了 Web 端中文输入和嵌入式板端中文字库编码不一致的问题。

### 3.6 PC 端录像

前端使用浏览器 `MediaRecorder` 对 WebRTC 收到的视频流进行录制：

```text
video.srcObject
    ↓
MediaRecorder
    ↓
WebM Blob
    ↓
浏览器自动下载
```

设计价值：

- 不依赖板端文件系统。
- 不增加板端 CPU 和存储压力。
- Windows 浏览器即可完成本地录像。

---

## 4. 创新点二：四路摄像头设备同时控制

### 创新描述

V6 版本不再是单摄像头控制页面，而是实现了四路摄像头的同时预览、分别控制和统一管理。

核心能力：

- 四路视频可以同时连接和播放。
- 每一路可以独立切换主码流/子码流。
- 控制面板可以选择 Camera 1 到 Camera 4。
- 同一套控制 API 通过 `camera_id` 定位不同设备。
- 时间同步可以一次广播到全部开发板。
- 每路摄像头的控制状态在前端独立保存，切换标签时不会混乱。

### 四路控制架构

```text
Windows 前端
    │
    │ HTTP POST /api/rotate { camera_id: 2, rotate: true }
    ▼
Ubuntu Node.js 控制代理
    │
    │ camera_id = 2 → 192.168.1.12
    │ 命令转换：ROTATE 1
    ▼
TCP 连接 192.168.1.12:9999
    │
    │ 写入 "ROTATE 1\n"
    ▼
FH8862 板端 control_server
    │
    │ 调用 ISP / VPU / OSD / LED / 录像等接口
    ▼
对应摄像头设备状态改变
```

### 设备映射表

Node.js 服务维护四路设备表：

```javascript
var cameras = {
    1: { ip: '192.168.1.11', ctrlPort: 9999 },
    2: { ip: '192.168.1.12', ctrlPort: 9999 },
    3: { ip: '192.168.1.13', ctrlPort: 9999 },
    4: { ip: '192.168.1.14', ctrlPort: 9999 }
};
```

所有控制请求都携带 `camera_id`：

```json
{
  "camera_id": 2,
  "rotate": true
}
```

Node.js 根据 `camera_id` 查表得到板端 IP，再把 HTTP 请求转换为板端 TCP 命令。

---

## 5. 创新点二：四路同时控制关键实现细节

### 5.1 视频路径和控制路径分离

视频链路：

```text
板端 RTSP:8554 → MediaMTX:8889 → 浏览器 WebRTC
```

控制链路：

```text
浏览器 HTTP:8080 → Node.js → 板端 TCP:9999
```

分离设计的好处：

- 视频流高带宽、持续传输，走 RTSP/WebRTC。
- 控制命令低带宽、短连接，走 HTTP/TCP。
- 视频播放异常不会直接阻塞控制命令。
- 控制命令失败不会破坏视频链路。

### 5.2 MediaMTX 四路转发配置

`web/mediamtx.yml` 中为四块板分别配置主码流和子码流：

```yaml
paths:
  cam1_main:
    source: rtsp://192.168.1.11:8554/main
    sourceOnDemand: yes

  cam1_sub:
    source: rtsp://192.168.1.11:8554/sub
    sourceOnDemand: yes

  cam2_main:
    source: rtsp://192.168.1.12:8554/main
    sourceOnDemand: yes

  cam2_sub:
    source: rtsp://192.168.1.12:8554/sub
    sourceOnDemand: yes
```

实际配置继续扩展到 `cam3_main/sub` 和 `cam4_main/sub`。

关键点：

- `sourceOnDemand: yes` 表示只有浏览器请求该路视频时，MediaMTX 才去板端拉流。
- 每块板有主、子两个 path，共 8 个 WebRTC path。
- 前端只需要按照 `camX_main` 和 `camX_sub` 拼 URL，即可连接对应流。

### 5.3 四路连接状态管理

前端使用三个核心状态表：

```text
camState
    保存每个摄像头当前码流和连接状态

peerConnections
    保存每个 WebRTC PeerConnection

camControls
    保存每个摄像头的控制面板状态
```

示例：

```text
camState[2] = {
    stream: 'sub',
    connected: true
}

peerConnections['cam2_sub'] = {
    pc: RTCPeerConnection,
    connected: true
}

camControls[2] = {
    rotate: true,
    osdR: 255,
    osdG: 255,
    osdB: 255,
    maskRegions: [...]
}
```

这样可以保证：

- 四路视频连接互不影响。
- Camera 1 的 OSD 设置不会显示到 Camera 2 的控制面板状态上。
- 切换控制标签时，先保存当前摄像头状态，再恢复目标摄像头状态。

### 5.4 全部连接与全部断开

`connectAll()` 遍历四路摄像头：

```text
for each camera:
    如果没有状态，默认使用 sub
    connectStream(camera.id, streamType)
    更新主/子码流按钮状态
```

默认连接子码流的原因：

- 四路同时预览时总带宽更低。
- 页面初始加载更稳。
- 用户需要高清画面时，可以单独切换某一路到主码流。

`disconnectAll()` 同样遍历四路摄像头，分别断开主码流和子码流，释放 WebRTC 连接资源。

### 5.5 控制命令统一封装

前端所有单设备控制都调用：

```text
boardApi(endpoint, data)
```

该函数自动加入当前选中的摄像头编号：

```javascript
data.camera_id = selectedCam;
```

例如用户在 Camera 2 标签下点击 LED 亮：

```text
前端调用 boardApi('/api/led', { mode: 1 })
    ↓
自动补充 camera_id = 2
    ↓
POST /api/led
    ↓
Node.js 查表得到 192.168.1.12
    ↓
发送 TCP 命令 LED 1
```

这个封装让前端控制函数不需要重复关心 IP 地址，页面逻辑更简洁。

### 5.6 Node.js HTTP 到 TCP 的协议转换

Node.js 接收到 HTTP 请求后，将其转换为板端控制协议。

示例映射：

```text
POST /api/rotate
    参数: { camera_id: 2, rotate: true }
    TCP: ROTATE 1

POST /api/osd/color
    参数: { camera_id: 2, r: 255, g: 0, b: 0 }
    TCP: OSD_COLOR 255 0 0

POST /api/mask/region/set
    参数: { camera_id: 2, index: 0, enable: 1, x: 100, y: 100, w: 200, h: 200 }
    TCP: MASK_REGION_SET 0 1 100 100 200 200
```

Node.js 通过短 TCP 连接发送命令：

```text
connect board_ip:9999
    → write(command + '\n')
    → 等待 OK
    → 返回 JSON 给浏览器
```

设计价值：

- 浏览器不需要直接访问板端 TCP。
- 板端协议保持简单，易于解析。
- Node.js 作为网关统一处理跨域、编码、超时和错误返回。

### 5.7 板端控制服务

每块 FH8862 开发板启动 `./demo` 后，同时启动两个服务：

```text
RTSP Server: 8554
Control Server: 9999
```

板端控制服务负责解析 TCP 命令：

```text
ROTATE
OSD_COLOR
OSD_TEXT_HEX
OSD_INVERT
MASK_COLOR
MASK_ENABLE
MASK_REGION_SET
RECORD_START
RECORD_STOP
TIME_SYNC
LED
```

对应调用板端能力：

- ISP 镜像翻转实现 180 度旋转。
- OSD 接口更新文字、颜色和反色模式。
- VPU Mask 接口更新遮罩区域、颜色、马赛克。
- 录像状态变量控制设备端录像。
- ioctl 控制 LED 驱动。
- `clock_settime` / `settimeofday` 完成时间同步。

### 5.8 四路时间同步广播

时间同步不是只发给当前摄像头，而是广播到全部设备。

实现逻辑：

```text
浏览器点击“时间同步”
    ↓
POST /api/time/sync { timestamp: Date.now() }
    ↓
Node.js 遍历 cameras 表
    ↓
分别向 192.168.1.11/.12/.13/.14:9999 发送 TIME_SYNC
    ↓
收集四路结果
    ↓
返回 JSON 给浏览器
```

创新价值：

- 多摄像头系统对时间一致性要求高。
- 一键同步可以保证四路录像、OSD 时间戳和事件发生时间保持一致。
- 广播控制逻辑可以扩展到其他全局命令。

### 5.9 多路控制的状态隔离

控制面板只有一套 UI，但可以控制四台设备。

关键机制：

```text
selectCamera(camId)
    → saveControlState(selectedCam)
    → selectedCam = camId
    → 更新标签样式
    → 更新标题 Camera X 控制
    → restoreControlState(camId)
```

解决的问题：

- 避免四套控制面板占用页面空间。
- 避免用户切换 Camera 后 UI 状态错乱。
- 让每个摄像头的遮罩区域、OSD、录像状态都能独立保存。

---

## 6. 创新设计价值总结

### 创新点一：Web 前端平台化

核心价值：

- 将嵌入式 RTSP 视频流转化为浏览器可直接播放的 WebRTC。
- 用户无需安装 VLC 或浏览器插件，打开网页即可预览。
- 将视频播放、设备控制、PC 录像、状态显示集成在同一页面。
- 通过 WHEP 标准接口建立 WebRTC 链路，结构清晰，延迟低。

可以在 PPT 中概括为：

```text
传统方式：单路 RTSP + 专用播放器
V6 方式：多路 RTSP → WebRTC → 浏览器监控平台
```

### 创新点二：四路设备统一控制

核心价值：

- 用 `camera_id` 将 UI 操作映射到不同开发板。
- 用统一 API 屏蔽不同设备 IP 和 TCP 协议细节。
- 四路视频并发预览，单路参数独立控制。
- 全局命令支持广播，例如时间同步。

可以在 PPT 中概括为：

```text
传统方式：每块板单独操作、单独拉流、单独控制
V6 方式：一个 Web 页面同时预览和控制 4 块板
```

### 最终展示亮点

- 2×2 多路视频墙。
- 主码流/子码流独立切换。
- HTTP API 到板端 TCP 控制协议转换。
- OSD 中文输入与 GB2312 编码转换。
- 多遮罩区域在线配置。
- 设备端录像和 PC 端录像两种方式。
- 四路设备一键连接、一键断开、一键时间同步。
- 摄像头控制状态独立保存，切换操作不混乱。

---

## 7. PPT 讲解建议

### 第 1 页：创新设计总览

讲解重点：

- 本系统不只是板端 RTSP 推流，而是实现了“板端采集编码 + Ubuntu 中继 + Windows Web 控制”的完整闭环。
- 两个创新点分别解决“怎么在浏览器看”和“怎么同时管四路设备”。

### 第 2 页：Web 前端架构

讲解重点：

- RTSP 不能被浏览器直接播放，因此引入 MediaMTX 做 RTSP 到 WebRTC 的转换。
- 浏览器通过 WHEP 拉流，video 标签直接播放。

### 第 3 页：Web 前端功能

讲解重点：

- 页面不是静态播放器，而是动态生成四路视频墙。
- 每一路都有状态显示和码流切换。
- 控制面板覆盖图像、OSD、遮罩、录像、LED 等功能。

### 第 4 页：四路同时控制架构

讲解重点：

- 前端只关心 `camera_id`。
- Node.js 负责把 `camera_id` 转换成板端 IP。
- 板端控制服务监听 9999 端口，解析命令并调用硬件接口。

### 第 5 页：四路状态隔离和广播控制

讲解重点：

- 每一路视频连接、控制状态、录像计时都独立保存。
- 时间同步采用广播方式，一次操作同步四块板。

### 第 6 页：创新价值总结

讲解重点：

- 从单板、单流、单控制，提升为多路、Web 化、统一管理。
- 降低使用门槛，提高系统展示效果和工程完整度。

