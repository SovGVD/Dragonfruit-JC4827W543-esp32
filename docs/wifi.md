# Palm PPP internet over Wi-Fi

The JC4827W543 build includes an internal PPP provider. Palm OS uses its normal
serial networking driver; the ESP32 terminates PPP and routes IPv4 traffic onto
its Wi-Fi station interface with NAT. No external adapter or serial cable is
needed. This feature contains Wi-Fi settings, PPP, DNS, internet routing and a
web proxy for Palm browsers.
It does not include IrDA, HotSync services, Google integration, diagnostics pages,
or request logs.

## Enable it

Build with `./tools/build-jc4827w543.sh` and flash the combined
`build-jc4827w543/dragonfruit-jc4827w543.bin` at address `0x0`, using the
[board guide](jc4827w543.md#flash).

The JC4827W543 default is now `serial.mode=wifi`. **An existing SD `palm.cfg`
containing `serial.mode=console` or `auto` overrides that default.** Change it to:

```ini
serial.mode = wifi
```

Restart after changing this setting. Wi-Fi settings are remembered in ESP32 NVS
flash independently of the Palm RAM checkpoint and the SD card. Up to five
networks can be saved. UART0 remains the debug console at 115200 baud; native
USB continues carrying boot output. UART injection, loopback and live serial
mode changes are rejected while PPP owns the Palm's serial port.

Set `serial.mode=console` and restart to leave the Wi-Fi gateway inactive.
`auto` and `passthrough` retain the physical UART0 bridge. To omit the gateway
from a build, disable `CONFIG_PALM_WIFI_GATEWAY` in menuconfig. Other board
profiles do not enable it by default.

## Connect Palm OS

1. Use a **direct serial/cable connection**, not infrared or a modem. In Palm OS
   4, create a connection in Prefs → Connection: Connect to **PC**, Via
   **Cradle/Cable**. A speed such as 57600 is fine; no physical UART carries
   these bytes.
2. In Prefs → Network, create/select a service using that connection. Use any
   user name, leave the password and phone number empty, and remove login
   scripts (Script should contain only **End**).
3. Under Details, select **PPP**, automatic IP address and **Query DNS**.
4. Tap **Connect**. The Palm receives `192.168.99.2`; its gateway and DNS server
   are `192.168.99.1`. PPP can connect before any Wi-Fi network is configured.
5. In a Palm browser that supports direct HTTP, open **`http://wifi.setup/`**
   or **`http://192.168.99.1/`**.
6. Tap **Scan**, refresh after a few seconds, choose a network and enter its
   password. Alternatively type the SSID; this also permits joining a hidden
   network immediately. Tap **Save and connect**, then refresh to see the result.

The portal provides Wi-Fi status, nearby personal/open networks, add/update and
forget. To change a password, submit the same SSID again. Stored passwords are
not displayed. It uses plain HTML forms without JavaScript. The HTTP and DNS
services bind to the PPP gateway address and accept the negotiated Palm peer
only; they do not provide a configuration portal on the Wi-Fi LAN.

Open networks use an empty password. WPA/WPA2/WPA3 personal networks use an
8–63 character ASCII passphrase or a 64-digit hexadecimal PSK. Enterprise/WEP
networks are not offered. The ESP32-S3 uses 2.4 GHz Wi-Fi. After startup or a
disconnect it scans and tries saved networks in signal-strength order, with
another scan after 20 seconds if none connect. A newly entered SSID is tried
first even when hidden; automatic discovery after a restart requires a visible
SSID. The status page reports connection failures without printing credentials.

## Internet behavior and limits

The **web proxy is on by default**. As in the original gateway, the Palm's DNS
queries are answered with the PPP gateway address so browser HTTP requests reach
the ESP32. It fetches the requested site over Wi-Fi, follows up to four redirects
including HTTP → HTTPS, and returns the result to Palm over plain HTTP. If an
HTTP site cannot be opened, a GET is retried over HTTPS (port 443 for port 80,
or the same custom port). HTTPS connections use ESP-IDF's certificate bundle;
certificate failures return an error. TLS ends at the ESP32; the Palm leg is HTTP.

The streaming filter removes scripts, styles, SVG, comments, pictures and
stylesheet links; converts UTF-8 to Palm-friendly Latin-1/ASCII; and rewrites
`https://` links to `http://`. Text and HTML pages are relayed up to 48,000
output bytes. Oversized/slow pages are cut off with a notice. Compressed or
binary responses are not relayed. Form POSTs are supported; 307/308 redirects
preserve the form, while 301/302/303 redirects fetch the destination with GET.
A failed POST is not automatically replayed on another connection.

Use **Turn proxy off** on `http://wifi.setup/` for mail, telnet, IRC or other
applications that need real server addresses. This setting persists in NVS.
Disconnect and reconnect PPP after toggling it to clear cached DNS results.
With the proxy off, other names are relayed to the Wi-Fi network's DNS server
(or `1.1.1.1` if none was supplied), and IPv4 traffic goes directly through NAT.
`wifi.setup` always resolves locally, and the portal remains available before
Wi-Fi is configured.

Simple text sites work best; JavaScript applications still will not work.
Xiino works with **DataServer** cleared; its old DataServer URL wrapper is also
recognized while the proxy is on. WAP browsers requiring a separate WSP gateway
are unsupported.
The fixed PPP addresses must not overlap the upstream Wi-Fi subnet.

Disconnect through Prefs → Network when finished. Palm warm resets and sustained
serial-port closes end the old PPP session and discard its queued bytes. Short
UART reconfiguration gaps are debounced. Wi-Fi scanning, socket handling and
PPP lifecycle run outside the emulation task; bounded stream buffers pace
traffic into the emulated UART. Overload can drop PPP output, with recovery
provided by the application's transport protocol.

## Verification

```sh
cmake -S src/host -B build-host
cmake --build build-host -j 4
ctest --test-dir build-host --output-on-failure
./tools/test-ppp-stack.sh
./tools/build-jc4827w543.sh
```

Host checks exercise form decoding, HTML escaping, partial HTTP requests,
malformed/oversized DNS questions, response matching, UART back-pressure and
reset isolation. The separate SDK stack test compiles ESP-IDF's actual lwIP PPP
implementation for a native, synchronous pair of peers. It negotiates LCP/IPCP,
verifies both fixed addresses and DNS assignment, and repeats three sessions.
It does not exercise the device task scheduler or its queues. The ESP-IDF build
checks the real PPP/Wi-Fi APIs, linking and partition fit.

Proxy checks cover host-based routing, absolute URLs, Xiino request wrappers,
relative/HTTPS redirects, streamed HTML/UTF-8 across chunk boundaries, form
handling, unsupported resources and the page limit. A host adapter test runs
the production fetching loop against simulated ESP-IDF HTTP responses and local
stream writes; it checks certificate-bundle configuration, not a real TLS handshake.

Validated locally on 2026-10-17: all 14 host tests and the SDK PPP stack test
pass. Form/DNS parsing and UART checks also pass AddressSanitizer and
UndefinedBehaviorSanitizer (leak detection disabled because the sandbox tracer
prevents LeakSanitizer). The combined firmware image builds; the application
uses about 1.2 MB of its 3 MB partition, leaving 62% free.

These checks do not establish a successful PPP handshake with a
Palm ROM, Wi-Fi association on the board or actual internet browsing. Those
require flashing and checking Connect → portal → Wi-Fi → internet, followed by
disconnect/reconnect and a Palm reset on the real device.

Uploaded on 2026-10-17 to the connected ESP32-S3 revision 0.2. Esptool verified
the bootloader, partition table and application hashes. Separate flash regions
preserved NVS and SD contents. The boot capture confirms the Palm RAM checkpoint
was restored, Wi-Fi station initialized, `PPP Wi-Fi ready` was reported and the
emulator started. Real Palm PPP connection, Wi-Fi association and HTTPS browsing
have not yet been verified on the board.
