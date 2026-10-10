package com.nordicsemi.nrfUARTv2;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RectF;
import android.nfc.tech.IsoDep;

import java.io.IOException;

/**
 * GoodDisplay / Fudan Micro (FMSC) ESL 价签 —— RF 侧 ISO14443-4 APDU 命令封装。
 *
 * 依据文档：
 *   - NFC FMSC ESL User Development Manual (tmp/esl_manual.txt)
 *   - FMSC NFC 协议 (tmp/fmsc_protocol.txt)
 *
 * 关键点：
 *   1. 芯片自带墨水屏驱动，不需要外部 MCU 参与，手机直接发 APDU 即可刷屏。
 *   2. Android 用 IsoDep.transceive() 直接发 APDU。
 *      （手册里的 NDEF 文件封装流程是给 iOS 用的，iOS 不能直接发 APDU，Android 不需要。）
 *   3. CLA='F0' 的命令（D2/D4/DE/D7/D8...）需要先通过 PIN 认证；
 *      认证命令本身 CLA='00'。
 *   4. 初始 PIN = 1122334455（明文）。
 */
public final class EslNfcWriter {

    /** 初始 PIN（明文，2~6 字节 BCD） */
    public static final byte[] DEFAULT_PIN =
            { (byte) 0x11, (byte) 0x22, (byte) 0x33, (byte) 0x44, (byte) 0x55 };

    /** 每个 Load Image 数据包固定 250 字节（最后一包可小于 250） */
    public static final int PACKET_SIZE = 250;

    /** 2.13 寸屏分辨率 */
    public static final int SCREEN_W = 122;
    public static final int SCREEN_H = 250;
    /** 122 像素按字节对齐 -> 每行 16 字节 */
    public static final int ROW_BYTES = 16;
    /** 整幅图 = 16 * 250 = 4000 字节 */
    public static final int IMAGE_BYTES = ROW_BYTES * SCREEN_H;

    private EslNfcWriter() { }

    /* ------------------------------------------------------------------ */
    /* 工具                                                                */
    /* ------------------------------------------------------------------ */

    /** "F0D4058000" -> byte[] */
    public static byte[] hex(String s) {
        s = s.replaceAll("\\s+", "");
        int n = s.length();
        if ((n & 1) != 0) throw new IllegalArgumentException("odd hex length: " + s);
        byte[] out = new byte[n / 2];
        for (int i = 0; i < n; i += 2) {
            out[i / 2] = (byte) Integer.parseInt(s.substring(i, i + 2), 16);
        }
        return out;
    }

    /** byte[] -> "F0 D4 05 80 00" */
    public static String toHex(byte[] b) {
        if (b == null) return "(null)";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < b.length; i++) {
            if (i > 0) sb.append(' ');
            sb.append(String.format("%02X", b[i] & 0xFF));
            if (sb.length() > 240) { sb.append(" ..."); break; }
        }
        return sb.toString();
    }

    /** 取响应末尾的状态字 SW1SW2 */
    public static String sw(byte[] resp) {
        if (resp == null || resp.length < 2) return "----";
        return String.format("%02X%02X", resp[resp.length - 2] & 0xFF,
                resp[resp.length - 1] & 0xFF);
    }

    public static boolean isOk(byte[] resp) {
        return resp != null && resp.length >= 2
                && (resp[resp.length - 2] & 0xFF) == 0x90
                && (resp[resp.length - 1] & 0xFF) == 0x00;
    }

    /** 状态字 -> 中文说明（手册附录 A） */
    public static String swText(String sw) {
        if (sw.startsWith("90")) return "成功";
        if (sw.startsWith("6281")) return "回送数据可能有错";
        if (sw.startsWith("63C")) return "PIN 错误，剩余重试次数 " + sw.substring(3);
        if (sw.startsWith("64")) return "配置/初始化错误 " + sw;
        if (sw.startsWith("6581")) return "写 EEPROM 不成功";
        if (sw.startsWith("6583")) return "EEPROM 状态错误";
        if (sw.startsWith("65")) return "NVM 错误 " + sw;
        if (sw.startsWith("6700")) return "Lc 或 Le 错误";
        if (sw.startsWith("6781")) return "分包发送的数据总长度错误";
        if (sw.startsWith("68C6")) return "刷屏复位失败或屏幕不存在";
        if (sw.startsWith("68CA")) return "刷屏低压报警";
        if (sw.startsWith("68CB")) return "刷屏超时报警";
        if (sw.startsWith("6982")) return "安全状态不满足（PIN 未认证？）";
        if (sw.startsWith("6985")) return "使用条件不满足";
        if (sw.startsWith("6983")) return "PIN 已锁定，需先改 PIN 解锁";
        if (sw.startsWith("6A8")) return "P1 P2 错误";
        if (sw.startsWith("6A80")) return "数据域参数错误";
        if (sw.startsWith("6AE4")) return "写入数据超出 CC 文件属性（>250 字节）";
        if (sw.startsWith("6AE5")) return "NDEF 中的 APDU 不可重复执行";
        return "未知状态 " + sw;
    }

    /* ------------------------------------------------------------------ */
    /* 命令                                                                */
    /* ------------------------------------------------------------------ */

    /** Select NDEF Application。部分固件要求先选应用，失败不影响后续。 */
    public static byte[] selectNdefApp(IsoDep d) throws IOException {
        return d.transceive(hex("00A4040007D276000085010100"));
    }

    /** Get RND：取随机数，Le 指定长度 */
    public static byte[] getRandom(IsoDep d, int len) throws IOException {
        return d.transceive(hex(String.format("00840000%02X", len)));
    }

    /** VERIFY PIN：CLA=00 INS=20 P1=00 P2=01(USER PIN) Lc Data(PIN 明文) */
    public static byte[] verifyPin(IsoDep d, byte[] pin) throws IOException {
        byte[] apdu = new byte[5 + pin.length];
        apdu[0] = (byte) 0x00;
        apdu[1] = (byte) 0x20;
        apdu[2] = (byte) 0x00;
        apdu[3] = (byte) 0x01;          // 01 = USER PIN
        apdu[4] = (byte) pin.length;
        System.arraycopy(pin, 0, apdu, 5, pin.length);
        return d.transceive(apdu);
    }

    /**
     * Set Screen Type：内置 2.13 寸黑白 122x250 驱动。
     * 手册 p16：切到内置驱动需要发两条 DA 命令，只需发一次、不必重复。
     */
    public static byte[] setScreenType(IsoDep d) throws IOException {
        return d.transceive(hex("F0DA000003700020"));
    }

    /** 屏幕颜色控制参数：P2=01，黑白 / 单图 */
    public static byte[] setScreenColor(IsoDep d) throws IOException {
        // Data: 20(黑白,单图) 00 00
        return d.transceive(hex("F0DA000103200000"));
    }

    /**
     * Load Image 的一个分包。
     * 格式：F0 D2 P1(图片索引) P2(包序号) Lc(数据长度) Data
     */
    public static byte[] loadImagePacket(IsoDep d, int imgIndex, int pktIndex,
                                         byte[] image, int offset, int len)
            throws IOException {
        byte[] apdu = new byte[5 + len];
        apdu[0] = (byte) 0xF0;
        apdu[1] = (byte) 0xD2;
        apdu[2] = (byte) imgIndex;
        apdu[3] = (byte) pktIndex;
        apdu[4] = (byte) len;
        System.arraycopy(image, offset, apdu, 5, len);
        return d.transceive(apdu);
    }

    /** 整幅图分包下载，返回失败信息（成功返回 null） */
    public static String loadImage(IsoDep d, int imgIndex, byte[] image, Log log) {
        return loadImage(d, imgIndex, image, log, null);
    }

    /** 分包进度回调（跑在调用线程上，别在里面碰控件） */
    public interface Progress {
        void onPacket(int done, int total);
    }

    /** 整幅图分包下载，带回进度；返回失败信息（成功返回 null） */
    public static String loadImage(IsoDep d, int imgIndex, byte[] image, Log log,
                                   Progress prog) {
        int total = image.length;
        int npkt = (total + PACKET_SIZE - 1) / PACKET_SIZE;
        for (int i = 0; i < npkt; i++) {
            int len = Math.min(PACKET_SIZE, total - i * PACKET_SIZE);
            try {
                byte[] r = loadImagePacket(d, imgIndex, i, image, i * PACKET_SIZE, len);
                String s = sw(r);
                if (log != null) {
                    log.line(String.format("D2 包 %d/%d len=%d -> %s %s",
                            i + 1, npkt, len, s, swText(s)));
                }
                if (prog != null) prog.onPacket(i + 1, npkt);
                if (!isOk(r)) {
                    return "第 " + (i + 1) + " 包失败: " + s + " " + swText(s);
                }
            } catch (IOException e) {
                return "第 " + (i + 1) + " 包异常: " + e.getMessage();
            }
        }
        return null;
    }

    /**
     * Redraw Screen。
     * P1 = 0x05 -> Bit7=0 检测屏幕存在，Bit6~0=5 上电延时 5*100ms
     * P2 = 0x80 -> Bit7=1 立即返回，Bit6~0=0 图片索引 0
     */
    public static byte[] redraw(IsoDep d, int imgIndex, boolean immediateReturn)
            throws IOException {
        int p2 = (immediateReturn ? 0x80 : 0x00) | (imgIndex & 0x7F);
        return d.transceive(hex(String.format("F0D405%02X00", p2)));
    }

    /** Get EPD Busy Status：返回 1 字节，00=空闲(刷新完成)，01=忙 */
    public static byte[] getBusy(IsoDep d) throws IOException {
        return d.transceive(hex("F0DE000001"));
    }

    /* ------------------------------------------------------------------ */
    /* 显示模式                                                            */
    /* ------------------------------------------------------------------ */

    /**
     * 显示模式：
     *   0 = 时钟  —— 默认。每分钟到点自动重画表盘
     *   1 = 图片  —— 传上去的画面一直保留，时钟不再覆盖它
     *
     * APDU：F0 D5 P1 P2 Le
     *   F0 D5 00 00 01   切到时钟模式
     *   F0 D5 00 01 01   切到图片模式
     *   F0 D5 01 00 01   读当前模式
     * 应答：1 字节模式 + 90 00（和 F0 DE 同形状）
     */
    public static final int MODE_CLOCK = 0;
    public static final int MODE_IMAGE = 1;

    /** 切换显示模式，返回该模式的当前值（-1 = 没读到） */
    public static int setDisplayMode(IsoDep d, int mode) throws IOException {
        byte[] r = d.transceive(hex(String.format("F0D500%02X01", mode & 0xFF)));
        return modeOf(r);
    }

    /** 读取当前显示模式，返回 0 / 1（-1 = 没读到） */
    public static int getDisplayMode(IsoDep d) throws IOException {
        byte[] r = d.transceive(hex("F0D5010001"));
        return modeOf(r);
    }

    /** 从 D5 的应答里取模式字节 */
    public static int modeOf(byte[] resp) {
        if (resp == null || resp.length < 3) return -1;
        if ((resp[resp.length - 2] & 0xFF) != 0x90) return -1;
        return resp[0] & 0x01;
    }

    /** 模式 -> 中文 */
    public static String modeText(int m) {
        if (m == MODE_IMAGE) return "图片";
        if (m == MODE_CLOCK) return "时钟";
        return "未知";
    }

    /* ------------------------------------------------------------------ */
    /* 测试图案                                                            */
    /* ------------------------------------------------------------------ */

    /**
     * 生成 1bpp 测试图案：边框 + 32px 棋盘格。
     * 位序：MSB = 每行最左边的像素。
     *
     * @param invert true 时黑白反转（如果发现刷出来是反的，勾上这个）
     */
    public static byte[] makeTestPattern(boolean invert) {
        byte[] img = new byte[IMAGE_BYTES];
        for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                boolean on;
                boolean border = (x < 2 || x >= SCREEN_W - 2 || y < 2 || y >= SCREEN_H - 2);
                if (border) {
                    on = true;                              // 边框画黑
                } else {
                    on = (((x / 16) + (y / 16)) & 1) == 0;  // 16px 棋盘格
                }
                if (invert) on = !on;
                if (on) {
                    img[y * ROW_BYTES + (x >> 3)] |= (byte) (0x80 >> (x & 7));
                }
            }
        }
        return img;
    }

    /** 全白 */
    public static byte[] makeSolid(boolean white) {
        byte[] img = new byte[IMAGE_BYTES];
        if (white) java.util.Arrays.fill(img, (byte) 0xFF);
        return img;
    }

    /* ------------------------------------------------------------------ */
    /* 图片 -> 面板显存                                                     */
    /* ------------------------------------------------------------------ */

    /**
     * 4x4 Bayer 有序抖动矩阵。灰度阈值法是硬切的，照片会丢中间调；
     * 抖动用"网点"换来灰阶观感，照片/渐变色块好看很多。
     * （纯文字、条码、二维码请关掉抖动，硬阈值才清晰。）
     */
    private static final int[] BAYER4 = {
             0,  8,  2, 10,
            12,  4, 14,  6,
             3, 11,  1,  9,
            15,  7, 13,  5,
    };

    /**
     * 把任意尺寸的图片摆进 122x250 的画布。
     *
     * @param rotate90 先顺时针转 90°（竖图/横图对不上时用）
     * @param cropFill true  = 等比放大铺满，溢出部分裁掉（满屏无白边）
     *                 false = 等比缩小到完整可见，四周留白居中（推荐，不丢内容）
     * @return 122x250 的白底 Bitmap（调用方可直接 recycle）
     */
    public static Bitmap fitToPanel(Bitmap src, boolean rotate90, boolean cropFill) {
        if (src == null) return null;

        Bitmap work = src;
        if (rotate90) {
            Matrix m = new Matrix();
            m.postRotate(90);
            Bitmap r = Bitmap.createBitmap(src, 0, 0, src.getWidth(), src.getHeight(), m, true);
            if (r != null && r != src) work = r;
        }

        Bitmap out = Bitmap.createBitmap(SCREEN_W, SCREEN_H, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(out);
        c.drawColor(Color.WHITE);                       // 留白 = 白纸

        int sw = Math.max(1, work.getWidth());
        int sh = Math.max(1, work.getHeight());
        float scale = cropFill
                ? Math.max((float) SCREEN_W / sw, (float) SCREEN_H / sh)
                : Math.min((float) SCREEN_W / sw, (float) SCREEN_H / sh);
        float dw = sw * scale;
        float dh = sh * scale;
        float left = (SCREEN_W - dw) / 2f;
        float top = (SCREEN_H - dh) / 2f;

        Paint p = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        c.drawBitmap(work, null, new RectF(left, top, left + dw, top + dh), p);

        if (work != src) work.recycle();
        return out;
    }

    /**
     * 122x250 的 Bitmap -> 4000 字节面板数据（可直接喂给 D2 Load Image）。
     *
     * 位序与极性已按实测锁定，和 SSD1680 面板显示一致：
     *   - 每行 16 字节（122 像素右对齐补白），MSB = 每行最左像素
     *   - **bit = 1 -> 白，bit = 0 -> 黑（上墨）**
     *     （旁证：makeSolid(true) 全 0xFF 刷出白屏；BLE 传图路径也是暗像素清位。）
     *
     * @param panel     必须是 122x250 的 Bitmap（fitToPanel 的产物）
     * @param threshold 硬阈值 0~255，越小越"黑"（默认 128）
     * @param invert    黑白互换（万一实机刷出来相反就勾这个）
     * @param dither    用 4x4 Bayer 抖动代替硬阈值（照片/渐变用）
     */
    public static byte[] makeFromBitmap(Bitmap panel, int threshold,
                                        boolean invert, boolean dither) {
        byte[] img = new byte[IMAGE_BYTES];
        java.util.Arrays.fill(img, (byte) 0xFF);         // 底 = 白
        int[] row = new int[SCREEN_W];

        for (int y = 0; y < SCREEN_H; y++) {
            panel.getPixels(row, 0, SCREEN_W, 0, y, SCREEN_W, 1);
            int base = y * ROW_BYTES;
            for (int x = 0; x < SCREEN_W; x++) {
                int c = row[x];
                // 人眼感知灰度（整数近似 0.299/0.587/0.114，避免浮点）
                int gray = (Color.red(c) * 77 + Color.green(c) * 151 + Color.blue(c) * 28) >> 8;

                boolean white;
                if (dither) {
                    int b = BAYER4[(y & 3) * 4 + (x & 3)];
                    white = gray * 32 > (b * 2 + 1) * 255;
                } else {
                    white = gray >= threshold;
                }
                if (invert) white = !white;

                if (!white) {                            // 黑 = 清位（上墨）
                    img[base + (x >> 3)] &= (byte) ~(0x80 >> (x & 7));
                }
            }
        }
        return img;
    }

    /** 把 4000 字节面板数据还原成图，用于"所见即所得"预览 */
    public static Bitmap bytesToBitmap(byte[] img) {
        Bitmap bmp = Bitmap.createBitmap(SCREEN_W, SCREEN_H, Bitmap.Config.ARGB_8888);
        int[] px = new int[SCREEN_W * SCREEN_H];
        for (int y = 0; y < SCREEN_H; y++) {
            int base = y * ROW_BYTES;
            for (int x = 0; x < SCREEN_W; x++) {
                boolean white = ((img[base + (x >> 3)] >> (7 - (x & 7))) & 1) != 0;
                px[y * SCREEN_W + x] = white ? 0xFFFFFFFF : 0xFF000000;
            }
        }
        bmp.setPixels(px, 0, SCREEN_W, 0, 0, SCREEN_W, SCREEN_H);
        return bmp;
    }

    /** 简单日志接口，避免 Activity 引用 */
    public interface Log {
        void line(String s);
    }
}
