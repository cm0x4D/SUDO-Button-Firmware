#include "zephyr/usb/class/hid.h"
#include <sample_usbd.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

enum kb_report_idx {
    KB_MOD_KEY = 0,
    KB_RESERVED,
    KB_KEY_CODE1,
    KB_KEY_CODE2,
    KB_KEY_CODE3,
    KB_KEY_CODE4,
    KB_KEY_CODE5,
    KB_KEY_CODE6,
    KB_REPORT_COUNT,
};

struct kb_event {
    uint16_t code;
    int32_t value;
};

K_MSGQ_DEFINE(kb_msgq, sizeof(struct kb_event), 2, 1);

UDC_STATIC_BUF_DEFINE(report, KB_REPORT_COUNT);
static uint32_t kb_duration;
static bool kb_ready;

static void input_cb(struct input_event* evt, void* user_data) {
    ARG_UNUSED(user_data);

    struct kb_event kb_evt;
    kb_evt.code = evt->code;
    kb_evt.value = evt->value;

    if (k_msgq_put(&kb_msgq, &kb_evt, K_NO_WAIT) != 0) {
        LOG_ERR("Failed to put new input event");
    }
}

INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

static void kb_iface_ready(const struct device* dev, const bool ready) {
    ARG_UNUSED(dev);

    LOG_INF("HID device %s interface is %s",
      dev->name, ready ? "ready" : "not ready");

    kb_ready = ready;
}

static int kb_get_report(const struct device* dev, const uint8_t type, const uint8_t id, const uint16_t len, uint8_t* const buf) {
    ARG_UNUSED(dev);
    ARG_UNUSED(type);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);

    LOG_WRN("Get Report not implemented, Type %u ID %u", type, id);

    return 0;
}

static int kb_set_report(const struct device* dev, const uint8_t type, const uint8_t id, const uint16_t len, const uint8_t* const buf) {
    ARG_UNUSED(dev);
    ARG_UNUSED(id);
    ARG_UNUSED(len);
    ARG_UNUSED(buf);

    if (type != HID_REPORT_TYPE_OUTPUT) {
        LOG_WRN("Unsupported report type");
        return -ENOTSUP;
    }

    return 0;
}

static void kb_set_idle(const struct device* dev, const uint8_t id, const uint32_t duration) {
    ARG_UNUSED(dev);
    ARG_UNUSED(id);

    LOG_INF("Set Idle %u to %u", id, duration);

    kb_duration = duration;
}

static uint32_t kb_get_idle(const struct device* dev, const uint8_t id) {
    ARG_UNUSED(dev);
    ARG_UNUSED(id);

    LOG_INF("Get Idle %u to %u", id, kb_duration);

    return kb_duration;
}

static void kb_set_protocol(const struct device* dev, const uint8_t proto) {
    ARG_UNUSED(dev);

    LOG_INF("Protocol changed to %s", proto == 0U ? "Boot Protocol" : "Report Protocol");
}

static void kb_output_report(const struct device* dev, const uint16_t len, const uint8_t* const buf) {
    kb_set_report(dev, HID_REPORT_TYPE_OUTPUT, 0U, len, buf);
}

struct hid_device_ops kb_ops = {
  .iface_ready = kb_iface_ready,
  .get_report = kb_get_report,
  .set_report = kb_set_report,
  .set_idle = kb_set_idle,
  .get_idle = kb_get_idle,
  .set_protocol = kb_set_protocol,
  .output_report = kb_output_report,
};

static void msg_cb(struct usbd_context* const usbd_ctx, const struct usbd_msg* const msg) {
    LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));

    if (msg->type == USBD_MSG_CONFIGURATION) {
        LOG_INF("\tConfiguration value %d", msg->status);
    }

    if (usbd_can_detect_vbus(usbd_ctx)) {
        if (msg->type == USBD_MSG_VBUS_READY) {
            if (usbd_enable(usbd_ctx)) {
                LOG_ERR("Failed to enable device support");
            }
        }

        if (msg->type == USBD_MSG_VBUS_REMOVED) {
            if (usbd_disable(usbd_ctx)) {
                LOG_ERR("Failed to disable device support");
            }
        }
    }
}

void send_key(const struct device* dev, uint8_t key, uint8_t modifiers) {
    report[KB_MOD_KEY] = modifiers;
    report[KB_KEY_CODE1] = key;
    int ret = hid_device_submit_report(dev, KB_REPORT_COUNT, report);
    if (ret) {
        LOG_ERR("HID submit report error, %d", ret);
    }
}

int main(void) {
    struct usbd_context* sample_usbd;
    const struct device* hid_dev;
    int ret;

    hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
    if (!device_is_ready(hid_dev)) {
        LOG_ERR("HID Device is not ready");
        return -EIO;
    }

    ret = hid_device_register(hid_dev, hid_report_desc, sizeof(hid_report_desc),&kb_ops);
    if (ret != 0) {
        LOG_ERR("Failed to register HID Device, %d", ret);
        return ret;
    }

    sample_usbd = sample_usbd_init_device(msg_cb);
    if (sample_usbd == NULL) {
        LOG_ERR("Failed to initialize USB device");
        return -ENODEV;
    }

    if (!usbd_can_detect_vbus(sample_usbd)) {
        ret = usbd_enable(sample_usbd);
        if (ret) {
            LOG_ERR("Failed to enable device support");
            return ret;
        }
    }

    LOG_INF("SUDO Button is initialized");

    int64_t pressed_at = 0;

    while (true) {
        struct kb_event kb_evt;

        k_msgq_get(&kb_msgq, &kb_evt, K_FOREVER);

        LOG_WRN("GOT KEY %i", kb_evt.code);

        if (!kb_ready) {
            LOG_ERR("USB HID device is not ready");
            continue;
        }

        if (kb_evt.value) {
            pressed_at = k_uptime_get();
        } else {
            int64_t delta = k_uptime_get() - pressed_at;
            if (delta < 500) {
                send_key(hid_dev, HID_KEY_S, 0);
                send_key(hid_dev, HID_KEY_U, 0);
                send_key(hid_dev, HID_KEY_D, 0);
                send_key(hid_dev, HID_KEY_O, 0);
                send_key(hid_dev, HID_KEY_SPACE, 0);
                send_key(hid_dev, HID_KEY_RIGHTBRACE, HID_KBD_MODIFIER_LEFT_SHIFT);
                send_key(hid_dev, 0, 0);
                send_key(hid_dev, HID_KEY_RIGHTBRACE, HID_KBD_MODIFIER_LEFT_SHIFT);
                send_key(hid_dev, HID_KEY_ENTER, 0);
                send_key(hid_dev, 0, 0);
            } else {
                send_key(hid_dev, HID_KEY_S, 0);
                send_key(hid_dev, HID_KEY_U, 0);
                send_key(hid_dev, HID_KEY_D, 0);
                send_key(hid_dev, HID_KEY_O, 0);
                send_key(hid_dev, HID_KEY_SPACE, 0);
                send_key(hid_dev, HID_KEY_SLASH, 0);
                send_key(hid_dev, HID_KEY_S, 0);
                send_key(hid_dev, HID_KEY_ENTER, 0);
                send_key(hid_dev, 0, 0);
            }
        }
    }

    return 0;
}
