# M5Paper (IT8951) E-paper Refresh Timings

Measured on the M5Paper e-paper display (IT8951 controller, firmware `5mtsca`,
LUT `M841`, 960×540 panel) at room temperature.

Each value is the time from issuing `display_area(mode)` until the display engine
reports idle (`LUTAFSR == 0`), for a full white→black transition. Captured with the
`wavetime` debug-console command:

```powershell
python tools/serial_console.py --port COM5 --wait 75 wavetime
```

| Mode | Full panel 960×540 | Screen region 480×480 |
|------|-------------------:|----------------------:|
| INIT (mode 0) | 1088 ms | 1083 ms |
| GC16 (mode 2) |  467 ms |  463 ms |
| GL16 (mode 3) |  473 ms |  468 ms |
| DU   (mode 1) |  260 ms |  256 ms |
| A2   (mode 6) |  161 ms |  155 ms |
