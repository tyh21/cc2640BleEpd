package com.nordicsemi.nrfUARTv2;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.nfc.NfcAdapter;
import android.nfc.Tag;
import android.nfc.TagLostException;
import android.nfc.tech.IsoDep;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.io.IOException;
import java.io.InputStream;

/**
 * NFC 传图 / 刷屏 —— ESL 价签（FM11NC08S + 2.13" 122x250 黑白）。
 *
 * 主线流程（不需要蓝牙）：
 *   选相册图片 -> 等比/铺满摆进 122x250 -> 阈值或抖动二值化 -> 所见即所得预览
 *   -> 贴卡 -> PIN 认证 -> D2 分包下载 16 包 x 250B -> D4 刷新 -> DE 查忙。
 *
 * 说明：
 *   1. 一次贴卡只能走完一整轮会话，中途手机移开会 Tag was lost，需要重贴。
 *   2. 刷屏时屏幕必须常亮，息屏会掐断 NFC（已加 FLAG_KEEP_SCREEN_ON）。
 *   3. 位序/极性已按实测锁定：bit=1 -> 白，bit=0 -> 黑，MSB 在每行最左。
 *      若哪天换屏刷出来黑白相反，勾「反色」即可，不用改代码。
 *   4. 诊断/实验类工具全部收在「高级 / 调试」里，日常用不到。
 */
public class NfcEslActivity extends Activity implements NfcAdapter.ReaderCallback {

    private NfcAdapter nfcAdapter;
    private TextView statusText;
    private TextView logText;
    private TextView infoText;

    private ImageView preview;
    private CheckBox chkRotate;
    private CheckBox chkInvert;
    private CheckBox chkDither;
    private CheckBox chkFill;
    private SeekBar seek;
    private TextView seekLabel;

    private Button btnPick;
    private Button btnFlash;
    private Button btnClockMode;
    private TextView modeText;
    private Button btnAdvanced;
    private LinearLayout advancedBox;

    private Button btnProbe;
    private Button btnResetProbe;
    private Button btnPattern;
    private Button btnClear;
    private Button btnBusy;
    private Button btnBSelect;
    private Button btnClearLog;
    private Button btnCopyLog;
    private Button btnShareLog;

    private final Handler ui = new Handler(Looper.getMainLooper());

    /* ---- 动作 ---- */
    private static final int ACTION_IMAGE = 0;
    private static final int ACTION_PATTERN = 1;
    private static final int ACTION_WHITE = 2;
    private static final int ACTION_BUSY = 3;
    private static final int ACTION_BSELECT = 4;
    private static final int ACTION_PROBE = 5;
    /* 贴卡切回时钟模式（刷图成功后固件会自动锁在图片模式，所以需要一个出口） */
    private static final int ACTION_CLOCK = 6;

    /** 当前动作，粘性：贴卡就执行它 */
    private volatile int currentAction = ACTION_IMAGE;

    /* ---- UI 值缓存成 volatile：onTagDiscovered 不在主线程 ---- */
    private volatile boolean autoRun = true;
    private volatile boolean invert = false;
    private volatile boolean rotate90 = false;
    private volatile boolean dither = false;
    private volatile boolean cropFill = false;
    private volatile int threshold = 128;

    /** 二值化后的面板数据（4000B），贴卡时直接下发 */
    private volatile byte[] imageBytes = null;
    /** 相册原图（采样解码后、未旋转），改选项时重新摆进画布用 */
    private Bitmap srcBitmap = null;
    /** 当前摆进 122x250 画布的位图（预览与二值化的输入） */
    private Bitmap panelBitmap = null;
    private String imageName = "";

    private volatile boolean busy = false;

    private static final int MAX_RETRY = 5;
    private volatile int retryLeft = MAX_RETRY;

    private static final int REQUEST_PICK = 1001;

    /* ---- 诊断探针 ---- */
    private static final String[] PROBE_NAMES = {
            "F0 DE 00 00 01  查屏幕忙状态（最短的 F0 命令）",
            "00 20 00 01 05 1122334455  Verify PIN",
            "00 A4 04 00 07 D2760000850101  SELECT NDEF App",
            "00 A4 00 0C 02 E104  SELECT CC File",
            "00 B0 00 00 0F  READ BINARY 15B",
            "00 CA 00 00 00  GET DATA / UID (ISO)",
            "FF CA 00 00 00  GET UID (PC/SC)",
            "F0 DA 00 00 03 70 00 20  Set Screen Type",
    };
    private static final String[] PROBE_APDUS = {
            "F0DE000001",
            "00200001051122334455",
            "00A4040007D276000085010100",
            "00A4000C02E104",
            "00B000000F",
            "00CA000000",
            "FFCA000000",
            "F0DA000003700020",
    };
    private volatile int probeIndex = 0;

    /** 同一张卡短时间内只处理一次 */
    private String lastUid = null;
    private long lastUidAt = 0L;

    private int hbSec = 0;
    private final Runnable heartbeat = new Runnable() {
        @Override public void run() {
            hbSec++;
            if (!busy) setStatus("★ 监听中 " + hbSec + " 秒：" + actionHint());
            ui.postDelayed(this, 1000);
        }
    };

    private final EslNfcWriter.Log logger = new EslNfcWriter.Log() {
        @Override public void line(final String s) { appendLog(s); }
    };

    /* ------------------------------------------------------------------ */
    /* 界面                                                                */
    /* ------------------------------------------------------------------ */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // 刷屏要 4~6 秒，息屏会直接掐断 NFC
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        final float d = getResources().getDisplayMetrics().density;
        int pad = (int) (12 * d);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        statusText = new TextView(this);
        statusText.setTextSize(16f);
        statusText.setText("初始化中...");
        root.addView(statusText, lp());

        /* ---------- 上半：操作区（可滚动） ---------- */
        LinearLayout main = new LinearLayout(this);
        main.setOrientation(LinearLayout.VERTICAL);

        preview = new ImageView(this);
        preview.setBackgroundColor(0xFFDDDDDD);
        preview.setScaleType(ImageView.ScaleType.FIT_CENTER);
        preview.setAdjustViewBounds(false);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, (int) (170 * d));
        plp.topMargin = pad;
        main.addView(preview, plp);

        btnPick = new Button(this);
        btnPick.setText("1. 选择图片（相册）");
        btnPick.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pickImage(); }
        });
        LinearLayout.LayoutParams blp = lp();
        blp.topMargin = (int) (8 * d);
        main.addView(btnPick, blp);

        infoText = new TextView(this);
        infoText.setTextSize(12f);
        infoText.setText("还没有选图片");
        main.addView(infoText, lp());

        /* 选项行 */
        LinearLayout optRow = new LinearLayout(this);
        optRow.setOrientation(LinearLayout.HORIZONTAL);
        chkRotate = mkCheck("旋转90°", false, new Runnable() {
            @Override public void run() { rotate90 = !rotate90; rebuild(); }
        });
        chkInvert = mkCheck("反色", false, new Runnable() {
            @Override public void run() { invert = !invert; rebuild(); }
        });
        chkDither = mkCheck("抖动", false, new Runnable() {
            @Override public void run() { dither = !dither; updateSeekEnabled(); rebuild(); }
        });
        optRow.addView(chkRotate, weight());
        optRow.addView(chkInvert, weight());
        optRow.addView(chkDither, weight());
        main.addView(optRow, lp());

        chkFill = mkCheck("铺满裁切（取消 = 完整显示留白）", false, new Runnable() {
            @Override public void run() { cropFill = !cropFill; rebuild(); }
        });
        main.addView(chkFill, lp());

        seekLabel = new TextView(this);
        seekLabel.setTextSize(12f);
        seekLabel.setText("黑白分界（阈值）：128");
        main.addView(seekLabel, lp());

        seek = new SeekBar(this);
        seek.setMax(255);
        seek.setProgress(threshold);
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int p, boolean fromUser) {
                threshold = p;
                seekLabel.setText("黑白分界（阈值）：" + p
                        + (dither ? "　（抖动已开，阈值不生效）" : ""));
            }
            @Override public void onStartTrackingTouch(SeekBar sb) { }
            @Override public void onStopTrackingTouch(SeekBar sb) { rebuild(); }
        });
        main.addView(seek, lp());

        btnFlash = new Button(this);
        btnFlash.setTextSize(17f);
        btnFlash.setText("2. 贴卡刷屏（把手机贴到价签天线上）");
        btnFlash.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                currentAction = ACTION_IMAGE;
                setStatus("已选：传图片。" + actionHint());
                hbSec = 0;
            }
        });
        LinearLayout.LayoutParams flp = lp();
        flp.topMargin = (int) (10 * d);
        main.addView(btnFlash, flp);

        /* 显示模式：刷图成功后会一直停在图片模式（时钟不再覆盖），
         * 这里给一个"切回时钟"的出口。贴卡才发命令，和刷屏同一个用法。 */
        modeText = new TextView(this);
        modeText.setTextSize(12f);
        modeText.setText("显示模式：刷图成功 = 图片模式（画面一直保留）");
        main.addView(modeText, lp());

        btnClockMode = new Button(this);
        btnClockMode.setText("切回时钟模式（贴卡执行）");
        btnClockMode.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                currentAction = ACTION_CLOCK;
                setStatus("已选：切回时钟模式。" + actionHint());
                hbSec = 0;
            }
        });
        LinearLayout.LayoutParams clp = lp();
        clp.topMargin = (int) (6 * d);
        main.addView(btnClockMode, clp);

        btnAdvanced = new Button(this);
        btnAdvanced.setText("高级 / 调试 ▾");
        btnAdvanced.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                boolean show = advancedBox.getVisibility() != View.VISIBLE;
                advancedBox.setVisibility(show ? View.VISIBLE : View.GONE);
                btnAdvanced.setText(show ? "高级 / 调试 ▴" : "高级 / 调试 ▾");
            }
        });
        main.addView(btnAdvanced, lp());

        /* ---------- 高级 / 调试区 ---------- */
        advancedBox = new LinearLayout(this);
        advancedBox.setOrientation(LinearLayout.VERTICAL);
        advancedBox.setVisibility(View.GONE);

        CheckBox autoBox = new CheckBox(this);
        autoBox.setText("贴卡自动执行（取消勾选则只打印诊断信息）");
        autoBox.setChecked(true);
        autoBox.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
            @Override public void onCheckedChanged(android.widget.CompoundButton b, boolean c) {
                autoRun = c;
            }
        });
        advancedBox.addView(autoBox);

        btnProbe = new Button(this);
        btnProbe.setText("【诊断】一次贴卡跑一条，自动轮完 8 条");
        btnProbe.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                currentAction = ACTION_PROBE;
                setStatus("已选：诊断。贴住不要动，约每 1.5 秒自动试下一条");
            }
        });
        advancedBox.addView(btnProbe);

        btnResetProbe = new Button(this);
        btnResetProbe.setText("重置诊断进度（回到第 1 条）");
        btnResetProbe.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                probeIndex = 0;
                setStatus("诊断进度已重置到第 1 条");
            }
        });
        advancedBox.addView(btnResetProbe);

        btnBSelect = new Button(this);
        btnBSelect.setText("【实验B】固定只发 SELECT NDEF App");
        btnBSelect.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                currentAction = ACTION_BSELECT;
                setStatus("已选：实验B。每次贴卡都发同一条 A4 04 00");
            }
        });
        advancedBox.addView(btnBSelect);

        btnPattern = new Button(this);
        btnPattern.setText("刷测试图案（棋盘格 + 边框）");
        btnPattern.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                currentAction = ACTION_PATTERN;
                setStatus("已选：测试图案。" + actionHint());
            }
        });
        advancedBox.addView(btnPattern);

        btnClear = new Button(this);
        btnClear.setText("刷全白");
        btnClear.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                currentAction = ACTION_WHITE;
                setStatus("已选：全白。" + actionHint());
            }
        });
        advancedBox.addView(btnClear);

        btnBusy = new Button(this);
        btnBusy.setText("只查询屏幕忙状态（DE）");
        btnBusy.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                currentAction = ACTION_BUSY;
                setStatus("已选：查询忙状态。" + actionHint());
            }
        });
        advancedBox.addView(btnBusy);

        main.addView(advancedBox, lp());

        ScrollView mainScroll = new ScrollView(this);
        mainScroll.addView(main);
        root.addView(mainScroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 3f));

        /* ---------- 下半：日志 ---------- */
        logText = new TextView(this);
        logText.setTextSize(11f);
        logText.setSingleLine(false);
        logText.setTextIsSelectable(true);
        ScrollView logScroll = new ScrollView(this);
        logScroll.addView(logText);
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 2f);
        llp.topMargin = (int) (8 * d);
        root.addView(logScroll, llp);

        LinearLayout logBtnRow = new LinearLayout(this);
        logBtnRow.setOrientation(LinearLayout.HORIZONTAL);

        btnClearLog = new Button(this);
        btnClearLog.setText("清空");
        btnClearLog.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { logText.setText(""); }
        });
        logBtnRow.addView(btnClearLog, weight());

        btnCopyLog = new Button(this);
        btnCopyLog.setText("复制日志");
        btnCopyLog.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                String s = logText.getText().toString();
                ClipboardManager cm =
                        (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
                if (cm != null) {
                    cm.setPrimaryClip(ClipData.newPlainText("nfc_log", s));
                    setStatus("日志已复制到剪贴板（" + s.length() + " 字）");
                }
            }
        });
        logBtnRow.addView(btnCopyLog, weight());

        btnShareLog = new Button(this);
        btnShareLog.setText("分享日志");
        btnShareLog.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                Intent send = new Intent(Intent.ACTION_SEND);
                send.setType("text/plain");
                send.putExtra(Intent.EXTRA_SUBJECT, "NFC 刷屏日志");
                send.putExtra(Intent.EXTRA_TEXT, logText.getText().toString());
                startActivity(Intent.createChooser(send, "分享日志"));
            }
        });
        logBtnRow.addView(btnShareLog, weight());

        root.addView(logBtnRow, lp());

        setContentView(root);

        nfcAdapter = NfcAdapter.getDefaultAdapter(this);
        if (nfcAdapter == null) {
            setStatus("这台设备没有 NFC 硬件");
            setButtonsEnabled(false);
        } else {
            appendLog("NFC 传图  v3.2 (versionCode 46)");
            appendLog("NFC 适配器存在，是否开启：" + nfcAdapter.isEnabled());
        }
        updateSeekEnabled();
    }

    /* ---------- 小布局工具 ---------- */

    private LinearLayout.LayoutParams lp() {
        return new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams weight() {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
    }

    /** 造一个 CheckBox：内部自己更新 volatile 字段，再把后续处理交给回调 */
    private CheckBox mkCheck(String text, boolean checked, final Runnable after) {
        final CheckBox c = new CheckBox(this);
        c.setText(text);
        c.setTextSize(13f);
        c.setChecked(checked);
        c.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
            @Override public void onCheckedChanged(android.widget.CompoundButton b, boolean is) {
                after.run();
            }
        });
        return c;
    }

    /* ------------------------------------------------------------------ */
    /* 图片：选择 / 转换 / 预览                                             */
    /* ------------------------------------------------------------------ */

    private void pickImage() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("image/*");
        try {
            startActivityForResult(i, REQUEST_PICK);
        } catch (Exception e) {
            // 个别 ROM 没有 DocumentsUI，退回到旧的选择器
            Intent i2 = new Intent(Intent.ACTION_GET_CONTENT);
            i2.setType("image/*");
            startActivityForResult(i2, REQUEST_PICK);
        }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQUEST_PICK || res != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        imageName = queryName(uri);
        try {
            Bitmap src = decodeSampled(uri, 1600);
            if (src == null) {
                setStatus("这个文件解不出来（不是图片？）");
                return;
            }
            if (srcBitmap != null && !srcBitmap.isRecycled()) srcBitmap.recycle();
            srcBitmap = src;
            rebuild();
            appendLog("已选图：" + imageName + "  " + src.getWidth() + "x" + src.getHeight()
                    + " -> 摆进 122x250");
        } catch (Throwable t) {
            setStatus("读图片失败：" + t.getMessage());
            appendLog("读图片异常 " + t.getClass().getSimpleName() + ": " + t.getMessage());
        }
    }

    /** 按目标上限做采样解码，避免大图 OOM */
    private Bitmap decodeSampled(Uri uri, int maxSide) throws IOException {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        InputStream in1 = getContentResolver().openInputStream(uri);
        BitmapFactory.decodeStream(in1, null, o);
        if (in1 != null) in1.close();

        int sample = 1;
        int big = Math.max(o.outWidth, o.outHeight);
        while (big / sample > maxSide) sample *= 2;

        BitmapFactory.Options o2 = new BitmapFactory.Options();
        o2.inSampleSize = sample;
        o2.inPreferredConfig = Bitmap.Config.ARGB_8888;
        InputStream in2 = getContentResolver().openInputStream(uri);
        Bitmap bmp = BitmapFactory.decodeStream(in2, null, o2);
        if (in2 != null) in2.close();
        return bmp;
    }

    private String queryName(Uri uri) {
        Cursor c = null;
        try {
            c = getContentResolver().query(uri, new String[]{ OpenableColumns.DISPLAY_NAME },
                    null, null, null);
            if (c != null && c.moveToFirst()) {
                int i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (i >= 0) {
                    String n = c.getString(i);
                    if (n != null && n.length() > 0) return n;
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (c != null) c.close();
        }
        String s = uri.getLastPathSegment();
        return s == null ? "(未知文件)" : s;
    }

    /** 选项变了就重新摆图 + 二值化 + 刷新预览（每次从原图重来，旋转/铺满才生效） */
    private void rebuild() {
        if (srcBitmap == null || srcBitmap.isRecycled()) return;
        try {
            Bitmap fitted = EslNfcWriter.fitToPanel(srcBitmap, rotate90, cropFill);
            if (fitted == null) return;
            if (panelBitmap != null && !panelBitmap.isRecycled()) panelBitmap.recycle();
            panelBitmap = fitted;
        } catch (Throwable t) {
            appendLog("摆图失败 " + t.getClass().getSimpleName() + ": " + t.getMessage());
            return;
        }
        byte[] img = EslNfcWriter.makeFromBitmap(panelBitmap, threshold, invert, dither);
        imageBytes = img;
        Bitmap show = EslNfcWriter.bytesToBitmap(img);
        preview.setImageBitmap(show);
        infoText.setText(imageName + "　122x250　4000B = 16 包"
                + "　阈值" + threshold + (dither ? " +抖动" : "")
                + (invert ? "　反色" : "") + (cropFill ? "　铺满" : "　留白")
                + (rotate90 ? "　转了90°" : ""));
        if (currentAction == ACTION_IMAGE) {
            setStatus("图片就绪 —— " + actionHint());
        }
    }

    private void updateSeekEnabled() {
        boolean en = !dither;
        seek.setEnabled(en);
        seekLabel.setText("黑白分界（阈值）：" + threshold
                + (dither ? "　（抖动已开，阈值不生效）" : ""));
    }

    /* ------------------------------------------------------------------ */

    private String actionHint() {
        if (currentAction == ACTION_IMAGE) {
            return imageBytes == null
                    ? "还没选图片，请先点「选择图片」"
                    : "把手机贴到价签天线上即可刷屏";
        }
        if (currentAction == ACTION_PATTERN) return "贴卡刷测试图案";
        if (currentAction == ACTION_WHITE) return "贴卡刷全白";
        if (currentAction == ACTION_CLOCK) return "贴卡切回时钟模式（画面立刻重画表盘）";
        if (currentAction == ACTION_BUSY) return "贴卡查屏幕忙状态";
        if (currentAction == ACTION_BSELECT) return "贴卡发 SELECT NDEF App";
        if (currentAction == ACTION_PROBE) return "贴卡跑诊断探针";
        return "把手机贴到价签天线上";
    }

    @Override
    protected void onResume() {
        super.onResume();
        startReaderMode();
        handleTagIntent(getIntent());
    }

    @Override
    protected void onPause() {
        super.onPause();
        stopReaderMode();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (panelBitmap != null && !panelBitmap.isRecycled()) {
            panelBitmap.recycle();
            panelBitmap = null;
        }
        if (srcBitmap != null && !srcBitmap.isRecycled()) {
            srcBitmap.recycle();
            srcBitmap = null;
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleTagIntent(intent);
    }

    /* ------------------------------------------------------------------ */
    /* Reader mode                                                         */
    /* ------------------------------------------------------------------ */

    private void startReaderMode() {
        if (nfcAdapter == null) return;
        if (!nfcAdapter.isEnabled()) {
            setStatus("NFC 未开启，请在系统设置里打开后回到本页");
            setButtonsEnabled(false);
            return;
        }
        // ★ FLAG_READER_SKIP_NDEF_CHECK 必须加：
        // 不加它平台会在回调前先跑一遍 NDEF 检查（等于替我们发一轮 APDU），
        // 芯片在应用层不应答 -> 检查失败 -> 标签派发被放弃 -> onTagDiscovered 永不触发。
        int flags = NfcAdapter.FLAG_READER_NFC_A
                | NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK
                | NfcAdapter.FLAG_READER_NO_PLATFORM_SOUNDS;
        try {
            nfcAdapter.enableReaderMode(this, this, flags, null);
            hbSec = 0;
            ui.removeCallbacks(heartbeat);
            ui.post(heartbeat);
            appendLog("ReaderMode 已开启 (flags=0x" + Integer.toHexString(flags) + ")");
            setStatus("★ 监听中：" + actionHint());
            setButtonsEnabled(true);
        } catch (Exception e) {
            setStatus("开启 NFC 读取失败：" + e.getMessage());
            appendLog("enableReaderMode 异常 " + e.getClass().getSimpleName() + ": " + e.getMessage());
        }
    }

    private void stopReaderMode() {
        ui.removeCallbacks(heartbeat);
        if (nfcAdapter == null) return;
        try { nfcAdapter.disableReaderMode(this); } catch (Exception ignored) { }
    }

    /** reader mode 回调，跑在 NFC binder 线程上，别在这里做重活 */
    @Override
    public void onTagDiscovered(Tag tag) {
        if (tag == null) return;
        appendLog("──── 发现标签 ────");
        String uid = EslNfcWriter.toHex(tag.getId());
        appendLog("UID=" + uid);
        String[] techs = tag.getTechList();
        StringBuilder sb = new StringBuilder();
        boolean hasIsoDep = false;
        for (int i = 0; i < techs.length; i++) {
            if (i > 0) sb.append(", ");
            sb.append(techs[i].replace("android.nfc.tech.", ""));
            if ("android.nfc.tech.IsoDep".equals(techs[i])) hasIsoDep = true;
        }
        appendLog("技术=" + sb);
        if (!hasIsoDep) {
            appendLog("★ 没有 IsoDep —— 这张卡连 ISO14443-4 都没激活，无法发 APDU");
        }
        handleTag(tag);
    }

    /** 处理 intent 里带的标签（App 未打开时贴卡被拉起） */
    private void handleTagIntent(Intent intent) {
        if (intent == null) return;
        String action = intent.getAction();
        if (!NfcAdapter.ACTION_TAG_DISCOVERED.equals(action)
                && !NfcAdapter.ACTION_TECH_DISCOVERED.equals(action)
                && !NfcAdapter.ACTION_NDEF_DISCOVERED.equals(action)) {
            return;   // 普通启动，没有标签要处理
        }
        Tag tag = getTagFromIntent(intent);
        if (tag == null) return;
        intent.removeExtra(NfcAdapter.EXTRA_TAG);
        appendLog("（由贴卡 intent 拉起）UID=" + EslNfcWriter.toHex(tag.getId()));
        handleTag(tag);
    }

    @SuppressWarnings("deprecation")
    private static Tag getTagFromIntent(Intent intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return intent.getParcelableExtra(NfcAdapter.EXTRA_TAG, Tag.class);
        }
        return intent.getParcelableExtra(NfcAdapter.EXTRA_TAG);
    }

    /** 统一入口：去重 + busy 保护 + 自动/手动分流 */
    private void handleTag(final Tag tag) {
        if (busy) {
            appendLog("（上一次操作还没结束，忽略这张卡）");
            return;
        }
        if (isDuplicate(tag)) return;

        if (!autoRun) {
            appendLog("（已关闭自动执行，仅显示诊断信息）");
            return;
        }
        if (currentAction == ACTION_IMAGE && imageBytes == null) {
            appendLog("★ 还没选图片 —— 先点「1. 选择图片（相册）」");
            setStatus("还没选图片，请先点「选择图片」");
            return;
        }
        runAction(tag, currentAction);
    }

    private boolean isDuplicate(Tag tag) {
        byte[] id = tag.getId();
        String uid = (id == null) ? "" : EslNfcWriter.toHex(id);
        long now = System.currentTimeMillis();
        if (uid.equals(lastUid) && (now - lastUidAt) < 1000L) return true;
        lastUid = uid;
        lastUidAt = now;
        return false;
    }

    /* ------------------------------------------------------------------ */
    /* 实际流程                                                            */
    /* ------------------------------------------------------------------ */

    private void runAction(final Tag tag, final int action) {
        busy = true;
        setButtonsEnabled(false);
        setStatus("正在通信，请让手机贴住不要移动...");

        new Thread(new Runnable() {
            @Override public void run() {
                long t0 = System.currentTimeMillis();
                IsoDep iso = IsoDep.get(tag);
                if (iso == null) {
                    appendLog("标签不支持 ISO-DEP(ISO14443-4)");
                    finishRun("失败：不是 ISO14443-4 标签");
                    return;
                }
                try {
                    long tc = System.currentTimeMillis();
                    iso.connect();
                    appendLog("ISO-DEP 连接成功，耗时 "
                            + (System.currentTimeMillis() - tc) + " ms");
                    logAts(iso);

                    if (action == ACTION_BSELECT) {
                        iso.setTimeout(3000);
                        long tb = System.currentTimeMillis();
                        try {
                            byte[] rb = EslNfcWriter.selectNdefApp(iso);
                            appendLog("实验B A4 选NDEF应用 -> " + (System.currentTimeMillis() - tb)
                                    + " ms  返回 " + EslNfcWriter.toHex(rb) + "  "
                                    + EslNfcWriter.swText(EslNfcWriter.sw(rb)));
                            appendLog("★★★ 拿到状态字了 —— 芯片到手机方向通了");
                        } catch (IOException e) {
                            appendLog("实验B A4 -> " + (System.currentTimeMillis() - tb)
                                    + " ms  " + e.getClass().getSimpleName() + ": " + e.getMessage());
                        } catch (RuntimeException e) {
                            appendLog("实验B A4 -> " + (System.currentTimeMillis() - tb)
                                    + " ms  " + e.getClass().getSimpleName() + ": " + e.getMessage());
                        }
                        finishRun("实验B：请移开 2 秒再贴一次");
                        return;
                    }

                    if (action == ACTION_PROBE) {
                        iso.setTimeout(1500);
                        runOneProbe(iso);
                        int done = probeIndex % PROBE_APDUS.length;
                        finishRun("诊断：已发第 " + done + "/" + PROBE_APDUS.length
                                + " 条，贴住别动，会自动试下一条");
                        return;
                    }

                    iso.setTimeout(3000);

                    // PIN 认证（F0 开头的命令必须先认证）
                    byte[] r = EslNfcWriter.verifyPin(iso, EslNfcWriter.DEFAULT_PIN);
                    String s = EslNfcWriter.sw(r);
                    appendLog("20 Verify PIN -> " + s + " " + EslNfcWriter.swText(s));
                    if (!EslNfcWriter.isOk(r)) {
                        finishRun("PIN 认证失败: " + s + " " + EslNfcWriter.swText(s));
                        return;
                    }
                    success();

                    if (action == ACTION_CLOCK) {
                        // 刷图成功后固件会把显示锁在图片模式，这里给个出口
                        int m = EslNfcWriter.setDisplayMode(iso, EslNfcWriter.MODE_CLOCK);
                        appendLog("D5 切显示模式 -> " + m + " (" + EslNfcWriter.modeText(m) + ")");
                        if (m == EslNfcWriter.MODE_CLOCK) {
                            setModeLabel(m);
                            finishRun("已切回时钟模式：屏幕马上重画一次表盘");
                        } else {
                            finishRun("没切成，设备报的模式是 " + EslNfcWriter.modeText(m));
                        }
                        return;
                    }

                    if (action == ACTION_BUSY) {
                        pollBusy(iso);
                        finishRun("查询完成");
                        return;
                    }

                    try {
                        byte[] a = EslNfcWriter.setScreenType(iso);
                        appendLog("DA 屏幕类型 -> " + EslNfcWriter.sw(a)
                                + " " + EslNfcWriter.swText(EslNfcWriter.sw(a)));
                    } catch (TagLostException e) {
                        throw e;
                    } catch (IOException e) {
                        appendLog("DA 屏幕类型异常（继续）: " + e.getClass().getSimpleName()
                                + ": " + e.getMessage());
                    }
                    try {
                        byte[] b = EslNfcWriter.setScreenColor(iso);
                        appendLog("DA 颜色参数 -> " + EslNfcWriter.sw(b)
                                + " " + EslNfcWriter.swText(EslNfcWriter.sw(b)));
                    } catch (TagLostException e) {
                        throw e;
                    } catch (IOException e) {
                        appendLog("DA 颜色参数异常（继续）: " + e.getClass().getSimpleName()
                                + ": " + e.getMessage());
                    }

                    final byte[] img;
                    if (action == ACTION_WHITE) {
                        img = EslNfcWriter.makeSolid(true);
                    } else if (action == ACTION_PATTERN) {
                        img = EslNfcWriter.makeTestPattern(invert);
                    } else {
                        img = imageBytes;                       // ACTION_IMAGE
                        if (img == null) {
                            finishRun("还没选图片");
                            return;
                        }
                    }
                    appendLog("图片 " + img.length + " 字节 = "
                            + ((img.length + 249) / 250) + " 包");

                    final int npkt = (img.length + EslNfcWriter.PACKET_SIZE - 1)
                            / EslNfcWriter.PACKET_SIZE;
                    String err = EslNfcWriter.loadImage(iso, 0, img, logger,
                            new EslNfcWriter.Progress() {
                                @Override public void onPacket(int done, int total) {
                                    setStatus("下载中 " + done + "/" + total + " 包"
                                            + "（" + (done * 100 / total) + "%）请贴住");
                                }
                            });
                    if (err != null) {
                        finishRun("下载图片失败: " + err);
                        return;
                    }

                    setStatus("下载完成（" + npkt + " 包），正在触发刷新...");
                    byte[] d = EslNfcWriter.redraw(iso, 0, true);
                    String ds = EslNfcWriter.sw(d);
                    appendLog("D4 刷屏 -> " + ds + " " + EslNfcWriter.swText(ds));
                    if (!EslNfcWriter.isOk(d)) {
                        finishRun("刷屏命令失败: " + ds + " " + EslNfcWriter.swText(ds));
                        return;
                    }

                    pollBusy(iso);
                    // 固件在刷新成功时已自动切到图片模式；读回来让界面和真机对上
                    readAndShowMode(iso);
                    finishRun("完成：屏幕正在刷新，松手即可");

                } catch (TagLostException e) {
                    onTagLost(t0);
                } catch (IOException e) {
                    appendLog("IO 异常 " + e.getClass().getSimpleName() + ": " + e.getMessage());
                    finishRun("通信失败（多半是手机移开了）: " + e.getMessage());
                } catch (RuntimeException e) {
                    appendLog("异常 " + e.getClass().getSimpleName() + ": " + e.getMessage());
                    finishRun("异常: " + e.getMessage());
                } finally {
                    try { iso.close(); } catch (Exception ignored) { }
                    appendLog("（本次耗时 " + (System.currentTimeMillis() - t0) + " ms）");
                }
            }
        }).start();
    }

    /** 发一条探针命令；所有异常都兜住，绝不中途断掉 */
    private void runOneProbe(IsoDep iso) {
        int i = probeIndex % PROBE_APDUS.length;
        probeIndex++;
        appendLog(">> 诊断 " + (i + 1) + "/" + PROBE_APDUS.length + ": " + PROBE_NAMES[i]);
        long t = System.currentTimeMillis();
        try {
            byte[] r = iso.transceive(EslNfcWriter.hex(PROBE_APDUS[i]));
            appendLog("   " + (System.currentTimeMillis() - t) + " ms  返回 "
                    + EslNfcWriter.toHex(r) + "  " + EslNfcWriter.swText(EslNfcWriter.sw(r)));
            appendLog("   ★★★ 有应答！这条命令芯片会处理");
        } catch (IOException e) {
            appendLog("   " + (System.currentTimeMillis() - t) + " ms  "
                    + e.getClass().getSimpleName() + ": " + e.getMessage());
        } catch (RuntimeException e) {
            appendLog("   " + (System.currentTimeMillis() - t) + " ms  "
                    + e.getClass().getSimpleName() + ": " + e.getMessage());
        }
        if (probeIndex % PROBE_APDUS.length == 0) {
            appendLog("★ 8 条已全部试完。若全是 TagLostException，"
                    + "说明芯片在 RF 上只做防碰撞/激活、不应答应用层");
        }
    }

    /** 把 ATS（ISO14443-4 激活应答）打出来，判断"像不像标准 Type-4 卡" */
    private void logAts(IsoDep iso) {
        try {
            byte[] ats = iso.getHistoricalBytes();
            appendLog("ATS 历史字节 = " + (ats == null ? "(null)" : EslNfcWriter.toHex(ats))
                    + "  长度=" + (ats == null ? -1 : ats.length));
        } catch (Exception e) {
            appendLog("读 ATS 失败 " + e.getClass().getSimpleName());
        }
        try {
            byte[] hi = iso.getHiLayerResponse();
            if (hi != null) appendLog("上层应答 = " + EslNfcWriter.toHex(hi));
        } catch (Exception ignored) { }
    }

    private void onTagLost(long t0) {
        if (retryLeft > 1) {
            retryLeft--;
            lastUid = null;   // 允许同一张卡再次触发
            appendLog("Tag was lost —— 还剩 " + retryLeft + " 次重试");
            setStatus("Tag was lost。请保持贴住，或移开 1 秒再贴（剩 " + retryLeft + " 次）");
            busy = false;
            ui.post(new Runnable() {
                @Override public void run() {
                    setButtonsEnabled(nfcAdapter != null && nfcAdapter.isEnabled());
                }
            });
        } else {
            retryLeft = MAX_RETRY;
            appendLog("连续 " + MAX_RETRY + " 次 Tag was lost，停止");
            finishRun("连续 " + MAX_RETRY + " 次 Tag was lost：请移开重贴");
        }
    }

    private void success() {
        retryLeft = MAX_RETRY;
    }

    private void pollBusy(IsoDep iso) throws IOException {
        for (int i = 0; i < 40; i++) {
            byte[] b = EslNfcWriter.getBusy(iso);
            if (b == null || b.length < 1) {
                appendLog("DE 返回异常: " + EslNfcWriter.toHex(b));
                return;
            }
            int st = b[0] & 0xFF;
            appendLog("DE 第 " + (i + 1) + " 次 -> " + (st == 0 ? "空闲(刷新完成)" : "忙"));
            if (st == 0) return;
            try { Thread.sleep(500); } catch (InterruptedException e) { return; }
        }
        appendLog("等待 BUSY 超时（20 秒）");
    }

    /**
     * 读一次显示模式，同步顶部那行提示。
     * 读取失败（比如手移开了）只打印，绝不影响已经完成的刷屏。
     */
    private void readAndShowMode(IsoDep iso) {
        try {
            int m = EslNfcWriter.getDisplayMode(iso);
            appendLog("D5 显示模式 = " + m + " (" + EslNfcWriter.modeText(m) + ")");
            if (m >= 0) setModeLabel(m);
        } catch (IOException e) {
            appendLog("读显示模式失败（不影响刷屏）: " + e.getClass().getSimpleName());
        }
    }

    /** 更新"显示模式"那行提示 */
    private void setModeLabel(final int m) {
        ui.post(new Runnable() {
            @Override public void run() {
                if (modeText == null) return;
                if (m == EslNfcWriter.MODE_IMAGE) {
                    modeText.setText("显示模式：图片（画面会一直保留，时钟不再覆盖）");
                } else if (m == EslNfcWriter.MODE_CLOCK) {
                    modeText.setText("显示模式：时钟（每分钟自动重画表盘）");
                } else {
                    modeText.setText("显示模式：未知（没读到）");
                }
            }
        });
    }

    /* ------------------------------------------------------------------ */
    /* 小工具                                                              */
    /* ------------------------------------------------------------------ */

    private void finishRun(final String msg) {
        busy = false;
        ui.post(new Runnable() {
            @Override public void run() {
                setStatus(msg);
                setButtonsEnabled(nfcAdapter != null && nfcAdapter.isEnabled());
            }
        });
    }

    private void setStatus(final String s) {
        ui.post(new Runnable() {
            @Override public void run() { statusText.setText(s); }
        });
    }

    private void appendLog(final String s) {
        ui.post(new Runnable() {
            @Override public void run() { logText.append(s + "\n"); }
        });
    }

    private void setButtonsEnabled(boolean en) {
        if (btnFlash != null) btnFlash.setEnabled(en);
        if (btnClockMode != null) btnClockMode.setEnabled(en);
        if (btnPick != null) btnPick.setEnabled(en);
        if (btnProbe != null) btnProbe.setEnabled(en);
        if (btnResetProbe != null) btnResetProbe.setEnabled(en);
        if (btnPattern != null) btnPattern.setEnabled(en);
        if (btnClear != null) btnClear.setEnabled(en);
        if (btnBusy != null) btnBusy.setEnabled(en);
        if (btnBSelect != null) btnBSelect.setEnabled(en);
    }
}
