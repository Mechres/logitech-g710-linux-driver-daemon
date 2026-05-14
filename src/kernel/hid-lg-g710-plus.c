/*
 *  Logitech G710+ Keyboard Input Driver
 *
 *  Driver generates additional key events for the keys M1-MR, G1-G6
 *  and supports setting the backlight levels of the keyboard
 *
 *  Copyright (c) 2013 Filip Wieladke <Wattos@gmail.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb.h>

#include "hid-ids.h"

#define USB_DEVICE_ID_LOGITECH_KEYBOARD_G710_PLUS 0xc24d

/* 20 seconds timeout for report roundtrips. */
#define WAIT_TIMEOUT_MS 20000

#define LOGITECH_KEY_MAP_SIZE 16

static const u8 g710_plus_key_map[LOGITECH_KEY_MAP_SIZE] = {
    0, /* unused */
    0, /* unused */
    0, /* unused */
    0, /* unused */
    KEY_F13, /* M1 */
    KEY_F14, /* M2 */
    KEY_F15, /* M3 */
    KEY_F16, /* MR */
    KEY_F17, /* G1 */
    KEY_F18, /* G2 */
    KEY_F19, /* G3 */
    KEY_F20, /* G4 */
    KEY_F21, /* G5 */
    KEY_F22, /* G6 */
    0, /* unused */
    0, /* unused */
};

/* Convenience macros */
#define lg_g710_plus_get_data(hdev) \
        ((struct lg_g710_plus_data *)(hid_get_drvdata(hdev)))

#define BIT_AT(var,pos) ((var) & (1<<(pos)))

struct lg_g710_plus_data {
    struct hid_report *g_mr_buttons_support_report; /* Needs to be written to enable G1-G6 and M1-MR keys */
    struct hid_report *mr_buttons_led_report; /* Controls the backlight of M1-MR buttons */
    struct hid_report *other_buttons_led_report; /* Controls the backlight of other buttons */
    struct hid_report *gamemode_report; /* Controls the backlight of other buttons */

    u16 macro_button_state; /* Holds the last state of the G1-G6, M1-MR buttons. Required to know which buttons were pressed and which were released */
    struct hid_device *hdev; 
    struct input_dev *input_dev;
    struct attribute_group attr_group;

    u8 led_macro; /* state of the M1-MR macro leds as returned by the keyboard ==> binary coded 0 -> 0xF*/
    u8 led_keys; /* state of the WASD key leds as returned by the keyboard  ==> 0 -> 4 */

    struct mutex lock; /* lock for communication with user space */
    struct completion ready; /* ready indicator */
};

static ssize_t lg_g710_plus_show_led_macro(struct device *device, struct device_attribute *attr, char *buf);
static ssize_t lg_g710_plus_store_led_macro(struct device *device, struct device_attribute *attr, const char *buf, size_t count);
static ssize_t lg_g710_plus_show_led_keys(struct device *device, struct device_attribute *attr, char *buf);
static ssize_t lg_g710_plus_store_led_keys(struct device *device, struct device_attribute *attr, const char *buf, size_t count);

static DEVICE_ATTR(led_macro, 0660, lg_g710_plus_show_led_macro, lg_g710_plus_store_led_macro);
static DEVICE_ATTR(led_keys,  0660, lg_g710_plus_show_led_keys,  lg_g710_plus_store_led_keys);

static struct attribute *lg_g710_plus_attrs[] = {
        &dev_attr_led_macro.attr,
        &dev_attr_led_keys.attr,
        NULL,
};

static int lg_g710_plus_extra_key_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    u8 i;
    u16 keys_pressed;
    struct lg_g710_plus_data* g710_data = lg_g710_plus_get_data(hdev);
    if (g710_data == NULL || size < 3 || data[0] != 3) {
        return 1; /* cannot handle the event */
    }

    keys_pressed= data[1] << 8 | data[2];
    for (i = 0; i < LOGITECH_KEY_MAP_SIZE; i++) {
        if (g710_plus_key_map[i] != 0 && (BIT_AT(keys_pressed, i) != BIT_AT(g710_data->macro_button_state, i))) {
            input_report_key(g710_data->input_dev, g710_plus_key_map[i], BIT_AT(keys_pressed, i) != 0);
        }
    }
    input_sync(g710_data->input_dev);
    g710_data->macro_button_state= keys_pressed;
    return 1;
}

static int lg_g710_plus_extra_led_mr_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    struct lg_g710_plus_data* g710_data = lg_g710_plus_get_data(hdev);
    if (g710_data == NULL || size < 2)
        return 1;
    g710_data->led_macro= (data[1] >> 4) & 0xF;
    complete_all(&g710_data->ready);
    return 1;
}

static int lg_g710_plus_extra_led_keys_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    struct lg_g710_plus_data* g710_data = lg_g710_plus_get_data(hdev);
    if (g710_data == NULL || size < 3)
        return 1;
    g710_data->led_keys= data[1] << 4 | data[2];
    complete_all(&g710_data->ready);
    return 1;
}

static int lg_g710_plus_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
    switch(report->id) {
        case 3: return lg_g710_plus_extra_key_event(hdev, report, data, size);
        case 6: return lg_g710_plus_extra_led_mr_event(hdev, report, data, size);
        case 8: return lg_g710_plus_extra_led_keys_event(hdev, report, data, size);
        default: return 0;
    }
}

static int lg_g710_plus_input_mapping(struct hid_device *hdev, struct hid_input *hi, struct hid_field *field, struct hid_usage *usage, unsigned long **bit, int *max) 
{
    int i;
    struct lg_g710_plus_data* data = lg_g710_plus_get_data(hdev);
    if (data != NULL) {
        // Only bind input_dev to the interface that has the extra keys (Report ID 3)
        if (field->report->id == 3) {
            data->input_dev = hi->input;
        }
        
        /* Ensure the system knows we have these keys on THIS specific interface */
        if (field->report->id == 3) {
            for (i = 0; i < LOGITECH_KEY_MAP_SIZE; i++) {
                if (g710_plus_key_map[i] != 0) {
                    set_bit(EV_KEY, hi->input->evbit);
                    set_bit(g710_plus_key_map[i], hi->input->keybit);
                }
            }
        }
    }
    return 0;
}

enum req_type {
    REQTYPE_READ,
    REQTYPE_WRITE
};

static void hidhw_request(struct hid_device *hdev, struct hid_report *report, enum req_type reqtype) {
    hid_hw_request(hdev, report, reqtype == REQTYPE_READ ? HID_REQ_GET_REPORT : HID_REQ_SET_REPORT);
}

static int lg_g710_plus_initialize(struct hid_device *hdev) {
    int ret = 0;
    struct lg_g710_plus_data *data;
    struct list_head *feature_report_list = &hdev->report_enum[HID_FEATURE_REPORT].report_list;
    struct hid_report *report;

    if (list_empty(feature_report_list)) {
        return 0; /* Currently, the keyboard registers as two different devices */
    }

    data = lg_g710_plus_get_data(hdev);
    list_for_each_entry(report, feature_report_list, list) {
        switch(report->id) {
            case 6: data->mr_buttons_led_report= report; break;
            case 8: data->other_buttons_led_report= report; break;
            case 9:
                data->g_mr_buttons_support_report= report;
                hidhw_request(hdev, report, REQTYPE_WRITE);
                break;
        }
    }

    ret= sysfs_create_group(&hdev->dev.kobj, &data->attr_group);
    return ret;
}

static struct lg_g710_plus_data* lg_g710_plus_create(struct hid_device *hdev)
{
    struct lg_g710_plus_data* data;
    data= kzalloc(sizeof(struct lg_g710_plus_data), GFP_KERNEL);
    if (data == NULL) {
        return NULL;
    }

    data->attr_group.name= "logitech-g710";
    data->attr_group.attrs= lg_g710_plus_attrs;
    data->hdev= hdev;

    mutex_init(&data->lock);
    init_completion(&data->ready);
    return data;
}

static int lg_g710_plus_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;
    struct lg_g710_plus_data *data;

    data = lg_g710_plus_create(hdev);
    if (data == NULL) {
        dev_err(&hdev->dev, "can't allocate space for Logitech G710+ device attributes\n");
        ret= -ENOMEM;
        goto err_free;
    }
    hid_set_drvdata(hdev, data);

    /*
     * Without this, the device would send a first report with a key down event for
     * certain buttons, but never the key up event
     */
    hdev->quirks |= HID_QUIRK_NOGET;

    ret = hid_parse(hdev);
    if (ret) {
        hid_err(hdev, "parse failed\n");
        goto err_free;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        hid_err(hdev, "hw start failed\n");
        goto err_free;
    }

    ret= lg_g710_plus_initialize(hdev);
    if (ret) {
        hid_hw_stop(hdev);
        goto err_free;
    }

    return 0;

err_free:
    if (data != NULL) {
        kfree(data);
    }
    return ret;
}

static void lg_g710_plus_remove(struct hid_device *hdev)
{
    struct lg_g710_plus_data* data = lg_g710_plus_get_data(hdev);
    struct list_head *feature_report_list = &hdev->report_enum[HID_FEATURE_REPORT].report_list;

    if (data != NULL && !list_empty(feature_report_list))
        sysfs_remove_group(&hdev->dev.kobj, &data->attr_group);

    hid_hw_stop(hdev);
    if (data != NULL) {
        kfree(data);
    }
}

static struct lg_g710_plus_data *lg_g710_plus_data_from_dev(struct device *device)
{
    return hid_get_drvdata(to_hid_device(device));
}

static ssize_t lg_g710_plus_show_led_macro(struct device *device, struct device_attribute *attr, char *buf)
{
    struct lg_g710_plus_data *data = lg_g710_plus_data_from_dev(device);

    if (data == NULL || data->mr_buttons_led_report == NULL)
        return -ENODEV;

    mutex_lock(&data->lock);
    init_completion(&data->ready);
    hidhw_request(data->hdev, data->mr_buttons_led_report, REQTYPE_READ);
    if (!wait_for_completion_timeout(&data->ready, msecs_to_jiffies(WAIT_TIMEOUT_MS))) {
        mutex_unlock(&data->lock);
        return -ETIMEDOUT;
    }
    mutex_unlock(&data->lock);

    return sysfs_emit(buf, "%d\n", data->led_macro);
}

static ssize_t lg_g710_plus_show_led_keys(struct device *device, struct device_attribute *attr, char *buf)
{
    struct lg_g710_plus_data *data = lg_g710_plus_data_from_dev(device);

    if (data == NULL || data->other_buttons_led_report == NULL)
        return -ENODEV;

    mutex_lock(&data->lock);
    init_completion(&data->ready);
    hidhw_request(data->hdev, data->other_buttons_led_report, REQTYPE_READ);
    if (!wait_for_completion_timeout(&data->ready, msecs_to_jiffies(WAIT_TIMEOUT_MS))) {
        mutex_unlock(&data->lock);
        return -ETIMEDOUT;
    }
    mutex_unlock(&data->lock);

    return sysfs_emit(buf, "%d\n", data->led_keys);
}

static ssize_t lg_g710_plus_store_led_macro(struct device *device, struct device_attribute *attr, const char *buf, size_t count)
{
    unsigned long key_mask;
    int retval;
    struct lg_g710_plus_data *data = lg_g710_plus_data_from_dev(device);

    if (data == NULL || data->mr_buttons_led_report == NULL)
        return -ENODEV;

    retval = kstrtoul(buf, 10, &key_mask);
    if (retval)
        return retval;

    mutex_lock(&data->lock);
    data->mr_buttons_led_report->field[0]->value[0]= (key_mask & 0xF) << 4;
    hidhw_request(data->hdev, data->mr_buttons_led_report, REQTYPE_WRITE);
    mutex_unlock(&data->lock);
    return count;
}

static ssize_t lg_g710_plus_store_led_keys(struct device *device, struct device_attribute *attr, const char *buf, size_t count)
{
    int retval;
    unsigned long key_mask;
    u8 wasd_mask, keys_mask;
    struct lg_g710_plus_data *data = lg_g710_plus_data_from_dev(device);

    if (data == NULL || data->other_buttons_led_report == NULL)
        return -ENODEV;

    retval = kstrtoul(buf, 10, &key_mask);
    if (retval)
        return retval;

    wasd_mask= (key_mask >> 4) & 0xF;
    keys_mask= (key_mask) & 0xF;

    wasd_mask= wasd_mask > 4 ? 4 : wasd_mask;
    keys_mask= keys_mask > 4 ? 4 : keys_mask;

    mutex_lock(&data->lock);
    data->other_buttons_led_report->field[0]->value[0]= wasd_mask;
    data->other_buttons_led_report->field[0]->value[1]= keys_mask;
    hidhw_request(data->hdev, data->other_buttons_led_report, REQTYPE_WRITE);
    mutex_unlock(&data->lock);
    return count;
}

static const struct hid_device_id lg_g710_plus_devices[] = {
    { HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_KEYBOARD_G710_PLUS) },
    { }
};

MODULE_DEVICE_TABLE(hid, lg_g710_plus_devices);
static struct hid_driver lg_g710_plus_driver = {
    .name = "hid-lg-g710-plus",
    .id_table = lg_g710_plus_devices,
    .raw_event = lg_g710_plus_raw_event,
    .input_mapping = lg_g710_plus_input_mapping,
    .probe= lg_g710_plus_probe,
    .remove= lg_g710_plus_remove,
};

static int __init lg_g710_plus_init(void)
{
    return hid_register_driver(&lg_g710_plus_driver);
}

static void __exit lg_g710_plus_exit(void)
{
    hid_unregister_driver(&lg_g710_plus_driver);
}

module_init(lg_g710_plus_init);
module_exit(lg_g710_plus_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Filip Wieladek <Wattos@gmail.com>");
MODULE_DESCRIPTION("Logitech G710+ driver");
