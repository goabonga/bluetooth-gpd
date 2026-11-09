# Changelog

All notable changes to this project are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/).

## [0.1.1] - 2026-11-09

### Fixed

- install the ALSA definitions in /etc/alsa/conf.d (`b535f5b`)

## [0.1.0] - 2026-11-09

### Added

- add the cli printing its version, built with make (`7584713`)
- **device**: add the device table and address formatting (`e65f296`)
- **store**: persist pairing keys in a private locked directory (`7c8c883`)
- **mgmt**: enumerate adapters, scan and track devices via the kernel (`444b88a`)
- **mgmt**: pair, answer authentication prompts, unpair and disconnect (`ecbf1ba`)
- **cli**: replace the version stub with the ctui device browser (`df4ccdc`)
- **mgmt**: log the management packets to a file with --trace (`b1a16b5`)
- **mgmt**: timestamp the packets of the --trace log (`46c31c0`)
- **mgmt**: detect a running bluetoothd that refuses pairing requests (`d2acc87`)
- **ui**: show the adapter warning above the status line (`54281d3`)
- **cli**: warn at start when bluetoothd is running (`db9d3f4`)
- **hog**: connect a BLE HID device and forward its reports to uhid (`9134f15`)
- **ui**: connect BLE HID devices with c and after a pairing (`a0f2491`)
- **cli**: poll the BLE connection and close it on exit (`096fd1e`)
- **hog**: log the connection steps to the --trace file (`6f628f8`)
- **ui**: give the BLE connection the --trace file (`50e628e`)
- **daemon**: add bluetooth-daemon keeping paired LE devices connected (`4a98cc5`)
- **ipc**: define the snapshot and command format of the protocol (`b14e589`)
- **ipc**: serve the control protocol on a Unix socket (`755e570`)
- **ipc**: add the client side that mirrors the daemon state (`4fd1a76`)
- **daemon**: serve interfaces on the control socket (`2914e1b`)
- **ui**: drive the daemon through the control socket (`bc666c9`)
- **cli**: connect the interface to the daemon instead of the kernel (`1aa6eba`)
- **mgmt**: make the adapter visible so other devices can pair (`a05f5fc`)
- **mgmt**: ask the user about pairing requests from other devices (`b5afe47`)
- let the interface make the adapter visible through the daemon (`36a6f06`)
- **daemon**: accept pairing requests only while an interface is open (`e8038a0`)
- **ui**: add the v key to make the adapter visible (`8e554da`)
- **avdtp**: negotiate and open an A2DP SBC stream with a sink (`dbeab6d`)
- **avdtp**: keep the SBC codec information that was configured (`065caa6`)
- **audio**: encode a test tone to SBC and stream it in real time (`871a033`)
- connect Classic audio devices and play a test tone via the daemon (`0750515`)
- **ui**: add the t key to play a test tone on an audio device (`a983405`)
- **audio**: stream the PCM of a client of the audio socket (`d578eda`)
- **daemon**: serve the audio socket for PCM clients (`53d847a`)
- **alsa**: add the bluetooth PCM plugin that plays through the daemon (`b178df1`)
- **alsa**: ship the bluetooth and bluetooth_out PCM definitions (`d93af4b`)
- publish the audio device and its volume to the interfaces (`4cd0791`)
- **ui**: show the audio device and change its volume with + and - (`5fa8c73`)
- **avrcp**: follow and set the hardware volume of an audio device (`6e5d7bf`)
- **audio**: use the hardware volume of the device when it has one (`a86ee4b`)
- **sdp**: find the RFCOMM channel of a service of a Classic device (`70043e3`)
- **hfp**: establish the hands-free control link with a headset (`1259c8e`)
- **hfp**: carry the headset voice on a SCO link in CVSD or mSBC (`e450a8a`)
- **hfp**: serve the headset microphone to a capture client (`df7b79c`)
- **daemon**: serve the capture socket for the headset microphone (`e7ac1d4`)
- **audio**: accept stream clients and read which device they want (`59122cd`)
- **audio**: give a device a client whose greeting was read (`7cac7e3`)
- **hfp**: give the microphone to a client whose greeting was read (`cde5521`)
- publish every audio device and address the volume command (`2546f2b`)
- serve several audio devices, each stream to the one it names (`d670795`)
- **ipc**: serve the control socket to a group from /run/bluetooth (`56b1f3d`)
- **audio**: serve the stream sockets to a group from /run/bluetooth (`7709304`)

### Fixed

- **ui**: open the authentication dialog when the prompt arrives (`d0bb710`)
- **mgmt**: stop the scan before pairing (`999c8fc`)
- **mgmt**: forget a scan that the kernel has ended (`48394f4`)
- **hog**: bind the ATT channel before connecting it, like BlueZ (`48cedc8`)
- **mgmt**: mention nosuid filesystems when CAP_NET_ADMIN is missing (`e30d183`)
- **daemon**: report a too long socket path, not a missing directory (`e2866b9`)
- **cli**: report a too long daemon socket path, not a missing directory (`f293d92`)
- **mgmt**: point to the systemd service when CAP_NET_ADMIN is missing (`8b8b1a4`)
- **ipc**: name the system service in the waiting message (`b101af9`)
- **build**: drop the uaccess tag of /dev/uhid so the group can open it (`c89309d`)
