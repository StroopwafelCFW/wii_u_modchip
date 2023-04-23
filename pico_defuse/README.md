# pico de_Fuse

Based on [PicoBoot](https://github.com/webhdx/PicoBoot)

## Wiring

| Pico Pin Name  | Wii U Testpoint | Description |
| -------------- | --------------- | ----------- |
| GP2            | TP50            | DEBUG0      |
| GP3            | TP51            | DEBUG1      |
| GP4            | TP52            | DEBUG2      |
| GP5            | TP53            | DEBUG3      |
|                |                 |             |
| GP6            | TP55            | DEBUG4      |
| GP7            | TP54            | DEBUG5      |
| GP8            | TP48            | DEBUG6      |
| GP9            | TP49            | DEBUG7      |
|                |                 |             |
| GP10           | TP176           | EXI0 MISO   |
| GP11           | TP176           | EXI0 MISO   |
| GP12           | TP176           | EXI0 MISO   |
| GP13           | TP176           | EXI0 MISO   |
|                |                 |             |
| GP14           | TP101           | EXI0 CLK    |
| GP15           | TP144 (alternatively: R677) | NRST |
|                |                 |             |
| GP18           | TP48            | DEBUG6      |
| GP19           | TP48            | DEBUG6      |
| GP20           | TP48            | DEBUG6      |
| GP21           | TP48            | DEBUG6      |
|                |                 |             |
| GND            |  GND, and/or TP164 |      GND |
| 3V3(OUT)       |  TP122             | Always-On 3.3v |

For wiring to the Pico, I highly suggest using [~38AWG enameled magnet wire](https://www.amazon.com/BNTECHGO-AWG-Magnet-Wire-Transformers/dp/B0823C7C2H) (**not** solid core or stranded). Some of the test points are close to the GPU voltage rails (which are high amperage); A poor solder joint breaking off and touching something else can be catastrophic.

The best location to mount the Pico is still an open question. For my main Wii U, I'm currently planning on placing kapton tape over the Nintendo logo on the top side of the PCB, wrapping my Pico in kapton tape, and then adhering the Pico to the kapton tape with double-sided foam. Wires can be safely routed from the bottom of the PCB to the top through the WiFi antenna divots. I'm also planning on using a [microUSB breakout board](https://www.amazon.com/Adafruit-Micro-B-Breakout-Board-ADA1833/dp/B00KLDPZVU) (wired to the Pico Testpoints) above the HDMI port for serial output.

Apparently some people have managed to [shove entire SSDs](https://gbatemp.net/threads/wii-u-internal-ssd-with-activity-led.606315/) on the underside of the board using electrical tape, so space clearly isn't that big of an issue. But the bigger issue is that the Pico microUSB is currently the only way to perform backups/restores/diagnostics (and the firmware hasn't quite finalized yet), so the USB cable from the Pico has to go ~somewhere out the front of the console or the back.