# zmk-ble-diag

nice!nano v2 / XIAO nRF52840용 BLE 진단 펌웨어 스켈레톤입니다.

## 구현 내용
- USB-C 전원 인가 후 자동 진단 시작 (`main()` 즉시 수행)
- 슬립 없이 무한 루프에서 로그 기록 유지
- 5개 채널 순환 진단(RX test mode): `0 -> 10 -> 20 -> 30 -> 39`
- 채널별 독립 진단: 한 채널에서 오류가 발생해도 나머지 채널은 계속 진단
- 한 라운드 완료 후 채널별 결과 요약 화면 표시 (3초 후 다음 라운드 시작)
- SSD1306(128x64, I2C 0x3C)에 현재 진단 상태 표시
- 보드별 오버레이 분리
  - `boards/shields/nice_nano_v2_ble_diag.overlay`
  - `boards/shields/xiao_nrf52840_ble_diag.overlay`

## SSD1306 배선
자세한 배선은 아래 문서 참고:
- `docs/pinout.md`

요약:
- VCC -> 3V3
- GND -> GND
- SDA -> SDA
- SCL -> SCL

## 상태 표시 예시

### 스캔 중 (채널 진단 진행)
- 1행: `BLE DIAG R:<라운드번호>`
- 2행: `CH:<채널> <인덱스>/5`
- 3행: `PKT:<현재 슬롯 누적 패킷>`

### 라운드 완료 요약
- 1행: `DONE R:<라운드> E:<에러수>`
- 2행: `c0:<결과> c10:<결과>`
- 3행: `c20:<결과> c30:<결과>`
- 4행: `c39:<결과>`

결과 약어: `OK`=성공, `RX`=RX 시작 실패, `EN`=테스트 종료 실패

## 빌드 예시 (west workspace)

```bash
# 1. 워크스페이스 디렉터리를 만들고 이 저장소를 config/ 에 클론
mkdir zmk-workspace && cd zmk-workspace
git clone https://github.com/sbaek293/zmk-ble-diag config

# 2. west 워크스페이스 초기화 (zmk-workspace/ 에서 실행)
west init -l config
west update
west zephyr-export

# 3. 보드별 빌드 (zmk-workspace/ 에서 실행)
west build -b nice_nano/nrf52840 config \
  -- -DDTC_OVERLAY_FILE="$(pwd)/config/boards/shields/nice_nano_v2_ble_diag.overlay" \
     -DBOARD_ROOT="$(pwd)/zmk/app/module"

west build -b xiao_ble config \
  -- -DDTC_OVERLAY_FILE="$(pwd)/config/boards/shields/xiao_nrf52840_ble_diag.overlay" \
     -DBOARD_ROOT="$(pwd)/zmk/app/module"
```

## GitHub Actions 빌드
- 워크플로우: `.github/workflows/build-firmware.yml`
- 빌드 대상:
  - `firmware-xiao-uf2` 아티팩트 (`firmware-xiao-uf2.uf2`)
  - `firmware-nice-nano-v2-uf2` 아티팩트 (`firmware-nice-nano-v2-uf2.uf2`)
- 실행 후 Actions 아티팩트에서 각 보드의 `.uf2` 파일을 개별 다운로드할 수 있습니다.
