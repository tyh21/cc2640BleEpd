package com.nordicsemi.nrfUARTv2;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;

public class EpdDownloader {
    private static String TAG = "EpdDownloader";
    //private final static int STATE_ = 1;
    private  final  static int WIDTH = 104;
    private  final static int HEIGHT = 212;
    private  final static int BYTES_PER_ROW = WIDTH / 8;  // 13 bytes per row
    private  final static int RAM_ROW_SIZE = 16;          // SSD1680 RAM row width = 16 bytes

    // buf of data to send: 212 rows × 16 bytes = 3392 bytes (13 data + 3 padding per row)
    private  byte[] black_buf  = new byte[HEIGHT * RAM_ROW_SIZE];

    public  final static int FRAME_DATA_SIZE = 16;  // 1 frame = 1 row = 16 bytes (13 data + 3 × 0xFF)

    private  String errString;

    public final static byte EPD_CMD_PING = 0x10;
    public final static byte EPD_CMD_INIT = 0x11;
    public final static byte EPD_CMD_DEINIT = 0x12;

    public final static byte EPD_CMD_PREPARE_BLK_RAM = 0x13;
    public final static byte EPD_CMD_WRITE_BLK_RAM = 0x14;
    public final static byte EPD_CMD_GET_BLK_RAM_CRC = 0x15;
    public final static byte EPD_CMD_WRITE_BLK_RAM_BULK = 0x16;  // 多行块写: [0x16][off_hi][off_lo][data...]
    public final static byte EPD_CMD_PREPARE_RED_RAM = 0x18;
    public final static byte EPD_CMD_WRITE_RED_RAM = 0x19;
    public final static byte EPD_CMD_GET_RED_RAM_CRC = 0x20;

    public final static byte EPD_CMD_UPDATE_DISPLAY = 0x21;  // ask epd to show ram data

    public final static byte EPD_CMD_READ_VERSION = 0x22;

    public final static byte EPD_CMD_LED_TEST = 0x40;      // LED test: blue->red->green->yellow->cyan->purple->white, 2s each
    public final static byte EPD_CMD_LED_TOGGLE = 0x41;    // LED on/off toggle

    public final static byte EPD_CMD_SET_TIME = 0x50;      // Set time: year_hi, year_lo, month, day, hour, minute, second
    public final static byte EPD_CMD_GET_TIME = 0x51;       // Get time: returns 8 bytes

    public final static byte EPD_CMD_SWITCH_FACE = 0x60;   // Switch clock face: returns [0x60, face] (0=dot-matrix, 1=7-seg LED)

    // Display mode.  0 = clock (the calendar, repainted every minute),
    // 1 = image (whatever was pushed stays on screen; the clock stops
    // overwriting it).  The firmware switches to image mode by itself after
    // every successful image push, so the only way back to the calendar is
    // an explicit SET_MODE 0 - which is what the buttons next to "Sync Time"
    // and the NFC page's "back to clock" button send.
    public final static byte EPD_CMD_SET_MODE = 0x70;       // data[0] = 0 clock / 1 image
    public final static byte EPD_CMD_GET_MODE = 0x71;       // returns: 0x71 <mode>

    public final static byte EPD_MODE_CLOCK = 0;
    public final static byte EPD_MODE_IMAGE = 1;

    public EpdDownloader() {
        for (int i = 0; i < black_buf.length; i++) {
            black_buf[i] = (byte) 0xff;
        }
    }

    public String getErrString() {
        return  errString;
    }

    public boolean loadFile(String filePath) {
        Log.i(TAG, "loading file: " + filePath);
        File file = new File(filePath);
        try {
            InputStream inputStream = new FileInputStream(file);
            byte[] raw = new byte[WIDTH / 8 * HEIGHT];
            inputStream.read(raw, 0, WIDTH / 8 * HEIGHT);
            inputStream.close();
            // 按16字节/行重新打包: 13字节数据 + 3字节0xFF
            for (int y = 0; y < HEIGHT; y++) {
                int srcOff = y * BYTES_PER_ROW;
                int dstOff = y * RAM_ROW_SIZE;
                System.arraycopy(raw, srcOff, black_buf, dstOff, BYTES_PER_ROW);
                black_buf[dstOff + 13] = (byte) 0xFF;
                black_buf[dstOff + 14] = (byte) 0xFF;
                black_buf[dstOff + 15] = (byte) 0xFF;
            }
            return true;
        } catch (FileNotFoundException e) {
            e.printStackTrace();
            errString = "file not found";
            return false;
        } catch (IOException e) {
            e.printStackTrace();
            errString = "ioException";
            return false;
        }
    }

    public boolean loadFromUri(InputStream inputStream) {
        Log.i(TAG, "loading from URI stream");
        try {
            if (inputStream == null) {
                errString = "input stream is null";
                return false;
            }

            Bitmap original = BitmapFactory.decodeStream(inputStream);
            inputStream.close();

            if (original == null) {
                errString = "cannot decode image";
                return false;
            }

            Log.i(TAG, "original image size: " + original.getWidth() + "x" + original.getHeight());

            Bitmap scaled = Bitmap.createScaledBitmap(original, WIDTH, HEIGHT, true);
            if (original != scaled) original.recycle();

            // 重置buffer为全0xFF
            for (int i = 0; i < black_buf.length; i++) black_buf[i] = (byte) 0xFF;

            // 一次性取回整幅像素 (比逐点 getPixel 快几十倍), 再打包成 16 字节/行
            int[] pixels = new int[WIDTH * HEIGHT];
            scaled.getPixels(pixels, 0, WIDTH, 0, 0, WIDTH, HEIGHT);
            scaled.recycle();

            for (int y = 0; y < HEIGHT; y++) {
                int rowOffset = y * RAM_ROW_SIZE;
                int p = y * WIDTH;
                for (int x = 0; x < WIDTH; x++) {
                    int pixel = pixels[p + x];
                    int gray = (int)(Color.red(pixel) * 0.299 + Color.green(pixel) * 0.587 + Color.blue(pixel) * 0.114);
                    if (gray < 128) {
                        black_buf[rowOffset + (x >> 3)] &= ~(1 << (7 - (x & 7)));
                    }
                }
                // 每行末尾3字节已是0xFF, 无需额外操作
            }

            Log.i(TAG, "image converted to " + black_buf.length + " bytes");
            return true;
        } catch (IOException e) {
            e.printStackTrace();
            errString = "ioException reading URI";
            return false;
        }
    }

    // ---- 快速路径: 多行块写 (0x16) ----
    // 一次发送 chunkSize 字节(可跨多行), 用 2 字节大端偏移定位到 4000B RAM 窗口。
    // 相比"每行一帧"(212 次往返), 现在约 27 次, 传图快 8 倍。
    private int byteOffset = 0;
    private int chunkSize = 120;   // 会被 setChunkSize() 按协商 MTU 调整

    public void setChunkSize(int n) {
        if (n < 16) n = 16;
        if (n > 128) n = 128;      // 固件 RX 帧缓冲 160B (3 头 + 最多 157 数据)
        chunkSize = n;
    }

    public void start() {
        byteOffset = 0;
    }

    public boolean isDataFinish() {
        return byteOffset >= black_buf.length;
    }

    public int getProgress() {
        if (byteOffset >= black_buf.length) return 100;
        return (byteOffset * 100 / black_buf.length);
    }

    /** 组装下一个块: [0x16][off_hi][off_lo][data...], 并推进偏移 */
    public byte[] buildNextBulkFrame() {
        if (isDataFinish()) return null;
        int len = Math.min(chunkSize, black_buf.length - byteOffset);
        byte[] f = new byte[3 + len];
        f[0] = EPD_CMD_WRITE_BLK_RAM_BULK;
        f[1] = (byte) ((byteOffset >> 8) & 0xFF);
        f[2] = (byte) (byteOffset & 0xFF);
        System.arraycopy(black_buf, byteOffset, f, 3, len);
        byteOffset += len;
        return f;
    }

    /** 整图 CRC-16/CCITT-FALSE, 与固件 s_blk_crc 使用同一算法 */
    public int computeBlkCrc() {
        int crc = 0xFFFF;
        for (int i = 0; i < black_buf.length; i++) {
            crc ^= ((black_buf[i] & 0xFF) << 8);
            for (int b = 0; b < 8; b++) {
                if ((crc & 0x8000) != 0) crc = ((crc << 1) ^ 0x1021) & 0xFFFF;
                else crc = (crc << 1) & 0xFFFF;
            }
        }
        return crc & 0xFFFF;
    }

}
