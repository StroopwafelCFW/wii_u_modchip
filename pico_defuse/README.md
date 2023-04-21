# pico de_Fuse

Based on [PicoBoot](https://github.com/webhdx/PicoBoot)

## Wiring

| Pico Pin Name  | Wii U Testpoint | Description |
| ---- | ------------- |
| GP2  | TP50 | DEBUG0 |
| GP3  | TP51 | DEBUG1 |
| GP4  | TP52 | DEBUG2 |
| GP5  | TP53 | DEBUG3 |
| ---- | ------------- |
| GP6  | TP55 | DEBUG4 |
| GP7  | TP54 | DEBUG5 |
| GP8  | TP48 | DEBUG6 |
| GP9  | TP49 | DEBUG7 |
| ---- | ------------- |
| GP10 | TP176 | EXI0 MISO |
| GP11 | TP176 | EXI0 MISO |
| GP12 | TP176 | EXI0 MISO |
| GP13 | TP176 | EXI0 MISO |
| ---- | ------------- |
| GP14  | TP101 | EXI0 CLK |
| GP15  | TP144 (alternatively: R677) | NRST |
| ---- | ------------- |
| GP18 |  TP48 | DEBUG6 |
| GP19 |  TP48 | DEBUG6 |
| GP20 |  TP48 | DEBUG6 |
| GP21 |  TP48 | DEBUG6 |
| ---- | ------------- |
| GND |  GND, and/or TP137 | GND |
| 3V3(OUT) |  TP138 | Always-On 3.3v (TODO: should this go in VSYS instead?) |