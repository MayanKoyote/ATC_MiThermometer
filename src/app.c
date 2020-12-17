#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "vendor/common/blt_common.h"

#include "battery.h"
#include "ble.h"
#include "flash.h"
#include "lcd.h"
#include "sensor.h"

RAM uint8_t main_loop_count = 254;
RAM uint32_t last_delay = 0xFFFF0000, last_adv_delay = 0xFFFF0000, last_battery_delay = 0xFFFF0000;
RAM bool last_smiley;
RAM uint8_t adv_count = 0;
RAM uint8_t meas_count = 254;
RAM int16_t last_temp;
RAM uint16_t last_humi;
RAM uint8_t battery_level;
RAM uint16_t battery_mv;
RAM bool show_batt_or_humi;

//Settings
RAM bool temp_C_or_F;
RAM bool blinking_smiley = false;
RAM bool comfort_smiley = true;
RAM bool show_batt_enabled = true;
RAM bool advertising_type = false;//Custom or Mi Advertising (true)
RAM uint8_t advertising_interval = 6;//advise new values - multiply by 10 for value
RAM uint8_t measure_interval = 10;//time = loop interval * factor (def: about 7 * X)
RAM uint8_t temp_alarm_point = 5;//divide by ten for value
RAM uint8_t humi_alarm_point = 5;

RAM int16_t comfort_x[] = {2000, 2560, 2700, 2500, 2050, 1700, 1600, 1750};
RAM uint16_t comfort_y[] = {2000, 1980, 3200, 6000, 8200, 8600, 7700, 3800};

_attribute_ram_code_ bool is_comfort(int16_t t, uint16_t h) {
	bool c = 0;
	uint8_t npol = sizeof(comfort_x);
	for (uint8_t i = 0, j = npol - 1; i < npol; j = i++) {
		if (((comfort_y[i] < comfort_y[j]) && (comfort_y[i] <= h) && (h
				<= comfort_y[j]) && ((comfort_y[j] - comfort_y[i]) * (t
				- comfort_x[i]) > (comfort_x[j] - comfort_x[i]) * (h
				- comfort_y[i]))) || ((comfort_y[i] > comfort_y[j])
				&& (comfort_y[j] <= h) && (h <= comfort_y[i]) && ((comfort_y[j]
				- comfort_y[i]) * (t - comfort_x[i]) < (comfort_x[j]
				- comfort_x[i]) * (h - comfort_y[i]))))
			c = !c;
	}
	return c;
}

_attribute_ram_code_ void WakeupLowPowerCb(int par) {
	(void) par;
	read_sensor_cb();
	if ((temp - last_temp > temp_alarm_point) || (last_temp - temp
			> temp_alarm_point) || (humi - last_humi > humi_alarm_point)
			|| (last_humi - humi > humi_alarm_point)) {// instant advertise on to much sensor difference
		set_adv_data(temp, humi, battery_level, battery_mv);
	}
	last_temp = temp;
	last_humi = humi;
}

_attribute_ram_code_ int app_suspend_enter(void) {
	if (ota_is_working) {
		bls_pm_setSuspendMask(SUSPEND_DISABLE);
		bls_pm_setManualLatency(0);
		return 0;
	} else {
		if (timer_measure_cb) {
			bls_pm_registerAppWakeupLowPowerCb(WakeupLowPowerCb);
			bls_pm_setAppWakeupLowPower(timer_measure_cb, 1);
			timer_measure_cb = 0;
		}
		else bls_pm_setAppWakeupLowPower(0, 0);
		bls_pm_setSuspendMask(
				SUSPEND_ADV | DEEPSLEEP_RETENTION_ADV | SUSPEND_CONN
						| DEEPSLEEP_RETENTION_CONN);
		return 1;
	}
}

_attribute_ram_code_ void func_suspend_enter(u8 e, u8 *p, int n) {
}
_attribute_ram_code_ void func_suspend_exit(u8 e, u8 *p, int n) { // ble_suspend_wakeup
}
_attribute_ram_code_ void func_gpio_wakeup(u8 e, u8 *p, int n) {
}

void user_init_normal(void) {//this will get executed one time after power up
	random_generator_init(); //must
	init_ble();
	//	bls_app_registerEventCallback(BLT_EV_FLAG_SUSPEND_ENTER, &func_suspend_enter);
	//	bls_app_registerEventCallback(BLT_EV_FLAG_SUSPEND_EXIT, &func_suspend_exit);
	//	bls_app_registerEventCallback(BLT_EV_FLAG_GPIO_EARLY_WAKEUP, &func_gpio_wakeup);
	init_sensor();
	init_lcd();
	init_flash();
	//	show_atc_mac();
	battery_mv = get_battery_mv();
	battery_level = get_battery_level(get_battery_mv());
	read_sensor_sleep();
	last_delay = clock_time();
	WakeupLowPowerCb(0);
}

_attribute_ram_code_ void user_init_deepRetn(void) {//after sleep this will get executed
	blc_ll_initBasicMCU();
	rf_set_power_level_index(RF_POWER_P3p01dBm);
	blc_ll_recoverDeepRetention();
}

void lcd(void) {
	if (temp_C_or_F) {
		show_temp_symbol(2);
		show_big_number(((((last_temp * 10) / 5) * 9) + 3200) / 10, 1);//convert C to F
	} else {
		show_temp_symbol(1);
		show_big_number(last_temp, 1);
	}
	if (!show_batt_enabled)
		show_batt_or_humi = true;

	if (show_batt_or_humi) { // Change between Humidity displaying and battery level if show_batt_enabled=true
		show_small_number(last_humi, 1);
		show_battery_symbol(0);
	} else {
		show_small_number((battery_level == 100) ? 99 : battery_level, 1);
		show_battery_symbol(1);
	}
	show_batt_or_humi = !show_batt_or_humi;
	if (ble_get_connected()) { // If connected notify Sensor data
		ble_send_temp(last_temp);
		ble_send_humi(last_humi);
		ble_send_battery(battery_level);
	}
	if ((clock_time() - last_adv_delay) > (advertising_type ? 5000 : 10000)
			* CLOCK_SYS_CLOCK_1MS) { // Advetise data delay
		if (adv_count >= advertising_interval) {
			set_adv_data(last_temp, last_humi, battery_level, battery_mv);
			last_adv_delay = clock_time();
			adv_count = 0;
		}
		adv_count++;
	}
	if (comfort_smiley) {
		if (is_comfort(last_temp * 10, last_humi * 100)) {
			show_smiley(1);
		} else {
			show_smiley(2);
		}
	}
	if (blinking_smiley) {//If Smiley should blink do it
		last_smiley = !last_smiley;
		show_smiley(last_smiley);
	}
	update_lcd();
}

void main_loop() {
	blt_sdk_main_loop();
	//bls_pm_setSuspendMask(SUSPEND_DISABLE);
	if ((clock_time() - last_delay) > 2 * CLOCK_SYS_CLOCK_1S) { // main loop delay x sec
		last_delay = clock_time();
		if (++main_loop_count == 1) {
			battery_mv = get_battery_mv();
			battery_level = get_battery_level(get_battery_mv());
			last_battery_delay = clock_time();
		} else if (main_loop_count == 2) {
			read_sensor_deep_sleep();
		} else if (main_loop_count >= 3) {
			main_loop_count = 0;
			lcd();
		}
	}
	app_suspend_enter();
}