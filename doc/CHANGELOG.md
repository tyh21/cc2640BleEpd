# CHANGELOG

CC2640R2 BLE 电子价签 —— 固件 / Android App / PC 工具 的主要改动记录。

- 固件版本号见 `src/app/task_epd.c` 的 `VERSION_MAJOR.VERSION_MINOR`
  （BLE `READ_VERSION 0x22` 会返回），当前 **0.5**。
- 时间以本机日期为准；同一轮的多项改动合并记录。

---

## 固件 0.5（2026-10）—— 传输提速 / 面板仲裁 / 低功耗 / NFC 中断 / 协议加固

### 传输：多行块写 + 整图校验（P0-2）
- 新增 `0x16 WRITE_BLK_RAM_BULK`：`[0x16][off_hi][off_lo][data...]`，一次带多行；
  驱动每次调用重装 RAM 地址计数器 → 同一块重发也安全（幂等）。
  - 传图往返从 **212 次降到约 27 次**（约 8 倍），保留旧 `0x14` 逐帧命令兼容。
- 新增 `0x15 GET_BLK_RAM_CRC`：固件按写入顺序累积 `CRC-16/CCITT-FALSE`
  （poly 0x1021, init 0xFFFF，check("123456789")=0x29B1），手机端整幅比对。
- 客户端同步：Android / Python(`epd_tool.py`) / 网页(`html/index.html`) 全部改用
  0x16 + CRC；`doc/protocol.txt` 重写。

### 面板仲裁（A1）
- 新增 `EPD_Panel_Acquire/Release`（二进制信号量，`epd2in13.c`）。
- 时钟渲染、BLE 图像命令、NFC 流式写屏三条路径共用 SPI0 + SSD1680 RAM，
  现全部持锁，避免命令/数据序列交错导致的错位/花屏。

### 低功耗（B1）
- `main.c` 补上 `Power_init()`（本项目把 `Board_init()` 换成了裸 `PIN_init()`，
  电源驱动此前从未初始化）。
- app 工程开启 `POWER_SAVING`（与 stack 工程一致），设备可进 **STANDBY**。

### NFC 中断唤醒（IRQ_N）
- FM11NC08S `IRQ_N`（低有效）接在 `DIO_13`；下降沿中断 + `Event` 唤醒 NFC 任务，
  不再靠 1ms 轮询（硬件已验证：芯片默认即会拉低 IRQ_N）。
- 兜底轮询 `NFC_FALLBACK_MS`（`nfc_esl.c`，默认 **200ms**）；循环内所有"按槽数计时"
  的看门狗式阈值改为**按真实毫秒计时**，故兜底周期可自由放大。

### 广播名 / 设备地址
- 广播名改为 **`Peripheral_<MAC后3字节>`**（在 `GAPROLE_STARTED` 填充，
  并同步 GGS 设备名），便于分辨不同价签。
- 删除写死的 `HCI_EXT_SetBDADDRCmd({00,01,02,03,04,05})`，改用**芯片出厂唯一 IEEE
  地址**（CCFG `IEEE_BLE_0/1 = 0xFFFFFFFF`）。修复"所有板子 MAC 相同"的问题。

### 通知发送健壮性（A2）
- 通知（ACK）发送失败（无 HCI buffer）时，**有界重试 5 次后丢弃**，不再立即重投
  事件造成的忙等自旋；配合客户端的超时重传，端到端可恢复。

### 帧协议加固（A1–A3，本轮）
- 未知命令：回显 cmd（不再静默不回 → 客户端停等不会干等）。
- `0x16` 越界：`off+n ≤ 4000` 才写入，越界拒绝并拒绝计入 CRC。
- 超长帧（> `EPD_RX_FRAME_LEN`）：回显 cmd、不执行，同样不干等。

### 内存
- `MAX_NUM_PDU` 由默认 5 改为 **2**（本协议是停等，2 个缓冲足够），给 ICall 堆腾空间。

### 看门狗（B6）—— 暂缓
- 曾加入看门狗（`main.c`，3s 超时，1s Clock 喂狗），但在本板上**误复位**，
  已**默认关闭**（`#define ENABLE_WATCHDOG` 注释掉）。
- 结论：要加就应"**喂在 Idle 钩子**、只为兜死锁/挂死"，而不是当定时器心跳。以后再做。

---

## Android App（2026-10）

- **废弃 API 迁移**：扫描统一用 `BluetoothLeScanner`（删除 `startLeScan`/`LeScanCallback`）；
  写特征值/描述符改用 API 33+ 新签名（低版本回退）；新增 `onCharacteristicChanged`
  三参重载；`getDefaultAdapter()` → `BluetoothManager.getAdapter()`。
- **传输健壮性**：下载全流程每帧 **800ms 超时 + 最多 6 次重传**（0x16 幂等），超限中止并提示。
- **提速**：图片二值化由逐点 `getPixel` 改一次性 `getPixels`。
- **清理**：删除死代码 `DeviceListActivity`（及其清单声明）；去掉重复/遗留调试代码、无用字段。
- 待办：`versionName`/`versionCode` 仍为 `2.3 / 46`，可按需升版。

---

## PC 工具 / 工程

- `EpdTool/epd_tool.py`、`EpdTool/html/index.html`：同步 0x16 块写 + CRC。
- `doc/protocol.txt`：更新命令表 / 传输流程 / MTU / CRC 说明。
- `doc/pins.txt`、`doc/usage.txt`：记录 IRQ_N(DIO_13)、电源与 NFC 中断说明。
- 命令行构建脚本（仓库根）：
  - `build_fw.bat [label]`  —— IAR 编 stack→app，产物进 `out\`；
  - `build_app.bat [label]` —— Gradle 编 APK，产物进 `out\`；
  - `build_all.bat` —— 两者一起。
- `.gitignore`：忽略 `out/`、构建产物、`.gradle/`、`local.properties`、IDE 文件等。
- `test_bulk_crc.py`：主机侧 CRC/分块自测（无需设备）。

---

## 已知限制 / 待办

- **ICall 堆偏紧**（开机约 1995B / 连接后约 1779B）；若出现连接不稳，需继续腾 RAM
  （缩任务栈 / 降 `MAX_PDU_SIZE`）。
- **BLE 无配对/加密**；**NFC PIN 明文**（`1122334455`）。
- **无 OAD/DFU**（只能线刷）。
- **看门狗默认关闭**（见上）。
- Python 与 HTML 两个上位机功能重复，可考虑合并 / 共用一份协议定义。
