/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_ediabas.c
 * @brief Deep OBD wire front-end: detect telegrams and E46 DS2 K-Line path.
 */

#include <string.h>

#include "td_test.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ds2_codec.h"
#include "ediabas.h"
#include "ediabas_bmwfast.h"
#include "fake_board.h"
#include "fake_bus.h"
#include "fake_clock.h"
#include "fake_led.h"
#include "fake_port.h"
#include "vif.h"

static comm_port_id_t g_port = COMM_INVALID_PORT_ID;

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

static size_t finish(uint8_t *buf, size_t len, size_t cap) {
    return ediabas_bmwfast_finish(buf, len, cap);
}

/** Feed bytes and return host TX as a binary blob. */
static const uint8_t *ed_ask(const uint8_t *cmd, size_t cmd_len, size_t *out_len) {
    uint8_t buf[64];
    size_t n;

    fake_port_reset();

    TEST_ASSERT_EQUAL_INT(
        ESP_OK, comm_port_rx(fake_port_id(0), cmd, cmd_len));

    while ((n = comm_port_read(g_port, buf, sizeof(buf), 0)) > 0) {
        ediabas_frontend.feed(buf, n);
    }
    ediabas_frontend.poll();

    if (out_len) {
        *out_len = fake_port_len(0);
    }
    return (const uint8_t *)fake_port_text(0);
}

static void assert_mem(const uint8_t *got, size_t got_len, const uint8_t *want,
                       size_t want_len) {
    TEST_ASSERT_EQUAL_INT(want_len, got_len);
    TEST_ASSERT_EQUAL_MEM(want, got, want_len);
}

/* ------------------------------------------------------------------ *
 * Fixture
 * ------------------------------------------------------------------ */

static void ed_harness_init_once(void) {
    static bool done;

    if (done) {
        return;
    }
    done = true;

    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_iface_init());
    fake_port_register_all();
    g_port = fake_port_id(0);
    TEST_ASSERT_MSG(g_port != COMM_INVALID_PORT_ID, "no ediabas port");
}

void td_setup(void) {
    uint8_t drain[64];

    ed_harness_init_once();

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());

    fake_clock_reset();
    fake_led_reset();
    fake_board_reset();
    fake_bus_reset_all();

    while (comm_port_read(g_port, drain, sizeof(drain), 0) > 0) {
        ;
    }
    fake_port_reset();

    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    ediabas_register();
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&ediabas_frontend));
    vif_shell_bind();
    vif_link_set_port(g_port);

    TEST_ASSERT_TRUE(ediabas_frontend.start());
}

void td_teardown(void) {
    ediabas_frontend.stop();
    TEST_ASSERT_MSG(idf_stub_lock_balance() == 0, "vif left %d locks held",
                    idf_stub_lock_balance());
}

/* ------------------------------------------------------------------ *
 * BMW-FAST codec
 * ------------------------------------------------------------------ */

TEST(bmwfast_checksum_is_the_byte_sum) {
    static const uint8_t d[] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE};

    TEST_ASSERT_EQUAL_INT(0x60, ediabas_bmwfast_checksum(d, sizeof(d)));
}

TEST(bmwfast_short_frame_length) {
    uint8_t frame[] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE, 0x00};

    frame[5] = ediabas_bmwfast_checksum(frame, 5);
    TEST_ASSERT_EQUAL_INT(6, ediabas_bmwfast_frame_len(frame, 6));
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(frame, 6));
}

TEST(kline_setup_tel2_length) {
    /* tel 0x02, baud_half 4800, flags, 5-byte payload + checksum */
    uint8_t frame[16] = {0x00, 0x02, 0x12, 0xC0, 0x21, 0x00, 0x00, 0x3C,
                         0x00, 0x05, 0x12, 0x05, 0x00, 0x00, 0x00, 0x00};
    size_t n = finish(frame, 15, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(16, n);
    TEST_ASSERT_EQUAL_INT(16, ediabas_bmwfast_frame_len(frame, 16));
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(frame, 16));
}

/* ------------------------------------------------------------------ *
 * Detect / internal telegrams
 * ------------------------------------------------------------------ */

TEST(fe_echoes_and_reports_ignition) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    /* echo (6) + response (6) */
    TEST_ASSERT_EQUAL_INT(12, out_len);
    assert_mem(out, 6, req, 6);
    TEST_ASSERT_EQUAL_INT(0x82, out[6]);
    TEST_ASSERT_EQUAL_INT(0x81, out[10]); /* ignition + invalid mark */
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 6));
}

TEST(fd_reports_adapter_type_and_version) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFD, 0xFD, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    TEST_ASSERT_EQUAL_INT(6 + 9, out_len);
    assert_mem(out, 6, req, 6);
    TEST_ASSERT_EQUAL_INT(0x85, out[6]);
    TEST_ASSERT_EQUAL_INT(0x00, out[10]); /* type hi */
    TEST_ASSERT_EQUAL_INT(0x46, out[11]); /* type lo = 0x0046 */
    TEST_ASSERT_EQUAL_INT(0x00, out[12]); /* ver hi */
    TEST_ASSERT_EQUAL_INT(0x0F, out[13]); /* ver lo = 0x000F */
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 9));
}

TEST(escape_mode_configure_round_trip) {
    /* Write escape: mode (send|rec)=3, code=FF, mask=80, all XOR 55 */
    uint8_t req[] = {0x84, 0xF1, 0xF1, 0x06, 0x00, 0x00, 0x00, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[4] = (uint8_t)(0x03 ^ 0x55);
    req[5] = (uint8_t)(0xFF ^ 0x55);
    req[6] = (uint8_t)(0x80 ^ 0x55);
    req[7] = ediabas_bmwfast_checksum(req, 7);

    out = ed_ask(req, sizeof(req), &out_len);
    TEST_ASSERT_EQUAL_INT(8 + 8, out_len);
    assert_mem(out, 8, req, 8);
    TEST_ASSERT_EQUAL_INT(req[4], out[12]);
    TEST_ASSERT_EQUAL_INT(req[5], out[13]);
    TEST_ASSERT_EQUAL_INT(req[6], out[14]);
}

TEST(fb_returns_serial_bytes) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFB, 0xFB, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    TEST_ASSERT_EQUAL_INT(6 + 13, out_len);
    TEST_ASSERT_EQUAL_INT(0x89, out[6]);
    TEST_ASSERT_EQUAL_MEM("OpenDIAG", out + 10, 8);
}

TEST(fc_reports_battery_voltage_tenths) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFC, 0xFC, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    TEST_ASSERT_EQUAL_INT(12, out_len);
    /* fake board default vbatt is 12600 mV → 126 tenths */
    TEST_ASSERT_EQUAL_INT(126, out[10]);
}

/* ------------------------------------------------------------------ *
 * DS2 K-Line path
 * ------------------------------------------------------------------ */

TEST(ds2_setup_sends_on_kline_at_9600_even_parity) {
    uint8_t frame[8];
    uint8_t reply_frame[8];
    uint8_t setup[24];
    size_t n;
    size_t rn;
    size_t setup_len;
    size_t out_len = 0;
    size_t sent_len = 0;
    const uint8_t *sent;
    const uint8_t *out;
    const uint8_t data[] = {0x00};

    n = ds2_frame_build(0x12, data, sizeof(data), frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(4, n);

    rn = ds2_frame_build(0x12, (const uint8_t *)"\xA0\x00", 2, reply_frame,
                         sizeof(reply_frame));
    TEST_ASSERT_TRUE(rn >= 5);
    fake_bus_stage_response(FAKE_BUS_KLINE, reply_frame, rn, 5);

    /* tel2: baud_half=4800 (9600), flags1=NO_ECHO|EVEN=0x21 */
    setup[0] = 0x00;
    setup[1] = 0x02;
    setup[2] = 0x12;
    setup[3] = 0xC0;
    setup[4] = 0x21;
    setup[5] = 0x00;
    setup[6] = 0x00;
    setup[7] = 0x3C;
    setup[8] = 0x00;
    setup[9] = (uint8_t)n;
    memcpy(&setup[10], frame, n);
    setup_len = finish(setup, 10 + n, sizeof(setup));

    out = ed_ask(setup, setup_len, &out_len);

    TEST_ASSERT_EQUAL_INT(9600, (int)fake_bus_kline_baud());
    {
        uint32_t parity = 0;
        uint32_t mode = 0;

        TEST_ASSERT_EQUAL_INT(
            0, vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_PARITY,
                                 &parity));
        TEST_ASSERT_EQUAL_INT(BUS_PARITY_EVEN, (int)parity);
        TEST_ASSERT_EQUAL_INT(
            0, vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_KLINE,
                                 BUS_P_KLINE_FRAME_MODE, &mode));
        TEST_ASSERT_EQUAL_INT(BUS_KLINE_FRAME_DS2, (int)mode);
    }

    TEST_ASSERT_EQUAL_INT(1, fake_bus_sent_count(FAKE_BUS_KLINE));
    sent = fake_bus_sent(FAKE_BUS_KLINE, 0, &sent_len);
    TEST_ASSERT_EQUAL_INT(n, sent_len);
    TEST_ASSERT_EQUAL_MEM(frame, sent, n);

    TEST_ASSERT_EQUAL_INT(rn, out_len);
    TEST_ASSERT_EQUAL_MEM(reply_frame, out, rn);
}

TEST(frontend_is_registered_as_ediabas) {
    TEST_ASSERT_NOT_NULL(vif_frontend_find("ediabas"));
    TEST_ASSERT_EQUAL_STRING("ediabas", vif_frontend_find("ediabas")->name);
}

/* ------------------------------------------------------------------ *
 * Codec edges, internal telegrams, escape stuffing and K-Line setup
 * ------------------------------------------------------------------ */

/** Copy @p body, append the BMW-FAST checksum and return the frame length. */
static size_t mk(uint8_t *dst, size_t cap, const uint8_t *body, size_t len) {
    memcpy(dst, body, len);
    return finish(dst, len, cap);
}

/** Stuff 00 -> FF 80 and FF -> FF 7F, as a host in escape mode would send. */
static size_t esc(uint8_t *dst, const uint8_t *src, size_t len) {
    size_t n = 0;

    for (size_t i = 0; i < len; i++) {
        if (src[i] == 0x00 || src[i] == 0xFF) {
            dst[n++] = 0xFF;
            dst[n++] = (uint8_t)(src[i] ^ 0x80);
        } else {
            dst[n++] = src[i];
        }
    }
    return n;
}

/** Feed straight into the front-end (no port round trip); return host TX. */
static const uint8_t *ed_feed(const uint8_t *data, size_t len, size_t *out_len) {
    fake_port_reset();
    ediabas_frontend.feed(data, len);
    if (out_len) {
        *out_len = fake_port_len(0);
    }
    return (const uint8_t *)fake_port_text(0);
}

/** Ask an internal 82 F1 F1 xx xx telegram; the first 6 TX bytes are the echo. */
static const uint8_t *ed_internal(uint8_t cmd, size_t *out_len) {
    static uint8_t req[6];
    const uint8_t body[] = {0x82, 0xF1, 0xF1, cmd, cmd};

    mk(req, sizeof(req), body, sizeof(body));
    return ed_ask(req, sizeof(req), out_len);
}

/** Enable escaping (bit0 send, bit1 receive) with code FF and mask 80. */
static void ed_escape_on(uint8_t mode) {
    const uint8_t body[] = {0x84, 0xF1, 0xF1, 0x06, (uint8_t)(mode ^ 0x55),
                            0xFF ^ 0x55, 0x80 ^ 0x55};
    uint8_t req[8];
    size_t n = mk(req, sizeof(req), body, sizeof(body));

    ed_feed(req, n, NULL);
}

/** Build a tel-0x00 K-Line setup frame around @p payload. */
static size_t mk_setup0(uint8_t *dst, size_t cap, uint16_t baud_half,
                        uint8_t flags1, uint8_t inter, const uint8_t *payload,
                        size_t plen) {
    uint8_t hdr[8] = {0x00,
                      0x00,
                      (uint8_t)(baud_half >> 8),
                      (uint8_t)baud_half,
                      flags1,
                      inter,
                      (uint8_t)(plen >> 8),
                      (uint8_t)plen};

    memcpy(dst, hdr, 8);
    if (plen) {
        memcpy(dst + 8, payload, plen);
    }
    return finish(dst, 8 + plen, cap);
}

/* --- codec --- */

TEST(bmwfast_codec_rejects_null_and_short_input) {
    uint8_t b[8] = {0x82, 0xF1, 0xF1, 0xFE};

    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_checksum(NULL, 4));
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_frame_len(NULL, 4));
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_frame_len(b, 3));
    TEST_ASSERT_FALSE(ediabas_bmwfast_frame_ok(NULL, 6));
    TEST_ASSERT_FALSE(ediabas_bmwfast_frame_ok(b, 3));
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_finish(NULL, 4, 16));
    /* No room for the checksum byte. */
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_finish(b, 4, 4));
    TEST_ASSERT_EQUAL_INT(5, ediabas_bmwfast_finish(b, 4, 5));
}

TEST(bmwfast_frame_len_for_each_setup_telegram) {
    uint8_t b[16] = {0};

    /* tel 0x00: needs 8 bytes, then payload + 9. */
    b[0] = 0x00;
    b[1] = 0x00;
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_frame_len(b, 7));
    b[7] = 0x03;
    TEST_ASSERT_EQUAL_INT(12, ediabas_bmwfast_frame_len(b, 8));
    b[6] = 0x01;
    TEST_ASSERT_EQUAL_INT(0x103 + 9, ediabas_bmwfast_frame_len(b, 8));

    /* tel 0x01 (CAN 1): needs 10 bytes, then payload + 11. */
    memset(b, 0, sizeof(b));
    b[1] = 0x01;
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_frame_len(b, 9));
    b[9] = 0x02;
    TEST_ASSERT_EQUAL_INT(13, ediabas_bmwfast_frame_len(b, 10));

    /* tel 0x02 (K-Line 2): needs 10 bytes, then payload + 11. */
    b[1] = 0x02;
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_frame_len(b, 9));
    TEST_ASSERT_EQUAL_INT(13, ediabas_bmwfast_frame_len(b, 10));

    /* tel 0x03 (CAN 2): needs 13 bytes, then payload + 14. */
    memset(b, 0, sizeof(b));
    b[1] = 0x03;
    b[12] = 0x01;
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_frame_len(b, 12));
    TEST_ASSERT_EQUAL_INT(15, ediabas_bmwfast_frame_len(b, 13));

    /* Anything else is not a telegram. */
    b[1] = 0x04;
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_len(b, 13) == (size_t)-1);
}

TEST(bmwfast_frame_len_for_extended_standard_frames) {
    /* Length nibble 0, byte 3 non-zero: byte 3 is the length. */
    uint8_t one[] = {0x80, 0xF1, 0xF1, 0x03, 0, 0, 0, 0};
    /* Length nibble 0, byte 3 zero: two length bytes at 4 and 5. */
    uint8_t two[] = {0x80, 0xF1, 0xF1, 0x00, 0x01, 0x02, 0, 0};

    TEST_ASSERT_EQUAL_INT(8, ediabas_bmwfast_frame_len(one, sizeof(one)));
    TEST_ASSERT_EQUAL_INT(0, ediabas_bmwfast_frame_len(two, 5));
    TEST_ASSERT_EQUAL_INT(0x102 + 7, ediabas_bmwfast_frame_len(two, 6));
}

TEST(bmwfast_frame_ok_rejects_wrong_length_and_checksum) {
    uint8_t f[6] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE, 0x00};
    uint8_t unknown[5] = {0x00, 0x09, 0x00, 0x00, 0x00};

    f[5] = ediabas_bmwfast_checksum(f, 5);
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(f, 6));
    TEST_ASSERT_FALSE(ediabas_bmwfast_frame_ok(f, 5)); /* length mismatch */
    f[5] ^= 0x01;
    TEST_ASSERT_FALSE(ediabas_bmwfast_frame_ok(f, 6)); /* bad checksum */
    TEST_ASSERT_FALSE(ediabas_bmwfast_frame_ok(unknown, 5)); /* unknown tel */
}

/* --- internal telegrams --- */

TEST(cmd00_reports_default_block_size) {
    size_t n = 0;
    const uint8_t *out = ed_internal(0x00, &n);

    TEST_ASSERT_EQUAL_INT(12, n);
    TEST_ASSERT_EQUAL_INT(0x0F, out[10]);
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 6));
}

TEST(cmd01_reports_separation_time) {
    size_t n = 0;
    const uint8_t *out = ed_internal(0x01, &n);

    TEST_ASSERT_EQUAL_INT(12, n);
    TEST_ASSERT_EQUAL_INT(0x0A, out[10]);
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 6));
}

TEST(cmd02_reports_can_mode_off) {
    size_t n = 0;
    const uint8_t *out = ed_internal(0x02, &n);

    TEST_ASSERT_EQUAL_INT(12, n);
    TEST_ASSERT_EQUAL_INT(0x00, out[10]);
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 6));
}

TEST(cmd04_and_cmd05_acknowledge_without_data) {
    static const uint8_t cmds[] = {0x04, 0x05, 0x84};

    for (size_t i = 0; i < sizeof(cmds); i++) {
        size_t n = 0;
        const uint8_t *out = ed_internal(cmds[i], &n);
        const uint8_t body[] = {0x81, 0xF1, 0xF1, cmds[i]};
        uint8_t exp[5];

        mk(exp, sizeof(exp), body, sizeof(body));
        TEST_ASSERT_EQUAL_INT(6 + 5, n);
        assert_mem(out + 6, 5, exp, 5);
    }
}

TEST(fa_reports_can_off_and_clamp_invalid) {
    size_t n = 0;
    const uint8_t *out = ed_internal(0xFA, &n);

    TEST_ASSERT_EQUAL_INT(6 + 7, n);
    TEST_ASSERT_EQUAL_INT(0x83, out[6]);
    TEST_ASSERT_EQUAL_INT(0xFA, out[9]);
    TEST_ASSERT_EQUAL_INT(0x00, out[10]);
    TEST_ASSERT_EQUAL_INT(0x00, out[11]);
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 7));
}

TEST(fc_clamps_negative_battery_reading_to_zero) {
    size_t n = 0;
    const uint8_t *out;

    fake_board_set_vbatt_mv(-500);
    out = ed_internal(0xFC, &n);
    TEST_ASSERT_EQUAL_INT(12, n);
    TEST_ASSERT_EQUAL_INT(0, out[10]);
}

TEST(legacy_poke_is_echoed_and_acknowledged) {
    static const uint8_t poke[] = {0x81, 0x00, 0x00, 0x00, 0x81};
    static const uint8_t want[] = {0x81, 0x00, 0x00, 0x00, 0x81,
                                   0x81, 0x00, 0x00, 0xFF, 0x80};
    size_t n = 0;
    const uint8_t *out = ed_ask(poke, sizeof(poke), &n);

    assert_mem(out, n, want, sizeof(want));
}

TEST(cmd06_with_zero_field_restores_defaults) {
    /* A zero in any field disables escaping and restores FF / 80. */
    const uint8_t body[] = {0x84, 0xF1, 0xF1, 0x06, 0x00, 0x00, 0x00};
    static const uint8_t poke[] = {0x81, 0x00, 0x00, 0x00, 0x81};
    uint8_t req[8];
    uint8_t wire[24];
    size_t n = mk(req, sizeof(req), body, sizeof(body));
    size_t wn = esc(wire, req, n);
    size_t out_len = 0;
    const uint8_t *out;

    ed_escape_on(0x03);
    out = ed_ask(wire, wn, &out_len);
    /* Echo goes out under the old (escaped) mode: 3 zeros -> +3 bytes. The
     * reply follows unescaped: mode 0, code FF, mask 80, each ^ 55. */
    TEST_ASSERT_EQUAL_INT(11 + 8, out_len);
    TEST_ASSERT_EQUAL_INT(0x00 ^ 0x55, out[15]);
    TEST_ASSERT_EQUAL_INT(0xFF ^ 0x55, out[16]);
    TEST_ASSERT_EQUAL_INT(0x80 ^ 0x55, out[17]);

    /* Escaping is off again: the poke comes back unescaped. */
    out = ed_ask(poke, sizeof(poke), &out_len);
    TEST_ASSERT_EQUAL_INT(10, out_len);
    TEST_ASSERT_EQUAL_INT(0x00, out[1]);
}

TEST(cmd06_read_only_request_leaves_settings_alone) {
    /* Bit 7 of the command byte set: report the current settings only. */
    const uint8_t body[] = {0x84, 0xF1, 0xF1, 0x86, 0x00, 0x00, 0x00};
    uint8_t req[8];
    uint8_t wire[24];
    size_t n = mk(req, sizeof(req), body, sizeof(body));
    size_t wn = esc(wire, req, n);
    size_t out_len = 0;
    const uint8_t *out;

    ed_escape_on(0x03);
    out = ed_feed(wire, wn, &out_len);
    /* Escaped echo (11 bytes), then the reply still reflecting mode 3. */
    TEST_ASSERT_EQUAL_INT(11 + 8, out_len);
    TEST_ASSERT_EQUAL_INT(0x03 ^ 0x55, out[15]);
    TEST_ASSERT_EQUAL_INT(0xFF ^ 0x55, out[16]);
    TEST_ASSERT_EQUAL_INT(0x80 ^ 0x55, out[17]);
}

TEST(soft_reset_clears_escaping) {
    static const uint8_t poke[] = {0x81, 0x00, 0x00, 0x00, 0x81};
    uint8_t req[6];
    uint8_t wire[16];
    const uint8_t body[] = {0x82, 0xF1, 0xF1, 0xFF, 0xFF};
    size_t n = mk(req, sizeof(req), body, sizeof(body));
    size_t wn = esc(wire, req, n);
    size_t out_len = 0;
    const uint8_t *out;

    ed_escape_on(0x03);
    ed_feed(wire, wn, &out_len); /* soft reset */
    out = ed_ask(poke, sizeof(poke), &out_len);
    TEST_ASSERT_EQUAL_INT(10, out_len);
    TEST_ASSERT_EQUAL_INT(0xFF, out[8]);
}

/* --- escape stuffing --- */

TEST(escape_mode_stuffs_outgoing_and_decodes_incoming_bytes) {
    /* Host sends the poke 81 00 00 00 81 with 00 escaped as FF 80. */
    static const uint8_t in[] = {0x81, 0xFF, 0x80, 0xFF, 0x80,
                                 0xFF, 0x80, 0x81};
    /* Echo, then the ack 81 00 00 FF 80 where 00 -> FF 80 and FF -> FF 7F. */
    static const uint8_t want[] = {0x81, 0xFF, 0x80, 0xFF, 0x80, 0xFF, 0x80,
                                   0x81, 0x81, 0xFF, 0x80, 0xFF, 0x80, 0xFF,
                                   0x7F, 0x80};
    size_t n = 0;
    const uint8_t *out;

    ed_escape_on(0x03);
    out = ed_ask(in, sizeof(in), &n);
    assert_mem(out, n, want, sizeof(want));
}

TEST(escape_mode_splits_long_writes_into_chunks) {
    /* SEND_PULSE returns right after the echo, so the 300 zero payload bytes
     * are echoed as 600 escaped bytes: one full 512-byte chunk plus a tail. */
    uint8_t payload[300] = {0};
    uint8_t frame[320];
    size_t n = mk_setup0(frame, sizeof(frame), 4800, 0x10, 0, payload,
                         sizeof(payload));
    size_t out_len = 0;
    const uint8_t *out;

    ed_escape_on(0x01); /* send only, so the frame itself is unescaped */
    out = ed_feed(frame, n, &out_len);
    TEST_ASSERT_EQUAL_INT(600, out_len);
    for (size_t i = 0; i < out_len; i += 2) {
        TEST_ASSERT_EQUAL_INT(0xFF, out[i]);
        TEST_ASSERT_EQUAL_INT(0x80, out[i + 1]);
    }
}

/* --- receive framing --- */

TEST(rx_overflow_discards_the_partial_frame) {
    uint8_t junk[513];
    size_t n = 0;

    /* tel 0x00 with payload length 0xFFFF never completes; byte 513 resets. */
    memset(junk, 0x55, sizeof(junk));
    junk[0] = 0x00;
    junk[1] = 0x00;
    junk[6] = 0xFF;
    junk[7] = 0xFF;
    ed_feed(junk, sizeof(junk), &n);
    TEST_ASSERT_EQUAL_INT(0, n);

    /* The receiver recovered: a good telegram is answered. */
    {
        size_t out_len = 0;
        const uint8_t *out = ed_internal(0xFD, &out_len);

        TEST_ASSERT_EQUAL_INT(6 + 9, out_len);
        TEST_ASSERT_EQUAL_INT(0x85, out[6]);
    }
}

TEST(unknown_setup_telegram_resets_the_receiver) {
    static const uint8_t bad[] = {0x00, 0x05, 0x00, 0x00};
    size_t n = 0;

    ed_feed(bad, sizeof(bad), &n);
    TEST_ASSERT_EQUAL_INT(0, n);
    {
        size_t out_len = 0;
        const uint8_t *out = ed_internal(0xFE, &out_len);

        TEST_ASSERT_EQUAL_INT(12, out_len);
        TEST_ASSERT_EQUAL_INT(0x81, out[10]);
    }
}

TEST(bad_checksum_frame_is_dropped_silently) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE, 0x00};
    size_t n = 0;

    req[5] = (uint8_t)(ediabas_bmwfast_checksum(req, 5) + 1);
    ed_feed(req, sizeof(req), &n);
    TEST_ASSERT_EQUAL_INT(0, n);
}

TEST(can_setup_and_foreign_frames_are_ignored) {
    const uint8_t can1[] = {0x00, 0x01, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t other[] = {0x82, 0x11, 0x22, 0x33, 0x44};
    uint8_t f[16];
    size_t n = 0;
    size_t len = mk(f, sizeof(f), can1, sizeof(can1));

    ed_feed(f, len, &n);
    TEST_ASSERT_EQUAL_INT(0, n);

    len = mk(f, sizeof(f), other, sizeof(other));
    ed_feed(f, len, &n);
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_EQUAL_INT(0, fake_bus_sent_count(FAKE_BUS_KLINE));
}

/* --- K-Line setup --- */

TEST(kline_setup_rejects_invalid_baud) {
    uint8_t pl[1] = {0x00};
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 0, 0x20, 0, pl, 1);
    size_t out_len = 0;

    /* baud_half 0 would be BMW-FAST 115200: not supported. */
    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(0, fake_bus_setup_count(FAKE_BUS_KLINE));

    /* 100 -> 200 baud, below the 980 floor. */
    n = mk_setup0(f, sizeof(f), 100, 0x20, 0, pl, 1);
    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(0, fake_bus_setup_count(FAKE_BUS_KLINE));

    /* 12501 -> 25002 baud, above the 25000 ceiling. */
    n = mk_setup0(f, sizeof(f), 12501, 0x20, 0, pl, 1);
    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(0, fake_bus_setup_count(FAKE_BUS_KLINE));
    TEST_ASSERT_EQUAL_INT(0, fake_bus_sent_count(FAKE_BUS_KLINE));
}

TEST(kline_setup_open_failure_sends_nothing) {
    uint8_t pl[2] = {0x12, 0x00};
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x20, 0, pl, 2);
    size_t out_len = 0;

    fake_bus_open_result(FAKE_BUS_KLINE, ESP_FAIL);
    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(0, out_len);
    TEST_ASSERT_EQUAL_INT(0, fake_bus_sent_count(FAKE_BUS_KLINE));
    TEST_ASSERT_FALSE(fake_bus_is_up(FAKE_BUS_KLINE));
}

TEST(kline_tel0_auto_baud_odd_parity_l_line_and_fast_init) {
    uint8_t frame[8];
    uint8_t reply[8];
    uint8_t setup[24];
    const uint8_t data[] = {0x00};
    size_t n = ds2_frame_build(0x12, data, sizeof(data), frame, sizeof(frame));
    size_t rn = ds2_frame_build(0x12, (const uint8_t *)"\xA0\x00", 2, reply,
                                sizeof(reply));
    /* flags: odd parity | L-line | fast init, no NO_ECHO so payload echoes. */
    size_t sl = mk_setup0(setup, sizeof(setup), 1, 0x02 | 0x08 | 0x40, 7,
                          frame, n);
    size_t out_len = 0;
    const uint8_t *out;
    uint32_t v = 1;
    uint8_t want[16];

    fake_bus_stage_response(FAKE_BUS_KLINE, reply, rn, 5);
    out = ed_feed(setup, sl, &out_len);

    /* baud_half 1 is auto-baud: mapped to 9600. */
    TEST_ASSERT_EQUAL_INT(9600, (int)fake_bus_kline_baud());
    TEST_ASSERT_EQUAL_INT(BUS_PARITY_ODD, (int)fake_bus_kline_parity());
    TEST_ASSERT_EQUAL_INT(
        0, vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_K_LINE_ONLY,
                             &v));
    TEST_ASSERT_EQUAL_INT(0, (int)v); /* L-line in use */
    TEST_ASSERT_NOT_NULL(fake_bus_kline_last_init());
    TEST_ASSERT_EQUAL_INT(0x33, fake_bus_kline_last_init()->address);
    TEST_ASSERT_EQUAL_INT(1, fake_bus_sent_count(FAKE_BUS_KLINE));

    /* Echo of the payload, then the ECU reply. */
    memcpy(want, frame, n);
    memcpy(want + n, reply, rn);
    assert_mem(out, out_len, want, n + rn);
}

TEST(kline_default_parity_is_none_and_second_setup_reuses_the_bus) {
    uint8_t pl[1] = {0x12};
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x20, 0, pl, 1);
    size_t out_len = 0;

    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(BUS_PARITY_NONE, (int)fake_bus_kline_parity());
    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(1, fake_bus_setup_count(FAKE_BUS_KLINE));
}

TEST(kline_send_pulse_applies_config_without_sending) {
    uint8_t pl[3] = {0x01, 0x05, 0x00};
    uint8_t shortpl[1] = {0x01};
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x10 | 0x20, 0, pl, 3);
    size_t out_len = 0;

    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(0, out_len);
    TEST_ASSERT_EQUAL_INT(9600, (int)fake_bus_kline_baud());

    n = mk_setup0(f, sizeof(f), 4800, 0x10, 0, shortpl, 1);
    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(1, (int)out_len); /* echo only */
    TEST_ASSERT_EQUAL_INT(0, fake_bus_sent_count(FAKE_BUS_KLINE));
}

TEST(kline_empty_payload_only_configures) {
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x00, 0, NULL, 0);
    size_t out_len = 0;

    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(0, out_len);
    TEST_ASSERT_EQUAL_INT(9600, (int)fake_bus_kline_baud());
    TEST_ASSERT_EQUAL_INT(0, fake_bus_sent_count(FAKE_BUS_KLINE));
}

TEST(kline_send_failure_gives_no_reply) {
    uint8_t pl[2] = {0x12, 0x00};
    uint8_t reply[8];
    uint8_t f[16];
    size_t rn = ds2_frame_build(0x12, (const uint8_t *)"\xA0\x00", 2, reply,
                                sizeof(reply));
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x20, 0, pl, 2);
    size_t out_len = 0;

    fake_bus_stage_response(FAKE_BUS_KLINE, reply, rn, 5);
    fake_bus_fail_next_send(FAKE_BUS_KLINE);
    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(0, out_len);
    TEST_ASSERT_EQUAL_INT(0, fake_bus_sent_count(FAKE_BUS_KLINE));
}

TEST(kline_no_reply_times_out_quietly) {
    uint8_t pl[2] = {0x12, 0x00};
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x20, 0, pl, 2);
    size_t out_len = 0;

    ed_feed(f, n, &out_len);
    TEST_ASSERT_EQUAL_INT(1, fake_bus_sent_count(FAKE_BUS_KLINE));
    TEST_ASSERT_EQUAL_INT(0, out_len);
}

TEST(soft_reset_releases_an_open_kline) {
    uint8_t pl[1] = {0x00};
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x30, 0, pl, 1);
    size_t out_len = 0;

    ed_feed(f, n, &out_len);
    TEST_ASSERT_TRUE(fake_bus_is_up(FAKE_BUS_KLINE));
    ed_internal(0xFF, &out_len);
    TEST_ASSERT_FALSE(fake_bus_is_up(FAKE_BUS_KLINE));
}

TEST(stop_releases_an_open_kline) {
    uint8_t pl[1] = {0x00};
    uint8_t f[16];
    size_t n = mk_setup0(f, sizeof(f), 4800, 0x30, 0, pl, 1);
    size_t out_len = 0;

    ed_feed(f, n, &out_len);
    TEST_ASSERT_TRUE(fake_bus_is_up(FAKE_BUS_KLINE));
    ediabas_frontend.stop();
    TEST_ASSERT_FALSE(fake_bus_is_up(FAKE_BUS_KLINE));
    TEST_ASSERT_TRUE(ediabas_frontend.start());
}

/* --- registration --- */

TEST(registering_a_second_frontend_named_ediabas_is_refused) {
    static const vif_frontend_t other = {.name = "ediabas"};

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, vif_frontend_register(&other));
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_frontend_register(&ediabas_frontend));
    ediabas_register(); /* idempotent */
    TEST_ASSERT_TRUE(vif_frontend_find("ediabas") == &ediabas_frontend);
}
