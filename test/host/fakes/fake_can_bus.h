/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file fake_can_bus.h
 * @brief Test double for main/can_bus.c.
 *
 * Two ways to stage a received frame, because the ELM327 layer drains the
 * receive queue before every transmit:
 *
 *   fake_can_stage_response() - delivered only after the next can_send(), the
 *                               way an ECU answers a request.
 *   fake_can_stage_stale()    - available immediately, so a test can prove the
 *                               pre-send drain really discards it.
 *
 * Each staged frame carries a delivery delay in milliseconds that is charged
 * to the fake clock when the frame is handed over. That is how a test makes a
 * reply arrive after the AT ST timeout, or inside an extended response-pending
 * window.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "can_frame.h"

void fake_can_reset(void);

/* --- Staging received traffic --- */

/** Queue a frame the ECU sends @p delay_ms after the next request goes out. */
void fake_can_stage_response(uint32_t id, uint8_t dlc, const uint8_t *data,
                             uint32_t delay_ms);

/**
 * @brief Queue a frame released only after @p after_sends further transmits.
 *
 * Use 2 for the consecutive frames of a multi-frame reply, which a real ECU
 * only sends once it has seen the flow control frame.
 */
void fake_can_stage_response_at(uint32_t id, uint8_t dlc, const uint8_t *data,
                                uint32_t delay_ms, int after_sends);

/** Queue a frame that is already sitting in the driver before any request. */
void fake_can_stage_stale(uint32_t id, uint8_t dlc, const uint8_t *data);

/** As fake_can_stage_stale(), with BUS_RX_* flags on the received message. */
void fake_can_stage_stale_status(uint32_t id, uint8_t dlc, const uint8_t *data,
                                 uint32_t status);

/** The next frame received reports BUS_RX_BUFFER_OVERFLOW, as after a lost burst. */
void fake_can_stage_overflow(void);

/* --- Inspecting the driver's lifecycle --- */

int fake_can_setup_count(void);
int fake_can_teardown_count(void);

/** Baud rate of the most recent can_bus_setup(), or -1 if never set up. */
int fake_can_last_baud(void);

bool fake_can_is_up(void);

/** Whether the most recent open asked for listen-only. */
bool fake_can_last_listen_only(void);

/** Makes the next can_send() fail, to exercise the error path. */
void fake_can_fail_next_send(void);

/** Lets @p sends more can_send() calls succeed, then fails the one after. */
void fake_can_fail_send_after(int sends);
/** Reports each controlled send as having ended @p lag_us before it returns,
 * as the hardware finishes before the task that collects it hears. */
void fake_can_tx_end_lag_us(uint32_t lag_us);

/** Makes can_bus_teardown() return @p result (ESP_OK by default). */
void fake_can_teardown_result(esp_err_t result);

/** Lets the BUS_IOCTL_BUS_ON control through to the driver. Default refused. */
void fake_can_allow_bus_on(bool allow);

/** Accepts parameter set/get on the driver. Default refused, as in can_bus.c. */
void fake_can_enable_params(bool on);

/** Installs or removes the get_stats/reset_stats table entries. Default none. */
void fake_can_enable_stats(bool on);
int fake_can_stats_calls(void);
int fake_can_reset_stats_calls(void);

/** Runs @p hook once, before the next receive. */
void fake_can_on_receive(void (*hook)(void *), void *arg);

/** Makes can_bus_setup() refuse, to exercise a driver that will not start. */
void fake_can_fail_setup(bool fail);

/** Called inside setup/teardown to exercise a reserved, inactive CAN claim. */
void fake_can_on_lifecycle(void (*hook)(void));

/* --- Inspecting transmitted traffic --- */

int fake_can_sent_count(void);

/** Frame at @p idx of the transmit log, or NULL. */
const struct can_frame *fake_can_sent(int idx);

void fake_can_fail_confirmed_send(void);
void fake_can_abort_confirmed_send(void);
