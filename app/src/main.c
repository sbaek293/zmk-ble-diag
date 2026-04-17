#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

LOG_MODULE_REGISTER(ble_diag, LOG_LEVEL_INF);

#define CHANNEL_COUNT 5
#define CHANNEL_DWELL_MS 5000
#define DISPLAY_LINE_BUFFER_SIZE 21

static const uint8_t diag_channels[CHANNEL_COUNT] = {0, 10, 20, 30, 39};
static uint32_t packet_count[CHANNEL_COUNT];
static int current_channel_idx;

static const struct device *display_dev;

static void display_status(void)
{
	if (!display_dev) {
		return;
	}

	char line0[DISPLAY_LINE_BUFFER_SIZE];
	char line1[DISPLAY_LINE_BUFFER_SIZE];
	char line2[DISPLAY_LINE_BUFFER_SIZE];

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

static int run_rx_test(uint8_t channel)
{
	struct bt_hci_cp_le_rx_test *cp;
	struct net_buf *buf;

	buf = bt_hci_cmd_create(BT_HCI_OP_LE_RX_TEST, sizeof(*cp));
	if (!buf) {
		return -ENOMEM;
	}

	cp = net_buf_add(buf, sizeof(*cp));
	cp->rx_ch = channel;

	return bt_hci_cmd_send_sync(BT_HCI_OP_LE_RX_TEST, buf, NULL);
}

static int stop_test_and_read_count(uint32_t *count)
{
	struct bt_hci_rp_le_test_end *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	int err;

	buf = bt_hci_cmd_create(BT_HCI_OP_LE_TEST_END, 0);
	if (!buf) {
		return -ENOMEM;
	}

	err = bt_hci_cmd_send_sync(BT_HCI_OP_LE_TEST_END, buf, &rsp);
	if (err) {
		return err;
	}

	rp = (struct bt_hci_rp_le_test_end *)rsp->data;
	*count = sys_le16_to_cpu(rp->rx_pkt_count);
	net_buf_unref(rsp);

	return 0;
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

	while (1) {
		uint8_t channel = diag_channels[current_channel_idx];

		err = run_rx_test(channel);
		if (err) {
			LOG_ERR("LE RX test start failed, exiting diagnostic loop (ch=%u, err=%d)",
				channel, err);
			return err;
		}

		k_sleep(K_MSEC(CHANNEL_DWELL_MS));

		err = stop_test_and_read_count(&packet_count[current_channel_idx]);
		if (err) {
			LOG_ERR("LE test end failed, exiting diagnostic loop (ch=%u, err=%d)",
				channel, err);
			return err;
		}

		uptime_sec = k_uptime_get_32() / 1000U;
		LOG_INF("t=%us CH=%u idx=%d pkt=%u", uptime_sec, channel,
			current_channel_idx + 1, packet_count[current_channel_idx]);
		display_status();

		current_channel_idx = (current_channel_idx + 1) % CHANNEL_COUNT;
	}

	return 0;
}
