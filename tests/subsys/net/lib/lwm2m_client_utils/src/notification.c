/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/ztest.h>
#include <zephyr/fff.h>

#include <net/lwm2m_client_utils_location.h>
#include "stubs.h"


static void setup(void)
{
	/* Register resets */
	DO_FOREACH_FAKE(RESET_FAKE);

	/* reset common FFF internal structures */
	FFF_RESET_HISTORY();
}

static lte_lc_evt_handler_t handler;

static void copy_event_handler(lte_lc_evt_handler_t hd)
{
	handler = hd;
}

ZTEST_SUITE(lwm2m_client_utils_lte_notification, NULL, NULL, NULL, NULL, NULL);


ZTEST(lwm2m_client_utils_lte_notification, test_ncell_schedule_measurement)
{
	int rc;
	struct lte_lc_evt evt = {0};

	setup();
	lwm2m_ncell_schedule_measurement();
	zassert_equal(lte_lc_neighbor_cell_measurement_fake.call_count, 0,
		      "Cell_measurement call count should be 0");
	lte_lc_register_handler_fake.custom_fake = copy_event_handler;
	rc = lwm2m_ncell_handler_register();
	zassert_equal(rc, 0, "Wrong return value");
	evt.type = LTE_LC_EVT_RRC_UPDATE;
	evt.rrc_mode = LTE_LC_RRC_MODE_CONNECTED;
	handler(&evt);
	zassert_equal(lte_lc_neighbor_cell_measurement_fake.call_count, 0,
		      "No call to lte_lc_neighbor_cell_measurement()");
	evt.rrc_mode = LTE_LC_RRC_MODE_IDLE;
	handler(&evt);
	zassert_equal(lte_lc_neighbor_cell_measurement_fake.call_count, 1,
		      "No call to lte_lc_neighbor_cell_measurement()");
	lwm2m_ncell_schedule_measurement();
}

static struct lwm2m_ctx ctx;
ZTEST(lwm2m_client_utils_lte_notification, test_tau_prewarning)
{
	int rc;
	struct lte_lc_evt evt = {0};

	setup();

	lte_lc_register_handler_fake.custom_fake = copy_event_handler;
	rc = lwm2m_ncell_handler_register();
	zassert_equal(rc, 0, "Wrong return value");
	evt.type = LTE_LC_EVT_TAU_PRE_WARNING;
	handler(&evt);
	zassert_equal(lwm2m_rd_client_update_fake.call_count, 0,
		      "LwM2M RD client update call count should be 0");

	lwm2m_rd_client_ctx_fake.return_val = &ctx;
	handler(&evt);
	zassert_equal(lwm2m_rd_client_update_fake.call_count, 1,
		      "LwM2M RD client not updated");
}

ZTEST(lwm2m_client_utils_lte_notification, test_neighbor_cell_meas)
{
	int rc;
	struct lte_lc_evt evt = {0};

	setup();

	lte_lc_register_handler_fake.custom_fake = copy_event_handler;
	rc = lwm2m_ncell_handler_register();
	zassert_equal(rc, 0, "Wrong return value");
	evt.type = LTE_LC_EVT_NEIGHBOR_CELL_MEAS;
	handler(&evt);

	zassert_equal(lwm2m_update_signal_meas_objects_fake.call_count, 1,
		      "No call to lwm2m_update_signal_meas_objects()");
}
