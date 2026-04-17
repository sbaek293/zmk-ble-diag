#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(ble_diag, LOG_LEVEL_INF);

#define CHANNEL_COUNT 5
#define CHANNEL_DWELL_MS 5000

static const uint8_t diag_channels[CHANNEL_COUNT] = {37, 38, 39, 0, 20};
static uint32_t packet_count[CHANNEL_COUNT];
static int current_channel_idx;

static const struct device *display_dev;

static void display_status(void)
{
if (!display_dev) {
return;
}

char line0[21];
char line1[21];
char line2[21];

snprintk(line0, sizeof(line0), "BLE DIAG RUN");
snprintk(line1, sizeof(line1), "CH:%u IDX:%d", diag_channels[current_channel_idx],
 current_channel_idx + 1);
snprintk(line2, sizeof(line2), "PKT:%u", packet_count[current_channel_idx]);

cfb_framebuffer_clear(display_dev, true);
cfb_print(display_dev, line0, 0, 0);
cfb_print(display_dev, line1, 0, 1);
cfb_print(display_dev, line2, 0, 2);
cfb_framebuffer_finalize(display_dev);
}

static void scan_recv(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
ARG_UNUSED(ad);

if (!info) {
return;
}

if (info->chan == diag_channels[current_channel_idx]) {
packet_count[current_channel_idx]++;
}
}

static struct bt_le_scan_cb scan_callbacks = {
.recv = scan_recv,
};

static void setup_display(void)
{
display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
if (!device_is_ready(display_dev)) {
display_dev = NULL;
LOG_WRN("SSD1306 display is not ready");
return;
}

if (cfb_framebuffer_init(display_dev)) {
display_dev = NULL;
LOG_WRN("CFB init failed");
return;
}

cfb_framebuffer_clear(display_dev, true);
cfb_framebuffer_finalize(display_dev);
}

int main(void)
{
int err;
uint32_t uptime_sec;

LOG_INF("Starting BLE diagnostics");
setup_display();

err = bt_enable(NULL);
if (err) {
LOG_ERR("Bluetooth init failed (%d)", err);
return err;
}

bt_le_scan_cb_register(&scan_callbacks);
err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
if (err) {
LOG_ERR("Scan start failed (%d)", err);
return err;
}

while (1) {
uptime_sec = k_uptime_get_32() / 1000U;
LOG_INF("t=%us CH=%u idx=%d pkt=%u", uptime_sec,
diag_channels[current_channel_idx], current_channel_idx + 1,
packet_count[current_channel_idx]);

display_status();
k_sleep(K_MSEC(CHANNEL_DWELL_MS));
current_channel_idx = (current_channel_idx + 1) % CHANNEL_COUNT;
}

return 0;
}
