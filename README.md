# ECE 218 – Final Project: Parking Assistance and Collision Warning System

## Team Members
- Eric Greenberg
- Nada Aloussi

## System Description
This project implements a parking assistance and collision warning system using an ESP32-S3 microcontroller.  
The system uses four ultrasonic sensors placed at the front-left, front-right, back-left, and back-right sides of the vehicle model to detect nearby obstacles. Based on the measured distances, the system provides visual, audible, and display feedback to help the driver understand how close the vehicle is to surrounding objects.

The visual subsystem uses four LEDs arranged like a map of the car. Each LED corresponds to one side of the vehicle and increases in brightness as an obstacle gets closer on that side. The audible subsystem uses a passive buzzer to provide warning sounds: no sound in the safe zone, repeated beeping in the caution zone, and a continuous higher-pitched tone in the danger zone. A pushbutton allows the user to mute or unmute the buzzer without shutting down the system.

The display subsystem uses a 16x2 LCD to show the minimum detected distance and the current warning state. A potentiometer connected to the ESP32-S3 adjusts the safe-distance threshold, allowing the user to control how early the warning system begins reacting to nearby objects.

---

## Testing Results Summary

### Distance Detection and Warning Subsystem

| Specification | Test Process | Results |
|--------------|-------------|---------|
| System continuously measures distance from all four ultrasonic sensors | Obstacles were moved toward each sensor one side at a time while monitoring LCD and serial output | Passed. Distances were detected and updated continuously for all four sensors. |
| Minimum measured distance determines overall warning state | Obstacles were placed at different distances on multiple sides | Passed. The system correctly used the closest detected obstacle to determine SAFE, CAUTION, or DANGER state. |
| SAFE state is active when obstacle is farther than the safe threshold | Obstacles were kept beyond the safe threshold while varying potentiometer setting | Passed. LCD displayed SAFE and buzzer remained off. |
| CAUTION state is active between safe and danger thresholds | Obstacles were moved into the middle-distance range | Passed. LCD displayed CAUTION and buzzer produced repeated warning beeps. |
| DANGER state is active when obstacle is very close | Obstacles were moved within the danger threshold | Passed. LCD displayed DANGER and buzzer produced a continuous higher-pitched tone. |

---

### LED Feedback Subsystem

| Specification | Test Process | Results |
|--------------|-------------|---------|
| Each LED corresponds to the correct side of the vehicle | Obstacles were brought close to one sensor at a time | Passed. Only the LED associated with that side responded. |
| LED remains off when obstacle is beyond safe range | Obstacles were kept far from all sensors | Passed. LEDs remained off in the safe zone. |
| LED brightness increases as obstacle gets closer | Obstacles were slowly moved from far range toward each sensor | Passed. LED brightness increased gradually as distance decreased. |
| LED reaches maximum brightness in danger zone | Obstacles were moved within danger distance | Passed. Corresponding LED reached full brightness when obstacle was very close. |

---

### Buzzer and Button Subsystem

| Specification | Test Process | Results |
|--------------|-------------|---------|
| Buzzer stays off in safe zone | Obstacles were placed beyond the safe threshold | Passed. No buzzer sound occurred. |
| Buzzer beeps in caution zone | Obstacles were moved into caution range | Passed. Buzzer produced repeated warning beeps. |
| Buzzer sounds continuously in danger zone | Obstacles were moved into danger range | Passed. Buzzer produced a continuous high warning tone. |
| Button mutes buzzer when pressed | System was placed in caution/danger zone and button was pressed | Passed. Buzzer stopped while other system behavior continued normally. |
| Button can unmute buzzer when pressed again | Button was pressed a second time after muting | Passed. Buzzer resumed normal warning behavior. |

---

### LCD and Potentiometer Subsystem

| Specification | Test Process | Results |
|--------------|-------------|---------|
| LCD displays minimum detected distance | Obstacles were moved toward and away from the vehicle model | Passed. LCD updated to show current minimum distance in centimeters. |
| LCD displays correct warning state | Obstacles were placed in safe, caution, and danger ranges | Passed. LCD correctly displayed SAFE, CAUTION, or DANGER. |
| Potentiometer changes safe threshold | Potentiometer was rotated while observing LCD threshold value and system behavior | Passed. Safe threshold changed as expected over the set range. |
| Potentiometer affects when warnings begin | With a fixed obstacle distance, potentiometer was turned to different positions | Passed. System entered warning states earlier or later depending on the threshold setting. |

---

## Notes
- All ultrasonic sensors are read **one at a time** to reduce sensor interference.
- The buzzer mute button is wired as an **active-low input** using the ESP32-S3 internal pull-up resistor.
- The potentiometer is read through an **ADC channel** on GPIO 8.
- The four LEDs use **PWM brightness control** so intensity increases as obstacles get closer.
- The LCD is driven in **4-bit mode** using direct GPIO control.
- Distance values outside the expected sensor range are rejected to reduce noise and unstable readings.