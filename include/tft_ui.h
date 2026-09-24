#pragma once
#include "ui_model.h"

// ST7796 480x320 TFT 的显示层。只负责把 UiSnapshot 画到屏幕上,
// 不持有任何业务状态;所有数据由 main.cpp 每 500ms 通过 render() 喂入。
// 绘制在独立低优先级任务中执行(见 tft_ui.cpp 的 uiRenderTask),
// 并在 PSRAM 可用时使用两块全屏 sprite 做双缓冲,避免闪烁与 SPI 传输
// 阻塞主循环。
class TftUi {
 public:
  // 初始化 TFT、可选地在 PSRAM 中创建双缓冲 sprite,并启动渲染任务。
  // PSRAM 不可用时自动回退为直接刷屏(无缓冲),功能不受影响。
  void begin();
  // Non-blocking: the newest snapshot replaces the pending frame.  Actual SPI
  // drawing runs in a dedicated low-priority task, so a 480x320 transfer can
  // never stall the 50 ms thermal-control cadence in loop().
  // 非阻塞投递一帧:新快照会直接覆盖队列中尚未绘制的旧快照,因此渲染再慢
  // 也不会堆积,只显示最新状态。
  void render(const UiSnapshot &snapshot);
  // 是否成功启用了 PSRAM 双缓冲(调试/诊断用)。
  bool doubleBuffered() const { return doubleBuffered_; }
  // OTA 屏显:升级期间在屏幕底部叠加进度条与百分比文本。
  // active=false 时清掉横幅;pct 范围 0..100。主循环调用,低写入量,不影响渲染。
  void setOtaProgress(bool active, uint8_t pct);
 private:
  bool ready_ = false;       // begin() 是否完成(tft.init 成功)
  bool doubleBuffered_ = false; // 全屏双缓冲 sprite 是否创建成功
  void *queue_ = nullptr;    // FreeRTOS 队列句柄(QueueHandle_t),main 任务投递、渲染任务取出
  volatile bool otaActive_ = false; // 正在 OTA:渲染任务据此决定是否叠加横幅
  volatile uint8_t otaPct_ = 0;     // 最近一次 OTA 进度 0..100
};
