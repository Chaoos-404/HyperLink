# Hyperlink

Hyperlink is a C++20 library skeleton for high-speed peer-to-peer data transfer between computers over a USB link, with automatic peer discovery and connection configuration.

## Development Setup

On macOS:

```sh
brew install cmake ninja pkg-config libusb
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

On Linux:

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config libusb1-devel

cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

On Windows, use one of these dependency paths:

```powershell
# vcpkg
vcpkg install libusb
cmake --preset debug -DCMAKE_TOOLCHAIN_FILE=C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build --preset debug
ctest --preset debug
```

```sh
# MSYS2 MinGW shell
pacman -S mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libusb
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

If `libusb` is not installed, the project still builds and tests the portable library layer. The USB probing example will print that USB support is disabled.

## Project Layout

- `include/hyperlink/` public headers
- `src/` library implementation and transport backends
- `examples/` small programs for manual testing
- `tests/` assert-based smoke tests wired into CTest

## Performance Target

The long-term goal is to use the fastest transport exposed by the connected hardware, including USB4-class links when the OS and cable/device path make that available.

Important constraints:

- A USB4 port is not automatically a raw peer-to-peer data pipe between two computers.
- Generic `libusb` code can work well for USB devices with bulk endpoints, but it normally sees USB device speeds exposed by the operating system, not raw USB4 fabric bandwidth.
- Reaching USB4 20Gbps, 40Gbps, or 80Gbps class throughput may require a USB4/Thunderbolt networking interface, PCIe tunneling, a certified bridge device, or a custom device-mode/gadget endpoint depending on platform.
- Hyperlink should negotiate the best available link and report the detected/theoretical speed, then benchmark real application throughput separately.

## USB4/Thunderbolt Network Benchmark

When the OS exposes a USB4 or Thunderbolt connection as a network interface, use the TCP benchmark transport to measure the real application-level throughput.

On the receiving machine:

```sh
./build/release/hyperlink_bench --server --host 0.0.0.0 --port 47777
```

On the sending machine, use the receiver's IP address on the USB4/Thunderbolt network interface:

```sh
./build/release/hyperlink_bench --client --host <receiver-usb4-ip> --port 47777 --seconds 10
```

For faster links, run multiple TCP streams and larger socket buffers:

```sh
# receiver
./build/release/hyperlink_bench --server --host 0.0.0.0 --port 47777 \
  --parallel 8 --socket-buffer-bytes 8388608

# sender
./build/release/hyperlink_bench --client --host <receiver-usb4-ip> --port 47777 \
  --seconds 10 --chunk-bytes 4194304 --parallel 8 --socket-buffer-bytes 8388608
```

`--parallel 8` uses ports `47777` through `47784`, so allow that range through the firewall if needed.

Helpful commands for finding the right interface:

```sh
# macOS
ifconfig
networksetup -listallhardwareports

# Linux
ip addr
ip route

# Windows PowerShell
Get-NetAdapter
Get-NetIPAddress
```

The benchmark uses TCP intentionally. That lets the operating system select the high-speed USB4/Thunderbolt network interface when the peer IP belongs to that link, while the same code remains portable across macOS, Linux, and Windows.

## Deploy Server Over The USB4/Thunderbolt Link

From the MacBook, you can push the current source to the Mac mini over the USB4/Thunderbolt network IP, rebuild it there, and start the benchmark server:

```sh
scripts/deploy_server.sh --host 169.254.119.3 --user user
```

That command uses SSH and rsync. On the Mac mini, enable **System Settings > General > Sharing > Remote Login** first, then check that SSH works:

```sh
ssh user@169.254.119.3
```

The deploy script defaults to:

- remote directory: `~/Hyperlink`
- CMake preset: `release`
- server ports: `47777..47784`
- parallel streams: `8`
- socket buffer: `8388608`

After deployment, run the client from the MacBook:

```sh
./build/release/hyperlink_bench --client --host 169.254.119.3 --port 47777 \
  --seconds 10 --chunk-bytes 4194304 --parallel 8 --socket-buffer-bytes 8388608
```

Watch the remote server log:

```sh
ssh user@169.254.119.3 'tail -f ~/Hyperlink/hyperlink_server.log'
```

## File Transfer

Use `hyperlink_file` for one-shot file transfer over the same USB4/Thunderbolt network path.

On the receiving computer:

```sh
./build/release/hyperlink_file --receive --host 0.0.0.0 --port 47790 --output-dir ~/Downloads
```

On the sending computer:

```sh
./build/release/hyperlink_file --send --host <receiver-bridge0-ip> --port 47790 --file /path/to/file
```

Example using the Mac mini Thunderbolt Bridge IP:

```sh
./build/release/hyperlink_file --send --host 169.254.50.61 --port 47790 --file ./large-video.mov
```

The receiver saves only the base filename, so paths from the sender are not recreated on the receiving machine.

Directories and `.app` bundles can be sent directly:

```sh
./build/release/hyperlink_file --send --host 169.254.50.61 --port 47790 \
  --file /Applications/MATLAB_R2026a.app
```

For large regular files and big directories, use parallel streams:

```sh
# receiver
./build/release/hyperlink_file --receive --host 0.0.0.0 --port 47790 \
  --output-dir ~/Downloads --parallel 8

# sender
./build/release/hyperlink_file --send --host 169.254.50.61 --port 47790 \
  --file ./large-file.tar --parallel 8

# sender, app bundle/direct directory
./build/release/hyperlink_file --send --host 169.254.50.61 --port 47790 \
  --file /Applications/MATLAB_R2026a.app --parallel 8
```

With `--parallel`, directory transfer uses Hyperlink's native directory mode instead of tar. It transfers files across multiple sockets, recreates directories, preserves regular file permissions, and recreates symlinks. It can still be limited by many small filesystem writes, but it avoids the slow pre-tar step.

## Windows Laptop Receiver

Install MSYS2 from <https://www.msys2.org>, then open **MSYS2 UCRT64** and install the build tools:

```sh
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-pkgconf
```

Build in the **MSYS2 UCRT64** shell:

```sh
cmake --preset release -DHYPERLINK_ENABLE_LIBUSB=OFF
cmake --build --preset release
```

Find the Windows laptop IP on the USB4/Thunderbolt network:

```powershell
Get-NetAdapter
Get-NetIPAddress
```

Allow the benchmark and file-transfer ports through Windows Firewall:

```powershell
New-NetFirewallRule -DisplayName "Hyperlink Bench" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 47777-47797
New-NetFirewallRule -DisplayName "Hyperlink File" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 47790-47797
```

Start a Windows receiver:

```sh
./build/release/hyperlink_file.exe --receive --host 0.0.0.0 --port 47790 \
  --output-dir /c/Users/<your-user>/Downloads --parallel 8
```

Send from macOS to the Windows laptop:

```sh
./build/release/hyperlink_file --send --host <windows-usb4-ip> --port 47790 \
  --file /path/to/file-or-directory --parallel 8
```

Windows may block symlink creation unless Developer Mode or elevated permissions are enabled. If symlink creation fails, Hyperlink writes a small `*.hyperlink-symlink.txt` file containing the symlink target instead of failing the whole transfer.

## Current Milestones

1. Define the public session, transport, and auto-configuration API.
2. Build a libusb discovery backend for compatible USB data-transfer devices.
3. Add the wire protocol handshake for role selection, packet sizing, encryption preference, and endpoint capabilities.
4. Add throughput and latency benchmarks using loopback/fake transport first, then real USB hardware.
5. Add OS-specific high-speed transports where useful, such as Thunderbolt/USB4 networking or PCIe-tunneled devices.

## Notes

Normal USB cables do not make two computers peers by themselves; one side normally acts as host and the other as device. For computer-to-computer transfer, plan around a USB bridge/data-transfer cable, a USB gadget/device-capable board, or another link that exposes bulk endpoints the library can claim.

The public API is intentionally platform-neutral. OS-specific work should stay behind `Transport` implementations so the session and auto-configuration layers remain portable.
