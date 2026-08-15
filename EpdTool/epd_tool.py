"""
EPD BLE Tool - CC2640R2 电子价签上位机
功能: BLE扫描/连接, 图片下发(PNG/JPG/BMP→黑白位图), 快捷命令(清屏/棋盘格/全黑)
依赖: bleak, Pillow (pip install bleak Pillow)
"""

import asyncio
import struct
import sys
import os
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from threading import Thread, Event
from PIL import Image

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    print("请先安装 bleak: pip install bleak")
    sys.exit(1)


# ============================================================
# BLE UUID 定义 (与固件 uarttrans_service.c 一致)
# ============================================================
SERVICE_UUID = "f000fff0-0451-4000-b000-000000000000"
CHAR_UUID    = "f000fff1-0451-4000-b000-000000000000"  # 单 Characteristic 双向

# EPD 命令 (与固件 task_epd.c 一致)
CMD_PING             = 0x10
CMD_INIT             = 0x11
CMD_DEINIT           = 0x12
CMD_PREPARE_BLK_RAM  = 0x13
CMD_WRITE_BLK_RAM    = 0x14
CMD_GET_BLK_RAM_CRC  = 0x15
CMD_PREPARE_RED_RAM  = 0x18
CMD_WRITE_RED_RAM    = 0x19
CMD_GET_RED_RAM_CRC  = 0x20
CMD_UPDATE_DISPLAY   = 0x21
CMD_READ_VERSION     = 0x22
CMD_CLEAR            = 0x30
CMD_TEST_PATTERN     = 0x31
CMD_ALL_BLACK        = 0x32
CMD_LED_TEST         = 0x40
CMD_LED_TOGGLE       = 0x41
CMD_SET_TIME         = 0x50
CMD_GET_TIME         = 0x51
CMD_SWITCH_FACE      = 0x60

# EPD 参数
EPD_WIDTH  = 104   # 像素
EPD_HEIGHT = 212   # 像素
EPD_BUF_SIZE = EPD_WIDTH // 8 * EPD_HEIGHT  # 2756 字节 (212行×13字节)
FRAME_DATA_SIZE = 16  # 每帧16字节(13字节数据+3字节0xFF填充), 对齐SSD1680 RAM行宽
TOTAL_FRAME_COUNT = EPD_BUF_SIZE // 13  # 212 帧 (每帧=1行16字节)


# ============================================================
# EPD 图片处理
# ============================================================
class EpdImage:
    """图片加载与转换: PNG/JPG/BMP → 104×212 黑白位图"""

    def __init__(self):
        self.black_buf = bytearray([0xFF] * (EPD_HEIGHT * FRAME_DATA_SIZE))  # 212行×16字节

    def load_image(self, filepath: str, mode: str = "stretch") -> bool:
        """加载图片文件, 自动缩放到 104×212, 阈值化转黑白, 按16字节/行打包
        
        mode: 缩放模式
          - stretch:  拉伸填满 (默认)
          - fit:      等比缩放居中, 空白填白
          - crop:     等比缩放剪裁填满
        """
        try:
            img = Image.open(filepath)
            img = img.convert("L")

            if mode == "fit":
                # 等比缩放, 居中, 空白填白
                img = self._resize_fit(img, EPD_WIDTH, EPD_HEIGHT)
            elif mode == "crop":
                # 等比缩放, 剪裁填满
                img = self._resize_crop(img, EPD_WIDTH, EPD_HEIGHT)
            else:
                # 拉伸填满
                img = img.resize((EPD_WIDTH, EPD_HEIGHT), Image.LANCZOS)

            self._pixels_to_buf(img)
            return True
        except Exception as e:
            print(f"图片加载失败: {e}")
            return False

    def _resize_fit(self, img: Image.Image, w: int, h: int) -> Image.Image:
        """等比缩放居中, 空白填白"""
        iw, ih = img.size
        scale = min(w / iw, h / ih)
        nw, nh = int(iw * scale), int(ih * scale)
        img = img.resize((nw, nh), Image.LANCZOS)
        canvas = Image.new("L", (w, h), 255)
        canvas.paste(img, ((w - nw) // 2, (h - nh) // 2))
        return canvas

    def _resize_crop(self, img: Image.Image, w: int, h: int) -> Image.Image:
        """等比缩放, 剪裁填满"""
        iw, ih = img.size
        scale = max(w / iw, h / ih)
        nw, nh = int(iw * scale), int(ih * scale)
        img = img.resize((nw, nh), Image.LANCZOS)
        left = (nw - w) // 2
        top = (nh - h) // 2
        img = img.crop((left, top, left + w, top + h))
        return img

    def _pixels_to_buf(self, img: Image.Image):
        """将灰度图逐像素写入 black_buf (16字节/行对齐)"""
        pixels = img.load()
        buf = self.black_buf
        for i in range(len(buf)):
            buf[i] = 0xFF

        for y in range(EPD_HEIGHT):
            row_offset = y * FRAME_DATA_SIZE
            for x in range(EPD_WIDTH):
                pixel = pixels[x, y]
                if pixel < 128:
                    byte_idx = row_offset + (x >> 3)
                    bit_idx = 7 - (x & 7)
                    buf[byte_idx] &= ~(1 << bit_idx)

    def load_raw(self, filepath: str) -> bool:
        """加载原始二进制文件 (2756 字节), 按16字节/行重新打包"""
        try:
            with open(filepath, "rb") as f:
                data = f.read(EPD_BUF_SIZE)  # 2756字节(13字节/行)
            # 按16字节/行重新打包: 每行13字节数据 + 3字节0xFF
            buf = self.black_buf
            for y in range(EPD_HEIGHT):
                src_off = y * 13
                dst_off = y * FRAME_DATA_SIZE
                buf[dst_off:dst_off+13] = data[src_off:src_off+13]
                buf[dst_off+13:dst_off+16] = [0xFF, 0xFF, 0xFF]
            return True
        except Exception as e:
            print(f"原始文件加载失败: {e}")
            return False

    def load_bin(self, filepath: str) -> bool:
        """加载 .bin 文件 (自动按16字节/行重新打包)"""
        return self.load_raw(filepath)

    def get_frame(self, frame_idx: int) -> bytes:
        """获取指定帧数据 (16 字节 = 1整行)"""
        buf = self.black_buf
        start = frame_idx * FRAME_DATA_SIZE
        end = start + FRAME_DATA_SIZE
        return bytes(buf[start:end])




# ============================================================
# BLE 通信层
# ============================================================
class BleEpdClient:
    """BLE EPD 客户端: 扫描, 连接, 命令收发"""

    def __init__(self):
        self.client: BleakClient | None = None
        self.connected = False
        self._notify_queue: asyncio.Queue = asyncio.Queue()

    async def scan(self, timeout: float = 5.0) -> list[tuple[str, str]]:
        """扫描 BLE 设备, 返回 [(address, name), ...]"""
        devices = await BleakScanner.discover(timeout=timeout)
        result = []
        for d in devices:
            name = d.name or "Unknown"
            result.append((d.address, name))
        return result

    async def connect(self, address: str) -> bool:
        """连接指定设备"""
        self.client = BleakClient(address)
        try:
            await self.client.connect()
            self.connected = True
            # 开启 Notify
            await self.client.start_notify(CHAR_UUID, self._on_notify)
            return True
        except Exception as e:
            print(f"连接失败: {e}")
            self.connected = False
            return False

    async def disconnect(self):
        """断开连接"""
        if self.client and self.connected:
            try:
                await self.client.stop_notify(CHAR_UUID)
            except:
                pass
            try:
                await self.client.disconnect()
            except:
                pass
        self.connected = False
        self.client = None

    def _on_notify(self, sender, data: bytearray):
        """Notify 回调"""
        try:
            self._notify_queue.put_nowait(bytes(data))
        except:
            pass

    async def write(self, data: bytes):
        """写数据到 Characteristic"""
        if self.client and self.connected:
            await self.client.write_gatt_char(CHAR_UUID, data, response=False)

    async def read_notify(self, timeout: float = 10.0) -> bytes | None:
        """读取一条 Notify 数据 (带超时)"""
        try:
            return await asyncio.wait_for(self._notify_queue.get(), timeout=timeout)
        except asyncio.TimeoutError:
            return None


# ============================================================
# EPD 下载状态机
# ============================================================
class EpdDownloader:
    """EPD 图片下载状态机 (应答式分帧传输)"""

    def __init__(self, ble: BleEpdClient, image: EpdImage):
        self.ble = ble
        self.image = image
        self.frame_idx_black = 0
        self.frame_idx_red = 0
        self.state = "idle"
        self.progress = 0
        self.on_progress = None  # callback(state, progress, msg)

    def _report(self, state: str, progress: int, msg: str):
        self.state = state
        self.progress = progress
        if self.on_progress:
            self.on_progress(state, progress, msg)

    async def download(self):
        """执行完整下载流程 (与 Android App 一致)"""
        self.frame_idx_black = 0
        self.frame_idx_red = 0

        # 1. ReadVersion
        self._report("version", 0, "读取版本...")
        await self.ble.write(bytes([CMD_READ_VERSION]))
        resp = await self.ble.read_notify(timeout=5)
        if resp and resp[0] == CMD_READ_VERSION:
            ver = f"{resp[1]}.{resp[2]}" if len(resp) >= 3 else "?"
            self._report("version", 0, f"固件版本: {ver}")
        else:
            self._report("version", 0, "版本读取超时, 继续下载")

        # 2. Init
        self._report("init", 2, "初始化EPD...")
        await self.ble.write(bytes([CMD_INIT]))
        resp = await self.ble.read_notify(timeout=10)
        if not resp:
            self._report("error", 0, "Init 超时")
            return False

        # 3. 写黑图 RAM
        self._report("black", 5, "写入黑图数据...")
        await self.ble.write(bytes([CMD_PREPARE_BLK_RAM]))
        resp = await self.ble.read_notify(timeout=5)
        if not resp:
            self._report("error", 0, "PrepareBlkRAM 超时")
            return False

        for i in range(TOTAL_FRAME_COUNT):
            frame = self.image.get_frame(i)
            await self.ble.write(bytes([CMD_WRITE_BLK_RAM]) + frame)
            resp = await self.ble.read_notify(timeout=5)
            if not resp:
                self._report("error", 0, f"WriteBlkRAM 帧{i} 超时")
                return False
            pct = 5 + int(i / TOTAL_FRAME_COUNT * 80)
            self._report("black", pct, f"写入黑图 {i+1}/{TOTAL_FRAME_COUNT}")

        # 4. Update
        self._report("update", 88, "刷新显示...")
        await self.ble.write(bytes([CMD_UPDATE_DISPLAY]))
        resp = await self.ble.read_notify(timeout=30)  # 刷新较慢
        if not resp:
            self._report("error", 0, "UpdateDisplay 超时")
            return False

        # 5. Deinit
        self._report("deinit", 95, "休眠EPD...")
        await self.ble.write(bytes([CMD_DEINIT]))
        resp = await self.ble.read_notify(timeout=5)

        self._report("done", 100, "下载完成!")
        return True

    async def quick_cmd(self, cmd: int):
        """发送快捷命令"""
        await self.ble.write(bytes([cmd]))
        resp = await self.ble.read_notify(timeout=30)


# ============================================================
# GUI 界面
# ============================================================
class EpdToolApp:
    """tkinter GUI 主界面"""

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("EPD BLE Tool - CC2640R2 电子价签")
        self.root.geometry("520x760")
        self.root.resizable(True, True)

        self.ble = BleEpdClient()
        self.image = EpdImage()
        self.downloader = EpdDownloader(self.ble, self.image)
        self.loop: asyncio.AbstractEventLoop | None = None
        self.thread: Thread | None = None
        self.stop_event = Event()

        self._build_ui()

    def _build_ui(self):
        # --- BLE 连接区 ---
        lf_ble = ttk.LabelFrame(self.root, text="BLE 连接", padding=8)
        lf_ble.pack(fill=tk.X, padx=10, pady=(10, 5))

        self.btn_scan = ttk.Button(lf_ble, text="扫描设备", command=self._on_scan)
        self.btn_scan.pack(side=tk.LEFT, padx=2)
        self.btn_connect = ttk.Button(lf_ble, text="连接", command=self._on_connect, state=tk.DISABLED)
        self.btn_connect.pack(side=tk.LEFT, padx=2)
        self.btn_disconnect = ttk.Button(lf_ble, text="断开", command=self._on_disconnect, state=tk.DISABLED)
        self.btn_disconnect.pack(side=tk.LEFT, padx=2)

        self.lbl_status = ttk.Label(lf_ble, text="未连接", foreground="gray")
        self.lbl_status.pack(side=tk.RIGHT, padx=5)

        # 设备列表
        self.device_list = tk.Listbox(self.root, height=5, font=("Consolas", 9))
        self.device_list.pack(fill=tk.X, padx=10, pady=2)
        self.device_list.bind("<<ListboxSelect>>", self._on_device_select)
        self.devices = []  # [(address, name), ...]

        # --- 图片区 ---
        lf_img = ttk.LabelFrame(self.root, text="图片", padding=8)
        lf_img.pack(fill=tk.X, padx=10, pady=5)

        row1 = ttk.Frame(lf_img)
        row1.pack(fill=tk.X, pady=2)
        ttk.Button(row1, text="选择图片", command=self._load_image).pack(side=tk.LEFT, padx=2)
        self.lbl_black = ttk.Label(row1, text="未加载 (全白)", foreground="gray")
        self.lbl_black.pack(side=tk.LEFT, padx=5)

        row2 = ttk.Frame(lf_img)
        row2.pack(fill=tk.X, pady=2)
        ttk.Label(row2, text="缩放:").pack(side=tk.LEFT, padx=2)
        self.scale_mode = tk.StringVar(value="stretch")
        for text, val in [("拉伸", "stretch"), ("等比居中", "fit"), ("等比剪裁", "crop")]:
            ttk.Radiobutton(row2, text=text, value=val, variable=self.scale_mode).pack(side=tk.LEFT, padx=2)

        row3 = ttk.Frame(lf_img)
        row3.pack(fill=tk.X, pady=2)
        ttk.Label(row3, text="预览:").pack(side=tk.LEFT, padx=2)
        self.preview_canvas = tk.Canvas(row3, width=104, height=212, bg="white", highlightthickness=1, highlightbackground="#ccc")
        self.preview_canvas.pack(side=tk.LEFT, padx=2)
        self.lbl_info = ttk.Label(row3, text="104×212", foreground="gray")
        self.lbl_info.pack(side=tk.LEFT, padx=5)

        # --- 操作区 ---
        lf_cmd = ttk.LabelFrame(self.root, text="操作", padding=8)
        lf_cmd.pack(fill=tk.X, padx=10, pady=5)

        self.btn_download = ttk.Button(lf_cmd, text="下发图片到EPD", command=self._on_download)
        self.btn_download.pack(fill=tk.X, pady=3)

        sep = ttk.Separator(lf_cmd, orient=tk.HORIZONTAL)
        sep.pack(fill=tk.X, pady=5)

        ttk.Label(lf_cmd, text="快捷命令:").pack(anchor=tk.W)
        row_cmd = ttk.Frame(lf_cmd)
        row_cmd.pack(fill=tk.X, pady=3)
        ttk.Button(row_cmd, text="清屏 (0x30)", command=lambda: self._on_quick(CMD_CLEAR, "清屏")).pack(side=tk.LEFT, padx=2)
        ttk.Button(row_cmd, text="棋盘格 (0x31)", command=lambda: self._on_quick(CMD_TEST_PATTERN, "棋盘格")).pack(side=tk.LEFT, padx=2)
        ttk.Button(row_cmd, text="全黑 (0x32)", command=lambda: self._on_quick(CMD_ALL_BLACK, "全黑")).pack(side=tk.LEFT, padx=2)

        row_led = ttk.Frame(lf_cmd)
        row_led.pack(fill=tk.X, pady=3)
        ttk.Label(row_led, text="LED控制:").pack(side=tk.LEFT, padx=2)
        ttk.Button(row_led, text="LED测试 (0x40)", command=lambda: self._on_quick(CMD_LED_TEST, "LED测试")).pack(side=tk.LEFT, padx=2)
        ttk.Button(row_led, text="LED开关 (0x41)", command=lambda: self._on_quick(CMD_LED_TOGGLE, "LED开关")).pack(side=tk.LEFT, padx=2)

        # --- 时钟区 ---
        lf_clock = ttk.LabelFrame(self.root, text="时钟", padding=8)
        lf_clock.pack(fill=tk.X, padx=10, pady=5)

        row_clk = ttk.Frame(lf_clock)
        row_clk.pack(fill=tk.X, pady=2)
        ttk.Button(row_clk, text="同步本机时间", command=self._on_sync_time).pack(side=tk.LEFT, padx=2)
        ttk.Button(row_clk, text="读取设备时间", command=self._on_get_time).pack(side=tk.LEFT, padx=2)
        self.lbl_time = ttk.Label(row_clk, text="--:--", foreground="gray")
        self.lbl_time.pack(side=tk.LEFT, padx=10)

        ttk.Button(row_clk, text="切换表盘", command=self._on_switch_face).pack(side=tk.LEFT, padx=2)
        self.lbl_face = ttk.Label(row_clk, text="点阵", foreground="gray")
        self.lbl_face.pack(side=tk.LEFT, padx=4)

        # --- 进度 ---
        lf_prog = ttk.LabelFrame(self.root, text="进度", padding=8)
        lf_prog.pack(fill=tk.X, padx=10, pady=5)

        self.progress = ttk.Progressbar(lf_prog, maximum=100, mode="determinate")
        self.progress.pack(fill=tk.X, pady=2)
        self.lbl_progress = ttk.Label(lf_prog, text="就绪")
        self.lbl_progress.pack(anchor=tk.W)

        # --- 日志 ---
        lf_log = ttk.LabelFrame(self.root, text="日志", padding=8)
        lf_log.pack(fill=tk.BOTH, expand=True, padx=10, pady=(5, 10))

        self.log_text = tk.Text(lf_log, height=6, font=("Consolas", 9), state=tk.DISABLED)
        scrollbar = ttk.Scrollbar(lf_log, orient=tk.VERTICAL, command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scrollbar.set)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        self.log_text.pack(fill=tk.BOTH, expand=True)

    def log(self, msg: str):
        """写日志"""
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, msg + "\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)

    # --- BLE 事件 ---
    def _on_scan(self):
        self.btn_scan.configure(state=tk.DISABLED)
        self.log("正在扫描BLE设备...")
        self._run_async(self._scan_async())

    async def _scan_async(self):
        try:
            devices = await self.ble.scan(timeout=5.0)
            self.devices = devices
            self.device_list.delete(0, tk.END)
            for addr, name in devices:
                self.device_list.insert(tk.END, f"{name}  [{addr}]")
            self.log(f"扫描到 {len(devices)} 个设备")
        except Exception as e:
            self.log(f"扫描失败: {e}")
        finally:
            self.btn_scan.configure(state=tk.NORMAL)

    def _on_device_select(self, event):
        sel = self.device_list.curselection()
        if sel:
            self.btn_connect.configure(state=tk.NORMAL)
        else:
            self.btn_connect.configure(state=tk.DISABLED)

    def _on_connect(self):
        sel = self.device_list.curselection()
        if not sel:
            return
        idx = sel[0]
        addr = self.devices[idx][0]
        name = self.devices[idx][1]
        self.log(f"正在连接 {name} [{addr}]...")
        self.btn_connect.configure(state=tk.DISABLED)
        self._run_async(self._connect_async(addr, name))

    async def _connect_async(self, addr: str, name: str):
        try:
            ok = await self.ble.connect(addr)
            if ok:
                self.lbl_status.configure(text=f"已连接: {name}", foreground="green")
                self.log(f"已连接: {name}")
                self.btn_connect.configure(state=tk.DISABLED)
                self.btn_disconnect.configure(state=tk.NORMAL)
            else:
                self.lbl_status.configure(text="连接失败", foreground="red")
                self.log("连接失败")
                self.btn_connect.configure(state=tk.NORMAL)
        except Exception as e:
            self.lbl_status.configure(text="连接失败", foreground="red")
            self.log(f"连接异常: {e}")
            self.btn_connect.configure(state=tk.NORMAL)

    def _on_disconnect(self):
        self.log("正在断开...")
        self._run_async(self._disconnect_async())

    async def _disconnect_async(self):
        await self.ble.disconnect()
        self.lbl_status.configure(text="未连接", foreground="gray")
        self.btn_connect.configure(state=tk.NORMAL)
        self.btn_disconnect.configure(state=tk.DISABLED)
        self.log("已断开")

    # --- 图片 ---
    def _load_image(self):
        filepath = filedialog.askopenfilename(
            title="选择图片文件",
            filetypes=[
                ("图片文件", "*.png *.jpg *.jpeg *.bmp *.gif *.tiff"),
                ("原始数据", "*.bin"),
                ("所有文件", "*.*")
            ]
        )
        if not filepath:
            return

        filename = os.path.basename(filepath)
        ext = os.path.splitext(filepath)[1].lower()

        if ext == ".bin":
            ok = self.image.load_raw(filepath)
        else:
            ok = self.image.load_image(filepath, mode=self.scale_mode.get())

        if ok:
            self.lbl_black.configure(text=f"{filename} (已加载)", foreground="green")
            self.log(f"已加载图片: {filename} [{self.scale_mode.get()}]")
            self._update_preview()
        else:
            self.log("图片加载失败!")

    def _update_preview(self):
        """更新EPD预览 (将black_buf渲染到Canvas, 1:1像素)"""
        c = self.preview_canvas
        c.delete("all")
        buf = self.image.black_buf
        # 用PhotoImage逐像素绘制, 比create_rectangle快很多
        photo = tk.PhotoImage(width=EPD_WIDTH, height=EPD_HEIGHT)
        for y in range(EPD_HEIGHT):
            row_colors = []
            for x in range(EPD_WIDTH):
                byte_idx = y * FRAME_DATA_SIZE + (x >> 3)
                bit_idx = 7 - (x & 7)
                is_black = not (buf[byte_idx] & (1 << bit_idx))
                row_colors.append("black" if is_black else "white")
            photo.put("{" + " ".join(row_colors) + "}", to=(0, y))
        self._preview_photo = photo  # keep reference
        c.create_image(0, 0, anchor=tk.NW, image=photo)

    # --- 下载 ---
    def _on_download(self):
        if not self.ble.connected:
            messagebox.showwarning("提示", "请先连接BLE设备")
            return
        self.btn_download.configure(state=tk.DISABLED)
        self.log("开始下发图片...")
        self._run_async(self._download_async())

    async def _download_async(self):
        def on_progress(state, pct, msg):
            self.progress["value"] = pct
            self.lbl_progress.configure(text=msg)
            self.log(msg)

        self.downloader.on_progress = on_progress
        try:
            ok = await self.downloader.download()
            if not ok:
                self.log("下载失败!")
        except Exception as e:
            self.log(f"下载异常: {e}")
        finally:
            self.btn_download.configure(state=tk.NORMAL)

    # --- 快捷命令 ---
    def _on_quick(self, cmd: int, name: str):
        if not self.ble.connected:
            messagebox.showwarning("提示", "请先连接BLE设备")
            return
        self.log(f"发送快捷命令: {name} (0x{cmd:02X})")
        self._run_async(self._quick_async(cmd, name))

    async def _quick_async(self, cmd: int, name: str):
        try:
            await self.downloader.quick_cmd(cmd)
            self.log(f"{name} 完成")
        except Exception as e:
            self.log(f"{name} 失败: {e}")

    # --- 时钟对时 ---
    def _on_sync_time(self):
        if not self.ble.connected:
            messagebox.showwarning("提示", "请先连接BLE设备")
            return
        self._run_async(self._sync_time_async())

    async def _sync_time_async(self):
        import datetime
        now = datetime.datetime.now()
        year_lo = now.year - 2000
        ms = now.microsecond // 1000  # 0-999
        ms_hi = (ms >> 8) & 0xFF
        ms_lo = ms & 0xFF
        data = bytes([CMD_SET_TIME, year_lo, now.month, now.day, now.hour, now.minute, now.second, ms_hi, ms_lo])
        self.log(f"同步时间: {now.strftime('%Y-%m-%d %H:%M:%S')}.{ms:03d}")
        await self.ble.write(data)
        resp = await self.ble.read_notify(timeout=5)
        if resp and resp[0] == CMD_SET_TIME:
            self.log("时间同步成功")
        elif resp:
            self.log(f"时间同步失败: 响应异常 (0x{resp[0]:02X})")
        else:
            self.log("时间同步失败: 未收到响应")

    def _on_get_time(self):
        if not self.ble.connected:
            messagebox.showwarning("提示", "请先连接BLE设备")
            return
        self._run_async(self._get_time_async())

    async def _get_time_async(self):
        self.log("读取设备时间...")
        await self.ble.write(bytes([CMD_GET_TIME]))
        resp = await self.ble.read_notify(timeout=5)
        if resp and resp[0] == CMD_GET_TIME and len(resp) >= 9:
            year = 2000 + resp[1]
            ms = (resp[7] << 8) | resp[8]
            t = f"{year}-{resp[2]:02d}-{resp[3]:02d} {resp[4]:02d}:{resp[5]:02d}:{resp[6]:02d}.{ms:03d}"
            self.lbl_time.configure(text=t, foreground="green")
            self.log(f"设备时间: {t}")
        else:
            self.lbl_time.configure(text="读取失败", foreground="red")
            self.log("读取时间超时")

    def _on_switch_face(self):
        if not self.ble.connected:
            messagebox.showwarning("提示", "请先连接BLE设备")
            return
        self._run_async(self._switch_face_async())

    async def _switch_face_async(self):
        self.log("切换表盘...")
        await self.ble.write(bytes([CMD_SWITCH_FACE]))
        resp = await self.ble.read_notify(timeout=5)
        if resp and resp[0] == CMD_SWITCH_FACE and len(resp) >= 2:
            face = resp[1]
            face_name = "7段LED" if face == 1 else "点阵"
            self.lbl_face.configure(text=face_name)
            self.log(f"表盘切换为: {face_name}")
        else:
            self.log("切换表盘超时")

    # --- 异步运行器 ---
    def _run_async(self, coro):
        """在后台线程的 asyncio 事件循环中运行协程"""
        if self.loop is None or not self.loop.is_running():
            self.thread = Thread(target=self._run_loop, daemon=True)
            self.thread.start()
            # 等循环启动
            import time
            while self.loop is None:
                time.sleep(0.05)

        asyncio.run_coroutine_threadsafe(coro, self.loop)

    def _run_loop(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    # --- 启动 ---
    def run(self):
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.mainloop()

    def _on_close(self):
        if self.loop:
            asyncio.run_coroutine_threadsafe(self.ble.disconnect(), self.loop)
        self.root.destroy()


# ============================================================
# 入口
# ============================================================
if __name__ == "__main__":
    app = EpdToolApp()
    app.run()
