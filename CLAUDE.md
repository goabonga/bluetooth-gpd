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
- `avdtp`: A2DP stream setup and media transport over AVDTP
- `avrcp`: remote control and hardware volume of audio devices over AVCTP
- `sdp`: service discovery of Classic devices
- `hfp`: hands-free calls, voice link and headset microphone
- `audio`: SBC audio streaming to a Bluetooth sink
- `alsa`: ALSA plugin sending a PCM stream to the daemon
- `ui`: terminal interface built on ctui
- `cli`: command line and program entry point
