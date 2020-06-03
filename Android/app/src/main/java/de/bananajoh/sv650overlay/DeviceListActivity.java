package de.bananajoh.sv650overlay;

import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Bundle;
import android.text.Html;
import android.text.Spanned;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.Window;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;

import java.util.Set;


public class DeviceListActivity extends AppCompatActivity {
    private BluetoothAdapter bluetoothAdapter = null;
    private ArrayAdapter<Spanned> newDevicesArrayAdapter;
    public static String EXTRA_DEVICE_ADDRESS = "device_address";


    // Return the device type as string //
    private String deviceTypeString(int deviceType) {
        switch(deviceType) {
            case BluetoothDevice.DEVICE_TYPE_CLASSIC: {
                return "Classic";
            }
            case BluetoothDevice.DEVICE_TYPE_LE: {
                return "Low Energy";
            }
            case BluetoothDevice.DEVICE_TYPE_DUAL: {
                return "Dual";
            }
            default: {
                return "Unknown";
            }
        }
    }

    // Listen for device discovery broadcasts //
    private final BroadcastReceiver broadcastReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if(action.equals(BluetoothDevice.ACTION_FOUND)) {
                BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
                if(device != null && device.getBondState() != BluetoothDevice.BOND_BONDED) {
                    newDevicesArrayAdapter.add(Html.fromHtml("<b>" + device.getName() + "</b> <i>" + deviceTypeString(device.getType()) + "</i><br>" + device.getAddress()));
                }
            } else if(action.equals(BluetoothAdapter.ACTION_DISCOVERY_FINISHED)) {
                setProgressBarIndeterminateVisibility(false);
                setTitle(R.string.bluetooth_title_select_device);
                if(newDevicesArrayAdapter.getCount() == 0) {
                    Spanned noDevices = Html.fromHtml("<i>" + getResources().getText(R.string.bluetooth_no_new_devices).toString() + "</i>");
                    newDevicesArrayAdapter.add(noDevices);
                }
            }
        }
    };


    // On-click listener for all devices in the ListViews //
    private AdapterView.OnItemClickListener deviceClickListener = new AdapterView.OnItemClickListener() {
        public void onItemClick(AdapterView<?> av, View v, int arg2, long arg3) {
            bluetoothAdapter.cancelDiscovery();

            // Get the device MAC address, which is the last 17 chars in the View
            String info = ((TextView) v).getText().toString();
            String address = info.substring(info.length() - 17);

            // Create the result Intent and include the MAC address
            Intent intent = new Intent();
            intent.putExtra(EXTRA_DEVICE_ADDRESS, address);

            // Set result and finish this Activity
            setResult(Activity.RESULT_OK, intent);
            finish();
        }
    };


    // Discover nearby Bluetooth devices //
    private void bluetoothDiscovery() {
        newDevicesArrayAdapter.clear();
        setProgressBarIndeterminateVisibility(true); // Indicate scanning in the title
        setTitle(R.string.bluetooth_title_scanning);
        findViewById(R.id.bluetooth_title_new_devices).setVisibility(View.VISIBLE);
        if(bluetoothAdapter.isDiscovering()) {
            bluetoothAdapter.cancelDiscovery();
        }
        bluetoothAdapter.startDiscovery();
    }


    // Create callback //
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        // Setup the window
        requestWindowFeature(Window.FEATURE_INDETERMINATE_PROGRESS);
        setContentView(R.layout.activity_device_list);
        Toolbar toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);

        // Set result CANCELED in case the user backs out
        setResult(Activity.RESULT_CANCELED);

        // Initialize array adapters. One for already paired devices and one for newly discovered devices
        ArrayAdapter<Spanned> pairedDevicesArrayAdapter = new ArrayAdapter<Spanned>(this, R.layout.device_list_entry);
        newDevicesArrayAdapter = new ArrayAdapter<Spanned>(this, R.layout.device_list_entry);

        // Find and set up the ListView for paired devices
        ListView pairedListView = findViewById(R.id.bluetooth_paired_devices);
        pairedListView.setAdapter(pairedDevicesArrayAdapter);
        pairedListView.setOnItemClickListener(deviceClickListener);

        // Find and set up the ListView for newly discovered devices
        ListView newDevicesListView = findViewById(R.id.bluetooth_new_devices);
        newDevicesListView.setAdapter(newDevicesArrayAdapter);
        newDevicesListView.setOnItemClickListener(deviceClickListener);

        // Register for broadcasts when a device is discovered
        IntentFilter filter = new IntentFilter(BluetoothDevice.ACTION_FOUND);
        this.registerReceiver(broadcastReceiver, filter);

        // Register for broadcasts when discovery has finished
        filter = new IntentFilter(BluetoothAdapter.ACTION_DISCOVERY_FINISHED);
        this.registerReceiver(broadcastReceiver, filter);

        // Get the local Bluetooth adapter
        bluetoothAdapter = BluetoothAdapter.getDefaultAdapter();

        // Get a set of currently paired devices
        Set<BluetoothDevice> pairedDevices = bluetoothAdapter.getBondedDevices();

        // If there are paired devices, add each one to the ArrayAdapter
        if(pairedDevices.size() > 0) {
            findViewById(R.id.bluetooth_title_paired_devices).setVisibility(View.VISIBLE);
            for(BluetoothDevice device : pairedDevices) {
                pairedDevicesArrayAdapter.add(Html.fromHtml("<b>" + device.getName() + "</b> <i>" + deviceTypeString(device.getType()) + "</i><br>" + device.getAddress()));
            }
        } else {
            Spanned noDevices = Html.fromHtml("<i>" + getResources().getText(R.string.bluetooth_no_paired_devices).toString() + "</i>");
            pairedDevicesArrayAdapter.add(noDevices);
        }

        // Start device discovery
        bluetoothDiscovery();
    }


    // Destroy callback //
    @Override
    public void onDestroy() {
        if(bluetoothAdapter != null) {
            bluetoothAdapter.cancelDiscovery();
        }
        this.unregisterReceiver(broadcastReceiver);
        super.onDestroy();
    }


    // Options menu creation callback //
    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_device_list, menu);
        return true;
    }


    // Options menu item selected callback //
    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        // Handle action bar item clicks here. The action bar will
        // automatically handle clicks on the Home/Up button, so long
        // as you specify a parent activity in AndroidManifest.xml.
        int id = item.getItemId();

        //noinspection SimplifiableIfStatement
        if(id == R.id.action_scan_bluetooth) {
            bluetoothDiscovery();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }
}
