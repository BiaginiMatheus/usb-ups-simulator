# usb-ups-simulator

An Arduino Pro Micro that pretends to be a USB UPS so that a QNAP TS-419P II can
shut itself down cleanly when mains power fails.

The UPS in this setup is a Siera UPS500, a cheap 500VA line-interactive unit with
no data port of any kind. The NAS therefore has no way to learn that the power
went out. This project fills that gap. A mains-presence circuit watches a wall
outlet, an ATmega32U4 reports itself to the NAS as a HID Power Device, and the NUT
daemon already running inside QTS reads it like any other smart UPS.

The host here is a QNAP TS-419P II, which is what this was built and tested on.
Nothing in the firmware is specific to it. The Arduino presents a standard USB HID
Power Device, so any host that reads one will see it, whether that is a Linux
machine running NUT, Windows, macOS or a different NAS.

Two of the choices in this project exist only because QTS 4.3.3 ships NUT 2.7.4.
On a host running NUT 2.8.x or newer you can keep the real Arduino vendor ID and
leave CDC enabled, because that release added both the Arduino subdriver and
support for a HID UPS on an interface above 0. Read the next section before
copying the board definition onto a modern system.

The simulated battery is a software countdown, not a measurement. The Arduino has
no sensor on the real battery. Read the calibration section before trusting the
numbers.

## Contents

English

- [Signal chain](#signal-chain)
- [Hardware](#hardware)
- [Why the Arduino has to lie about its identity](#why-the-arduino-has-to-lie-about-its-identity)
- [Repository contents](#repository-contents)
- [Reproduction](#reproduction)
- [Wiring](#wiring)
- [Calibration](#calibration)
- [Shutdown policy](#shutdown-policy)
- [Known limitations](#known-limitations)
- [Troubleshooting](#troubleshooting)
- [Diagnostic commands](#diagnostic-commands)
- [If the MGE identity is rejected](#if-the-mge-identity-is-rejected)
- [Traps already paid for](#traps-already-paid-for)
- [Still to do](#still-to-do)
- [References](#references)

Português

- [Cadeia do sinal](#cadeia-do-sinal)
- [Hardware](#hardware-1)
- [Por que o Arduino precisa mentir sobre a própria identidade](#por-que-o-arduino-precisa-mentir-sobre-a-própria-identidade)
- [Conteúdo do repositório](#conteúdo-do-repositório)
- [Reprodução](#reprodução)
- [Ligação](#ligação)
- [Calibração](#calibração)
- [Política de desligamento](#política-de-desligamento)
- [Limitações conhecidas](#limitações-conhecidas)
- [Solução de problemas](#solução-de-problemas)
- [Comandos de diagnóstico](#comandos-de-diagnóstico)
- [Se a identidade MGE for recusada](#se-a-identidade-mge-for-recusada)
- [Armadilhas já pagas](#armadilhas-já-pagas)
- [Ainda por fazer](#ainda-por-fazer)
- [Referências](#referências)

## Signal chain

1. A mains-presence detector holds Arduino pin 4 at ground while mains power is
   present, and releases it when mains fails. That circuit is not designed yet.
   The wiring section below states what it has to do.
2. The internal pull-up takes pin 4 high as soon as the detector releases it, so a
   broken wire or a dead component also reads as a power failure.
3. The Arduino runs a simulated battery. It reports capacity, runtime and status
   flags over USB HID, using the HID Power Device Class.
4. The QNAP runs `usbhid-ups` from NUT 2.7.4. It reads the Arduino as a UPS and
   `upsd` publishes the state.
5. QTS shuts the NAS down on its own timer, with the low battery flag as a
   backstop.

The Arduino stays powered because it draws from the QNAP USB port, and the QNAP
is on the UPS.

The polarity is deliberately inverted so that failure is safe. A broken wire, a
dead component or an unplugged supply all read as "the power went out". A false
alarm costs an unnecessary shutdown. An alarm that never fires costs the array.

## Hardware

| Item | Detail |
|---|---|
| NAS | QNAP TS-419P II, ARM Marvell Kirkwood ARMv5te, QTS 4.3.3 build 20240619 |
| NUT inside QTS | 2.7.4, binaries under `/usr/local/ups/` |
| Microcontroller | Arduino Pro Micro clone, ATmega32U4, 5V, 16MHz |
| UPS | Siera UPS500, 500VA, line-interactive, 220V, no USB port |
| Mains detection | Circuit not designed yet. See the wiring section for what it has to do. |

The TS-419P II draws 26W in operation according to QNAP, measured with four 500GB
drives. Larger drives push that to roughly 30 to 35W. That figure matters for
calibration.

## Why the Arduino has to lie about its identity

This is the part that took the longest to find, and the part that will not be
obvious a year from now.

The obvious approach fails twice on QTS 4.3.3, and both failures were measured
rather than guessed.

The first problem is the USB interface number. The `usbhid-ups` shipped with NUT
2.7.4 claims interface 0 and nothing else. A Pro Micro built with the stock
Arduino core puts the CDC serial port on interfaces 0 and 1 and the HID on
interface 2, so the driver reaches for the wrong interface and gets a broken pipe:

```
Device matches
nut_usb_set_altinterface: skipped usb_set_altinterface(udev, 0)
Unable to get HID descriptor (Broken pipe)
```

Support for a HID UPS on an interface above 0 arrived in NUT 2.8.x, in pull
request 1044. QTS 4.3.3 predates it.

The fix is to compile with `-DCDC_DISABLED`. That sets `CDC_INTERFACE_COUNT` to 0
in `USBDesc.h`, and `PluggableUSB` then hands interface 0 to the HID. Endpoints
shift to 1 and 2, so the control endpoint is not disturbed.

The second problem is the vendor ID. The same pull request added the "Arduino HID"
subdriver, so 2.7.4 has no subdriver that claims vendor 0x2341. Running the driver
by hand shows it walking past the device:

```
Checking device (2341/8036)
Trying to match device
Device does not match - skipping
```

QTS goes further. On hotplug it appends a verdict to
`/etc/config/ups/upsdrv.map`, and for the Arduino it wrote:

```
0x2341,0x8036,NOT_UPS
```

With that line present QTS never starts a driver at all.

The fix is to enumerate as a UPS that 2.7.4 already supports. The subdrivers
compiled into the QTS binary are APC, Belkin/Liebert, CyberPower, EXPLORE,
Phoenixtec/Liebert, MGE, PowerCOM, TrippLite, iDowell and openUPS. The map file
routes several vendor IDs to `usbhid-ups` without restricting the product ID:

```
0x51d,,usbhid-ups     APC
0x463,,usbhid-ups     MGE
0x764,,usbhid-ups     CyberPower
0x9ae,,usbhid-ups     TrippLite
```

This project uses MGE, vendor 0x0463, product 0x0001. The `mge-hid` subdriver
follows the HID Power Device spec more closely than the others, and the
HIDPowerDevice library exposes exactly the `UPS.PowerSummary.*` paths it expects.
Usage 0x24 on the Power Device page is PowerSummary, which is what the library
declares.

Both fixes live on the Arduino side. Nothing in the QNAP firmware is modified.

## Repository contents

| File | Purpose |
|---|---|
| `UPS.ino` | The sketch. Simulated battery, mains detection, HID reporting. |
| `boards.local.txt` | Board definition "UPS NUT". Install it into the Arduino AVR core. |
| `gravar.bat` | Flashes the board by calling avrdude directly, retrying for 30 seconds. |
| `projeto-nobreak-qnap-handoff.md` | Working log in Portuguese. Every measurement and every dead end. |

## Reproduction

### 1. Install the library

Install `abratchik/HIDPowerDevice` into your Arduino libraries folder. On Windows
that is usually `Documents\Arduino\libraries\HIDPowerDevice`.

### 2. Patch the library for a build without CDC

The library takes an optional debug stream with `setOutput(Serial_&)`. The Arduino
core only declares the `Serial_` type when CDC is enabled, so with
`-DCDC_DISABLED` the library stops compiling. Wrap three declarations in
`#if defined(CDC_ENABLED)`.

In `src/HID/HID.h`, both the method and the member it writes to:

```cpp
#if defined(CDC_ENABLED)
    void setOutput(Serial_& out) {
        dbg = &out;
    }
#endif
```

```cpp
#if defined(CDC_ENABLED)
    Serial_ *dbg;
#endif
```

In `src/HIDPowerDevice.h`:

```cpp
#if defined(CDC_ENABLED)
  void setOutput(Serial_&);
#endif
```

In `src/HIDPowerDevice.cpp`:

```cpp
#if defined(CDC_ENABLED)
void HIDPowerDevice_::setOutput(Serial_& out) {
    HID().setOutput(out);
}
#endif
```

Nothing else in the library reads `dbg`, so guarding it costs nothing.

### 3. Install the board definition

Copy `boards.local.txt` next to `boards.txt` in the Arduino AVR core. On Windows
with core 1.8.8 that is:

```
C:\Users\<you>\AppData\Local\Arduino15\packages\arduino\hardware\avr\1.8.8\
```

Restart the IDE. It reads `boards.local.txt` only at startup. A board named
"UPS NUT (32u4, no CDC, MGE VID)" appears under Arduino AVR Boards.

The definition is a copy of the Leonardo entry with three changes. It adds
`-DCDC_DISABLED` to `build.extra_flags`, sets `build.vid` to `0x0463` and
`build.pid` to `0x0001`, and renames the device to MGE UPS SYSTEMS / Evolution
650. The bootloader keeps its own identity of 0x2341:0x0036, so `gravar.bat` and
the IDE still recognise it during upload.

If the PID 0x0001 is ever rejected by `mge-hid`, try 0xffff.

### 4. Set the sketch parameters

Open `UPS.ino` and check these before flashing:

```cpp
#define DEBUG_SERIAL          0        // must be 0, there is no Serial without CDC
#define BATT_RUNTIME_FULL_S   1000UL   // simulated autonomy at 100%, seconds
#define BATT_RECHARGE_S       3600UL   // simulated 0 to 100% recharge, seconds
#define SOC_WARN_PCT          20
#define SOC_LOW_PCT           10
#define AC_PRESENT_LEVEL      LOW      // optocoupler conducts while mains is present
```

`DEBUG_SERIAL` must be 0 on this board. The guards in the sketch skip
`Serial.begin`, `PowerDevice.setOutput` and the status printing, which is the only
reason it compiles without CDC.

Keep `iRemainTimeLimit` well below `BATT_RUNTIME_FULL_S`. The HID report
descriptor restricts that field to the range 120 to 1380 seconds, and if it sits
close to the full runtime the expiry flag is true from the first second of an
outage.

### 5. Flash the board

The first upload is easy, because the board is still running an older sketch that
has CDC. Select the UPS NUT board and the existing COM port and upload normally.

Every upload after that is harder, because the board no longer exposes a serial
port at all. The IDE does not switch to the bootloader port on its own, so avrdude
knocks on the port of the running sketch and gets nowhere:

```
connecting to programmer: .
Error: butterfly_recv(pgm, &c, 1) failed
Error: initialization failed  (rc = -1)
```

`gravar.bat` avoids that. Compile in the IDE with Ctrl+R, close the IDE so it
releases the port, then run the script with the bootloader port as its argument:

```
gravar.bat COM7
```

It calls avrdude once per second for 30 seconds, so there is no timing pressure.
While it prints dots, put the board into its bootloader.

The Pro Micro has no reset button. Short the RST pad to GND twice in quick
succession with tweezers, a jumper or a paperclip. One touch gives about 750ms of
bootloader. A double touch gives about 8 seconds.

To find the bootloader port, open the port list, do the double short, and watch
for a new COM number that appears for a few seconds.

Edit the paths at the top of `gravar.bat` if your avrdude version or sketch build
folder differ from the ones recorded there.

### 6. Check the result on the host

On Windows the device should now appear as Evolution 650 by MGE UPS SYSTEMS, with
no COM port at all. A COM port still being there means `-DCDC_DISABLED` did not
reach the compiler.

Windows also shows it as a second battery. Do not use that reading as evidence of
anything. Its HID battery stack handles the case of a laptop with its own battery
plus a second HID battery badly. During testing it showed 10% while the serial log
said 95%. The serial output is the source of truth on the bench, and `upsc` is the
source of truth on the NAS.

### 7. Configure the QNAP

Enable SSH under Control Panel, Network Services, Telnet/SSH. A modern OpenSSH
client will refuse the old host key algorithms, so connect with:

```sh
ssh -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedKeyTypes=+ssh-rsa \
    -o KexAlgorithms=+diffie-hellman-group14-sha1 admin@<nas-ip>
```

Ground pin 4 before plugging the Arduino in. Left floating, the sketch reads that
as a power failure, counts down and raises the low battery flag about 15 minutes
later, which shuts the NAS down in the middle of whatever you were doing.

Plug the Arduino into a rear USB port. The front port is wired to the one touch
copy button and its auto copy routines.

In the web interface, go to Control Panel, External Device, UPS. Select USB
connection, choose "shut down the server after the AC power fails for" and set it
to 10 minutes. Apply.

Then unplug and replug the Arduino. QTS evaluates a USB device only when it is
plugged in, so a device that was already connected while UPS support was disabled
stays ignored. This step is easy to miss and looks like a failure of everything
that came before it.

### 8. Verify

```sh
ps | grep -i ups
upsc qnapups
```

Three processes should be running: `usbhid-ups`, `upsd` and the QNAP `upsutil`.
A working `upsc` looks like this:

```
battery.charge: 100
battery.charge.low: 10
battery.runtime: 1000
battery.runtime.low: 120
battery.type: PbAc
device.mfr: MGE UPS SYSTEMS
device.model: Evolution 650
driver.version.data: MGE HID 1.39
ups.status: OL
```

To watch a simulated outage:

```sh
while true; do
  echo "$(date +%T) $(upsc qnapups 2>/dev/null | grep -E 'ups.status|battery.charge:' | tr '\n' ' ')"
  sleep 5
done
```

Lift pin 4 from ground and the status changes to `OB` within a few seconds. Leave
it lifted and the NAS shuts down at the 10 minute mark.

To run the driver by hand instead, stop the service first, because two drivers
cannot claim the same USB device:

```sh
/usr/local/ups/bin/usbhid-ups -DDD -u admin -a qnapups -x vendorid=0463
```

Two messages in that output look alarming and are not. `libusb_get_interrupt:
Connection timed out` appears every cycle because the Arduino only sends an
interrupt report when something changes, so NUT falls back to polling the feature
reports. `find_nut_info: unknown info type: load.on.delay` is `mge-hid` looking
for a variable that a real MGE unit would have.

## Wiring

Not designed yet. The current setup uses a jumper between pin 4 and GND in place
of a real circuit.

Whatever goes here has to meet four requirements, because the firmware depends on
them.

It pulls pin 4 to ground while mains power is present and releases it when mains
fails. `AC_PRESENT_LEVEL` is `LOW` in the sketch. Invert that constant if your
circuit works the other way round.

It fails open. A broken wire, a dead component or an unplugged supply all have to
read as a power failure. A false alarm costs one unnecessary shutdown. An alarm
that never fires costs the array.

It takes its reference from a mains outlet, not from the UPS output. The UPS keeps
supplying power during an outage, so a detector fed from it never sees anything
change.

It isolates the Arduino from mains voltage. Pin 4 is a 5V logic input on a board
powered from the NAS.

Pin 4 is configured as `INPUT_PULLUP`, so an external pull-up is optional.

Pin 5 blinks once per second as a heartbeat. Pin 10 goes high when a HID report
fails to send, which means the host stopped listening. Both are optional.

## Calibration

`BATT_RUNTIME_FULL_S` is the only number that depends on the physical world, and
the Siera UPS500 has no published datasheet to consult.

Do not drain the UPS with the NAS attached. That is the dirty shutdown the project
exists to prevent. Use a resistive load of similar wattage instead. A 40W
incandescent bulb or a soldering iron works well. Avoid a laptop charger, since
its draw falls as the battery fills and the load will not be constant.

The measurement taken here used a Dell G2422HS monitor, roughly 20W, and reached
the UPS low battery beep after 40 minutes. Scaling that to the 30W of the NAS and
allowing for the Peukert effect, the beep should arrive at about 22 minutes and
the battery should die at about 32 minutes.

The Peukert effect is why the scaling is not linear. A lead acid battery delivers
less total energy the faster it is drained. A cell rated 7Ah delivers that only at
a slow discharge, typically specified over 20 hours. Doubling the load cuts the
runtime by more than half.

Set `BATT_RUNTIME_FULL_S` to roughly 60% of the measured figure. The countdown is
a timer, not a measurement, and the margin covers the ageing of the battery, which
is fast in a cheap sealed cell. If the real battery dies before the timer reaches
its threshold, the NAS loses power mid shutdown.

Recharge the UPS for 8 to 12 hours before any measurement. A second test run 15
minutes after the first one is worthless, because the battery is nowhere near
full.

## Shutdown policy

The two triggers are deliberately staggered so that they do not race.

| Elapsed | Event |
|---|---|
| 10 min | QTS shuts the NAS down on its own timer. This is the primary trigger. |
| ~12 min | Shutdown complete. |
| ~15 min | The Arduino raises the low battery flag. This is the backstop. |
| ~22 min | The real UPS starts beeping. |
| ~32 min | The battery is empty. |

The "automatic protection" option in the QNAP interface was rejected. It unmounts
the volumes but leaves the NAS running and draining the battery, and the autonomy
here is too short for that to pay off.

## Known limitations

The Arduino cannot tell the Siera to cut its output. With a real UPS, NUT does
that through `upsdrvctl shutdown`. The consequence is that if power returns before
the battery is exhausted, the NAS never loses standby power and will not start
again by itself. It only comes back automatically when the outage outlives the
battery, and only if "power on the server automatically after the power is
restored" is enabled under Control Panel, System, Power.

The reported capacity is simulated. It tracks elapsed time since the outage, not
the real battery. An old or damaged battery will die earlier than the timer
expects, which is why the 60% margin exists.

## Troubleshooting

Symptoms are listed roughly in the order you will meet them.

**The board does not appear in the IDE board list.** The IDE reads
`boards.local.txt` only at startup, so restart it. If it still does not appear,
your IDE version may not accept a whole new board defined in that file. Override
the Leonardo entry instead, by putting `leonardo.build.vid`, `leonardo.build.pid`
and `leonardo.build.extra_flags` in `boards.local.txt` with the same values.

**The compiler reports "redefinition of" for every symbol.** There is more than
one `.ino` in the sketch folder. The IDE concatenates all of them before
compiling. Delete or rename the extra one. Files ending in `.bak` are ignored,
files ending in `.ino` are not.

**The compiler complains about `Serial_`.** Either the library patch from step 2
is missing, or `DEBUG_SERIAL` is not 0. The `Serial_` type does not exist in a
build without CDC.

**The upload runs forever, or avrdude reports `butterfly_recv(pgm, &c, 1)
failed`.** avrdude is talking to the port of the running sketch instead of the
bootloader port. Use `gravar.bat` with the bootloader port and short RST to GND
twice while it retries.

**Windows still shows a COM port for the board.** `-DCDC_DISABLED` did not reach
the compiler. Check that the UPS NUT board is selected and not Leonardo, and that
`build.extra_flags` in `boards.local.txt` includes the flag.

**Windows shows the wrong battery percentage.** Ignore it. Its HID battery stack
mishandles a laptop that has its own battery plus a second HID battery. Use the
serial output on the bench and `upsc` on the NAS.

**`/etc/init.d/ups.sh restart` prints `UPS diseble`.** UPS support is switched off
in the QNAP interface. Enable it under Control Panel, External Device, UPS, and
apply.

**`upsc qnapups` returns `Error: Connection failure: Connection refused`.** `upsd`
is not running, which normally means no driver started. Check with
`ps | grep -i ups`. If UPS support is already enabled in the interface, unplug and
replug the Arduino, because QTS evaluates a USB device only at hotplug.

**A new `NOT_UPS` line appears in `/etc/config/ups/upsdrv.map`.** QTS decided at
hotplug that the device is not a UPS, and will never start a driver for it. The
vendor ID is the reason. Confirm the board really enumerated as 0x0463.

**The driver prints `Device does not match - skipping`.** No subdriver claims that
vendor ID. Either the new vendor ID did not take effect, or the QTS binary has no
subdriver for the one you chose.

**The driver prints `Unable to get HID descriptor (Broken pipe)`.** The HID is not
on interface 0, so CDC is still enabled in the build. This is the symptom that
`-DCDC_DISABLED` exists to fix.

**The driver cannot claim the device.** Another driver already has it. Stop the
service, or press Ctrl+C on the copy you started by hand. Two drivers cannot claim
the same USB device.

**`battery.charge.low` is not 10.** The host overwrote the HID variable. The
sketch rewrites it every cycle, so a single wrong reading between the write and
the report is expected and harmless. A value that stays wrong means the sketch is
an older build.

## Diagnostic commands

Run these on the NAS over SSH. Note that QTS uses BusyBox, whose `grep` has no
`-a` flag, and that there is no `lsusb`.

```sh
# QTS version and build. There is no /etc/version on this firmware.
getcfg System Version -f /etc/config/uLinux.conf
getcfg System 'Build Number' -f /etc/config/uLinux.conf

# NUT version
/usr/local/ups/sbin/upsd -V

# Which subdrivers the shipped binary actually contains
strings /usr/local/ups/bin/usbhid-ups | grep "HID [0-9]"

# Whether it has the Arduino subdriver at all
strings /usr/local/ups/bin/usbhid-ups | grep -i arduino

# USB devices, since lsusb is absent
for d in /sys/bus/usb/devices/*/; do [ -f "$d/idVendor" ] && echo "$(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null)"; done

# What QTS decided about each device at hotplug
tail -5 /etc/config/ups/upsdrv.map

# Kernel view of the HID device, including which interface it landed on
dmesg | tail -30

# Driver by hand, full debug
/usr/local/ups/bin/usbhid-ups -DDD -u admin -a qnapups -x vendorid=0463
```

Adding `-x explore` to the last command makes the driver accept a device that no
subdriver claims, and dump the HID tree. It produces no usable UPS state, but it
answers whether the descriptor can be read at all, which is how the interface
problem was found.

The NUT configuration on QTS lives in `/etc/config/ups/`, which survives reboots.
The UPS name is fixed. It has to be `qnapups`.

## If the MGE identity is rejected

If `mge-hid` ever refuses the device, work down this list.

Change the product ID to 0xffff, which the MGE device table also lists.

Change the vendor to another one the shipped binary supports and the map file
routes to `usbhid-ups`. The candidates are APC at 0x051d, CyberPower at 0x0764 and
TrippLite at 0x09ae. Each has its own quirks. APC is the next most likely to work.

Force the driver by hand in `/etc/config/ups/ups.conf`, with explicit `vendorid`
and `productid`, and `explore` if needed. That directory is persistent, but the
QNAP interface can overwrite the file when you change UPS settings.

Run a modern NUT on a separate machine. Any Linux box with NUT 2.8.x or newer has
the Arduino subdriver and the interface fix, so it can talk to an unmodified
Arduino and act as a NUT server, with the QNAP joining as a network client. The
cost is that the extra machine also has to be on the UPS, which is why this
project avoided it.

## Traps already paid for

The host writes to the HID feature reports. `HID_SET_REPORT` copies straight into
the variable that `setFeature` was given a pointer to, and Windows was observed
overwriting `iRemnCapacityLimit` continuously with values like 88, 85 and 81. That
kept `BelowRemainingCapacityLimit` asserted from 95% downwards, which on the NAS
would mean a permanent low battery state and an immediate shutdown. This is
probably why the flag is commented out in the library example. The sketch now uses
its own `#define` constants for the thresholds and rewrites the HID variables on
every cycle.

`bCapacityMode` is 1, meaning mWh, not 2, meaning percent. Windows reports 0%
forever in percent mode on a laptop that already has a battery. See issue 11 in
the library repository. It makes no difference to NUT, because
`FullChargeCapacity` is 100 and the ratio is the same either way.

Two `.ino` files in one sketch folder break the build. The IDE concatenates every
`.ino` in the folder before compiling, so a stray `UPS-1.ino` produces a
redefinition error for every symbol. Files ending in `.bak` are ignored.

The Arduino IDE 2.x writes its editor buffer over the file on disk when you save
or upload. If you edit the sketch outside the IDE, close the sketch without saving
before reopening it.

Do not use a low value resistor as a jumper. A 2.5 ohm resistor between 5V and GND
is a 2A short across the board.

The generic VID and PID pair 0x0001:0x0000 collides with `blazer_usb`,
`nutdrv_atcl_usb` and `nutdrv_qx`. That is what broke an earlier published project
whose Arduino Nano was grabbed by the QNAP `ups_yec` driver.

Uploading to a 32u4 after flashing a sketch that takes over USB makes the COM port
disappear or change. Double short RST to GND for an 8 second bootloader window.

Pin 10 is not the mains pin. `COMMLOSTPIN` signals that a HID report failed to
reach the host. The mains input is pin 4.

Library examples are read only in the Arduino IDE 2.x. Editing one makes the IDE
save a copy, and it is easy to end up flashing the untouched original instead of
your edit.

## Still to do

Build the optocoupler circuit and replace the jumper on pin 4. Measure the real
autonomy with a resistive load and adjust `BATT_RUNTIME_FULL_S`. Run one
controlled outage with the NAS on the UPS and confirm that the shutdown finishes
before the UPS starts beeping.

## References

| Resource | Link |
|---|---|
| Arduino UPS emulation library | https://github.com/abratchik/HIDPowerDevice |
| Issue 11, capacity stuck at 0% on laptops | https://github.com/abratchik/HIDPowerDevice/issues/11 |
| NUT pull request 1044, Arduino subdriver | https://github.com/networkupstools/nut/pull/1044 |
| usbhid-ups driver documentation | https://github.com/networkupstools/nut/blob/master/docs/man/usbhid-ups.txt |
| NUT vendor and product ID table | https://fossies.org/linux/nut/scripts/udev/nut-usbups.rules.in |
| USB HID Power Device Class specification | https://www.usb.org/sites/default/files/pdcv11.pdf |
| NUT hardware compatibility list | https://networkupstools.org/stable-hcl.html |
| QNAP TS-419P II hardware specs | https://www.qnap.com/en/product/ts-419p%20ii/specs/hardware |

### Background and related projects

None of these are needed to reproduce the build. They are the research trail that
led to it, kept because they cost time to find.

| Resource | Link |
|---|---|
| ESP32 acting as a NUT server | https://github.com/ludoux/esp32-nut-server-usbhid |
| Hackaday project, Arduino Nano and a QNAP | https://hackaday.io/project/27438-link-an-old-ups-to-a-qnap-nas |
| Brazilian project, UPS with udev and NUT | https://github.com/mrmodolo/ups-nut-tsshara |
| NUT device dump repository | https://github.com/networkupstools/nut-ddl |
| APC Back-UPS ES technical guide | https://www.mathstat.dal.ca/~selinger/ups/backups.html |
| Reverse engineering a UPS over USB HID | https://popovicu.com/posts/how-to-reverse-engineer-usb-hid-on-linux/ |

---

# Português

Um Arduino Pro Micro que finge ser um nobreak USB para que um QNAP TS-419P II
consiga se desligar de forma limpa quando a energia cai.

O nobreak aqui é um Siera UPS500, um 500VA line-interactive barato, sem porta de
dados nenhuma. O NAS, portanto, não tem como saber que a energia acabou. Este
projeto preenche essa lacuna. Um circuito detector de rede observa uma tomada, um
ATmega32U4 se apresenta ao NAS como um HID Power Device, e o NUT que já roda
dentro do QTS lê tudo como se fosse um nobreak inteligente qualquer.

O host aqui é um QNAP TS-419P II, que foi onde isto foi construído e testado.
Nada no firmware é específico dele. O Arduino se apresenta como um USB HID Power
Device padrão, então qualquer host que saiba ler um vai enxergá-lo, seja uma
máquina Linux rodando NUT, Windows, macOS ou outro NAS.

Duas das escolhas deste projeto existem só porque o QTS 4.3.3 traz o NUT 2.7.4.
Num host com NUT 2.8.x ou mais novo dá para manter o vendor ID real do Arduino e
deixar o CDC ligado, porque essa versão trouxe tanto o subdriver Arduino quanto o
suporte a UPS HID em interface acima de 0. Leia a próxima seção antes de copiar a
definição de board para um sistema moderno.

A bateria simulada é uma contagem regressiva por software, não uma medição. O
Arduino não tem sensor nenhum na bateria real. Leia a seção de calibração antes de
confiar nos números.

## Cadeia do sinal

1. Um detector de presença de rede mantém o pino 4 do Arduino no terra enquanto
   há energia, e o solta quando a rede cai. Esse circuito ainda não foi
   projetado. A seção de ligação abaixo diz o que ele precisa fazer.
2. O pull-up interno leva o pino 4 para nível alto assim que o detector o solta,
   então fio solto ou componente queimado também são lidos como queda de energia.
3. O Arduino roda uma bateria simulada e reporta carga, autonomia e flags de
   status por USB HID, usando a HID Power Device Class.
4. O QNAP roda o `usbhid-ups` do NUT 2.7.4, lê o Arduino como nobreak, e o `upsd`
   publica o estado.
5. O QTS desliga o NAS pelo próprio timer, com o flag de bateria baixa como rede
   de segurança.

O Arduino continua vivo porque se alimenta pela porta USB do QNAP, que está no
nobreak.

A polaridade é invertida de propósito, para que a falha seja segura. Fio solto,
componente queimado e alimentação desplugada são todos lidos como "a energia
caiu". Um alarme falso custa um desligamento desnecessário. Um alarme que nunca
dispara custa o array.

## Hardware

| Item | Detalhe |
|---|---|
| NAS | QNAP TS-419P II, ARM Marvell Kirkwood ARMv5te, QTS 4.3.3 build 20240619 |
| NUT dentro do QTS | 2.7.4, binários em `/usr/local/ups/` |
| Microcontrolador | Arduino Pro Micro clone, ATmega32U4, 5V, 16MHz |
| Nobreak | Siera UPS500, 500VA, line-interactive, 220V, sem porta USB |
| Detecção de rede | Circuito ainda não projetado. Veja a seção de ligação. |

O TS-419P II consome 26W em operação segundo a QNAP, medido com quatro discos de
500GB. Discos maiores levam isso para algo entre 30 e 35W. Esse número importa
para a calibração.

## Por que o Arduino precisa mentir sobre a própria identidade

Esta é a parte que deu mais trabalho para descobrir, e a que não vai ser óbvia
daqui a um ano.

O caminho óbvio falha duas vezes no QTS 4.3.3, e as duas falhas foram medidas, não
supostas.

O primeiro problema é o número da interface USB. O `usbhid-ups` do NUT 2.7.4
reivindica a interface 0 e mais nada. Um Pro Micro compilado com o core padrão do
Arduino põe o CDC serial nas interfaces 0 e 1 e o HID na 2, então o driver procura
na interface errada e leva um broken pipe:

```
Device matches
nut_usb_set_altinterface: skipped usb_set_altinterface(udev, 0)
Unable to get HID descriptor (Broken pipe)
```

O suporte a UPS HID em interface acima de 0 chegou no NUT 2.8.x, no pull request
1044. O QTS 4.3.3 é anterior a isso.

A correção é compilar com `-DCDC_DISABLED`. Isso zera o `CDC_INTERFACE_COUNT` no
`USBDesc.h`, e o `PluggableUSB` passa a entregar a interface 0 ao HID. Os
endpoints vão para 1 e 2, então o endpoint de controle não é afetado.

O segundo problema é o vendor ID. O mesmo pull request adicionou o subdriver
"Arduino HID", então o 2.7.4 não tem subdriver nenhum que reivindique o vendor
0x2341. Rodando o driver na mão, ele passa direto pelo dispositivo:

```
Checking device (2341/8036)
Trying to match device
Device does not match - skipping
```

O QTS vai além. No hotplug ele anexa um veredito ao
`/etc/config/ups/upsdrv.map`, e para o Arduino escreveu:

```
0x2341,0x8036,NOT_UPS
```

Com essa linha presente, o QTS nunca sobe driver nenhum.

A correção é enumerar como um nobreak que o 2.7.4 já suporta. Os subdrivers
compilados no binário do QTS são APC, Belkin/Liebert, CyberPower, EXPLORE,
Phoenixtec/Liebert, MGE, PowerCOM, TrippLite, iDowell e openUPS. O arquivo de mapa
roteia vários vendor IDs para o `usbhid-ups` sem restringir o product ID:

```
0x51d,,usbhid-ups     APC
0x463,,usbhid-ups     MGE
0x764,,usbhid-ups     CyberPower
0x9ae,,usbhid-ups     TrippLite
```

Este projeto usa MGE, vendor 0x0463, product 0x0001. O subdriver `mge-hid` segue a
spec HID Power Device mais de perto que os outros, e a biblioteca HIDPowerDevice
expõe exatamente os caminhos `UPS.PowerSummary.*` que ele espera. A usage 0x24 da
Power Device page é PowerSummary, que é o que a biblioteca declara.

As duas correções ficam do lado do Arduino. Nada no firmware do QNAP é alterado.

## Conteúdo do repositório

| Arquivo | Para quê |
|---|---|
| `UPS.ino` | O sketch. Bateria simulada, detecção de rede, relatórios HID. |
| `boards.local.txt` | Definição da board "UPS NUT". Instalar no core AVR do Arduino. |
| `gravar.bat` | Grava a placa chamando o avrdude direto, tentando por 30 segundos. |
| `projeto-nobreak-qnap-handoff.md` | Diário de bordo. Cada medição e cada beco sem saída. |

## Reprodução

### 1. Instalar a biblioteca

Instale a `abratchik/HIDPowerDevice` na sua pasta de bibliotecas do Arduino. No
Windows costuma ser `Documentos\Arduino\libraries\HIDPowerDevice`.

### 2. Patch na biblioteca, para compilar sem CDC

A biblioteca aceita um stream de debug opcional via `setOutput(Serial_&)`. O core
do Arduino só declara o tipo `Serial_` quando o CDC está ligado, então com
`-DCDC_DISABLED` a biblioteca para de compilar. Envolva três declarações em
`#if defined(CDC_ENABLED)`.

Em `src/HID/HID.h`, o método e o membro que ele escreve:

```cpp
#if defined(CDC_ENABLED)
    void setOutput(Serial_& out) {
        dbg = &out;
    }
#endif
```

```cpp
#if defined(CDC_ENABLED)
    Serial_ *dbg;
#endif
```

Em `src/HIDPowerDevice.h`:

```cpp
#if defined(CDC_ENABLED)
  void setOutput(Serial_&);
#endif
```

Em `src/HIDPowerDevice.cpp`:

```cpp
#if defined(CDC_ENABLED)
void HIDPowerDevice_::setOutput(Serial_& out) {
    HID().setOutput(out);
}
#endif
```

Nada mais na biblioteca lê o `dbg`, então guardar isso não custa nada.

### 3. Instalar a definição da board

Copie o `boards.local.txt` para junto do `boards.txt` no core AVR do Arduino. No
Windows, com o core 1.8.8:

```
C:\Users\<voce>\AppData\Local\Arduino15\packages\arduino\hardware\avr\1.8.8\
```

Reinicie a IDE. Ela lê o `boards.local.txt` só na inicialização. Uma board chamada
"UPS NUT (32u4, no CDC, MGE VID)" aparece em Arduino AVR Boards.

A definição é uma cópia da entrada do Leonardo com três mudanças. Acrescenta
`-DCDC_DISABLED` ao `build.extra_flags`, põe `build.vid` em `0x0463` e
`build.pid` em `0x0001`, e renomeia o dispositivo para MGE UPS SYSTEMS / Evolution
650. O bootloader mantém a identidade própria, 0x2341:0x0036, então o `gravar.bat`
e a IDE continuam reconhecendo a placa na hora de gravar.

Se algum dia o `mge-hid` recusar o PID 0x0001, tente 0xffff.

### 4. Ajustar os parâmetros do sketch

Abra o `UPS.ino` e confira antes de gravar:

```cpp
#define DEBUG_SERIAL          0        // tem que ser 0, sem CDC nao existe Serial
#define BATT_RUNTIME_FULL_S   1000UL   // autonomia simulada a 100%, em segundos
#define BATT_RECHARGE_S       3600UL   // recarga simulada de 0 a 100%, em segundos
#define SOC_WARN_PCT          20
#define SOC_LOW_PCT           10
#define AC_PRESENT_LEVEL      LOW      // o opto conduz enquanto ha rede
```

O `DEBUG_SERIAL` tem que ser 0 nesta board. As guardas no sketch pulam o
`Serial.begin`, o `PowerDevice.setOutput` e a impressão de status, e é só por isso
que ele compila sem CDC.

Mantenha o `iRemainTimeLimit` bem abaixo do `BATT_RUNTIME_FULL_S`. O descritor HID
limita esse campo à faixa de 120 a 1380 segundos, e se ele ficar perto da
autonomia cheia o flag de expiração é verdadeiro desde o primeiro segundo da
queda.

### 5. Gravar a placa

A primeira gravação é fácil, porque a placa ainda roda um sketch anterior que tem
CDC. Selecione a board UPS NUT e a porta COM existente, e grave normalmente.

Todas as gravações seguintes são mais difíceis, porque a placa não expõe mais
porta serial nenhuma. A IDE não troca para a porta do bootloader sozinha, então o
avrdude bate na porta do sketch em execução e não consegue nada:

```
connecting to programmer: .
Error: butterfly_recv(pgm, &c, 1) failed
Error: initialization failed  (rc = -1)
```

O `gravar.bat` contorna isso. Compile na IDE com Ctrl+R, feche a IDE para ela
soltar a porta, e rode o script passando a porta do bootloader como argumento:

```
gravar.bat COM7
```

Ele chama o avrdude uma vez por segundo durante 30 segundos, então não há pressa
de reflexo. Enquanto ele imprime pontinhos, ponha a placa no bootloader.

O Pro Micro não tem botão de reset. Curto-circuite o pad RST no GND duas vezes
rápido, com uma pinça, um jumper ou um clipe. Um toque dá cerca de 750ms de
bootloader. O duplo dá cerca de 8 segundos.

Para descobrir a porta do bootloader, abra a lista de portas, faça o duplo curto,
e veja qual COM nova aparece por alguns segundos.

Edite os caminhos no topo do `gravar.bat` se a sua versão do avrdude ou a pasta de
build do sketch forem diferentes das que estão anotadas lá.

### 6. Conferir no host

No Windows o dispositivo deve aparecer agora como Evolution 650 da MGE UPS
SYSTEMS, sem porta COM nenhuma. Se ainda houver COM, o `-DCDC_DISABLED` não chegou
ao compilador.

O Windows também mostra a placa como uma segunda bateria. Não use essa leitura
como evidência de coisa alguma. A stack de bateria HID dele lida mal com o caso de
um notebook que já tem bateria própria mais uma segunda bateria HID. Durante os
testes ele mostrou 10% enquanto o log serial dizia 95%. A saída serial é a verdade
na bancada, e o `upsc` é a verdade no NAS.

### 7. Configurar o QNAP

Habilite o SSH em Painel de Controle, Serviços de Rede, Telnet/SSH. Um cliente
OpenSSH moderno recusa os algoritmos antigos, então conecte com:

```sh
ssh -o HostKeyAlgorithms=+ssh-rsa \
    -o PubkeyAcceptedKeyTypes=+ssh-rsa \
    -o KexAlgorithms=+diffie-hellman-group14-sha1 admin@<ip-do-nas>
```

Aterre o pino 4 antes de plugar o Arduino. Flutuando, o sketch lê isso como queda
de energia, conta regressivamente e levanta o flag de bateria baixa uns 15 minutos
depois, o que desliga o NAS no meio do que você estiver fazendo.

Plugue o Arduino numa porta USB traseira. A frontal está ligada ao botão de cópia
rápida e às rotinas de auto-cópia.

Na interface web, vá em Painel de Controle, Dispositivo externo, UPS. Selecione
ligação USB, escolha "desligar o servidor depois de a energia CA falhar por" e
ponha 10 minutos. Aplique.

Depois disso, desplugue e replugue o Arduino. O QTS avalia um dispositivo USB só
quando ele é plugado, então um dispositivo que já estava conectado enquanto o
suporte a UPS estava desligado continua ignorado. Esse passo é fácil de esquecer e
parece uma falha de tudo o que veio antes.

### 8. Verificar

```sh
ps | grep -i ups
upsc qnapups
```

Três processos devem estar rodando: `usbhid-ups`, `upsd` e o `upsutil` da QNAP.
Um `upsc` funcionando parece com isto:

```
battery.charge: 100
battery.charge.low: 10
battery.runtime: 1000
battery.runtime.low: 120
battery.type: PbAc
device.mfr: MGE UPS SYSTEMS
device.model: Evolution 650
driver.version.data: MGE HID 1.39
ups.status: OL
```

Para acompanhar uma queda simulada:

```sh
while true; do
  echo "$(date +%T) $(upsc qnapups 2>/dev/null | grep -E 'ups.status|battery.charge:' | tr '\n' ' ')"
  sleep 5
done
```

Tire o pino 4 do terra e o status vira `OB` em poucos segundos. Deixe assim e o
NAS desliga na marca dos 10 minutos.

Para rodar o driver na mão, pare o serviço antes, porque dois drivers não podem
reivindicar o mesmo dispositivo USB:

```sh
/usr/local/ups/bin/usbhid-ups -DDD -u admin -a qnapups -x vendorid=0463
```

Duas mensagens nessa saída parecem alarmantes e não são. O `libusb_get_interrupt:
Connection timed out` aparece a cada ciclo porque o Arduino só envia relatório de
interrupção quando algo muda, então o NUT cai na leitura das features. O
`find_nut_info: unknown info type: load.on.delay` é o `mge-hid` procurando uma
variável que um MGE de verdade teria.

## Ligação

Ainda não projetada. O setup atual usa um jumper entre o pino 4 e o GND no lugar
de um circuito de verdade.

O que for para cá precisa cumprir quatro requisitos, porque o firmware depende
deles.

Puxar o pino 4 para o terra enquanto há rede, e soltá-lo quando a rede cai. O
`AC_PRESENT_LEVEL` no sketch é `LOW`. Inverta essa constante se o seu circuito
funcionar ao contrário.

Falhar em aberto. Fio solto, componente queimado e alimentação desplugada têm
todos que ser lidos como queda de energia. Um alarme falso custa um desligamento
desnecessário. Um alarme que nunca dispara custa o array.

Tirar a referência de uma tomada da rede, não da saída do nobreak. O nobreak
continua fornecendo energia durante a queda, então um detector alimentado por ele
nunca vê nada mudar.

Isolar o Arduino da tensão de rede. O pino 4 é uma entrada lógica de 5V numa placa
alimentada pelo NAS.

O pino 4 está configurado como `INPUT_PULLUP`, então um pull-up externo é
opcional.

O pino 5 pisca uma vez por segundo como heartbeat. O pino 10 vai para nível alto
quando um relatório HID falha ao ser enviado, o que significa que o host parou de
ouvir. Os dois são opcionais.

## Calibração

O `BATT_RUNTIME_FULL_S` é o único número que depende do mundo físico, e o Siera
UPS500 não tem datasheet público para consultar.

Não descarregue o nobreak com o NAS ligado nele. Esse é justamente o desligamento
sujo que o projeto existe para evitar. Use uma carga resistiva de potência
parecida. Uma lâmpada incandescente de 40W ou um ferro de solda servem bem. Evite
carregador de notebook, porque o consumo cai conforme a bateria enche e a carga
não fica constante.

A medição feita aqui usou um monitor Dell G2422HS, uns 20W, e chegou ao apito de
bateria baixa em 40 minutos. Escalando para os 30W do NAS e descontando o efeito
Peukert, o apito deve chegar por volta dos 22 minutos e a bateria deve morrer por
volta dos 32.

O efeito Peukert é o motivo de a conta não ser linear. Uma bateria de chumbo-ácido
entrega menos energia total quanto mais rápido é drenada. Uma célula de 7Ah
entrega isso só em descarga lenta, tipicamente especificada em 20 horas. Dobrar a
carga corta a autonomia para menos da metade.

Ponha o `BATT_RUNTIME_FULL_S` em torno de 60% do valor medido. A contagem
regressiva é um timer, não uma medição, e a margem cobre o envelhecimento da
bateria, que numa selada barata é rápido. Se a bateria real morrer antes de o
timer chegar no limiar, o NAS perde energia no meio do desligamento.

Deixe o nobreak carregando de 8 a 12 horas antes de qualquer medição. Um segundo
teste feito 15 minutos depois do primeiro não vale nada, porque a bateria está
longe de cheia.

## Política de desligamento

Os dois gatilhos são escalonados de propósito, para que não corram entre si.

| Decorrido | Evento |
|---|---|
| 10 min | O QTS desliga o NAS pelo próprio timer. Gatilho primário. |
| ~12 min | Desligamento concluído. |
| ~15 min | O Arduino levanta o flag de bateria baixa. Rede de segurança. |
| ~22 min | O nobreak de verdade começa a apitar. |
| ~32 min | A bateria acaba. |

A opção de "proteção automática" da interface da QNAP foi descartada. Ela
desmonta os volumes mas deixa o NAS ligado consumindo a bateria, e a autonomia
aqui é curta demais para isso compensar.

## Limitações conhecidas

O Arduino não consegue mandar o Siera cortar a saída. Com um nobreak de verdade, o
NUT faz isso pelo `upsdrvctl shutdown`. A consequência é que, se a energia voltar
antes de a bateria acabar, o NAS nunca perde a energia de standby e não religa
sozinho. Ele só volta automaticamente quando o apagão dura mais que a bateria, e
ainda assim só se "ligar o servidor automaticamente depois de a energia ser
restaurada" estiver marcado em Painel de Controle, Sistema, Energia.

A carga reportada é simulada. Ela acompanha o tempo decorrido desde a queda, não a
bateria real. Uma bateria velha ou danificada morre antes do que o timer espera, e
é para isso que existe a margem de 60%.

## Solução de problemas

Os sintomas estão mais ou menos na ordem em que você vai encontrá-los.

**A board não aparece na lista da IDE.** A IDE lê o `boards.local.txt` só na
inicialização, então reinicie. Se mesmo assim não aparecer, a sua versão da IDE
pode não aceitar uma board nova definida nesse arquivo. Sobrescreva a entrada do
Leonardo, pondo `leonardo.build.vid`, `leonardo.build.pid` e
`leonardo.build.extra_flags` no `boards.local.txt` com os mesmos valores.

**O compilador acusa "redefinition of" em todos os símbolos.** Há mais de um
`.ino` na pasta do sketch. A IDE concatena todos antes de compilar. Apague ou
renomeie o extra. Arquivos terminados em `.bak` são ignorados, os terminados em
`.ino` não.

**O compilador reclama do `Serial_`.** Ou falta o patch da biblioteca do passo 2,
ou o `DEBUG_SERIAL` não está em 0. O tipo `Serial_` não existe numa compilação
sem CDC.

**O upload roda para sempre, ou o avrdude acusa `butterfly_recv(pgm, &c, 1)
failed`.** O avrdude está falando com a porta do sketch em execução, não com a do
bootloader. Use o `gravar.bat` com a porta do bootloader e curto-circuite o RST no
GND duas vezes enquanto ele tenta.

**O Windows ainda mostra uma porta COM para a placa.** O `-DCDC_DISABLED` não
chegou ao compilador. Confira se a board selecionada é a UPS NUT e não a Leonardo,
e se o `build.extra_flags` do `boards.local.txt` traz a flag.

**O Windows mostra a porcentagem de bateria errada.** Ignore. A stack de bateria
HID dele lida mal com um notebook que tem bateria própria mais uma segunda bateria
HID. Use a saída serial na bancada e o `upsc` no NAS.

**O `/etc/init.d/ups.sh restart` imprime `UPS diseble`.** O suporte a UPS está
desligado na interface da QNAP. Habilite em Painel de Controle, Dispositivo
externo, UPS, e aplique.

**O `upsc qnapups` devolve `Error: Connection failure: Connection refused`.** O
`upsd` não está rodando, o que normalmente significa que nenhum driver subiu.
Confira com `ps | grep -i ups`. Se o suporte a UPS já estiver habilitado na
interface, desplugue e replugue o Arduino, porque o QTS avalia um dispositivo USB
só no hotplug.

**Uma linha nova de `NOT_UPS` aparece no `/etc/config/ups/upsdrv.map`.** O QTS
decidiu, no hotplug, que o dispositivo não é um nobreak, e nunca vai subir driver
para ele. O motivo é o vendor ID. Confirme que a placa enumerou mesmo como 0x0463.

**O driver imprime `Device does not match - skipping`.** Nenhum subdriver
reivindica aquele vendor ID. Ou o vendor ID novo não fez efeito, ou o binário do
QTS não tem subdriver para o que você escolheu.

**O driver imprime `Unable to get HID descriptor (Broken pipe)`.** O HID não está
na interface 0, ou seja, o CDC continua ligado na compilação. É exatamente o
sintoma que o `-DCDC_DISABLED` existe para resolver.

**O driver não consegue reivindicar o dispositivo.** Outro driver já o tem. Pare o
serviço, ou dê Ctrl+C na cópia que você subiu na mão. Dois drivers não podem
reivindicar o mesmo dispositivo USB.

**O `battery.charge.low` não é 10.** O host sobrescreveu a variável HID. O sketch
a reescreve a cada ciclo, então uma leitura errada isolada entre a escrita e o
relatório é esperada e inofensiva. Um valor que fica errado significa que o sketch
é de uma versão antiga.

## Comandos de diagnóstico

Rode estes no NAS por SSH. Note que o QTS usa BusyBox, cujo `grep` não tem a flag
`-a`, e que não existe `lsusb`.

```sh
# Versão e build do QTS. Não existe /etc/version neste firmware.
getcfg System Version -f /etc/config/uLinux.conf
getcfg System 'Build Number' -f /etc/config/uLinux.conf

# Versão do NUT
/usr/local/ups/sbin/upsd -V

# Quais subdrivers o binário embarcado realmente tem
strings /usr/local/ups/bin/usbhid-ups | grep "HID [0-9]"

# Se ele tem o subdriver Arduino
strings /usr/local/ups/bin/usbhid-ups | grep -i arduino

# Dispositivos USB, já que não há lsusb
for d in /sys/bus/usb/devices/*/; do [ -f "$d/idVendor" ] && echo "$(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null)"; done

# O que o QTS decidiu sobre cada dispositivo no hotplug
tail -5 /etc/config/ups/upsdrv.map

# Visão do kernel sobre o HID, incluindo em qual interface ele caiu
dmesg | tail -30

# Driver na mão, debug completo
/usr/local/ups/bin/usbhid-ups -DDD -u admin -a qnapups -x vendorid=0463
```

Acrescentar `-x explore` ao último comando faz o driver aceitar um dispositivo que
nenhum subdriver reivindica, e despejar a árvore HID. Não produz estado de nobreak
utilizável, mas responde se o descritor pode ser lido, que foi como o problema da
interface apareceu.

A configuração do NUT no QTS fica em `/etc/config/ups/`, que sobrevive a reboots.
O nome do nobreak é fixo. Tem que ser `qnapups`.

## Se a identidade MGE for recusada

Se algum dia o `mge-hid` recusar o dispositivo, siga esta lista.

Mude o product ID para 0xffff, que a tabela de dispositivos MGE também lista.

Mude o vendor para outro que o binário embarcado suporte e que o arquivo de mapa
roteie para o `usbhid-ups`. Os candidatos são APC em 0x051d, CyberPower em 0x0764
e TrippLite em 0x09ae. Cada um tem suas manias. O APC é o mais provável de
funcionar em seguida.

Force o driver na mão em `/etc/config/ups/ups.conf`, com `vendorid` e `productid`
explícitos, e `explore` se precisar. Esse diretório é persistente, mas a interface
da QNAP pode sobrescrever o arquivo quando você mexe nas configurações de UPS.

Rode um NUT moderno em outra máquina. Qualquer Linux com NUT 2.8.x ou mais novo
tem o subdriver Arduino e a correção da interface, então consegue falar com um
Arduino sem modificação nenhuma e agir como servidor NUT, com o QNAP entrando como
cliente de rede. O custo é que a máquina extra também precisa estar no nobreak, e
foi por isso que este projeto evitou esse caminho.

## Armadilhas já pagas

O host escreve nas features HID. O `HID_SET_REPORT` copia direto para a variável
cujo ponteiro foi dado ao `setFeature`, e o Windows foi observado sobrescrevendo o
`iRemnCapacityLimit` continuamente, com valores como 88, 85 e 81. Isso mantinha o
`BelowRemainingCapacityLimit` aceso desde os 95%, o que no NAS significaria bateria
baixa permanente e desligamento imediato. Provavelmente é por isso que esse flag
está comentado no exemplo da biblioteca. O sketch agora usa `#define` próprios para
os limiares e reescreve as variáveis HID a cada ciclo.

O `bCapacityMode` é 1, mWh, e não 2, porcentagem. O Windows reporta 0% para sempre
no modo porcentagem num notebook que já tem bateria. Veja o issue 11 no repositório
da biblioteca. Para o NUT não faz diferença, porque o `FullChargeCapacity` é 100 e
a razão dá no mesmo.

Dois arquivos `.ino` na mesma pasta quebram a compilação. A IDE concatena todos os
`.ino` da pasta antes de compilar, então um `UPS-1.ino` sobrando gera erro de
redefinição em cada símbolo. Arquivos terminados em `.bak` são ignorados.

A Arduino IDE 2.x escreve o buffer do editor por cima do arquivo em disco quando
você salva ou grava. Se editar o sketch fora da IDE, feche o sketch sem salvar
antes de reabrir.

Não use resistor de valor baixo como jumper. Um resistor de 2,5 ohm entre 5V e GND
é um curto de 2A na placa.

O par genérico de VID e PID 0x0001:0x0000 colide com o `blazer_usb`, o
`nutdrv_atcl_usb` e o `nutdrv_qx`. Foi isso que quebrou um projeto publicado
anteriormente, cujo Arduino Nano era agarrado pelo driver `ups_yec` do QNAP.

Gravar num 32u4 depois de subir um sketch que assume a USB faz a porta COM sumir
ou mudar. Duplo curto do RST no GND dá uma janela de 8 segundos de bootloader.

O pino 10 não é o pino da rede. O `COMMLOSTPIN` sinaliza que um relatório HID
falhou ao chegar no host. A entrada de rede é o pino 4.

Exemplos de biblioteca são somente leitura na Arduino IDE 2.x. Editar um faz a IDE
salvar uma cópia, e é fácil acabar gravando o original intocado em vez da sua
edição.

## Ainda por fazer

Montar o circuito do optoacoplador e substituir o jumper do pino 4. Medir a
autonomia real com carga resistiva e ajustar o `BATT_RUNTIME_FULL_S`. Rodar uma
queda controlada com o NAS no nobreak e confirmar que o desligamento termina antes
de o nobreak começar a apitar.

## Referências

| Recurso | Link |
|---|---|
| Biblioteca Arduino para emular nobreak | https://github.com/abratchik/HIDPowerDevice |
| Issue 11, capacidade travada em 0% em notebooks | https://github.com/abratchik/HIDPowerDevice/issues/11 |
| Pull request 1044 do NUT, subdriver Arduino | https://github.com/networkupstools/nut/pull/1044 |
| Documentação do driver usbhid-ups | https://github.com/networkupstools/nut/blob/master/docs/man/usbhid-ups.txt |
| Tabela de vendor e product ID do NUT | https://fossies.org/linux/nut/scripts/udev/nut-usbups.rules.in |
| Especificação da USB HID Power Device Class | https://www.usb.org/sites/default/files/pdcv11.pdf |
| Lista de compatibilidade de hardware do NUT | https://networkupstools.org/stable-hcl.html |
| Especificações de hardware do QNAP TS-419P II | https://www.qnap.com/en/product/ts-419p%20ii/specs/hardware |

### Contexto e projetos relacionados

Nenhum destes é necessário para reproduzir a montagem. São a trilha de pesquisa
que levou até ela, guardados porque custaram tempo para achar.

| Recurso | Link |
|---|---|
| ESP32 como servidor NUT | https://github.com/ludoux/esp32-nut-server-usbhid |
| Projeto no Hackaday, Arduino Nano e um QNAP | https://hackaday.io/project/27438-link-an-old-ups-to-a-qnap-nas |
| Projeto nacional, nobreak com udev e NUT | https://github.com/mrmodolo/ups-nut-tsshara |
| Repositório de dumps de dispositivos do NUT | https://github.com/networkupstools/nut-ddl |
| Guia técnico do APC Back-UPS ES | https://www.mathstat.dal.ca/~selinger/ups/backups.html |
| Engenharia reversa de nobreak por USB HID | https://popovicu.com/posts/how-to-reverse-engineer-usb-hid-on-linux/ |
