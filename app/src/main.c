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
#define SUMMARY_DISPLAY_MS 3000
#define DISPLAY_LINE_BUFFER_SIZE 21
#define DISPLAY_INIT_RETRY_COUNT 20
#define DISPLAY_INIT_RETRY_DELAY_MS 50
#define DISPLAY_BOOT_SPLASH_MS 300
#define BT_POST_ENABLE_DELAY_MS 200

/* Longest string we ever pass to cfb_print. The selected font must fit
 * this many characters within the display width (128 px). */
#define DISPLAY_MAX_LINE_CHARS 14U
#define DISPLAY_WIDTH_PX       128U
#define DISPLAY_LINE_SPACING_PX 2U

typedef enum {
	CH_PENDING = 0,
	CH_OK,
	CH_RX_ERR,
	CH_END_ERR,
} ch_status_t;

static const uint8_t diag_channels[CHANNEL_COUNT] = {0, 10, 20, 30, 39};
static uint32_t packet_count[CHANNEL_COUNT] = {0};
static ch_status_t ch_status[CHANNEL_COUNT] = {0};
static int ch_err_code[CHANNEL_COUNT] = {0};
static int current_channel_idx = 0;
static uint32_t round_count = 0;

static const struct device *display_dev;
static uint8_t font_height = 8; /* updated by setup_display() */
static uint8_t line_pitch = 10; /* updated by setup_display() */

static void display_print_line(const char *str, int row)
{
	cfb_print(display_dev, (char *)str, 0, (uint16_t)(row * line_pitch));
}

static void display_stopped(const char *line1_msg, const char *line2_msg)
{
	if (!display_dev) {
		return;
	}
	cfb_framebuffer_clear(display_dev, true);
	display_print_line("BLE DIAG STOP", 0);
	display_print_line(line1_msg, 1);
	if (line2_msg && line2_msg[0] != '\0') {
		display_print_line(line2_msg, 2);
	}
	cfb_framebuffer_finalize(display_dev);
}

static void display_status(void)
{
	if (!display_dev) {
		return;
	}

	char line0[DISPLAY_LINE_BUFFER_SIZE];
	char line1[DISPLAY_LINE_BUFFER_SIZE];
	char line2[DISPLAY_LINE_BUFFER_SIZE];

	snprintk(line0, sizeof(line0), "BLE DIAG R:%u", round_count + 1);
	snprintk(line1, sizeof(line1), "CH:%u %d/5",
		 diag_channels[current_channel_idx], current_channel_idx + 1);
	snprintk(line2, sizeof(line2), "PKT:%u", packet_count[current_channel_idx]);

	cfb_framebuffer_clear(display_dev, true);
	display_print_line(line0, 0);
	display_print_line(line1, 1);
	display_print_line(line2, 2);
	cfb_framebuffer_finalize(display_dev);
}

static void display_summary(uint32_t err_count)
{
	if (!display_dev) {
		return;
	}

	static const char *const status_str[] = {"??", "OK", "RX", "EN"};
	char line0[DISPLAY_LINE_BUFFER_SIZE];
	char line1[DISPLAY_LINE_BUFFER_SIZE];
	char line2[DISPLAY_LINE_BUFFER_SIZE];
	char line3[DISPLAY_LINE_BUFFER_SIZE];

	/* Row 0: "DONE R:N E:M" – round number and total error count */
	snprintk(line0, sizeof(line0), "DONE R:%u E:%u", round_count, err_count);
	/* Rows 1-3: two channels per row (XX = OK | RX | EN) */
	snprintk(line1, sizeof(line1), "c%u:%s c%u:%s",
		 diag_channels[0], status_str[ch_status[0]],
		 diag_channels[1], status_str[ch_status[1]]);
	snprintk(line2, sizeof(line2), "c%u:%s c%u:%s",
		 diag_channels[2], status_str[ch_status[2]],
		 diag_channels[3], status_str[ch_status[3]]);
	snprintk(line3, sizeof(line3), "c%u:%s",
		 diag_channels[4], status_str[ch_status[4]]);

	cfb_framebuffer_clear(display_dev, true);
	display_print_line(line0, 0);
	display_print_line(line1, 1);
	display_print_line(line2, 2);
	display_print_line(line3, 3);
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

	/* Select the smallest font that still fits the target line length to reduce
	 * overlap risk and improve readability on small OLED modules. */
	{
		int num_fonts = cfb_get_numof_fonts(display_dev);
		uint8_t sel_font = 0;
		uint8_t sel_font_width = 0;
		uint8_t sel_font_height = 0;
		bool found = false;

		for (int i = 0; i < num_fonts; i++) {
			uint8_t w = 0, h = 0;

			/* Prefer smaller glyph height first, then smaller width for ties. */
			if (cfb_get_font_size(display_dev, i, &w, &h) == 0 && w > 0 && h > 0 &&
			    (uint32_t)w * DISPLAY_MAX_LINE_CHARS <= DISPLAY_WIDTH_PX &&
			    (!found || h < sel_font_height ||
			     (h == sel_font_height && w < sel_font_width))) {
				found = true;
				sel_font = i;
				sel_font_width = w;
				sel_font_height = h;
			}
		}
		cfb_framebuffer_set_font(display_dev, sel_font);
		font_height = (sel_font_height > 0) ? sel_font_height : 8;
		line_pitch = font_height + DISPLAY_LINE_SPACING_PX;
		LOG_INF("CFB font selected: idx=%u width=%u height=%u pitch=%u", sel_font,
			sel_font_width, font_height, line_pitch);
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

	err = cfb_print(display_dev, "DISPLAY OK", 0, line_pitch);
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

static void stop_test_best_effort(void)
{
	struct net_buf *buf;
	struct net_buf *rsp = NULL;

	buf = bt_hci_cmd_create(BT_HCI_OP_LE_TEST_END, 0);
	if (buf && !bt_hci_cmd_send_sync(BT_HCI_OP_LE_TEST_END, buf, &rsp) && rsp) {
		net_buf_unref(rsp);
	}
}

int main(void)
{
	int err;

	LOG_INF("Starting BLE diagnostics");
	setup_display();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		char errline[DISPLAY_LINE_BUFFER_SIZE];

		snprintk(errline, sizeof(errline), "errno:%d", err);
		display_stopped("BT INIT ERR", errline);
		return err;
	}
	k_sleep(K_MSEC(BT_POST_ENABLE_DELAY_MS));

	while (1) {
		uint32_t err_count = 0;

		/* Reset per-channel state for this round */
		for (int i = 0; i < CHANNEL_COUNT; i++) {
			ch_status[i] = CH_PENDING;
			ch_err_code[i] = 0;
		}

		/* Scan all channels; errors are recorded but do not stop the round */
		for (current_channel_idx = 0; current_channel_idx < CHANNEL_COUNT;
		     current_channel_idx++) {
			uint8_t channel = diag_channels[current_channel_idx];

			display_status();

			err = run_rx_test(channel);
			if (err) {
				LOG_ERR("LE RX test start failed (ch=%u, err=%d)", channel, err);
				ch_status[current_channel_idx] = CH_RX_ERR;
				ch_err_code[current_channel_idx] = err;
				stop_test_best_effort();
				err_count++;
				continue;
			}

			k_sleep(K_MSEC(CHANNEL_DWELL_MS));

			err = stop_test_and_read_count(&packet_count[current_channel_idx]);
			if (err) {
				LOG_ERR("LE test end failed (ch=%u, err=%d)", channel, err);
				ch_status[current_channel_idx] = CH_END_ERR;
				ch_err_code[current_channel_idx] = err;
				stop_test_best_effort();
				err_count++;
				continue;
			}

			ch_status[current_channel_idx] = CH_OK;
			uint32_t uptime_sec = k_uptime_get_32() / 1000U;

			LOG_INF("t=%us CH=%u idx=%d pkt=%u", uptime_sec, channel,
				current_channel_idx + 1, packet_count[current_channel_idx]);
		}

		round_count++;
		LOG_INF("Round %u done: %u error(s)", round_count, err_count);
		display_summary(err_count);
		k_sleep(K_MSEC(SUMMARY_DISPLAY_MS));
	}

	return 0;
}
