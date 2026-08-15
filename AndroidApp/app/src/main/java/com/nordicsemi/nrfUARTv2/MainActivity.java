/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.nordicsemi.nrfUARTv2;


import java.io.File;
import java.io.InputStream;
import java.io.UnsupportedEncodingException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.Configuration;
import android.graphics.Color;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Message;
import androidx.core.content.FileProvider;
import androidx.localbroadcastmanager.content.LocalBroadcastManager;
import android.util.Log;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.Toast;

public class MainActivity extends Activity implements RadioGroup.OnCheckedChangeListener {
    private static final int REQUEST_ENABLE_BT = 2;
    private static final int UART_PROFILE_READY = 10;
    public static final String TAG = "MainActivity";
    private static final int UART_PROFILE_CONNECTED = 20;
    private static final int UART_PROFILE_DISCONNECTED = 21;
    private static final int STATE_OFF = 10;
    private static final int REQUEST_BLUETOOTH_PERMISSIONS = 100;

    TextView mRemoteRssiVal;
    RadioGroup mRg;
    private int mState = UART_PROFILE_DISCONNECTED;
    private UartService mService = null;
    private BluetoothDevice mDevice = null;
    private BluetoothAdapter mBtAdapter = null;
    private BluetoothLeScanner mLEScanner;
    private ListView messageListView;
    private ArrayAdapter<String> listAdapter;
    private Button btnConnectDisconnect, btnSend;
    private EditText edtMessage;

    // In-page BLE device list
    private LinearLayout deviceListPanel;
    private ListView deviceListView;
    private TextView scanStatus;
    private Button btnScan;
    private List<BluetoothDevice> bleDeviceList = new ArrayList<>();
    private Map<String, Integer> devRssiValues = new HashMap<>();
    private DeviceListAdapter deviceListAdapter;
    private Handler mHandler;
    private boolean mScanning;
    private static final long SCAN_PERIOD = 10000;

    private final static int FILEOPEN_BLACK_RESULT_CODE = 400;
    private final static int FILEOPEN_RED_RESULT_CODE = 401;
    private Button btnBrowseBlack;
    private Button btnBrowseRed;
    private Button btnStartDownload;
    private TextView textViewDownloadInfo;
    private TextView textViewFwVer;
    EpdDownloader epdDownloader = new EpdDownloader();

    private boolean hasBluetoothPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    private void requestBluetoothPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            requestPermissions(
                new String[]{
                    Manifest.permission.BLUETOOTH_CONNECT,
                    Manifest.permission.BLUETOOTH_SCAN,
                    Manifest.permission.ACCESS_FINE_LOCATION
                },
                REQUEST_BLUETOOTH_PERMISSIONS
            );
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            requestPermissions(
                new String[]{ Manifest.permission.ACCESS_FINE_LOCATION },
                REQUEST_BLUETOOTH_PERMISSIONS
            );
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_BLUETOOTH_PERMISSIONS) {
            boolean allGranted = true;
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    allGranted = false;
                    break;
                }
            }
            if (!allGranted) {
                Toast.makeText(this, "Bluetooth permissions required", Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        mHandler = new Handler();

        // Request runtime permissions
        if (!hasBluetoothPermissions()) {
            requestBluetoothPermissions();
        }

        mBtAdapter = BluetoothAdapter.getDefaultAdapter();
        if (mBtAdapter == null) {
            Toast.makeText(this, "Bluetooth is not available", Toast.LENGTH_LONG).show();
            finish();
            return;
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            mLEScanner = mBtAdapter.getBluetoothLeScanner();
        }

        messageListView = (ListView) findViewById(R.id.listMessage);
        listAdapter = new ArrayAdapter<String>(this, R.layout.message_detail);
        messageListView.setAdapter(listAdapter);
        messageListView.setDivider(null);
        btnConnectDisconnect = (Button) findViewById(R.id.btn_select);
        btnSend = (Button) findViewById(R.id.sendButton);
        edtMessage = (EditText) findViewById(R.id.sendText);
        service_init();

        // In-page device list
        deviceListPanel = (LinearLayout) findViewById(R.id.device_list_panel);
        deviceListView = (ListView) findViewById(R.id.device_list_view);
        scanStatus = (TextView) findViewById(R.id.scan_status);
        btnScan = (Button) findViewById(R.id.btn_scan);
        deviceListAdapter = new DeviceListAdapter(this, bleDeviceList);
        deviceListView.setAdapter(deviceListAdapter);

        deviceListView.setOnItemClickListener(new android.widget.AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(android.widget.AdapterView<?> parent, View view, int position, long id) {
                BluetoothDevice device = bleDeviceList.get(position);
                mDevice = device;
                stopScan();
                deviceListPanel.setVisibility(View.GONE);
                btnConnectDisconnect.setText("Disconnect");
                ((TextView) findViewById(R.id.deviceName)).setText(mDevice.getName() + " - connecting");
                mService.connect(device.getAddress());
            }
        });

        textViewDownloadInfo = (TextView) findViewById(R.id.textView_downlload_info);
        textViewFwVer = (TextView) findViewById(R.id.textViewFwVer);

        // Handler Disconnect & Connect button
        btnConnectDisconnect.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasBluetoothPermissions()) {
                    requestBluetoothPermissions();
                    return;
                }
                if (!mBtAdapter.isEnabled()) {
                    Log.i(TAG, "onClick - BT not enabled yet");
                    Intent enableIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        try { startActivityForResult(enableIntent, REQUEST_ENABLE_BT); } catch (SecurityException e) { Log.e(TAG, "BT enable error", e); }
                    } else {
                        startActivityForResult(enableIntent, REQUEST_ENABLE_BT);
                    }
                } else {
                    if (btnConnectDisconnect.getText().equals("Connect")) {
                        // Show in-page device list and start scan
                        deviceListPanel.setVisibility(View.VISIBLE);
                        bleDeviceList.clear();
                        devRssiValues.clear();
                        deviceListAdapter.notifyDataSetChanged();
                        startScan();
                    } else {
                        // Disconnect
                        if (mDevice != null) {
                            mService.disconnect();
                        }
                    }
                }
            }
        });

        // Scan button
        btnScan.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (mScanning) {
                    stopScan();
                } else {
                    bleDeviceList.clear();
                    devRssiValues.clear();
                    deviceListAdapter.notifyDataSetChanged();
                    startScan();
                }
            }
        });

        // Handler Send button
        btnSend.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                EditText editText = (EditText) findViewById(R.id.sendText);
                String message = editText.getText().toString();
                byte[] value;
                try {
                    value = message.getBytes("UTF-8");
                    mService.writeRXCharacteristic(value);
                    String currentDateTimeString = DateFormat.getTimeInstance().format(new Date());
                    listAdapter.add("[" + currentDateTimeString + "] TX: " + message);
                    messageListView.smoothScrollToPosition(listAdapter.getCount() - 1);
                    edtMessage.setText("");
                } catch (UnsupportedEncodingException e) {
                    e.printStackTrace();
                }
            }
        });

        // open black image data
        btnBrowseBlack = (Button) this.findViewById(R.id.button_browse_black);
        btnBrowseBlack.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, false);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(intent, FILEOPEN_BLACK_RESULT_CODE);
            }
        });

        btnBrowseRed = (Button) this.findViewById(R.id.button_browse_red);
        btnBrowseRed.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, false);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(intent, FILEOPEN_RED_RESULT_CODE);
            }
        });

        btnStartDownload = (Button) this.findViewById(R.id.button_start_download);
        btnStartDownload.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                epdDownloader.start();
                byte[] value = new byte[1];
                value[0] = EpdDownloader.EPD_CMD_READ_VERSION;
                sendMcuRequest(value);
            }
        });

        // LED test button
        Button btnLedTest = (Button) this.findViewById(R.id.button_led_test);
        btnLedTest.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                byte[] value = new byte[1];
                value[0] = EpdDownloader.EPD_CMD_LED_TEST;
                sendMcuRequest(value);
                Toast.makeText(MainActivity.this, "LED test started", Toast.LENGTH_SHORT).show();
            }
        });

        // LED toggle button
        Button btnLedToggle = (Button) this.findViewById(R.id.button_led_toggle);
        btnLedToggle.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                byte[] value = new byte[1];
                value[0] = EpdDownloader.EPD_CMD_LED_TOGGLE;
                sendMcuRequest(value);
            }
        });

        // Sync time button
        Button btnSyncTime = (Button) this.findViewById(R.id.button_sync_time);
        btnSyncTime.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                java.util.Calendar cal = java.util.Calendar.getInstance();
                int year = cal.get(java.util.Calendar.YEAR);
                int ms = cal.get(java.util.Calendar.MILLISECOND);
                byte[] value = new byte[9];
                value[0] = EpdDownloader.EPD_CMD_SET_TIME;
                value[1] = (byte) ((year - 2000) & 0xFF);
                value[2] = (byte) (cal.get(java.util.Calendar.MONTH) + 1);
                value[3] = (byte) cal.get(java.util.Calendar.DAY_OF_MONTH);
                value[4] = (byte) cal.get(java.util.Calendar.HOUR_OF_DAY);
                value[5] = (byte) cal.get(java.util.Calendar.MINUTE);
                value[6] = (byte) cal.get(java.util.Calendar.SECOND);
                value[7] = (byte) ((ms >> 8) & 0xFF);
                value[8] = (byte) (ms & 0xFF);
                sendMcuRequest(value);
            }
        });

        // Get time button
        Button btnGetTime = (Button) this.findViewById(R.id.button_get_time);
        btnGetTime.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                byte[] value = new byte[1];
                value[0] = EpdDownloader.EPD_CMD_GET_TIME;
                sendMcuRequest(value);
            }
        });
    }

    // BLE scan methods
    private void startScan() {
        if (!hasBluetoothPermissions()) {
            requestBluetoothPermissions();
            return;
        }
        scanStatus.setText("Scanning for devices...");
        btnScan.setText("Stop");
        mScanning = true;

        mHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                stopScan();
            }
        }, SCAN_PERIOD);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            mLEScanner.startScan(mScanCallback);
        } else {
            mBtAdapter.startLeScan(mLeScanCallback);
        }
    }

    private void stopScan() {
        mScanning = false;
        scanStatus.setText("Found " + bleDeviceList.size() + " devices");
        btnScan.setText("Scan");
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            mLEScanner.stopScan(mScanCallback);
        } else {
            mBtAdapter.stopLeScan(mLeScanCallback);
        }
    }

    // New API scan callback
    private ScanCallback mScanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            super.onScanResult(callbackType, result);
            final BluetoothDevice device = result.getDevice();
            final int rssi = result.getRssi();
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    addDevice(device, rssi);
                }
            });
        }
    };

    // Old API fallback
    private BluetoothAdapter.LeScanCallback mLeScanCallback = new BluetoothAdapter.LeScanCallback() {
        @Override
        public void onLeScan(final BluetoothDevice device, final int rssi, byte[] scanRecord) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    addDevice(device, rssi);
                }
            });
        }
    };

    private void addDevice(BluetoothDevice device, int rssi) {
        boolean found = false;
        for (BluetoothDevice d : bleDeviceList) {
            if (d.getAddress().equals(device.getAddress())) {
                found = true;
                break;
            }
        }
        devRssiValues.put(device.getAddress(), rssi);
        if (!found) {
            bleDeviceList.add(device);
            deviceListAdapter.notifyDataSetChanged();
        }
    }

    // Device list adapter
    class DeviceListAdapter extends BaseAdapter {
        Context context;
        List<BluetoothDevice> devices;
        LayoutInflater inflater;

        public DeviceListAdapter(Context context, List<BluetoothDevice> devices) {
            this.context = context;
            inflater = LayoutInflater.from(context);
            this.devices = devices;
        }

        @Override
        public int getCount() { return devices.size(); }
        @Override
        public Object getItem(int position) { return devices.get(position); }
        @Override
        public long getItemId(int position) { return position; }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            ViewGroup vg;
            if (convertView != null) {
                vg = (ViewGroup) convertView;
            } else {
                vg = (ViewGroup) inflater.inflate(R.layout.device_element, null);
            }
            BluetoothDevice device = devices.get(position);
            TextView tvname = (TextView) vg.findViewById(R.id.name);
            TextView tvadd = (TextView) vg.findViewById(R.id.address);
            TextView tvrssi = (TextView) vg.findViewById(R.id.rssi);
            TextView tvpaired = (TextView) vg.findViewById(R.id.paired);

            tvname.setText(device.getName());
            tvadd.setText(device.getAddress());

            Integer rssiVal = devRssiValues.get(device.getAddress());
            if (rssiVal != null) {
                tvrssi.setVisibility(View.VISIBLE);
                tvrssi.setText("Rssi = " + String.valueOf(rssiVal));
            }
            if (device.getBondState() == BluetoothDevice.BOND_BONDED) {
                tvpaired.setVisibility(View.VISIBLE);
                tvpaired.setText(R.string.paired);
            } else {
                tvpaired.setVisibility(View.GONE);
            }
            return vg;
        }
    }

    //UART service connected/disconnected
    private ServiceConnection mServiceConnection = new ServiceConnection() {
        public void onServiceConnected(ComponentName className, IBinder rawBinder) {
            mService = ((UartService.LocalBinder) rawBinder).getService();
            Log.d(TAG, "onServiceConnected mService= " + mService);
            if (!mService.initialize()) {
                Log.e(TAG, "Unable to initialize Bluetooth");
                finish();
            }
        }

        public void onServiceDisconnected(ComponentName classname) {
            mService = null;
        }
    };

    private Handler mMsgHandler = new Handler() {
        @Override
        public void handleMessage(Message msg) {
        }
    };

    private final BroadcastReceiver UARTStatusChangeReceiver = new BroadcastReceiver() {
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            final Intent mIntent = intent;

            if (action.equals(UartService.ACTION_GATT_CONNECTED)) {
                runOnUiThread(new Runnable() {
                    public void run() {
                        String currentDateTimeString = DateFormat.getTimeInstance().format(new Date());
                        Log.d(TAG, "UART_CONNECT_MSG");
                        btnConnectDisconnect.setText("Disconnect");
                        edtMessage.setEnabled(true);
                        btnSend.setEnabled(true);
                        ((TextView) findViewById(R.id.deviceName)).setText(mDevice.getName() + " - ready");
                        listAdapter.add("[" + currentDateTimeString + "] Connected to: " + mDevice.getName());
                        messageListView.smoothScrollToPosition(listAdapter.getCount() - 1);
                        mState = UART_PROFILE_CONNECTED;
                        deviceListPanel.setVisibility(View.GONE);
                    }
                });
            }

            if (action.equals(UartService.ACTION_GATT_DISCONNECTED)) {
                runOnUiThread(new Runnable() {
                    public void run() {
                        String currentDateTimeString = DateFormat.getTimeInstance().format(new Date());
                        Log.d(TAG, "UART_DISCONNECT_MSG");
                        btnConnectDisconnect.setText("Connect");
                        edtMessage.setEnabled(false);
                        btnSend.setEnabled(false);
                        ((TextView) findViewById(R.id.deviceName)).setText("Not Connected");
                        listAdapter.add("[" + currentDateTimeString + "] Disconnected");
                        mState = UART_PROFILE_DISCONNECTED;
                        mService.close();
                    }
                });
            }

            if (action.equals(UartService.ACTION_GATT_SERVICES_DISCOVERED)) {
                mService.enableTXNotification();
            }

            if (action.equals(UartService.ACTION_DATA_AVAILABLE)) {
                final byte[] txValue = intent.getByteArrayExtra(UartService.EXTRA_DATA);
                runOnUiThread(new Runnable() {
                    public void run() {
                        try {
                            handleMcuResponse(txValue);
                        } catch (Exception e) {
                            Log.e(TAG, e.toString());
                        }
                    }
                });
            }

            if (action.equals(UartService.DEVICE_DOES_NOT_SUPPORT_UART)) {
                showMessage("Device doesn't support UART. Disconnecting");
                mService.disconnect();
            }
        }
    };

    private void service_init() {
        Intent bindIntent = new Intent(this, UartService.class);
        bindService(bindIntent, mServiceConnection, Context.BIND_AUTO_CREATE);
        LocalBroadcastManager.getInstance(this).registerReceiver(UARTStatusChangeReceiver, makeGattUpdateIntentFilter());
    }

    private static IntentFilter makeGattUpdateIntentFilter() {
        final IntentFilter intentFilter = new IntentFilter();
        intentFilter.addAction(UartService.ACTION_GATT_CONNECTED);
        intentFilter.addAction(UartService.ACTION_GATT_DISCONNECTED);
        intentFilter.addAction(UartService.ACTION_GATT_SERVICES_DISCOVERED);
        intentFilter.addAction(UartService.ACTION_DATA_AVAILABLE);
        intentFilter.addAction(UartService.DEVICE_DOES_NOT_SUPPORT_UART);
        return intentFilter;
    }

    @Override
    public void onStart() { super.onStart(); }

    @Override
    public void onDestroy() {
        super.onDestroy();
        Log.d(TAG, "onDestroy()");
        try {
            LocalBroadcastManager.getInstance(this).unregisterReceiver(UARTStatusChangeReceiver);
        } catch (Exception ignore) {
            Log.e(TAG, ignore.toString());
        }
        unbindService(mServiceConnection);
        mService.stopSelf();
        mService = null;
    }

    @Override
    protected void onStop() {
        Log.d(TAG, "onStop");
        super.onStop();
    }

    @Override
    protected void onPause() {
        Log.d(TAG, "onPause");
        super.onPause();
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        Log.d(TAG, "onRestart");
    }

    @Override
    public void onResume() {
        super.onResume();
        Log.d(TAG, "onResume");
        if (!hasBluetoothPermissions()) {
            requestBluetoothPermissions();
            return;
        }
        if (!mBtAdapter.isEnabled()) {
            Log.i(TAG, "onResume - BT not enabled yet");
            Intent enableIntent = new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                try { startActivityForResult(enableIntent, REQUEST_ENABLE_BT); } catch (SecurityException e) { Log.e(TAG, "BT enable error", e); }
            } else {
                startActivityForResult(enableIntent, REQUEST_ENABLE_BT);
            }
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        switch (requestCode) {
        case REQUEST_ENABLE_BT:
            if (resultCode == Activity.RESULT_OK) {
                Toast.makeText(this, "Bluetooth has turned on ", Toast.LENGTH_SHORT).show();
            } else {
                Log.d(TAG, "BT not enabled");
                Toast.makeText(this, "Problem in BT Turning ON ", Toast.LENGTH_SHORT).show();
                finish();
            }
            break;

        case FILEOPEN_BLACK_RESULT_CODE:
            if (resultCode == Activity.RESULT_OK && data.getData() != null) {
                try {
                    Uri uri = data.getData();
                    Log.i(TAG, "open black file " + uri.toString());
                    InputStream is = getContentResolver().openInputStream(uri);
                    if (!epdDownloader.loadFromUri(is)) {
                        String err = epdDownloader.getErrString();
                        textViewDownloadInfo.setText(err);
                    } else {
                        textViewDownloadInfo.setText("black file loaded");
                    }
                } catch (Exception e) {
                    textViewDownloadInfo.setText("black file error: " + e.getMessage());
                    Log.e(TAG, "open black file error", e);
                }
            }
            break;

        case FILEOPEN_RED_RESULT_CODE:
            // 红图已废弃, 复用为第二个图片选择入口
            if (resultCode == Activity.RESULT_OK && data.getData() != null) {
                try {
                    Uri uri = data.getData();
                    Log.i(TAG, "open image file  " + uri.toString());
                    InputStream is = getContentResolver().openInputStream(uri);
                    if (!epdDownloader.loadFromUri(is)) {
                        String err = epdDownloader.getErrString();
                        textViewDownloadInfo.setText(err);
                    } else {
                        textViewDownloadInfo.setText("image file loaded");
                    }
                } catch (Exception e) {
                    textViewDownloadInfo.setText("image file error: " + e.getMessage());
                    Log.e(TAG, "open image file error", e);
                }
            }
            break;

        default:
            Log.e(TAG, "wrong request code");
            break;
        }
    }

    @Override
    public void onCheckedChanged(RadioGroup group, int checkedId) {
    }

    private void showMessage(String msg) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show();
    }

    @Override
    public void onBackPressed() {
        if (mState == UART_PROFILE_CONNECTED) {
            Intent startMain = new Intent(Intent.ACTION_MAIN);
            startMain.addCategory(Intent.CATEGORY_HOME);
            startMain.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(startMain);
            showMessage("nRFUART's running in background.\n             Disconnect to exit");
        } else {
            new AlertDialog.Builder(this)
            .setIcon(android.R.drawable.ic_dialog_alert)
            .setTitle(R.string.popup_title)
            .setMessage(R.string.popup_message)
            .setPositiveButton(R.string.popup_yes, new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    finish();
                }
            })
            .setNegativeButton(R.string.popup_no, null)
            .show();
        }
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

    protected void handleMcuResponse(byte[] frame) {
        if (frame.length == 0) {
            Log.w(TAG, "handleMcuResponse: zero length?");
            return;
        }
        Log.i(TAG, "handleMcuResponse " + bytesToHexString(frame));

        byte[] buf;

        switch (frame[0]) {
            case EpdDownloader.EPD_CMD_READ_VERSION:
                Log.i(TAG, "handleMcuResponse got epd version");
                if (frame.length >= 3) {
                    String ver = "FwVer: " + frame[1] + "." + frame[2];
                    textViewFwVer.setText(ver);
                }
                buf = new byte[1];
                buf[0] = EpdDownloader.EPD_CMD_INIT;
                sendMcuRequest(buf);
                break;
            case EpdDownloader.EPD_CMD_INIT:
                Log.i(TAG, "handleMcuResponse got epd init response, now send prepare");
                buf = new byte[1];
                buf[0] = EpdDownloader.EPD_CMD_PREPARE_BLK_RAM;
                sendMcuRequest(buf);
                break;
            case EpdDownloader.EPD_CMD_PREPARE_BLK_RAM:
            case EpdDownloader.EPD_CMD_WRITE_BLK_RAM:
                if (epdDownloader.isDataFinish()) {
                    Log.d(TAG, "black data finish");
                    buf = new byte[1];
                    buf[0] = EpdDownloader.EPD_CMD_UPDATE_DISPLAY;
                    sendMcuRequest(buf);
                } else {
                    byte[] ram_data = epdDownloader.getNextFrame();
                    buf = new byte[1 + epdDownloader.FRAME_DATA_SIZE];
                    buf[0] = EpdDownloader.EPD_CMD_WRITE_BLK_RAM;
                    for (int i = 0; i < epdDownloader.FRAME_DATA_SIZE; i++) {
                        buf[1 + i] = ram_data[i];
                    }
                    Log.i(TAG, "sending write ram command: " + bytesToHexString(buf));
                    sendMcuRequest(buf);
                    int progress = epdDownloader.getProgress();
                    textViewDownloadInfo.setText("writing " + String.valueOf(progress) + " %");
                }
                break;

            case EpdDownloader.EPD_CMD_UPDATE_DISPLAY:
                textViewDownloadInfo.setText("epd download finish");
                buf = new byte[1];
                buf[0] = EpdDownloader.EPD_CMD_DEINIT;
                sendMcuRequest(buf);
                break;
            case EpdDownloader.EPD_CMD_DEINIT:
                Log.i(TAG, "handleMcuResponse: got cmd deinit response, all finish");
                break;

            case EpdDownloader.EPD_CMD_SET_TIME:
                Log.i(TAG, "handleMcuResponse: set time ok");
                Toast.makeText(MainActivity.this, "Time synced", Toast.LENGTH_SHORT).show();
                break;

            case EpdDownloader.EPD_CMD_GET_TIME:
                if (frame.length >= 9) {
                    int year = 2000 + (frame[1] & 0xFF);
                    int ms = ((frame[7] & 0xFF) << 8) | (frame[8] & 0xFF);
                    String t = year + "-" +
                        String.format("%02d", frame[2] & 0xFF) + "-" +
                        String.format("%02d", frame[3] & 0xFF) + " " +
                        String.format("%02d", frame[4] & 0xFF) + ":" +
                        String.format("%02d", frame[5] & 0xFF) + ":" +
                        String.format("%02d", frame[6] & 0xFF) + "." +
                        String.format("%03d", ms);
                    ((TextView) findViewById(R.id.textView_time)).setText("Device time: " + t);
                    Toast.makeText(MainActivity.this, t, Toast.LENGTH_SHORT).show();
                }
                break;

            default:
                Log.w(TAG, "handleMcuResponse: unknown cmd type");
                break;
        }
    }

    protected void sendMcuRequest(byte[] frame) {
        try {
            mService.writeRXCharacteristic(frame);
            String currentDateTimeString = DateFormat.getTimeInstance().format(new Date());
            listAdapter.add("TX: " + bytesToHexString(frame));
            messageListView.smoothScrollToPosition(listAdapter.getCount() - 1);
            edtMessage.setText("");
        } catch (Exception e) {
            e.printStackTrace();
        }
    }
}
