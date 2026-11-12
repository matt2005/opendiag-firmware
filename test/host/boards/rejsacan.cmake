# SPDX-License-Identifier: GPL-3.0-only
# Host suites for the RejsaCAN v3.4 (OPENDIAG_BOARD=rejsacan). Included by
# CMakeLists.txt; sets SUITES, the suites built for this board.
#
# The stock suites open K-line and J1850 and expect the stock pin table, so
# they are not built here. These hold on a CAN only board, and the RejsaCAN
# suites cover the board's own refusals.
set(SUITES
        test_isotp
        test_bus_params
        test_j2534_scheduler
        test_vif_ctrl
        test_elm327_can
        test_elm327_at
        test_can_driver
        test_can_driver_cov
        test_comm_iface_cov
        test_utility_cov
        test_slcan_cov
        test_bus_tx_worker_cov
        test_kline_codec_cov
        test_vif_rejsacan
        test_vif_can_cov
        test_vif_ctrl_can_cov
        test_j2534_rejsacan
        test_j2534_rejsacan_cov
        test_j2534_can_cov
        test_j2534_wire_rx_cov
)
