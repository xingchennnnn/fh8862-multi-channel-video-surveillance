# RTSPserver 问题与调试汇总

> 项目: FH8862 RTSP 视频流服务器  
> 汇总日期: 2026-05-18

---

## 一、历次报错记录

### 1.1 第一次报错（2026-05-08 19:52）

板端启动后编码器/ISP 持续报错，VLC 可连接但无画面：

```
Error(-2130624502 - 0x8101400a): FH_VENC_GetStream_Block failed!
API_ISP_Run error with ret = 0xa0084101
wait pic_start outtime, id,0
```

**原因**: sensor/MIPI 通路无有效数据 → ISP Run 失败(0xa0084100/0xa0084101) → VENC 取流超时(0x8101400a) → 编码器无帧输出。

**关键发现**:
- `0xa0084100/0xa0084101` = ISP 驱动报错（sensor→ISP 通路）
- `0x8101400a` = VENC 驱动报错（上游无数据）
- 旧版手写 RTSP 用静态 SDP 掩盖了握手阶段的失败（连接成功但无画面）
- live555 需要从实际码流中提取 SPS/PPS 生成 SDP，直接暴露了编码器不出帧

**修复**: `src/main.c` 的 `start_isp()` 在 `isp_server_run()` 前添加 `usleep(500*1000)` 给 sensor 500ms 稳定延时。

---

### 1.2 第二次报错（2026-05-08 20:04）

现象同第一次，额外出现 `Segmentation fault` 和 `ERROR: Pkg load err !`：

```
ERROR: Pkg load err !
Segmentation fault
```

重启后 Segfault 持续出现，ps 显示 xbus-rx 进程处于 DW 状态。

---

### 1.3 第三次报错（2026-05-08 20:18）

重启后多次 Segmentation fault，无法正常启动。板端只有通过断电重置才能恢复。

---

### 1.4 第四次报错（2026-05-11 下午）— live555 迁移编译错误

**错误1**: `openssl/ssl.h: No such file`
- 修复: Makefile 添加 `-DNO_OPENSSL=1` 到 COMMON_FLAGS

**错误2**: SDK C 头文件 `private` 关键字冲突
```
./include/dsp/fh_common.h:212: error: expected unqualified-id before 'private'
  FH_UINT64  private[4];
```
- 修复: `main.cpp` 回退为 `main.c`，C 头文件无需 `extern "C"` 包裹

**错误3**: C++ enum `{0}` 初始化严格检查 → 修复: 回退 main.c

**错误4**: signed/unsigned 比较错误 `-Werror=sign-compare`
- 修复: `int i` → `unsigned int i`

**关键经验**:
- `rtsp_live555.h` 已提供 `extern "C"` API，main.c 保持纯 C 编译即可调用 live555
- live555 的宏 `-DNO_OPENSSL`、`-DLOCALE_NOT_USED` 必须与 .a 库编译时一致
- C 头文件不应 `extern "C"` 包裹后给 C++ 编译器，`extern "C"` 只改链接名不改关键字解析

---

### 1.5 第五次报错（2026-05-11）— VLC 无法打开 RTSP 流

VLC 无法打开 `rtsp://192.168.1.3:8554/main`，同时板端输出编码器错误。

**根因**: sensor/MIPI 无有效数据 → 是驱动层/硬件层问题，非应用层 bug  
**修复**: 添加 500ms sensor 稳定延时（同第一次报错）

---

### 1.6 第六次报错（2026-05-11）— 花屏严重（I 帧截断）

**现象**: VLC 播放主码流画面花屏严重。

**根因**: live555 `StreamParser.cpp` 内部 BANK_SIZE=150KB。帧超过此值时：
1. 截断帧至 150KB，`fNumTruncatedBytes` 保存剩余
2. `afterGettingBytes1()` 忽略了 `numTruncatedBytes`
3. 截断数据末尾为不完整 NALU → parser 再次请求
4. H264LiveSource 已释放当前帧 → 新旧数据混叠 → NALU 解析错乱

**修复**:
- `src/h264_live_source.h`: 新增 `m_currentData/m_currentSize/m_currentOffset/m_currentPts` 支持分片交付
- `src/h264_live_source.cpp`: 重写 `doGetNextFrame()`，大帧分多次交付
- `src/rtsp_live555.cpp`: FrameQueue 30→10 帧

---

### 1.7 第七次报错（2026-05-11）— 主码流仍花屏（起始码问题）

**现象**: 分片修复后子码流正常，主码流仍花屏。

**根因**: `includeStartCodeInOutput=True` → 每个 NALU 前附加 4 字节起始码 → H264VideoRTPSink 的 FU-A 分片器预期纯 NALU 输入 → 起始码字节干扰分片逻辑

- 子码流 NALU 小，不走 FU-A → 无影响
- 主码流 I 帧大，需 FU-A → 起始码 0x00 被误当 NALU header → 花屏

**修复**: `includeStartCodeInOutput` True → False

---

## 二、代码检查报告摘要

> 原检查日期: 2026-05-08

### 高优先级问题

| 问题 | 文件 | 状态 |
|------|------|------|
| SIGKILL 无法被捕获（无效代码） | main.c L847 | 需修复 |
| isp.c 线程死循环无法退出 | isp.c L250 | 需修复 |
| isp.c 栈变量传给分离线程 | isp.c L283 | 需修复 |

### 中优先级问题

| 问题 | 文件 |
|------|------|
| 数据竞争 client->used | rtsp_server.c L903（手写版，已废弃） |
| g_sig_stop 类型非 sig_atomic_t | main.c L46 |
| libpes.c/isp.c 中文注释乱码 | 需修正 |

### 低优先级问题

- Makefile 死代码 `OBJECTSEX`（obj/ 目录不存在，永远为空）
- I2C fd 重复打开/关闭（sensor.c）
- inet_ntoa 非线程安全（手写版，已废弃）
- 硬件编码器/ISP 无错误恢复机制

---

## 三、板端长时间运行崩溃分析方案

### 已识别的潜在原因及状态

| 序号 | 问题 | 风险 | 状态 |
|------|------|------|------|
| 1 | 编码推流线程栈溢出（原3KB） | 低 | **已修复** → 256KB |
| 2 | LED 闪烁线程泄漏 | 中低 | **已缓解** — 添加守卫防重复创建 |
| 3 | 控制服务器线程栈偏小（64KB） | 低 | 未修改 |
| 4 | 每帧 malloc/free（50次/秒）内存碎片化 | **中高** | 未优化 |
| 5 | FrameQueue 无容量上限 | 需调查 | 需检查 |
| 6 | DMC/PES 内部状态残留 | 需调查 | 需确认 |
| 7 | 硬件编码器/ISP 长时间运行稳定性 | 中 | 未处理 |

### 建议修复优先级

1. **P0**: 帧缓冲区内存池化 — 将每帧 malloc/free 改为预分配环形缓冲区
2. **P1**: FrameQueue 添加容量上限
3. **P2**: 编码器错误恢复 — 连续 N 次错误后重新初始化
4. **P3**: ISP 线程看门狗

---

## 四、xty 修改日志摘要（2026-05-11）

### 手写 RTSP → live555 迁移

**新建文件**:
| 文件 | 说明 |
|------|------|
| `src/frame_queue.h/cpp` | 线程安全帧队列 (pthread_mutex + std::queue, 最大30帧) |
| `src/h264_live_source.h/cpp` | 继承 FramedSource，从 FrameQueue 拉取 |
| `src/h264_subsession.h/cpp` | OnDemandServerMediaSubsession，创建 Source→Framer→Sink |
| `inc/rtsp_live555.h` | C 兼容 API: rtsp_live555_start/stop/push_frame |
| `src/rtsp_live555.cpp` | 创建 TaskScheduler→Env→FrameQueue×2→RTSPServer→doEventLoop |

**重命名/备份**:
| 旧路径 | 新路径 |
|--------|--------|
| `src/main.c` → `src/main.cpp` | 后回退为 main.c |
| `src/rtsp_server.c` | `src/rtsp_server.c.bak` → 已删除 |
| `inc/rtsp_server.h` | `inc/rtsp_server.h.bak` → 已删除 |

**架构**: 编码器 push → FrameQueue → live555 H264LiveSource::doGetNextFrame() pull
- 队列空时 10ms 延迟重试（非阻塞）
- `/main`: 1280×720, ~2Mbps; `/sub`: 704×576, ~700kbps

**后续修复**:
- NALU 前导零清洗：编码器输出的 NALU 可能带前导零字节 → 在 `sample_push_h264_stream_to_rtsp()` 中跳过
- 花屏二次修复：`includeStartCodeInOutput` True→False
- 帧分片交付：大帧超过 BANK_SIZE 时分多次交付
