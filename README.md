Logitech G710+ Keyboard Driver & Daemon
==========================

The Logitech G710+ mechanical keyboard is a great piece of hardware. Unfortunately, there is no support in the kernel for the additional features of the keyboard.

This project provides a kernel driver and a userspace daemon that allow the special keys (**M1-MR** and **G1-G6**) to be used for macros and profile switching.


Installation
-------------------------

Quick install (recommended):
<pre>
./install.sh
</pre>

or, to skip udev rule installation:
<pre>
./install.sh --skip-udev
</pre>

This will:
1. Compile and install the kernel module.
2. Compile and install the `g710d` daemon.
3. Install a default configuration to `/etc/g710d.conf` (if it doesn't exist).


Usage
--------------------------

### Running the Daemon
The installation script automatically sets up a **systemd service**, so the daemon starts automatically when you turn on your computer.

You can manage the service using these commands:
*   **Start:** `sudo systemctl start g710d`
*   **Stop:** `sudo systemctl stop g710d`
*   **Check Status/Logs:** `sudo systemctl status g710d`
*   **View Debug Output:** `journalctl -u g710d -f`

When running, M1, M2, and M3 keys will switch between three independent macro profiles, and the corresponding LEDs on the keyboard will update automatically. The daemon performs an **exclusive grab** on the special keys, so they will no longer trigger default system shortcuts (like opening settings).

#### Features:
*   **3 Independent Profiles:** Use **M1, M2, and M3** to switch macro sets.
*   **Smart Combinations:** Modifiers (Ctrl, Alt, Shift, Meta) are held down automatically while other keys in the macro are pressed.
*   **Smart Auto-Release:** If a macro contains a modifier followed by a sequence of regular keys, the daemon automatically releases the modifier when the sequence starts (e.g., `Alt+Q` followed by `R` will type `@R`).
*   **Quick Reload:** Press the **MR** button on the keyboard to instantly reload the configuration file. The MR LED will flash to confirm.

### Configuration
Macros are defined in `/etc/g710d.conf`.

**Format:**
`P<profile> G<key> <key1> <key2> ...`

**Keywords:**
*   `RELEASE`: Manually force all currently held modifiers and keys to be released.
*   `HOLD`: Prefix a key to keep it held down until the end of the macro or a `RELEASE` keyword.

**Examples:**
<pre>
# Profile 1 G1 sends 'ls' then Enter
P1 G1 KEY_L KEY_S KEY_ENTER

# Profile 1 G2 sends Ctrl+C (Smart Holding)
P1 G2 KEY_LEFTCTRL KEY_C

# Profile 1 G4 sends '@RENOXI' (Smart Auto-Release)
# RightAlt+Q gives '@', then Alt is released before typing the rest.
P1 G4 KEY_RIGHTALT KEY_Q KEY_R KEY_E KEY_N KEY_O KEY_X KEY_I

# Manual control using HOLD and RELEASE
# Holds A and B at the same time, then taps C.
P1 G5 HOLD KEY_A HOLD KEY_B KEY_C
</pre>

A full list of available key codes can be found in `/usr/include/linux/input-event-codes.h` (remove the `KEY_` prefix or use the full name).


Backlight Control (API)
--------------------------
The driver also exposes a way to set the keyboard backlight intensity via sysfs:

<pre>
/sys/bus/hid/devices/.../logitech-g710/led_macro
/sys/bus/hid/devices/.../logitech-g710/led_keys
</pre>

*   `led_macro`: A bitmask (0-15) for the M1-MR LEDs.
*   `led_keys`: Intensity for WASD and other keys (0-4).

Example:
<pre>
echo -n "5" > /sys/bus/hid/devices/.../logitech-g710/led_macro
</pre>
