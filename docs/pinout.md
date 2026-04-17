# SSD1306 배선

## 공통 연결
- SSD1306 VCC -> 보드 3V3
- SSD1306 GND -> 보드 GND
- SSD1306 SDA -> 보드 SDA
- SSD1306 SCL -> 보드 SCL

## 보드별 권장 핀
- nice!nano v2: Pro Micro 핀열의 `P1(SDA)`, `P0(SCL)` 사용
- XIAO nRF52840: `D4(SDA)`, `D5(SCL)` 사용

> 두 보드 모두 `boards/shields/*.overlay`에서 I2C0 + SSD1306(0x3C)로 설정됩니다.
