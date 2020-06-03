package de.bananajoh.sv650overlay;

import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothSocket;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.BitmapFactory;
import android.graphics.PixelFormat;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.SystemClock;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.Toast;

import androidx.localbroadcastmanager.content.LocalBroadcastManager;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.UUID;


public class OverlayService extends Service implements View.OnTouchListener, View.OnClickListener {
    private WindowManager windowManager = null;
    private ImageButton overlayButton = null;
    private View topCenterView = null;
    private float eventRelativeX = 0.0f;
    private float eventRelativeY = 0.0f;
    private int initialWidgetX = 0;
    private int initialWidgetY = 0;
    private boolean widgetMoving = false;

    private static final UUID SPP_UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final UUID BLE_UART_UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    private static final UUID BLE_CHAR_TX_UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
    private static final long BLUETOOTH_RECONNECT_INTERVAL_MS = 20000;
    private static final int GEAR_DATA_INDEX = 28;
    private static final String TEST_DATAFRAME = "6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,"
            + "23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,"
            + "44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64";
    private boolean bluetoothBusy = false;
    private BluetoothAdapter bluetoothAdapter = null;
    private BluetoothDevice bluetoothDevice = null;
    private BluetoothSocket bluetoothSocket = null;
    private OutputStream bluetoothOutputStream = null;
    private InputStream bluetoothInputStream = null;
    private Thread bluetoothWorkerThread = null;
    private byte[] bluetoothReadBuffer = {0};
    private int bluetoothReadBufferPosition = 0;
    private volatile boolean stopBluetoothWorkerThread = true;
    private BluetoothGatt bluetoothGatt = null;
    private BluetoothGattCallback gattCallback = null;
    private boolean bleConnected = false;
    private String lastDeviceAddress = null;
    private boolean lastDeviceSecure = false;
    private Handler bluetoothReconnectHandler = null;
    private Runnable bluetoothReconnect = null;
    private BufferedWriter logFileBuffer = null;


    // Class for clients to access this service
    public class LocalBinder extends Binder {
        OverlayService getService() {
            return OverlayService.this;
        }
    }
    private final IBinder binder = new LocalBinder();


    // Listen for Bluetooth device disconnect broadcasts //
    private final BroadcastReceiver broadcastReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            // No extra intent action check as there is only one filter registered
            BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            if(device.getAddress().equals(lastDeviceAddress)) {
                Toast.makeText(overlayButton.getContext(), "Connection to " + device.getName() + " lost.", Toast.LENGTH_LONG).show();
                disconnectBluetooth(true);
            }
        }
    };


    // Constructor //
    public OverlayService() {
    }


    // Setup overlay //
    private void setupOverlay() {
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);

        // Setup image button as overlay widget
        overlayButton = new ImageButton(this);
        overlayButton.setImageResource(R.drawable.sevenseg_dot);
        overlayButton.setPadding(0, 0, 0, 0);
        overlayButton.setAlpha(0.8f);
        overlayButton.setScaleType(ImageView.ScaleType.FIT_CENTER);
        overlayButton.setOnTouchListener(this);
        overlayButton.setOnClickListener(this);

        // Retrieve width and height of image
        BitmapFactory.Options dimensions = new BitmapFactory.Options();
        dimensions.inJustDecodeBounds = true;
        BitmapFactory.decodeResource(getResources(), R.drawable.sevenseg_empty, dimensions);
        int imageWidth = dimensions.outWidth;
        int imageHeight = dimensions.outHeight;

        // Add view for the image button to the window manager
        WindowManager.LayoutParams params = new WindowManager.LayoutParams(WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL, PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.CENTER_HORIZONTAL | Gravity.TOP;
        params.x = 0;
        params.y = 0;
        params.width = imageWidth;
        params.height = imageHeight;
        windowManager.addView(overlayButton, params);

        // Add another invisible view as reference point for handling touchscreen move events
        topCenterView = new View(this);
        WindowManager.LayoutParams topCenterParams = new WindowManager.LayoutParams(WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL, PixelFormat.TRANSLUCENT);
        topCenterParams.gravity = Gravity.CENTER_HORIZONTAL | Gravity.TOP;
        topCenterParams.x = 0;
        topCenterParams.y = 0;
        topCenterParams.width = 0;
        topCenterParams.height = 0;
        windowManager.addView(topCenterView, topCenterParams);
    }


    // Setup Bluetooth auto reconnect to last device //
    private void setupBluetoothReconnect() {
        bluetoothReconnectHandler = new Handler();
        bluetoothReconnect = new Runnable() {
            @Override
            public void run() {
                if(!isBluetoothConnected() && lastDeviceAddress != null) {
                    connectBluetooth(lastDeviceAddress, lastDeviceSecure, false);
                }
                bluetoothReconnectHandler.postDelayed(this, BLUETOOTH_RECONNECT_INTERVAL_MS);
            }
        };
    }


    // Retrieve gear out of received data for overlay //
    private void processReceivedData(String data) {
        sendDataBroadcastIntent(data);
        appendLog(data);

        String values[] = data.split(",");
        if(values.length < GEAR_DATA_INDEX+1) {
            return;
        }
        int gear = -1;
        try {
            gear = Integer.parseInt(values[GEAR_DATA_INDEX]);
        } catch(NumberFormatException ex) {
            return;
        }
        switch(gear) {
            case 0: {
                overlayButton.setImageResource(R.drawable.sevenseg_minus);
                break;
            }
            case 1: {
                overlayButton.setImageResource(R.drawable.sevenseg_1);
                break;
            }
            case 2: {
                overlayButton.setImageResource(R.drawable.sevenseg_2);
                break;
            }
            case 3: {
                overlayButton.setImageResource(R.drawable.sevenseg_3);
                break;
            }
            case 4: {
                overlayButton.setImageResource(R.drawable.sevenseg_4);
                break;
            }
            case 5: {
                overlayButton.setImageResource(R.drawable.sevenseg_5);
                break;
            }
            case 6: {
                overlayButton.setImageResource(R.drawable.sevenseg_6);
                break;
            }
            default: {
                overlayButton.setImageResource(R.drawable.sevenseg_empty);
            }
        }
    }


    // Start data logging to file //
    public void startDataLogging() {
        if(logFileBuffer != null) {
            return;
        }
        String fileTimestamp = new SimpleDateFormat("yyyyMMdd_HHmmss").format(new Date());
        File logFile = new File(this.getExternalFilesDir(null).getAbsolutePath(), "sensordata_" + fileTimestamp + ".log");
        if(!logFile.exists()) {
            try {
                logFile.createNewFile();
            } catch(IOException e) {
                e.printStackTrace();
            }
        }
        try {
            logFileBuffer = new BufferedWriter(new FileWriter(logFile, true));
            String logHeader = "Date,Time";
            for(DataInfoEntry dataInfoEntry : DataInfo.ENTRIES) {
                logHeader += "," + dataInfoEntry.label;
            }
            logFileBuffer.append(logHeader);
            logFileBuffer.newLine();
        } catch(IOException e) {
            e.printStackTrace();
        }
    }


    // Stop data logging to file //
    public void stopDataLogging() {
        if(logFileBuffer == null) {
            return;
        }
        try {
            logFileBuffer.flush();
            logFileBuffer.close();
        } catch(IOException e) {
            e.printStackTrace();
        }
        logFileBuffer = null;
    }


    // Check if data is written to log //
    public boolean isDataLogging() {
        return logFileBuffer != null;
    }


    // Write sensor data to end of log file //
    public void appendLog(String text) {
        if(logFileBuffer == null) {
            return;
        }
        try {
            String timestamp = new SimpleDateFormat("yyyyMMdd,HHmmssSSS,").format(new Date());
            logFileBuffer.append(timestamp + text);
            logFileBuffer.newLine();
        } catch(IOException e) {
            e.printStackTrace();
        }
    }


    // Send data to main activity //
    private void sendDataBroadcastIntent(String data) {
        Intent intent = new Intent(String.valueOf(R.string.bluetooth_message_intent_action));
        intent.putExtra("data", data);
        LocalBroadcastManager.getInstance(this).sendBroadcast(intent);
    }


    // Setup and start a worker thread for receiving Bluetooth data
    private void startBluetoothWorkerThread() {
        final Handler handler = new Handler();
        final byte delimiter = 10; // This is the ASCII code for a newline character

        stopBluetoothWorkerThread = false;
        bluetoothReadBufferPosition = 0;
        bluetoothReadBuffer = new byte[1024];
        bluetoothWorkerThread = new Thread(new Runnable() {
            public void run() {
                while(!Thread.currentThread().isInterrupted() && !stopBluetoothWorkerThread) {
                    try {
                        int bytesAvailable = bluetoothInputStream.available();
                        if(bytesAvailable > 0) {
                            byte[] packetBytes = new byte[bytesAvailable];
                            bluetoothInputStream.read(packetBytes);
                            for(int i = 0; i < bytesAvailable; i++) {
                                byte b = packetBytes[i];
                                if(b == delimiter) {
                                    byte[] encodedBytes = new byte[bluetoothReadBufferPosition];
                                    System.arraycopy(bluetoothReadBuffer, 0, encodedBytes, 0, encodedBytes.length);
                                    final String data = new String(encodedBytes, "US-ASCII");
                                    bluetoothReadBufferPosition = 0;

                                    handler.post(new Runnable() {
                                        public void run() {
                                            //Toast.makeText(overlayButton.getContext(), data, Toast.LENGTH_SHORT).show();
                                            processReceivedData(data);
                                        }
                                    });
                                } else {
                                    bluetoothReadBuffer[bluetoothReadBufferPosition++] = b;
                                }
                            }
                        }
                    } catch(final IOException ex) {
                        handler.post(new Runnable() {
                            public void run() {
                                Toast.makeText(overlayButton.getContext(), ex.toString(), Toast.LENGTH_LONG).show();
                            }
                        });
                        overlayButton.setImageResource(R.drawable.sevenseg_dot);
                        stopBluetoothWorkerThread = true;
                    }
                }
            }
        });
        bluetoothWorkerThread.start();
    }


    // Connect to Bluetooth device with serial port profile //
    public void connectBluetooth(final String deviceAddress, final boolean deviceSecure, final boolean invokeAutoReconnect) {
        final Handler handler = new Handler();
        new Thread(new Runnable() {
            public void run() {
                if(bluetoothBusy) {
                    handler.post(new Runnable() {
                        public void run() {
                            Toast.makeText(overlayButton.getContext(), R.string.bluetooth_busy, Toast.LENGTH_LONG).show();
                        }
                    });
                    return;
                }
                bluetoothBusy = true;

                bluetoothAdapter = BluetoothAdapter.getDefaultAdapter();
                if(!bluetoothAdapter.isEnabled()) {
                    handler.post(new Runnable() {
                        public void run() {
                            Toast.makeText(overlayButton.getContext(), R.string.bluetooth_not_activated, Toast.LENGTH_LONG).show();
                        }
                    });
                    return;
                }

                // Save data to for auto reconnect and activate reconnecting
                lastDeviceAddress = deviceAddress;
                lastDeviceSecure = deviceSecure;
                if(invokeAutoReconnect) {
                    bluetoothReconnectHandler.postDelayed(bluetoothReconnect, BLUETOOTH_RECONNECT_INTERVAL_MS);
                }

                // Get the BluetoothDevice object and attempt to connect to the device
                bluetoothDevice = bluetoothAdapter.getRemoteDevice(deviceAddress);
                int deviceType = bluetoothDevice.getType();
                if(deviceType == BluetoothDevice.DEVICE_TYPE_CLASSIC) {
                    try {
                        bluetoothSocket = bluetoothDevice.createRfcommSocketToServiceRecord(SPP_UUID);
                    } catch (final IOException ex) {
                        handler.post(new Runnable() {
                            public void run() {
                                Toast.makeText(overlayButton.getContext(), ex.toString(), Toast.LENGTH_LONG).show();
                            }
                        });
                        bluetoothSocket = null;
                        bluetoothBusy = false;
                        return;
                    }
                    try {
                        bluetoothSocket.connect();
                    } catch (final IOException ex) {
                        handler.post(new Runnable() {
                            public void run() {
                                if (overlayButton != null) {
                                    Toast.makeText(overlayButton.getContext(), ex.toString(), Toast.LENGTH_LONG).show();
                                }
                            }
                        });
                        bluetoothSocket = null;
                        bluetoothBusy = false;
                        return;
                    }
                    try {
                        bluetoothOutputStream = bluetoothSocket.getOutputStream();
                        bluetoothInputStream = bluetoothSocket.getInputStream();
                    } catch (final IOException ex1) {
                        handler.post(new Runnable() {
                            public void run() {
                                Toast.makeText(overlayButton.getContext(), ex1.toString(), Toast.LENGTH_LONG).show();
                            }
                        });
                        try {
                            bluetoothSocket.close();
                        } catch (final IOException ex2) {
                            handler.post(new Runnable() {
                                public void run() {
                                    Toast.makeText(overlayButton.getContext(), ex2.toString(), Toast.LENGTH_LONG).show();
                                }
                            });
                            bluetoothSocket = null;
                        }
                        bluetoothOutputStream = null;
                        bluetoothInputStream = null;
                        bluetoothBusy = false;
                        return;
                    }

                    // Connection established
                    handler.post(new Runnable() {
                        public void run() {
                            startBluetoothWorkerThread();
                            overlayButton.setImageResource(R.drawable.sevenseg_empty);
                            Toast.makeText(overlayButton.getContext(), "Connected to " + bluetoothDevice.getName() + ".", Toast.LENGTH_SHORT).show();
                        }
                    });
                } else if(deviceType == BluetoothDevice.DEVICE_TYPE_LE || deviceType == BluetoothDevice.DEVICE_TYPE_DUAL) {
                    gattCallback = new BluetoothGattCallback() {
                        @Override
                        public void onConnectionStateChange(BluetoothGatt gatt, int status, int newState) {
                            if(newState == BluetoothProfile.STATE_CONNECTED) {
                                bleConnected = true;
                                bluetoothGatt.discoverServices();
                                handler.post(new Runnable() {
                                    public void run() {
                                        Toast.makeText(overlayButton.getContext(), "Conn", Toast.LENGTH_SHORT).show();
                                    }
                                });
                            } else if(newState == BluetoothProfile.STATE_DISCONNECTED) {
                                bleConnected = false;
                                handler.post(new Runnable() {
                                    public void run() {
                                        if(overlayButton != null) {
                                            Toast.makeText(overlayButton.getContext(), "Disconn", Toast.LENGTH_SHORT).show();
                                        }
                                    }
                                });
                            }
                        }

                        @Override
                        public void onServicesDiscovered(BluetoothGatt gatt, int status) {
                            if(status == BluetoothGatt.GATT_SUCCESS) {
                                for(BluetoothGattService service : gatt.getServices()) {
                                    if(service.getUuid() == BLE_UART_UUID) {
                                        for(BluetoothGattCharacteristic characteristic : service.getCharacteristics()) {
                                            if(characteristic.getUuid() == BLE_CHAR_TX_UUID) {
                                                bluetoothGatt.setCharacteristicNotification(characteristic, true);
                                            }
                                        }
                                    }
                                }
                            } else {
                                handler.post(new Runnable() {
                                    public void run() {
                                        Toast.makeText(overlayButton.getContext(), "Error Service Disco", Toast.LENGTH_LONG).show();
                                    }
                                });
                            }
                        }

                        @Override
                        public void onCharacteristicChanged(BluetoothGatt gatt, BluetoothGattCharacteristic characteristic) {
                            final byte[] data = characteristic.getValue();
                            if(data != null && data.length > 0) {
                                handler.post(new Runnable() {
                                    public void run() {
                                        //Toast.makeText(overlayButton.getContext(), data.toString(), Toast.LENGTH_SHORT).show();
                                        processReceivedData(data.toString());
                                    }
                                });
                            }
                        }
                    };
                    bluetoothGatt = bluetoothDevice.connectGatt(overlayButton.getContext(), false, gattCallback);
                }
                bluetoothBusy = false;
            }
        }).start();
    }


    // Check if Bluetooth is connected //
    public synchronized boolean isBluetoothConnected() {
        if(bluetoothSocket != null) {
            return bluetoothSocket.isConnected();
        } else if(bluetoothGatt != null) {
            return bleConnected;
        }
        return false;
    }


    // Disconnect Bluetooth device if connected //
    public synchronized void disconnectBluetooth(boolean keepReconnecting) {
        if(!keepReconnecting) {
            bluetoothReconnectHandler.removeCallbacksAndMessages(null);
        }
        if(bluetoothBusy) {
            Toast.makeText(this, R.string.bluetooth_busy, Toast.LENGTH_LONG).show();
            return;
        }
        if(!isBluetoothConnected()) {
            return;
        }
        bluetoothBusy = true;
        stopBluetoothWorkerThread = true;
        if(bluetoothSocket != null) {
            while (bluetoothWorkerThread.isAlive()) {
                SystemClock.sleep(1);
            }
            if(bluetoothInputStream != null) {
                try {
                    bluetoothInputStream.close();
                } catch(IOException ex) {
                    Toast.makeText(this, ex.toString(), Toast.LENGTH_LONG).show();
                }
                bluetoothInputStream = null;
            }
            if(bluetoothOutputStream != null) {
                try {
                    bluetoothOutputStream.close();
                } catch(IOException ex) {
                    Toast.makeText(this, ex.toString(), Toast.LENGTH_LONG).show();
                }
                bluetoothOutputStream = null;
            }
            if(bluetoothSocket != null) {
                try {
                    bluetoothSocket.close();
                } catch(IOException ex) {
                    Toast.makeText(this, ex.toString(), Toast.LENGTH_LONG).show();
                }
                bluetoothSocket = null;
            }
        }
        if(bluetoothGatt != null) {
            bluetoothGatt.disconnect();
            bluetoothGatt.close();
            bluetoothGatt = null;
        }

        overlayButton.setImageResource(R.drawable.sevenseg_dot);
        bluetoothBusy = false;
    }


    // Bind callback //
    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }


    // Create callback //
    @Override
    public void onCreate() {
        super.onCreate();
        sendDataBroadcastIntent(TEST_DATAFRAME);
        this.registerReceiver(broadcastReceiver, new IntentFilter("android.bluetooth.device.action.ACL_DISCONNECTED"));
        setupOverlay();
        setupBluetoothReconnect();
    }


    // Destroy callback //
    @Override
    public void onDestroy() {
        this.unregisterReceiver(broadcastReceiver);
        disconnectBluetooth(false);
        stopDataLogging();

        if(overlayButton != null) {
            windowManager.removeView(overlayButton);
            windowManager.removeView(topCenterView);
            overlayButton = null;
            topCenterView = null;
        }
        super.onDestroy();
    }


    // Overlay widget touch callback //
    @Override
    public boolean onTouch(View v, MotionEvent event) {
        if(event.getAction() == MotionEvent.ACTION_DOWN) {
            // Retrieve absolute position of down event
            float downEventX = event.getRawX();
            float downEventY = event.getRawY();

            widgetMoving = false;

            // Retrieve absolute (top center) position of widget
            int[] location = new int[2];
            overlayButton.getLocationOnScreen(location);
            initialWidgetX = location[0] + overlayButton.getWidth()/2; // Offset to horizontal center of widget as this is the reference point (invisible view)
            initialWidgetY = location[1];

            // Calculate relative position of touch event to top center position of widget
            eventRelativeX = initialWidgetX - downEventX;
            eventRelativeY = initialWidgetY - downEventY;
        } else if(event.getAction() == MotionEvent.ACTION_MOVE) {
            // Retrieve absolute position of move event
            float moveEventX = event.getRawX();
            float moveEventY = event.getRawY();

            // Calculate new relative position
            WindowManager.LayoutParams params = (WindowManager.LayoutParams) overlayButton.getLayoutParams();
            int newX = (int) (eventRelativeX + moveEventX);
            int newY = (int) (eventRelativeY + moveEventY);

            // Nothing to do if differences in both directions without prior movement are too small
            if(Math.abs(newX - initialWidgetX) < 1 && Math.abs(newY - initialWidgetY) < 1 && !widgetMoving) {
                return false;
            }

            // Retrieve absolute position of invisible view
            int[] topCenterLocationOnScreen = new int[2];
            topCenterView.getLocationOnScreen(topCenterLocationOnScreen);

            // Calculate and apply new absolute position of overlay widget with position of invisible view
            params.x = newX - (topCenterLocationOnScreen[0]);
            params.y = newY - (topCenterLocationOnScreen[1]);
            windowManager.updateViewLayout(overlayButton, params);

            // Set flag that widget has been moved
            widgetMoving = true;
        } else if(event.getAction() == MotionEvent.ACTION_UP) {
            if(widgetMoving) {
                return true;
            }
        }
        return false;
    }


    // Overlay widget click callback //
    @Override
    public void onClick(View v) {
        Intent mainActivityIntent = new Intent(this, MainActivity.class);
        mainActivityIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(mainActivityIntent);
    }
}