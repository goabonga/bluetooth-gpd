# bluetooth-gpd

Terminal Bluetooth manager for Linux, built on ctui.

## Commit scopes

One scope per module. Never invent another one; ask or omit it.

- `device`: Bluetooth device table and address formatting
- `store`: persistent pairing keys
- `mgmt`: Linux kernel Bluetooth Management API client
- `hog`: BLE HID devices served through ATT and uhid
- `daemon`: background service that keeps the devices connected
- `ipc`: control protocol between the daemon and the interface
- `ui`: terminal interface built on ctui
- `cli`: command line and program entry point
