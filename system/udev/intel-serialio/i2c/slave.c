// Copyright 2016 The Fuchsia Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/char.h>
#include <ddk/protocol/i2c.h>
#include <intel_broadwell_serialio/reg.h>
#include <magenta/types.h>
#include <mxio/util.h>
#include <mxu/list.h>
#include <runtime/mutex.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "controller.h"
#include "slave.h"

// Implement the device protocol for the slave devices.

static mx_status_t intel_broadwell_serialio_i2c_slave_open(mx_device_t* dev,
                                                           uint32_t flags) {
    return NO_ERROR;
}

static mx_status_t intel_broadwell_serialio_i2c_slave_close(mx_device_t* dev) {
    return NO_ERROR;
}

static mx_status_t intel_broadwell_serialio_i2c_slave_release(mx_device_t* dev) {
    return NO_ERROR;
}

static mx_protocol_device_t intel_broadwell_serialio_i2c_slave_device_proto = {
    .get_protocol = &device_base_get_protocol,
    .open = &intel_broadwell_serialio_i2c_slave_open,
    .close = &intel_broadwell_serialio_i2c_slave_close,
    .release = &intel_broadwell_serialio_i2c_slave_release,
};

// Implement the functionality of the i2c slave devices.

static mx_status_t intel_broadwell_serialio_i2c_slave_transfer(
    mx_device_t* dev, i2c_slave_segment_t* segments, int segment_count) {
    intel_broadwell_serialio_i2c_slave_device_t* slave =
        get_intel_broadwell_serialio_i2c_slave_device(dev);

    if (!dev->parent) {
        printf("Orphaned I2C slave.\n");
        return ERR_BAD_STATE;
    }

    intel_broadwell_serialio_i2c_device_t* controller =
        get_intel_broadwell_serialio_i2c_device(dev->parent);

    uint32_t ctl_addr_mode_bit;
    uint32_t tar_add_addr_mode_bit;
    if (slave->chip_address_width == I2C_7BIT_ADDRESS) {
        ctl_addr_mode_bit = CTL_ADDRESSING_MODE_7BIT;
        tar_add_addr_mode_bit = TAR_ADD_WIDTH_7BIT;
    } else if (slave->chip_address_width == I2C_10BIT_ADDRESS) {
        ctl_addr_mode_bit = CTL_ADDRESSING_MODE_10BIT;
        tar_add_addr_mode_bit = TAR_ADD_WIDTH_10BIT;
    } else {
        printf("Bad address width.\n");
        return ERR_INVALID_ARGS;
    }

    mxr_mutex_lock(&controller->mutex);

    // Wait for the bus to become idle.
    uint32_t i2c_sta;
    do {
        i2c_sta = *REG32(&controller->regs->i2c_sta);
    } while ((i2c_sta & (0x1 << I2C_STA_CA)) ||
             !(i2c_sta & (0x1 << I2C_STA_TFCE)));

    // Set the target adress value and width.
    RMWREG32(&controller->regs->ctl, CTL_ADDRESSING_MODE, 1, ctl_addr_mode_bit);
    *REG32(&controller->regs->tar_add) =
        (tar_add_addr_mode_bit << TAR_ADD_WIDTH) |
        (slave->chip_address << TAR_ADD_IC_TAR);

    // Enable the controller.
    RMWREG32(&controller->regs->i2c_en, I2C_EN_ENABLE, 1, 1);

    int last_read = 0;
    if (segment_count)
        last_read = segments->read;

    while (segment_count--) {
        int len = segments->len;
        uint8_t* buf = segments->buf;

        // If this segment is in the same direction as the last, inject a
        // restart at its start.
        uint32_t restart = 0;
        if (last_read == segments->read)
            restart = 1;
        while (len--) {
            // Build the cmd register value.
            uint32_t cmd = (restart << DATA_CMD_RESTART);
            restart = 0;
            if (!segments->read) {
                while (!(*REG32(&controller->regs->i2c_sta) &
                         (0x1 << I2C_STA_TFNF))) {
                    ;
                }
                cmd |= (*buf << DATA_CMD_DAT);
                cmd |= (DATA_CMD_CMD_WRITE << DATA_CMD_CMD);
            } else {
                cmd |= (DATA_CMD_CMD_READ << DATA_CMD_CMD);
            }
            if (!len)
                cmd |= (0x1 << DATA_CMD_STOP);

            // Write the cmd value.
            *REG32(&controller->regs->data_cmd) = cmd;

            // If this is a read, extract the data.
            if (segments->read) {
                while (!(*REG32(&controller->regs->i2c_sta) &
                         (0x1 << I2C_STA_RFNE))) {
                    ;
                }
                *buf = *REG32(&controller->regs->data_cmd);
            }

            buf++;
        }
        last_read = segments->read;
        segments++;
    }

    while (*REG32(&controller->regs->raw_intr_stat) & INTR_STOP_DETECTION) {
        // Read the data_cmd register to pull data out of the RX FIFO.
        *REG32(&controller->regs->clr_stop_det);
    }

    // Wait for the bus to become idle.
    do {
        i2c_sta = *REG32(&controller->regs->i2c_sta);
    } while ((i2c_sta & (0x1 << I2C_STA_CA)) ||
             !(i2c_sta & (0x1 << I2C_STA_TFCE)));
    while ((*REG32(&controller->regs->i2c_sta) & (0x1 << I2C_STA_CA)) ||
           !(*REG32(&controller->regs->i2c_sta) & (0x1 << I2C_STA_TFCE))) {
        ;
    }

    while (*REG32(&controller->regs->i2c_sta) & (0x1 << I2C_STA_RFNE))
        *REG32(&controller->regs->data_cmd);

    mxr_mutex_unlock(&controller->mutex);
    return NO_ERROR;
}

// Implement the char protocol for the slave devices.

static ssize_t intel_broadwell_serialio_i2c_slave_read(
    mx_device_t* dev, void* buf, size_t count) {
    i2c_slave_segment_t segment = {
        .read = 1,
        .buf = buf,
        .len = count,
    };
    return intel_broadwell_serialio_i2c_slave_transfer(dev, &segment, 1);
}

static ssize_t intel_broadwell_serialio_i2c_slave_write(
    mx_device_t* dev, const void* buf, size_t count) {
    i2c_slave_segment_t segment = {
        .read = 0,
        .buf = (void*)buf,
        .len = count,
    };
    return intel_broadwell_serialio_i2c_slave_transfer(dev, &segment, 1);
}

static ssize_t intel_broadwell_serialio_i2c_slave_transfer_ioctl(
    mx_device_t* dev, uint32_t op, const void* in_buf, size_t in_len,
    void* out_buf, size_t out_len) {
    mx_status_t status;

    size_t size = in_len;
    size_t read_len = 0;
    size_t write_len = 0;
    int segment_count = 0;
    uintptr_t segment_addr = (uintptr_t)in_buf;
    // Check that the inputs and output buffer are valid.
    while (size) {
        const i2c_slave_ioctl_segment_t* ioctl_segment =
            (const i2c_slave_ioctl_segment_t*)segment_addr;
        size_t base_size = offsetof(i2c_slave_ioctl_segment_t, buf);
        int len = ioctl_segment->len;
        if (size < base_size) {
            status = ERR_INVALID_ARGS;
            goto slave_transfer_ioctl_finish_2;
        }

        size_t consumed = 0;
        if (ioctl_segment->read) {
            read_len += len;
            consumed = base_size;
        } else {
            write_len += len;
            consumed = base_size + len;
        }
        if (consumed > size) {
            status = ERR_INVALID_ARGS;
            goto slave_transfer_ioctl_finish_2;
        }
        segment_addr += consumed;
        size -= consumed;
        segment_count++;
    }
    if (out_len < write_len) {
        status = ERR_INVALID_ARGS;
        goto slave_transfer_ioctl_finish_2;
    }

    // Build a list of segments to transfer.
    i2c_slave_segment_t* segments =
        calloc(segment_count, sizeof(*segments));
    if (!segments) {
        status = ERR_NO_MEMORY;
        goto slave_transfer_ioctl_finish_2;
    }
    i2c_slave_segment_t* cur_segment = segments;
    segment_addr = (uintptr_t)in_buf;
    uintptr_t out_addr = (uintptr_t)out_buf;
    size = in_len;
    while (size) {
        const i2c_slave_ioctl_segment_t* ioctl_segment =
            (const i2c_slave_ioctl_segment_t*)segment_addr;
        const size_t base_size =
            offsetof(i2c_slave_ioctl_segment_t, buf);
        int len = ioctl_segment->len;

        size_t consumed = 0;
        if (ioctl_segment->read) {
            consumed = base_size;
            cur_segment->read = 1;
            cur_segment->len = len;
            cur_segment->buf = (uint8_t*)out_addr;
            out_addr += len;
        } else {
            consumed = base_size + len;
            cur_segment->read = 0;
            cur_segment->len = len;
            cur_segment->buf = (uint8_t*)(segment_addr + base_size);
        }

        cur_segment++;
        segment_addr += consumed;
        size -= consumed;
    }

    status = intel_broadwell_serialio_i2c_slave_transfer(
        dev, segments, segment_count);
    if (status == NO_ERROR)
        status = write_len;

slave_transfer_ioctl_finish_1:
    free(segments);
slave_transfer_ioctl_finish_2:
    return status;
}

static ssize_t intel_broadwell_serialio_i2c_slave_ioctl(
    mx_device_t* dev, uint32_t op, const void* in_buf, size_t in_len,
    void* out_buf, size_t out_len) {
    switch (op) {
    case I2C_SLAVE_TRANSFER:
        return intel_broadwell_serialio_i2c_slave_transfer_ioctl(
            dev, op, in_buf, in_len, out_buf, out_len);
        break;
    default:
        return ERR_INVALID_ARGS;
    }
}

static mx_protocol_char_t intel_broadwell_serialio_i2c_slave_char_proto = {
    .read = &intel_broadwell_serialio_i2c_slave_read,
    .write = &intel_broadwell_serialio_i2c_slave_write,
    .ioctl = &intel_broadwell_serialio_i2c_slave_ioctl,
};

// Initialize a slave device structure.

mx_status_t intel_broadwell_serialio_i2c_slave_device_init(
    mx_device_t* cont, intel_broadwell_serialio_i2c_slave_device_t* slave,
    uint8_t width, uint16_t address) {
    mx_status_t status = NO_ERROR;

    char name[sizeof(address) * 2 + 2] = {
            [sizeof(name) - 1] = '\0',
    };
    snprintf(name, sizeof(name) - 1, "%04x", address);

    status = device_init(&slave->device, cont->driver, name,
                         &intel_broadwell_serialio_i2c_slave_device_proto);
    if (status < 0)
        return status;

    slave->device.protocol_id = MX_PROTOCOL_CHAR;
    slave->device.protocol_ops =
        &intel_broadwell_serialio_i2c_slave_char_proto;

    slave->chip_address_width = width;
    slave->chip_address = address;

    return status;
}