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

    private  int frame_index_black = 0;
    public  final static int FRAME_DATA_SIZE = 16;  // 1 frame = 1 row = 16 bytes (13 data + 3 × 0xFF)
    private  final static int TOTAL_FRAME_COUNT = HEIGHT;  // 212 frames


    private  String errString;

    public final static byte EPD_CMD_PING = 0x10;
    public final static byte EPD_CMD_INIT = 0x11;
    public final static byte EPD_CMD_DEINIT = 0x12;

    public final static byte EPD_CMD_PREPARE_BLK_RAM = 0x13;
    public final static byte EPD_CMD_WRITE_BLK_RAM = 0x14;
    public final static byte EPD_CMD_GET_BLK_RAM_CRC = 0x15;
    public final static byte EPD_CMD_PREPARE_RED_RAM = 0x18;
    public final static byte EPD_CMD_WRITE_RED_RAM = 0x19;
    public final static byte EPD_CMD_GET_RED_RAM_CRC = 0x20;

    public final static byte EPD_CMD_UPDATE_DISPLAY = 0x21;  // ask epd to show ram data

    public final static byte EPD_CMD_READ_VERSION = 0x22;

    public final static byte EPD_CMD_LED_TEST = 0x40;      // LED test: blue->red->green->yellow->cyan->purple->white, 2s each
    public final static byte EPD_CMD_LED_TOGGLE = 0x41;    // LED on/off toggle

    public final static byte EPD_CMD_SET_TIME = 0x50;      // Set time: year_hi, year_lo, month, day, hour, minute, second
    public final static byte EPD_CMD_GET_TIME = 0x51;       // Get time: returns 8 bytes

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

            // 逐像素写入 black_buf (16字节/行对齐)
            for (int y = 0; y < HEIGHT; y++) {
                int rowOffset = y * RAM_ROW_SIZE;
                for (int x = 0; x < WIDTH; x++) {
                    int pixel = scaled.getPixel(x, y);
                    int gray = (int)(Color.red(pixel) * 0.299 + Color.green(pixel) * 0.587 + Color.blue(pixel) * 0.114);
                    if (gray < 128) {
                        int byteIdx = rowOffset + (x >> 3);
                        int bitIdx = 7 - (x & 7);
                        black_buf[byteIdx] &= ~(1 << bitIdx);
                    }
                }
                // 每行末尾3字节已是0xFF, 无需额外操作
            }
            scaled.recycle();

            Log.i(TAG, "image converted to " + black_buf.length + " bytes");
            return true;
        } catch (IOException e) {
            e.printStackTrace();
            errString = "ioException reading URI";
            return false;
        }
    }

    public void start() {
        frame_index_black = 0;
    }

    public boolean isDataFinish() {
        return frame_index_black >= TOTAL_FRAME_COUNT;
    }

    public int getProgress() {
        if (frame_index_black >= TOTAL_FRAME_COUNT) return 100;
        return (frame_index_black * 100 / TOTAL_FRAME_COUNT);
    }

    public byte[] getNextFrame() {
        byte[] ret = new byte[FRAME_DATA_SIZE];
        System.arraycopy(black_buf, FRAME_DATA_SIZE * frame_index_black, ret, 0, FRAME_DATA_SIZE);
        frame_index_black++;
        return ret;
    }

    private static String bytesToHexString(byte[] src) {
        StringBuilder stringBuilder = new StringBuilder("");
        if (src == null || src.length <= 0) {
            return null;
        }
        for (int i = 0; i < src.length; i++) {
            int v = src[i] & 0xFF;
            String hv = Integer.toHexString(v);
            if (hv.length() < 2) {
                stringBuilder.append(0);
            }
            stringBuilder.append(hv);
            stringBuilder.append(' ');
        }
        return stringBuilder.toString();
    }

}
