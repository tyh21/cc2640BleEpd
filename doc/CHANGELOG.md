---
AIGC:
  ContentProducer: '001191110102MAD55U9H0F10002'
  ContentPropagator: '001191110102MAD55U9H0F10002'
  Label: '1'
  ProduceID: '6f9881b4-bb2a-46ec-97c6-5b2edecca9ba'
  PropagateID: '6f9881b4-bb2a-46ec-97c6-5b2edecca9ba'
  ReservedCode1: 'd8bf52a8-f441-48f3-aaf2-17972aaa54a6'
  ReservedCode2: 'd8bf52a8-f441-48f3-aaf2-17972aaa54a6'
---

# CHANGELOG

CC2640R2 BLE 电子价签 —— 固件 / Android App / PC 工具 的主要改动记录。

- 固件版本号见 `src/app/task_epd.c` 的 `VERSION_MAJOR.VERSION_MINOR`
  （BLE `READ_VERSION 0x22` 会返回），当前 **0.7**。
- 时间以本机日期为准；同一轮的多项改动合并记录。

---

## 固件 0.8（2026-10-12）—— 修复"时钟有时乱跳"（P0 bug）

### Bug：32 位 tick 差值回绕 → 时间偶发倒退 11.9 小时
- 现象：时钟偶发跳变（时间/日期/星期/农历整体错位），且不定期出现。
- 根因：`now_epoch_sec()` 用 `Clock_getTicks() - s_set_ticks`（32 位差值）
  推算流逝秒数；CC26xx `Clock_tickPeriod = 10µs` 下 32 位 tick 计数
  **每 11.93 小时回绕一次**。SET_TIME 11.9 小时后差值塌缩，
  显示时间瞬间倒退 11.93h 的整数倍，之后继续正常走——与"有时乱跳"吻合。
- 修复（`epd_clock.c` + `task_epd.c`）：
  - 新增 `epd_clock_latch()`：把已流逝整秒结算进 `s_set_epoch_sec`，
    重置 tick 基准并保留亚秒残差；由 60s 周期 tick（**时钟/图片模式都跑**）
    每分钟调用 → 差值恒 ≤60s，永不进入回绕窗口。
  - `epd_clock_set_time()` 写基准三元组（epoch/ticks/flag）改入
    `Hwi_disable()` 临界区，消除与 Swi 上下文 latch 的竞争窗口。

### Bug：0x16 重传导致整图 CRC 必失败（同轮修复，v15 一并携带）
- 根因：`s_blk_crc` 按帧**到达顺序**累加，而手机端对整幅图一次性
  计算。客户端丢 ACK 后重发同一块（0x16 幂等）时，RAM 重写无害，
  但该块被**再次累进 CRC** → 端到端校验必不一致，重试永远失败。
- 修复（`task_epd.c`）：新增 `s_blk_next`（CRC 已覆盖到的字节前沿），
  `0x13 PREPARE` 时清零、`0x16` 写入时 `off >= s_blk_next` 才累加并推进
  前沿；`off < s_blk_next` 判为重传——RAM 照常幂等重写，
  **不再重复计入 CRC**（串口打 `bulk dup ... (kept)`）。
  协议为顺序发送（手机只重发最后一个未确认块），单调前沿判重即完备；
  手机端无需改动。

---

## 固件 0.7（2026-10）—— 空闲 / 广播功耗（P0-2）

### 广播
- 广播间隔 `DEFAULT_ADVERTISING_INTERVAL` **160（100ms）→ 1600（1000ms）**。
  价签在 GENERAL 可发现模式下**永久广播**，100ms 是最大的空闲电流项；放慢后
  广播占空比降约 10×，代价只是手机发现慢 1~2 秒。需要更快发现改回 800（500ms）。

### 空闲（走时基座改为 RTC）
- 走时不再靠 tick 计数：`epd_clock_get_time()` 直接由 RTC（`Clock_getTicks()`）
  推算当前时刻（从 `set_time` 记下的"时刻+ticks"起算）。因此 **tick 无论多慢、甚至不跑，
  走时都准**；BLE `GET_TIME` 的"秒"也恢复精确。
- 新增 `EPDTask_setClockTickEnabled()`（经 `epd_clock_set_mode()` 切换）：
  - **时钟模式**：tick 周期 **1 分钟**，且**首次到期对齐到分钟边界之后约 1s**
    （`epd_clock_ms_to_next_minute()`），到点即重绘 → 分钟**准点跳**、空闲唤醒 ↓60×；
  - **图片模式**：tick 仅作 60s 心跳（不重绘）。
- `SET_TIME` 之后重排对齐并立即重绘一次；覆盖所有模式切换路径
  （BLE `0x21/0x30/0x31/0x32/0x70`、NFC F0 D0 等）。

---

## 固件 0.6（2026-10）—— SPI 批量写提速（P0-1）

### 传输：SPI 批量写
- `epd2in13.c` 新增 `EPD_2IN13_SendDataBuf(buf,len)`：一次 `SPI_transfer` 发送整段数据，
  期间 e-paper CS 全程拉低。（本板 SPI0 硬件 CS 为 `PIN_UNASSIGNED`，CS 完全由软件
  `EPD_CS_PIN` 控制；SSD1680 在 CS 持续拉低时自动递增 RAM 地址，故整块流式写入安全。）
- 改造所有 RAM / LUT 写入，去掉逐字节 `SPI_transfer(count=1)` + 每字节翻 CS：
  - `Clear` / `PrepareBlkRAM` / `PrepareRedRAM` / `QuickCmd(0x30/0x31/0x32)`：
    整屏 4000B 由 4000 次单字节传输降为 **250 次**（每行 16B）。
  - `Display` / `DisplayPartBaseImage` / `DisplayPart`：每行打包 16B 一次发。
  - `WriteRAM` / `WriteRAMAt`：单次批量（时钟渲染每分钟走这里）。
  - `LoadFullLUT` / `SwitchToPartialLUT`：70B LUT 一次发（原 70 次单字节）。
- SPI 时钟 **1 MHz → 4 MHz**（`SPIparams.bitRate`；如某板异常可回落 1 MHz）。
- 效果：一次全屏 RAM 写的驱动调用数从 ~8000 降到 ~500，刷新阶段 CPU 占用与耗时下降。
- **待办：上板实测 4 MHz 是否稳定，稳定后可再试 8 MHz。**

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

> AI生成