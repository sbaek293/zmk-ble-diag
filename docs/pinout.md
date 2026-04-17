# SSD1306 배선

## 공통 연결
- SSD1306 VCC -> 보드 3V3
- SSD1306 GND -> 보드 GND
- SSD1306 SDA -> 보드 SDA
- SSD1306 SCL -> 보드 SCL

## 보드별 권장 핀
- nice!nano v2: `P0.17(SDA)`, `P0.20(SCL)` 사용
- XIAO nRF52840: `P0.04(SDA)`, `P0.05(SCL)` 사용

> `boards/shields/*.overlay`에서 pinctrl로 핀이 명시 지정되며, nice!nano는 i2c0, XIAO는 i2c1 버스를 사용합니다.
