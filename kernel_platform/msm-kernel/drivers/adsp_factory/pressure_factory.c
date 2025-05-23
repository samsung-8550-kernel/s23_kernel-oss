/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include "adsp.h"

#define CALIBRATION_FILE_PATH "/efs/FactoryApp/baro_delta"

#define	PR_MAX 8388607 /* 24 bit 2'compl */
#define	PR_MIN -8388608
#define SNS_SUCCESS 0
#define ST_PASS 1
#define ST_FAIL 0

#define PRESS_DEVICE_LIST_MAX    4

enum {
	OPTION_TYPE_PRESS_GET_DEVICE_ID,
	OPTION_TYPE_PRESS_MAX
};

static int sea_level_pressure;
static int pressure_cal;

static const struct device_id_t press_device_list[PRESS_DEVICE_LIST_MAX] = {
	/* ID, Vendor,      Name */
	{0x00, "Unknown",   "Unknown"},
	{0xB1, "STM",       "LPS22HB"},
	{0xB3, "STM",       "LPS22HH"},
	{0xB4, "STM",       "LPS22DF"}
};

static void press_get_device_id(struct adsp_data *data)
{
	int32_t cmd = OPTION_TYPE_PRESS_GET_DEVICE_ID, i;
	int32_t device_index = UNKNOWN_INDEX;
	uint8_t cnt = 0, device_id = 0;

	adsp_unicast(&cmd, sizeof(cmd), MSG_PRESSURE, 0, MSG_TYPE_OPTION_DEFINE);

	while (!(data->ready_flag[MSG_TYPE_OPTION_DEFINE]
		& 1 << MSG_PRESSURE) && cnt++ < TIMEOUT_CNT)
		usleep_range(1000, 1100);

	data->ready_flag[MSG_TYPE_OPTION_DEFINE] &= ~(1 << MSG_PRESSURE);

	if (cnt >= TIMEOUT_CNT)
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
	else
		device_id = (uint8_t)data->msg_buf[MSG_PRESSURE][2];
	pr_err("[FACTORY] %s: device_id : %d,%d,%d,%d\n", __func__, device_id,
		(int)data->msg_buf[MSG_PRESSURE][0],
		(int)data->msg_buf[MSG_PRESSURE][1],
		(int)data->msg_buf[MSG_PRESSURE][2]);

	if (device_id == 0) {
		pr_err("[FACTORY] %s: No information\n", __func__);
	} else {
		for (i = 0; i < PRESS_DEVICE_LIST_MAX; i++)
			if (device_id == press_device_list[i].device_id)
				break;
		if (i >= PRESS_DEVICE_LIST_MAX)
			pr_err("[FACTORY] %s: Unknown ID - (0x%x)\n",
				__func__, device_id);
		else
			device_index = i;
	}

	memcpy(data->press_device_vendor,
		press_device_list[device_index].device_vendor,
		sizeof(char) * DEVICE_INFO_LENGTH);
	memcpy(data->press_device_name,
		press_device_list[device_index].device_name,
		sizeof(char) * DEVICE_INFO_LENGTH);

	pr_info("[FACTORY] %s: Device ID - %s(%s)\n", __func__,
		data->press_device_name, data->press_device_vendor);
}

static ssize_t pressure_vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	if (!strcmp(data->press_device_vendor, press_device_list[0].device_vendor))
		press_get_device_id(data);

	return snprintf(buf, PAGE_SIZE, "%s\n", data->press_device_vendor);
}

static ssize_t pressure_name_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	if (!strcmp(data->press_device_name, press_device_list[0].device_name))
		press_get_device_id(data);

	return snprintf(buf, PAGE_SIZE, "%s\n", data->press_device_name);
}

static ssize_t sea_level_pressure_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", sea_level_pressure);
}

static ssize_t sea_level_pressure_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	if (sscanf(buf, "%10d", &sea_level_pressure) != 1) {
		pr_err("[FACTORY] %s: sscanf error\n", __func__);
		return size;
	}

	sea_level_pressure = sea_level_pressure / 100;

	pr_info("[FACTORY] %s: sea_level_pressure = %d\n", __func__,
		sea_level_pressure);

	return size;
}

/*
int pressure_open_calibration(struct adsp_data *data)
{
	int error = 0;

	return error;
}
*/

static ssize_t pressure_cabratioin_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);

	schedule_delayed_work(&data->pressure_cal_work, 0);
	return size;
}

static ssize_t pressure_cabratioin_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	//struct adsp_data *data = dev_get_drvdata(dev);

	//pressure_open_calibration(data);

	return snprintf(buf, PAGE_SIZE, "%d\n", pressure_cal);
}

static ssize_t temperature_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;

	adsp_unicast(NULL, 0, MSG_PRESSURE_TEMP, 0, MSG_TYPE_GET_RAW_DATA);

	while (!(data->ready_flag[MSG_TYPE_GET_RAW_DATA] &
		1 << MSG_PRESSURE_TEMP) && cnt++ < TIMEOUT_CNT)
		usleep_range(500, 550);

	data->ready_flag[MSG_TYPE_GET_RAW_DATA] &= ~(1 << MSG_PRESSURE_TEMP);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "-99\n");
	}
	return snprintf(buf, PAGE_SIZE, "%d\n",
		data->msg_buf[MSG_PRESSURE_TEMP][0]);
}

static ssize_t selftest_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;

	adsp_unicast(NULL, 0, MSG_PRESSURE, 0, MSG_TYPE_ST_SHOW_DATA);

	while (!(data->ready_flag[MSG_TYPE_ST_SHOW_DATA] &
		1 << MSG_PRESSURE) && cnt++ < TIMEOUT_CNT)
		msleep(26);

	data->ready_flag[MSG_TYPE_ST_SHOW_DATA] &= ~(1 << MSG_PRESSURE);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "0\n");
	}

	pr_info("[FACTORY] %s : P:%d, T:%d, RES:%d, retried:5-%d\n",
		__func__,
		data->msg_buf[MSG_PRESSURE][0],
		data->msg_buf[MSG_PRESSURE][1],
		data->msg_buf[MSG_PRESSURE][2],
		data->msg_buf[MSG_PRESSURE][3]);

	if (SNS_SUCCESS == data->msg_buf[MSG_PRESSURE][2])
		return snprintf(buf, PAGE_SIZE, "%d\n", ST_PASS);
	else
		return snprintf(buf, PAGE_SIZE, "%d\n", ST_FAIL);
}

static ssize_t pressure_dhr_sensor_info_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;
	int i = 0;

	adsp_unicast(NULL, 0, MSG_PRESSURE, 0, MSG_TYPE_GET_DHR_INFO);
	while (!(data->ready_flag[MSG_TYPE_GET_DHR_INFO] & 1 << MSG_PRESSURE) &&
		cnt++ < TIMEOUT_CNT)
		usleep_range(500, 550);

	data->ready_flag[MSG_TYPE_GET_DHR_INFO] &= ~(1 << MSG_PRESSURE);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
	} else {
		for (i = 0; i < 8; i++) {
			pr_info("[FACTORY] %s - %02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
				__func__,
				data->msg_buf[MSG_PRESSURE][i * 16 + 0],
				data->msg_buf[MSG_PRESSURE][i * 16 + 1],
				data->msg_buf[MSG_PRESSURE][i * 16 + 2],
				data->msg_buf[MSG_PRESSURE][i * 16 + 3],
				data->msg_buf[MSG_PRESSURE][i * 16 + 4],
				data->msg_buf[MSG_PRESSURE][i * 16 + 5],
				data->msg_buf[MSG_PRESSURE][i * 16 + 6],
				data->msg_buf[MSG_PRESSURE][i * 16 + 7],
				data->msg_buf[MSG_PRESSURE][i * 16 + 8],
				data->msg_buf[MSG_PRESSURE][i * 16 + 9],
				data->msg_buf[MSG_PRESSURE][i * 16 + 10],
				data->msg_buf[MSG_PRESSURE][i * 16 + 11],
				data->msg_buf[MSG_PRESSURE][i * 16 + 12],
				data->msg_buf[MSG_PRESSURE][i * 16 + 13],
				data->msg_buf[MSG_PRESSURE][i * 16 + 14],
				data->msg_buf[MSG_PRESSURE][i * 16 + 15]);
		}
	}

	return snprintf(buf, PAGE_SIZE, "%s\n", "Done");
}

static ssize_t pressure_sw_offset_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;
	int input = 0;
	int sw_offset = 0;
	int ret;

	ret = kstrtoint(buf, 10, &input);
	if (ret < 0) {
		pr_err("[FACTORY] %s: kstrtoint fail\n", __func__);
		return -EINVAL;
	}

	pr_info("[FACTORY] %s: write value = %d\n", __func__, input);

	adsp_unicast(&input, sizeof(int),
		MSG_PRESSURE, 0, MSG_TYPE_SET_THRESHOLD);

	while (!(data->ready_flag[MSG_TYPE_SET_THRESHOLD] & 1 << MSG_PRESSURE) &&
		cnt++ < TIMEOUT_CNT)
		usleep_range(500, 550);

	data->ready_flag[MSG_TYPE_SET_THRESHOLD] &= ~(1 << MSG_PRESSURE);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
	} else {
		sw_offset = data->msg_buf[MSG_PRESSURE][0];
	}

	pr_info("[FACTORY] %s: sw_offset %d\n", __func__, sw_offset);

	return size;
}

static ssize_t pressure_sw_offset_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;

	adsp_unicast(NULL, 0, MSG_PRESSURE, 0, MSG_TYPE_GET_THRESHOLD);

	while (!(data->ready_flag[MSG_TYPE_GET_THRESHOLD] &
		1 << MSG_PRESSURE) && cnt++ < TIMEOUT_CNT)
		msleep(20);

	data->ready_flag[MSG_TYPE_GET_THRESHOLD] &= ~(1 << MSG_PRESSURE);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "0\n");
	}

	pr_info("[FACTORY] %s : sw_offset %d\n",
		__func__, data->msg_buf[MSG_PRESSURE][0]);

	return snprintf(buf, PAGE_SIZE, "%d\n", data->msg_buf[MSG_PRESSURE][0]);
}

static ssize_t pressure_esn_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct adsp_data *data = dev_get_drvdata(dev);
	uint8_t cnt = 0;

	adsp_unicast(NULL, 0, MSG_PRESSURE, 0, MSG_TYPE_GET_REGISTER);

	while (!(data->ready_flag[MSG_TYPE_GET_REGISTER] &
		1 << MSG_PRESSURE) && cnt++ < TIMEOUT_CNT)
		msleep(20);

	data->ready_flag[MSG_TYPE_GET_REGISTER] &= ~(1 << MSG_PRESSURE);

	if (cnt >= TIMEOUT_CNT) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return snprintf(buf, PAGE_SIZE, "0\n");
	}

	pr_info("[FACTORY] %s : esn %02X%02X%02X%02X%02X%02X%02X%02X\n",
		__func__, data->msg_buf[MSG_PRESSURE][0], data->msg_buf[MSG_PRESSURE][1],
		data->msg_buf[MSG_PRESSURE][2], data->msg_buf[MSG_PRESSURE][3],
		data->msg_buf[MSG_PRESSURE][4], data->msg_buf[MSG_PRESSURE][5],
		data->msg_buf[MSG_PRESSURE][6], data->msg_buf[MSG_PRESSURE][7]);

	return snprintf(buf, PAGE_SIZE, "%02X%02X%02X%02X%02X%02X%02X%02X\n",
		data->msg_buf[MSG_PRESSURE][0], data->msg_buf[MSG_PRESSURE][1],
		data->msg_buf[MSG_PRESSURE][2], data->msg_buf[MSG_PRESSURE][3],
		data->msg_buf[MSG_PRESSURE][4], data->msg_buf[MSG_PRESSURE][5],
		data->msg_buf[MSG_PRESSURE][6], data->msg_buf[MSG_PRESSURE][7]);
}

static DEVICE_ATTR(vendor, 0444, pressure_vendor_show, NULL);
static DEVICE_ATTR(name, 0444, pressure_name_show, NULL);
static DEVICE_ATTR(calibration, 0664,
	pressure_cabratioin_show, pressure_cabratioin_store);
static DEVICE_ATTR(sea_level_pressure, 0664,
	sea_level_pressure_show, sea_level_pressure_store);
static DEVICE_ATTR(temperature, 0444, temperature_show, NULL);
static DEVICE_ATTR(selftest, 0444, selftest_show, NULL);
#ifdef CONFIG_SEC_FACTORY
static DEVICE_ATTR(dhr_sensor_info, 0444,
	pressure_dhr_sensor_info_show, NULL);
#else
static DEVICE_ATTR(dhr_sensor_info, 0440,
	pressure_dhr_sensor_info_show, NULL);
#endif
static DEVICE_ATTR(sw_offset, 0664,
	pressure_sw_offset_show, pressure_sw_offset_store);
static DEVICE_ATTR(esn, 0440, pressure_esn_show, NULL);

static struct device_attribute *pressure_attrs[] = {
	&dev_attr_vendor,
	&dev_attr_name,
	&dev_attr_calibration,
	&dev_attr_sea_level_pressure,
	&dev_attr_temperature,
	&dev_attr_selftest,
	&dev_attr_dhr_sensor_info,
	&dev_attr_sw_offset,
	&dev_attr_esn,
	NULL,
};

void pressure_cal_work_func(struct work_struct *work)
{
	struct adsp_data *data = container_of((struct delayed_work *)work,
		struct adsp_data, pressure_cal_work);
	int cnt = 0;
	int temp = 0;

	adsp_unicast(&temp, sizeof(temp), MSG_PRESSURE, 0, MSG_TYPE_SET_CAL_DATA);

	while (!(data->ready_flag[MSG_TYPE_SET_CAL_DATA] & 1 << MSG_PRESSURE) &&
		cnt++ < 3)
		msleep(30);

	data->ready_flag[MSG_TYPE_SET_CAL_DATA] &= ~(1 << MSG_PRESSURE);

	if (cnt >= 3) {
		pr_err("[FACTORY] %s: Timeout!!!\n", __func__);
		return;
	}

	pressure_cal = data->msg_buf[MSG_PRESSURE][0];
	if (!strcmp(data->press_device_vendor, press_device_list[0].device_vendor) ||
		!strcmp(data->press_device_name, press_device_list[0].device_name))
		press_get_device_id(data);

	pr_info("[FACTORY] %s: pressure_cal = %d (lsb)\n", __func__, data->msg_buf[MSG_PRESSURE][0]);
}
EXPORT_SYMBOL(pressure_cal_work_func);
void pressure_factory_init_work(struct adsp_data *data)
{
	memcpy(data->press_device_vendor,
		press_device_list[0].device_vendor,
		sizeof(char) * DEVICE_INFO_LENGTH);
	memcpy(data->press_device_name,
		press_device_list[0].device_name,
		sizeof(char) * DEVICE_INFO_LENGTH);

	schedule_delayed_work(&data->pressure_cal_work, msecs_to_jiffies(8000));
}
EXPORT_SYMBOL(pressure_factory_init_work);

int __init pressure_factory_init(void)
{
	adsp_factory_register(MSG_PRESSURE, pressure_attrs);

	pr_info("[FACTORY] %s\n", __func__);

	return 0;
}
void __exit pressure_factory_exit(void)
{
	adsp_factory_unregister(MSG_PRESSURE);

	pr_info("[FACTORY] %s\n", __func__);
}
