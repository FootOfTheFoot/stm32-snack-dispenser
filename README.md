# STM32 Snack Dispenser System

An embedded C snack dispenser prototype that simulates a vending-style workflow using keypad input, a 16x2 LCD, 7-segment countdown feedback, stepper motor dispensing, DAC sound cues, image-based UI rendering and a service/admin mode for diagnostics and restocking.

The project focuses on integrating user input, hardware feedback, timed motor control, stock tracking and service functions into one physical system workflow.

## What This Project Demonstrates

This project demonstrates embedded system integration across user input, hardware feedback, motor control, timing, stock tracking and service diagnostics.

It was useful for practising how a physical system needs clear states, user feedback, timeout handling and diagnostic tools to feel reliable and usable.

## System Overview

The snack dispenser supports both normal user operation and service/admin operation.

Key capabilities include:

- Product selection through keypad input.
- Quantity input from 1–15 items.
- Simulated payment using keypad input.
- Stock checking and out-of-stock handling.
- Stepper motor dispensing with timed cycles.
- LCD and 7-segment display feedback.
- DAC-based sound cues for user feedback.
- Image-based UI rendering through `pqiv`.
- Service mode for manual dispensing, restocking, sound testing and motor diagnostics.

## Demo Workflow

### Normal Mode

1. User enters a product index on the keypad.
2. System displays the selected product image.
3. System checks whether the product is in stock.
4. User enters the quantity, from 1 to 15.
5. Payment is simulated by pressing `0` twice (`00`).
6. The dispenser runs the stepper motor cycle for the selected quantity.
7. Dispensing animation and sound cues play during the motor cycle.
8. Stock is updated after dispensing.
9. If stock reaches 0, an out-of-stock image is shown.

### Service Mode

Service mode is entered through an admin flow using the service PIN and DIP gate switching.

Service features include:

- Manual dispensing by product index and quantity.
- Restocking by setting product stock directly.
- Sound testing with 8 selectable beep patterns.
- Motor diagnostics with 1–15 test cycles.
- Service mode visual feedback through the 7-segment display.

## Hardware / Peripherals Used

- Matrix keypad
- 16x2 LCD using 4-bit interface
- 7-segment display
- Stepper motor
- DAC audio output
- DIP switch / service gate input
- X display rendering using `pqiv`

Low-level I/O uses port functions from `library.h`, including:

```text
CM3_outport
CM3_inport
CM3PortWrite
```

## Products Implemented

The machine uses product indexes rather than fixed 1–4 buttons.

| Index | Product | Price | Initial Stock |
|---:|---|---:|---:|
| 3 | Cheetos | $1.50 | 1 |
| 8 | Lays | $1.50 | 2 |
| 11 | Doritos | $1.50 | 3 |
| 22 | Pocky | $1.75 | 4 |

Stock is decremented after dispensing. If stock is 0, the system displays an out-of-stock image.

## Key Operational Rules

| Function | Behaviour |
|---|---|
| Quantity input | 1–15 items |
| Payment simulation | Press `0` twice |
| Dispensing cycle | 3 seconds per item |
| Motor movement | 60 steps per item |
| Normal idle timeout | 9-second countdown |
| Sound test mode | 8 selectable beep patterns |
| Motor diagnostic mode | 1–15 test cycles |
| Image process limit | Rolling buffer of 8 `pqiv` processes |

## Port Mapping

The project supports two port maps, switched using a DIP gate.

### Normal Mapping

| Peripheral | Port |
|---|---|
| LED / 7-segment display | `0x3A` |
| LCD | `0x3B` |
| Stepper motor | `0x39` |
| Keypad | `0x3C` |

### Admin Mapping

| Peripheral | Port |
|---|---|
| LED / 7-segment display | `0x1A` |
| LCD | `0x1B` |
| Stepper motor | `0x19` |
| Keypad | `0x1C` |

Runtime switching is handled by:

```cpp
set_port_mapping(int admin)
```

## Key Controls

| Key | Function |
|---|---|
| `0–9` | Numeric input |
| `A` | Back / delete / return to service menu |
| `B` | Confirm / proceed |
| `1234` + `B` | Enter service gate |

## UI / Image Rendering System

The project uses `pqiv` to render image-based UI screens on an X display.

The image-rendering system uses a rolling ring buffer of 8 process IDs to:

- reduce desktop flicker
- avoid using `killall pqiv`
- limit active `pqiv` processes
- clean up spawned image processes on exit

Key functions include:

```cpp
show_image(path)
pqiv_kill_all_spawned()
```

The project also auto-detects the X display using `:0` or `:1` and configures `DISPLAY` / `XAUTHORITY`.

## Animation System

A non-blocking animation engine drives UI animations while the rest of the system continues running.

Animations include:

- door animation for service enter/exit
- dispensing animation during motor movement

Animation timing:

```text
4 frames
800 ms per frame
```

Key functions include:

```cpp
anim_start(Anim*, frames, nframes, direction, frame_ms)
anim_tick(Anim*)
```

## Motor Control and Dispensing

The stepper motor uses a full-step 4-phase drive sequence:

```cpp
full_seq_drive[4] = {0x08, 0x04, 0x02, 0x01}
```

Dispensing is synchronised with the animation system.

Timing:

```text
3 seconds per item
60 steps per item
```

The phase delay is computed using:

```cpp
DISP_PHASE_DELAY_US = 3000000 / (TOTAL_STEPS_PER_ITEM * 4)
```

Important functions:

```cpp
run_one_dispense_cycle_with_anim()
run_motor_test_cycles(cycles)
```

## Audio Feedback

Square-wave beeps provide immediate feedback through DAC output.

Feedback sounds include:

- keypress beep
- error beep
- success beep
- payment accepted beep
- slot-specific dispensing tones

DAC writes use:

```cpp
CM3PortWrite(3, v)
CM3PortWrite(5, v)
```

## Timeouts and UI Safety

### Normal Mode

A 9-second countdown begins after product index input starts.

The countdown also applies during:

- quantity input
- payment input

If the user is inactive for too long, the system resets back to the menu.

### Service Mode

Service mode disables the normal countdown and instead blinks `0` on the 7-segment display every 500 ms.

## State Machine Design

The program uses a structured state machine to manage the dispenser workflow.

Main states include:

- menu
- quantity input
- payment input
- dispensing
- service gate entry
- service return gate
- door opening / closing animation
- manual dispensing
- restocking
- sound selection
- motor diagnostic cycle selection

This state-based design helps keep user flow, hardware control, animations and diagnostics organised.

## How to Run

This project is designed for the target embedded environment that provides `library.h` and the required CM3 port functions.

General steps:

1. Ensure the target environment provides `library.h`.
2. Ensure required UI images exist in `/tmp/`.
3. Ensure `pqiv` is installed.
4. Ensure an X display is available.
5. Build and run the project in the supported embedded environment.

## Required UI Assets

The code expects UI assets in `/tmp/`, including:

```text
/tmp/menu.jpg
/tmp/success.jpg
/tmp/cheetos.jpg
/tmp/cheetos_oos.jpg
/tmp/door_1.jpg ... /tmp/door_4.jpg
/tmp/disp_1.jpg ... /tmp/disp_4.jpg
/tmp/service.jpg
/tmp/restock.jpg
/tmp/sound.jpg
/tmp/motor.jpg
```

## Technologies Used

- Embedded C
- STM32 / Cortex-M3 style I/O environment
- Matrix keypad input
- LCD control
- 7-segment display control
- Stepper motor control
- DAC sound output
- X display image rendering
- `pqiv`

## What I Learnt

This project strengthened my understanding of embedded software design by showing how a physical system depends on multiple small subsystems working together.

It helped me practise:

- building state-based workflows
- handling user input
- giving clear hardware feedback
- synchronising motor control with UI animation
- managing stock updates
- designing service/admin functions
- adding timeout behaviour for safer user interaction

## Portfolio Relevance

This project supports my interest in software and systems engineering by showing how embedded software, hardware control, timing, feedback and diagnostics can be integrated into one practical system.

It also demonstrates how physical systems need more than core functionality; they also need clear user flow, error handling, maintenance tools and feedback mechanisms to feel reliable.

## Notes

This repository is intended for project documentation and portfolio reference.

Some paths, assets or hardware-specific dependencies may need to be recreated depending on the target environment.
