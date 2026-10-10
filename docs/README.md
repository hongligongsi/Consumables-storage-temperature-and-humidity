# 智能耗材仓 · 文档

ESP32-S3 N16R8 3D 打印耗材干燥/恒温仓控制器。Arduino 框架 + PlatformIO。

| 文档 | 内容 |
| --- | --- |
| [hardware.md](hardware.md) | 引脚映射、传感器接线、上电前必须复核的项 |
| [build-and-flash.md](build-and-flash.md) | 编译、烧录、预编译固件、分区表与 OTA |
| [networking.md](networking.md) | WiFi 配网、REST API、MQTT 主题、NTP、OTA |
| [settings.md](settings.md) | 系统设置 32 项逐条说明与取值范围 |
| [control.md](control.md) | 双 PID 协同、过流软降、斜率限制与安全停机 |
| [development.md](development.md) | 纯逻辑分层、主机端单测、CI、工具脚本与固件发布流程 |

## 快速开始

```powershell
pio run -t upload
pio device monitor -b 115200
```

首次上电若无 WiFi 凭据，设备会开出 `FilamentChamber-Setup` 热点，手机连上后配网。

改代码请先看 [development.md](development.md)：`python tools/doc_check.py` 与
`python tools/run_host_tests.py` 能在本地把大部分文档漂移和纯逻辑回归提前拦下。
