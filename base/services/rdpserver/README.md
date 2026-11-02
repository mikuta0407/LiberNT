# LiberNT RDP console server

`rdpserver.exe` is the Remote Desktop server of LiberNT. Like Windows 11 Pro,
it serves the console session to one client at a time once Remote Desktop is
turned on; a client that signs in replaces the one already connected. FreeRDP implements the wire protocol, TLS and CredSSP; the server
captures the desktop with GDI and injects keyboard and mouse input with
`SendInput` once a client has authenticated. It does not implement Terminal
Services or create remote sessions. The RDP drive redirection channel also
carries authenticated file transfers to a configured local directory.

## Building

The server is built outside the LiberNT toolchain, with an x86_64 MinGW
compiler for amd64 or an aarch64 MinGW compiler (llvm-mingw) for arm64,
against FreeRDP 3.32.0 and OpenSSL 3:

1. Apply `freerdp-rdpdr.patch` to FreeRDP 3.32.0 and build and install it
   with MinGW (`WITH_SERVER=ON`, `WITH_CHANNELS=ON`, `WITH_SERVER_CHANNELS=ON`
   and the `rdpdr` channel; the client, shadow and proxy parts are not
   needed). The patch keeps the 64-bit offsets of RDPDR read and write
   requests and handles a write acknowledgement as a length without a data
   payload ([MS-RDPEFS]).
2. Configure and build `mingw/CMakeLists.txt` against that installation and
   install it to the same prefix.
3. Add the OpenSSL and MinGW runtime DLLs next to the server, so that the
   install directory holds:

   ```
   bin/rdpserver.exe
   bin/libfreerdp3.dll  bin/libfreerdp-server3.dll  bin/libwinpr3.dll
   bin/ossl-modules/legacy.dll
   licenses/FreeRDP-LICENSE  licenses/OpenSSL-LICENSE
   ```

   and, for amd64, `bin/libssl-3-x64.dll`, `bin/libcrypto-3-x64.dll`,
   `bin/libgcc_s_seh-1.dll` and `bin/libwinpthread-1.dll`, or, for arm64,
   `bin/libssl-3.dll` and `bin/libcrypto-3.dll`. OpenSSL 3.5 has no
   aarch64 MinGW target: configure it with a target that inherits
   `mingw-common` with `SIXTY_FOUR_BIT` and `no-asm`.

Point an amd64 or arm64 build directory at the install directory of its
architecture (or set *System > RDP console server build directory* in
rosconfig):

```sh
cmake -DRDPSERVER_DIR=/path/to/install .
```

The live and preinstalled images then carry `%SystemRoot%\system32\rdpserver`
and the automatic `LiberNTRDPServer` service. `RDPSERVER_DIR` is empty by
default, which leaves the server out.

The service (`rdpserver.exe /service [ini]`) runs in session 0, which has no
interactive desktop. While `fDenyTSConnections` under
`HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server` is 0, it starts
the server as SYSTEM in the active console session and restarts it when it
exits or when the console session changes; it stops the server within seconds
when the value changes back to 1. *Settings > System > Remote Desktop* sets
this value, and it is 1 on a new installation, as on Windows 11.
The server follows the desktop that receives input, so a client sees and
drives the logon screen and the lock screen as well as the user's desktop.
Started by hand, the server ignores `fDenyTSConnections`, runs in the
caller's session and reaches the Winlogon desktop only when it runs as SYSTEM.

The server listens on port 3389 of every address and only accepts Network
Level Authentication (CredSSP): a client that offers only TLS or standard
RDP security gets `HYBRID_REQUIRED_BY_SERVER`. The credentials go through the
Negotiate package, and the server logs the user on with `LogonUser`. The
account needs a password, and it, or one of its enabled groups, must hold
`SeRemoteInteractiveLogonRight` without `SeDenyRemoteInteractiveLogonRight`;
a new installation grants the right to Administrators and Remote Desktop
Users. At its first start the server creates a self-signed certificate for
the computer name (RSA 2048, server authentication, valid 183 days) as
`server.crt` and `server.key` next to `rdpserver.exe`; the key is readable by
SYSTEM and Administrators only. It creates a new one when the certificate has
expired or either file is missing.

## Configuration

`rdpserver.ini` only holds the optional file transfer root. The in-tree file
leaves it out; to set it, keep a private directory outside the source tree
with an `rdpserver.ini` and point the build at it with
`-DRDPSERVER_CONFIG_DIR=/path/to/private/rdpserver`:

```ini
[server]
transfer_root=C:\
```

`transfer_root` enables file transfers below that local directory after
authentication. Leave it out to disable transfers.

Authentication, the first frame and disconnects are logged as `RDPTEST`
lines on the kernel debugger and on stdout. An idle server captures nothing;
a connected session captures at most ten frames per second and sends the
changed 64x32 tiles.

## Client and file transfer

Connect with any Remote Desktop client, such as mstsc or a FreeRDP 3 client
with `/sec:nla`, and a local account allowed to log on remotely. For file
transfers, also redirect a dedicated host directory as the `ROSXFER` drive
(`/drive:ROSXFER,<directory>`).

The server polls `request.txt` in that share. A request is UTF-8 text with
LF line ends: a header `RDPFILES1<TAB><job id of 32 hex digits>`, then one
line per item, `<operation><TAB><size><TAB><sha256><TAB><source><TAB><destination>`:

- `PUT`: copy the share file `<source>` to `<destination>` below
  `transfer_root`, checking `<size>` and the SHA-256. A destination that
  already has that size and hash is skipped. The copy goes to a temporary
  name that is verified, flushed and renamed over the destination, so an
  interrupted upload keeps the previous file.
- `GET`: copy `<source>` below `transfer_root` to the share file
  `<destination>`; the size field must be a number and the hash field is
  not checked.
- `MKDIR`: create the directory `<destination>`.

The server writes `results/<job id>.txt` to the share: `RDPRESULT1<TAB><job>`,
one `<index><TAB><status><TAB><bytes><TAB><sha256><TAB>COPY|SKIP` line per
item, and `END<TAB><job>`. A job runs once; after a reconnect the same job
restarts and skips the files already verified.

Paths must be relative, without traversal, alternate data streams, device
names or reparse points, and fit in `MAX_PATH` with room for the temporary
suffix. One job holds at most 16,384 items and a 4 MiB request. Transfers do
not keep ACLs or timestamps.

FreeRDP 3.32.0's client `channels/drive/client/drive_file.c` passes the wrong
handle to `GetFileSize`: reads run past the end of the file and the server
rejects the request. Use a client with that call fixed.
