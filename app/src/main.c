#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>

LOG_MODULE_REGISTER(ble_diag, LOG_LEVEL_INF);

#define CHANNEL_COUNT 5
#define CHANNEL_DWELL_MS 5000
#define DISPLAY_LINE_BUFFER_SIZE 21
#define DISPLAY_INIT_RETRY_COUNT 20
#define DISPLAY_INIT_RETRY_DELAY_MS 50
#define DISPLAY_BOOT_SPLASH_MS 300
#define BT_POST_ENABLE_DELAY_MS 200
#define TEST_ERROR_BACKOFF_MS 200
#define PACKET_COUNT_ERROR UINT32_MAX

/* Longest string we ever pass to cfb_print. The selected font must fit
 * this many characters within the display width (128 px). */
#define DISPLAY_MAX_LINE_CHARS 14U
#define DISPLAY_WIDTH_PX       128U

static const uint8_t diag_channels[CHANNEL_COUNT] = {0, 10, 20, 30, 39};
static uint32_t packet_count[CHANNEL_COUNT] = {0};
static int current_channel_idx = 0;

static const struct device *display_dev;

static void display_status(void)
{
	if (!display_dev) {
		return;
	}

	char line0[DISPLAY_LINE_BUFFER_SIZE];
	char line1[DISPLAY_LINE_BUFFER_SIZE];
	char line2[DISPLAY_LINE_BUFFER_SIZE];
	int one_based_channel_idx = current_channel_idx + 1;

	snprintk(line0, sizeof(line0), "BLE DIAG RUN");
	snprintk(line1, sizeof(line1), "CH:%u IDX:%d", diag_channels[current_channel_idx],
		 one_based_channel_idx);
	if (packet_count[current_channel_idx] == PACKET_COUNT_ERROR) {
		snprintk(line2, sizeof(line2), "PKT:ERR");
	} else {
		snprintk(line2, sizeof(line2), "PKT:%u", packet_count[current_channel_idx]);
	}

	cfb_framebuffer_clear(display_dev, true);
	cfb_print(display_dev, line0, 0, 0);
	cfb_print(display_dev, line1, 0, 1);
	cfb_print(display_dev, line2, 0, 2);
	cfb_framebuffer_finalize(display_dev);
}

static void setup_display(void)
{
	int err;
	int retries_left;

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		display_dev = NULL;
		LOG_WRN("SSD1306 display is not ready");
		return;
	}

	retries_left = DISPLAY_INIT_RETRY_COUNT;
	do {
		err = cfb_framebuffer_init(display_dev);
		if (!err) {
			break;
		}
		retries_left--;
		if (retries_left > 0) {
			k_sleep(K_MSEC(DISPLAY_INIT_RETRY_DELAY_MS));
		}
	} while (retries_left > 0);
	if (err) {
		display_dev = NULL;
		LOG_WRN("CFB init failed after %d retries (%d)", DISPLAY_INIT_RETRY_COUNT, err);
		return;
	}

	/* Select the largest font whose character width lets DISPLAY_MAX_LINE_CHARS
	 * fit within 128 px. cfb_framebuffer_init() may default to a large font
	 * (e.g. 16x26) that causes cfb_print to fail for strings over ~7 chars. */
	{
		int num_fonts = cfb_get_numof_fonts(display_dev);
		uint8_t sel_font = 0;
		uint8_t best_font_width = 0;

		for (int i = 0; i < num_fonts; i++) {
			uint8_t w = 0, h = 0;

			if (cfb_get_font_size(display_dev, i, &w, &h) == 0 && w > 0 && h > 0 &&
			    (uint32_t)w * DISPLAY_MAX_LINE_CHARS <= DISPLAY_WIDTH_PX &&
			    w > best_font_width) {
				best_font_width = w;
				sel_font = i;
			}
		}
		cfb_framebuffer_set_font(display_dev, sel_font);
		LOG_INF("CFB font selected: idx=%u width=%u", sel_font, best_font_width);
	}

	retries_left = DISPLAY_INIT_RETRY_COUNT;
	do {
		err = display_blanking_off(display_dev);
		if (!err) {
			break;
		}
		retries_left--;
		if (retries_left > 0) {
			k_sleep(K_MSEC(DISPLAY_INIT_RETRY_DELAY_MS));
		}
	} while (retries_left > 0);
	if (err) {
		display_dev = NULL;
		LOG_WRN("Display blanking off failed after %d retries (%d)", DISPLAY_INIT_RETRY_COUNT,
			err);
		return;
	}

	cfb_framebuffer_clear(display_dev, true);
	err = cfb_print(display_dev, "BLE DIAG BOOT", 0, 0);
	if (err < 0) {
		LOG_WRN("Failed to print boot line 0 (%d)", err);
	}

	err = cfb_print(display_dev, "DISPLAY OK", 0, 1);
	if (err < 0) {
		LOG_WRN("Failed to print boot line 1 (%d)", err);
	}

	cfb_framebuffer_finalize(display_dev);
	k_sleep(K_MSEC(DISPLAY_BOOT_SPLASH_MS));
	display_status();
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

static void stop_test_if_running(void)
{
	struct net_buf *buf;
	struct net_buf *rsp = NULL;

	buf = bt_hci_cmd_create(BT_HCI_OP_LE_TEST_END, 0);
	if (buf && !bt_hci_cmd_send_sync(BT_HCI_OP_LE_TEST_END, buf, &rsp) && rsp) {
		net_buf_unref(rsp);
	}
}

static void display_stop_message(void)
{
	if (!display_dev) {
		return;
	}

	cfb_framebuffer_clear(display_dev, true);
	cfb_print(display_dev, "BLE DIAG STOP", 0, 0);
	cfb_framebuffer_finalize(display_dev);
}

int main(void)
{
	int err;

	LOG_INF("Starting BLE diagnostics");
	setup_display();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		display_stop_message();
		return err;
	}
	k_sleep(K_MSEC(BT_POST_ENABLE_DELAY_MS));

	while (1) {
		uint8_t channel = diag_channels[current_channel_idx];

		err = run_rx_test(channel);
		if (err) {
			LOG_WRN("LE RX test start failed, retrying (ch=%u, err=%d)",
				channel, err);
			stop_test_if_running();
			k_sleep(K_MSEC(TEST_ERROR_BACKOFF_MS));
			continue;
		}

		k_sleep(K_MSEC(CHANNEL_DWELL_MS));

		err = stop_test_and_read_count(&packet_count[current_channel_idx]);
		if (err) {
			LOG_WRN("LE test end failed, continuing (ch=%u, err=%d)",
				channel, err);
			packet_count[current_channel_idx] = PACKET_COUNT_ERROR;
			stop_test_if_running();
			k_sleep(K_MSEC(TEST_ERROR_BACKOFF_MS));
		} else {
			uint32_t uptime_sec = k_uptime_get_32() / 1000U;
			LOG_INF("t=%us CH=%u idx=%d pkt=%u", uptime_sec, channel,
				current_channel_idx + 1, packet_count[current_channel_idx]);
		}
		display_status();

		current_channel_idx = (current_channel_idx + 1) % CHANNEL_COUNT;
	}

	return 0;
}
