/**
 * @file status_led_test.c
 * @brief Simple Status LED Test - 5 Second Visual Confirmation
 *
 * Simple test with 5-second intervals for easy visual confirmation:
 * 1. Green LED ON - 5 seconds
 * 2. Red LED ON - 5 seconds
 * 3. Both LEDs ON - 5 seconds
 * 4. Both LEDs OFF - 5 seconds
 * Repeats continuously
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <drivers/status_led.h>

LOG_MODULE_REGISTER(status_led_test, LOG_LEVEL_INF);

/* Get the Status LED device from device tree */
#define STATUS_LED_DEV_NODE_A DT_NODELABEL(status_led_a)
#define STATUS_LED_DEV_NODE_B DT_NODELABEL(status_led_b)

/* Test interval - 10 seconds for visual confirmation */
#define TEST_INTERVAL_MS 10000

static const struct device *status_led_dev[2] = {NULL, NULL};

/* Test helper functions */
static void test_setup(void)
{
	int ret;

	status_led_dev[0] = DEVICE_DT_GET(STATUS_LED_DEV_NODE_A);

	if (!device_is_ready(status_led_dev[0])) {
		printk("ERROR: Status LED device A not ready\n");
	}

	status_led_dev[1] = DEVICE_DT_GET(STATUS_LED_DEV_NODE_B);
	if (!device_is_ready(status_led_dev[1])) {
		printk("ERROR: Status LED device B not ready\n");
	}
	for (int i = 0; i < 2; i++) {

		ret = z_status_led_set_state(status_led_dev[i], STATUS_LED_OFF);
		if (ret != 0) {
			printk("ERROR: Failed to turn off LED %d (ret=%d)\n", i, ret);
			return;
		}
	}
}

int test_status_led_simple_cycle(void)
{
	int ret;

	printk("=== SIMPLE STATUS LED TEST - Visual Confirmation ===\n");
	printk("Each step will last %d seconds for visual confirmation\n", TEST_INTERVAL_MS / 1000);
	printk("Press reset to stop the test\n\n");

	const enum status_led_state test_states[] = {STATUS_LED_ON, STATUS_LED_BLINK_SLOW,
						     STATUS_LED_BLINK_FAST, STATUS_LED_HEARTBEAT,
						     STATUS_LED_OFF};

	const char *const test_state_names[] = {
		"ON", "BLINK_SLOW", "BLINK_FAST", "HEARTBEAT", "OFF",
	};

	printk("Each state will last %d seconds\n", TEST_INTERVAL_MS / 1000);
	for (int i = 0; i < 2; i++) {
		for (size_t j = 0; j < ARRAY_SIZE(test_states); j++) {
			enum status_led_state state = test_states[j];

			/* Turn off both LEDs at the end */

			printk("Testing LED %d state: %s\n", i, test_state_names[j]);

			ret = z_status_led_set_state(status_led_dev[i], state);
			if (ret != 0) {
				printk("ERROR: Failed to set LED %d state %d (ret=%d)\n", i, state,
				       ret);
				return ret;
			}
			k_msleep(TEST_INTERVAL_MS);
		}
	}
	return 0;
}

int status_led_test_suite(void)
{
	/* Initialize test setup */
	test_setup();

	/* Run simple cycle test */
	return test_status_led_simple_cycle();
}

int main(void)
{
	while (1) {
		k_sleep(K_MSEC(1000));
		status_led_test_suite();
	}

	return 0;
}
