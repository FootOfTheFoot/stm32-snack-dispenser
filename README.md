# Snack Dispenser (Embedded C) — Keypad + LCD + 7-Seg + Stepper + UI Animations

An embedded C application that simulates a snack vending machine with a **keypad UI**, **16x2 LCD**, **7-segment countdown timer**, **stepper motor dispensing**, and a **service/admin mode** for diagnostics + restocking.

This project also renders a graphical UI on an X display using **pqiv**, with a **rolling PID ring** to avoid flicker and avoid `killall`.

---

## Demo Overview

### Normal Mode
1. User enters a product **index** on the keypad.
2. System shows the product image and checks stock.
3. User enters **amount (1–15)**.
4. Payment is simulated by pressing `0` twice (`00`).
5. Dispenser runs the motor cycle (**3 seconds per item**) while playing an animation and sound cues.
6. Stock is updated after dispensing.

### Service Mode (Admin)
- Enter `1234` then press `B` to enter service gate, then **flip DIP (SA5)** mapping and press any key.
- Options include:
  - Manual dispensing (by index + amount)
  - Restocking (set stock directly)
  - Sound test (8 selectable beep patterns)
  - Motor diagnostic (run N cycles)

---

## Hardware / Peripherals Used

- **Keypad** (matrix scanned via port writes/reads)
- **16x2 LCD** (4-bit interface)
- **7-segment display** (countdown + service blink indicator)
- **Stepper motor** (full-step drive sequence)
- **DAC audio output** (square-wave beeps for feedback)
- X display rendering via **pqiv** (image-based UI)

> Low-level IO uses `CM3_outport`, `CM3_inport`, and `CM3PortWrite` from `library.h`.

---

## Port Mapping

This project supports **two port maps**, switched using a DIP gate.

### Normal Mapping
- LED / 7-seg: `0x3A`
- LCD: `0x3B`
- Stepper motor: `0x39`
- Keypad: `0x3C`

### Admin Mapping (via DIP)
- LED / 7-seg: `0x1A`
- LCD: `0x1B`
- Stepper motor: `0x19`
- Keypad: `0x1C`

Runtime switching is handled by:
- `set_port_mapping(int admin)`

---

## Key Controls

- Digits `0-9`: input
- `A` = BACK (`*`): delete / back / return to service menu
- `B` = ENTER (`#`): confirm / proceed

---

## Products Implemented

The machine uses product *indexes* (not 1–4 buttons).

| Index | Product  | Price | Initial Stock |
|------:|----------|------:|--------------:|
| 3     | Cheetos  | $1.50 | 1 |
| 8     | Lays     | $1.50 | 2 |
| 11    | Doritos  | $1.50 | 3 |
| 22    | Pocky    | $1.75 | 4 |

Stock is decremented after dispensing.  
If stock is 0, an **OUT OF STOCK** image is shown.

---

## UI / Image Rendering System (pqiv)

Images are shown using `pqiv -f <file>` with a **rolling ring buffer of 8 PIDs**:

- Prevents desktop flicker
- Avoids `killall pqiv`
- Limits spawned pqiv processes to 8 at any time

Key functions:
- `show_image(path)`
- `pqiv_kill_all_spawned()` (called on exit)

It also auto-detects X display (`:0` or `:1`) and sets `DISPLAY` / `XAUTHORITY`.

---

## Non-blocking Dual Animation Engine

A shared animation engine (`Anim`) drives two animations without blocking the main loop:

- **Door animation** (service enter/exit)
- **Dispensing animation** (runs alongside motor motion)

Animation timing:
- 4 frames
- 800 ms per frame

Engine functions:
- `anim_start(Anim*, frames, nframes, direction, frame_ms)`
- `anim_tick(Anim*)`

---

## Motor Control and Dispense Timing

Stepper motor uses a full-step 4-phase sequence:

- `full_seq_drive[4] = {0x08, 0x04, 0x02, 0x01}`

Dispense timing is synchronized with animation:

- **3 seconds per item**
- 60 steps per item
- Phase delay computed by:

`DISP_PHASE_DELAY_US = 3000000 / (TOTAL_STEPS_PER_ITEM * 4)`

Dispense function:
- `run_one_dispense_cycle_with_anim()`

Service motor diagnostic:
- spins short bursts, repeated N cycles (1–15)
- `run_motor_test_cycles(cycles)`

---

## Audio Feedback (DAC)

Square-wave beeps provide immediate UI feedback:

- Keypress beep
- Error beep
- Success beep
- Payment accepted beep
- Slot-specific dispensing tones (1–4)

DAC writes use:
- `CM3PortWrite(3, v)`
- `CM3PortWrite(5, v)`

---

## Timeouts and UI Safety

### Normal Mode Idle Timer
- A **9-second countdown** (7-seg) starts after index input begins.
- Also runs during amount input and payment input.
- Timeout resets system back to menu.

Service mode disables the normal countdown and instead:
- blinks `0` on the 7-seg every 500 ms.

---

## State Machine Design

The program uses a structured state machine including:

- Menu / Amount / Pay flow
- Service gate entry + return gate
- Door opening + closing animation states
- Service submenu states:
  - Dispense index / amount
  - Restock index / qty
  - Sound select
  - Motor cycle select
- Dispensing state that runs motor + animation + updates stock

---

## How to Run

1. Ensure required images exist in `/tmp/`.
2. Ensure `pqiv` is installed and X display is available.
3. Build and run in the target environment that provides `library.h` and CM3 port functions.

---

## Assets in `/tmp/`

This code expects UI assets such as:

- `/tmp/menu.jpg`
- `/tmp/success.jpg`
- `/tmp/cheetos.jpg`, `/tmp/cheetos_oos.jpg`
- `/tmp/door_1.jpg` ... `/tmp/door_4.jpg`
- `/tmp/disp_1.jpg` ... `/tmp/disp_4.jpg`
- `/tmp/service.jpg`, `/tmp/restock.jpg`, `/tmp/sound.jpg`, `/tmp/motor.jpg`
