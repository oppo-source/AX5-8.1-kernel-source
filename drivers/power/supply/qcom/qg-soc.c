/* Copyright (c) 2018 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"QG-K: %s: " fmt, __func__

#include <linux/alarmtimer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <uapi/linux/qg.h>
#include "fg-alg.h"
#include "qg-sdam.h"
#include "qg-core.h"
#include "qg-reg.h"
#include "qg-util.h"
#include "qg-defs.h"

#define DEFAULT_UPDATE_TIME_MS			64000
#define SOC_SCALE_HYST_MS			2000

static int qg_delta_soc_interval_ms = 20000;
module_param_named(
	soc_interval_ms, qg_delta_soc_interval_ms, int, 0600
);

static int qg_delta_soc_cold_interval_ms = 4000;
module_param_named(
	soc_cold_interval_ms, qg_delta_soc_cold_interval_ms, int, 0600
);

static void get_next_update_time(struct qpnp_qg *chip)
{
	int soc_points = 0, batt_temp = 0;
	int min_delta_soc_interval_ms = qg_delta_soc_interval_ms;
	int rc = 0, rt_time_ms = 0, full_time_ms = DEFAULT_UPDATE_TIME_MS;

	get_fifo_done_time(chip, false, &full_time_ms);
	get_fifo_done_time(chip, true, &rt_time_ms);

	full_time_ms = CAP(0, DEFAULT_UPDATE_TIME_MS,
				full_time_ms - rt_time_ms);

	soc_points = abs(chip->msoc - chip->catch_up_soc);
	if (chip->maint_soc > 0)
		soc_points = max(abs(chip->msoc - chip->maint_soc), soc_points);
	soc_points /= chip->dt.delta_soc;

	/* Lower the delta soc interval by half at cold */
	rc = qg_get_battery_temp(chip, &batt_temp);
	if (rc < 0)
		pr_err("Failed to read battery temperature rc=%d\n", rc);
	else if (batt_temp < chip->dt.cold_temp_threshold)
		min_delta_soc_interval_ms = qg_delta_soc_cold_interval_ms;

	if (!min_delta_soc_interval_ms)
		min_delta_soc_interval_ms = 1000;	/* 1 second */

	chip->next_wakeup_ms = (full_time_ms / (soc_points + 1))
					- SOC_SCALE_HYST_MS;
	chip->next_wakeup_ms = max(chip->next_wakeup_ms,
				min_delta_soc_interval_ms);

	qg_dbg(chip, QG_DEBUG_SOC, "fifo_full_time=%d secs fifo_real_time=%d secs soc_scale_points=%d\n",
			full_time_ms / 1000, rt_time_ms / 1000, soc_points);
}

static bool is_scaling_required(struct qpnp_qg *chip)
{
	if (!chip->profile_loaded)
		return false;

	if (chip->maint_soc > 0 &&
		(abs(chip->maint_soc - chip->msoc) >= chip->dt.delta_soc))
		return true;

	if ((abs(chip->catch_up_soc - chip->msoc) < chip->dt.delta_soc) &&
		chip->catch_up_soc != 0 && chip->catch_up_soc != 100)
		return false;

	if (chip->catch_up_soc == chip->msoc)
		/* SOC has not changed */
		return false;


	if (chip->catch_up_soc > chip->msoc && !is_usb_present(chip))
		/* USB is not present and SOC has increased */
		return false;

	return true;
}

static void update_msoc(struct qpnp_qg *chip)
{
	int rc = 0, batt_temp = 0,  batt_soc_32bit = 0;
	bool usb_present = is_usb_present(chip);

#ifdef VENDOR_EDIT
/* Yichun.Chen  PSW.BSP.CHG  2018-06-13  avoid when reboot soc reduce 1% */
        if (chip->skip_scale_soc_count < 2) {
                chip->catch_up_soc = chip->msoc;
                chip->skip_scale_soc_count ++;
        }
#endif

	if (chip->catch_up_soc > chip->msoc) {
		/* SOC increased */
		if (usb_present) /* Increment if USB is present */
			chip->msoc += chip->dt.delta_soc;
	} else if (chip->catch_up_soc < chip->msoc) {
		/* SOC dropped */
		chip->msoc -= chip->dt.delta_soc;
	}
	chip->msoc = CAP(0, 100, chip->msoc);

	if (chip->maint_soc > 0 && chip->msoc < chip->maint_soc) {
		chip->maint_soc -= chip->dt.delta_soc;
		chip->maint_soc = CAP(0, 100, chip->maint_soc);
	}

	/* maint_soc dropped below msoc, skip using it */
	if (chip->maint_soc <= chip->msoc)
		chip->maint_soc = -EINVAL;

	/* update the SOC register */
	rc = qg_write_monotonic_soc(chip, chip->msoc);
	if (rc < 0)
		pr_err("Failed to update MSOC register rc=%d\n", rc);

	/* update SDAM with the new MSOC */
	chip->sdam_data[SDAM_SOC] = chip->msoc;
	rc = qg_sdam_write(SDAM_SOC, chip->msoc);
	if (rc < 0)
		pr_err("Failed to update SDAM with MSOC rc=%d\n", rc);

	if (!chip->dt.cl_disable && chip->cl->active) {
		rc = qg_get_battery_temp(chip, &batt_temp);
		if (rc < 0) {
			pr_err("Failed to read BATT_TEMP rc=%d\n", rc);
		} else {
			batt_soc_32bit = div64_u64(
						chip->batt_soc * BATT_SOC_32BIT,
						QG_SOC_FULL);
			cap_learning_update(chip->cl, batt_temp, batt_soc_32bit,
					chip->charge_status, chip->charge_done,
					usb_present, false);
		}
	}

	cycle_count_update(chip->counter,
			DIV_ROUND_CLOSEST(chip->msoc * 255, 100),
			chip->charge_status, chip->charge_done,
			usb_present);

	qg_dbg(chip, QG_DEBUG_SOC,
		"SOC scale: Update maint_soc=%d msoc=%d catch_up_soc=%d delta_soc=%d\n",
				chip->maint_soc, chip->msoc,
				chip->catch_up_soc, chip->dt.delta_soc);
}

static void scale_soc_stop(struct qpnp_qg *chip)
{
	chip->next_wakeup_ms = 0;
	alarm_cancel(&chip->alarm_timer);

	qg_dbg(chip, QG_DEBUG_SOC,
			"SOC scale stopped: msoc=%d catch_up_soc=%d\n",
			chip->msoc, chip->catch_up_soc);
}

static void scale_soc_work(struct work_struct *work)
{
	struct qpnp_qg *chip = container_of(work,
			struct qpnp_qg, scale_soc_work);

	mutex_lock(&chip->soc_lock);

	if (!is_scaling_required(chip)) {
		scale_soc_stop(chip);
		goto done;
	}

	update_msoc(chip);

	if (is_scaling_required(chip)) {
		alarm_start_relative(&chip->alarm_timer,
				ms_to_ktime(chip->next_wakeup_ms));
	} else {
		scale_soc_stop(chip);
		goto done_psy;
	}

	qg_dbg(chip, QG_DEBUG_SOC,
		"SOC scale: Work msoc=%d catch_up_soc=%d delta_soc=%d next_wakeup=%d sec\n",
			chip->msoc, chip->catch_up_soc, chip->dt.delta_soc,
			chip->next_wakeup_ms / 1000);

done_psy:
	power_supply_changed(chip->qg_psy);
done:
	pm_relax(chip->dev);
	mutex_unlock(&chip->soc_lock);
}

static enum alarmtimer_restart
	qpnp_msoc_timer(struct alarm *alarm, ktime_t now)
{
	struct qpnp_qg *chip = container_of(alarm,
				struct qpnp_qg, alarm_timer);

	/* timer callback runs in atomic context, cannot use voter */
	pm_stay_awake(chip->dev);
	schedule_work(&chip->scale_soc_work);

	return ALARMTIMER_NORESTART;
}

int qg_scale_soc(struct qpnp_qg *chip, bool force_soc)
{
	int rc = 0;

	mutex_lock(&chip->soc_lock);

	qg_dbg(chip, QG_DEBUG_SOC,
			"SOC scale: Start msoc=%d catch_up_soc=%d delta_soc=%d\n",
			chip->msoc, chip->catch_up_soc, chip->dt.delta_soc);

	if (force_soc) {
		chip->msoc = chip->catch_up_soc;
		rc = qg_write_monotonic_soc(chip, chip->msoc);
		if (rc < 0)
			pr_err("Failed to update MSOC register rc=%d\n", rc);

		qg_dbg(chip, QG_DEBUG_SOC,
			"SOC scale: Forced msoc=%d\n", chip->msoc);
		goto done_psy;
	}

	if (!is_scaling_required(chip)) {
		scale_soc_stop(chip);
		goto done;
	}

	update_msoc(chip);

	if (is_scaling_required(chip)) {
		get_next_update_time(chip);
		alarm_start_relative(&chip->alarm_timer,
					ms_to_ktime(chip->next_wakeup_ms));
	} else {
		scale_soc_stop(chip);
		goto done_psy;
	}

	qg_dbg(chip, QG_DEBUG_SOC,
		"SOC scale: msoc=%d catch_up_soc=%d delta_soc=%d next_wakeup=%d sec\n",
			chip->msoc, chip->catch_up_soc, chip->dt.delta_soc,
			chip->next_wakeup_ms / 1000);

done_psy:
	power_supply_changed(chip->qg_psy);
done:
	mutex_unlock(&chip->soc_lock);
	return rc;
}

int qg_soc_init(struct qpnp_qg *chip)
{
	if (alarmtimer_get_rtcdev()) {
		alarm_init(&chip->alarm_timer, ALARM_BOOTTIME,
			qpnp_msoc_timer);
	} else {
		pr_err("Failed to get soc alarm-timer\n");
		return -EINVAL;
	}
	INIT_WORK(&chip->scale_soc_work, scale_soc_work);

	return 0;
}

void qg_soc_exit(struct qpnp_qg *chip)
{
	alarm_cancel(&chip->alarm_timer);
}
