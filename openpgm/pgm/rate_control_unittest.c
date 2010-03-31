/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * unit tests for rate regulation.
 *
 * Copyright (c) 2009 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <signal.h>
#include <stdlib.h>
#include <glib.h>
#include <check.h>

#include <pgm/time.h>

/* mock state */

static pgm_time_t mock_pgm_time_now = 0x1;


/* mock functions for external references */

static
pgm_time_t
mock_pgm_time_update_now (void)
{
	return mock_pgm_time_now;
}


#define pgm_time_now		mock_pgm_time_now
#define pgm_time_update_now	mock_pgm_time_update_now

#define RATE_CONTROL_DEBUG
#include "rate_control.c"


/* target:
 *	void
 *	pgm_rate_create (
 *		rate_t**		bucket_,
 *		const guint		rate_per_sec,
 *		const guint		iphdr_len,
 *		const guint		max_tpdu
 *	)
 */

START_TEST (test_create_pass_001)
{
	rate_t* rate = NULL;
	pgm_rate_create (&rate, 100*1000, 10, 1500);
}
END_TEST

START_TEST (test_create_fail_001)
{
	pgm_rate_create (NULL, 0, 0, 1500);
	fail ("reached");
}
END_TEST

/* target:
 *	void
 *	pgm_rate_destroy (
 *		rate_t*			bucket
 *	)
 */

START_TEST (test_destroy_pass_001)
{
	rate_t* rate = NULL;
	pgm_rate_create (&rate, 100*1000, 10, 1500);
	pgm_rate_destroy (rate);
}
END_TEST

START_TEST (test_destroy_fail_001)
{
	pgm_rate_destroy (NULL);
	fail ("reached");
}
END_TEST

/* target:
 *	gboolean
 *	pgm_rate_check (
 *		rate_t*			bucket,
 *		const guint		data_size,
 *		const gboolean		is_nonblocking
 *	)
 *
 * 001: should use seconds resolution to allow 2 packets through then fault.
 */

START_TEST (test_check_pass_001)
{
	rate_t* rate = NULL;
	pgm_rate_create (&rate, 2*1010, 10, 1500);
	mock_pgm_time_now += pgm_secs(2);
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	pgm_rate_destroy (rate);
}
END_TEST

START_TEST (test_check_fail_001)
{
	pgm_rate_check (NULL, 1000, FALSE);
	fail ("reached");
}
END_TEST

/* 002: assert that only one packet should pass through small bucket 
 */

START_TEST (test_check_pass_002)
{
	rate_t* rate = NULL;
	pgm_rate_create (&rate, 2*900, 10, 1500);
	mock_pgm_time_now += pgm_secs(2);
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	pgm_rate_destroy (rate);
}
END_TEST

/* 003: millisecond resolution should initiate millisecond fills.
 */

START_TEST (test_check_pass_003)
{
	rate_t* rate = NULL;
	pgm_rate_create (&rate, 2*1010*1000, 10, 1500);
	mock_pgm_time_now += pgm_secs(2);
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
/* duplicate check at same time point */
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
/* advance time causing a millisecond fill to occur */
	mock_pgm_time_now += pgm_msecs(1);
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
/* advance time to fill bucket enough for only one packet */
	mock_pgm_time_now += pgm_usecs(500);
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
/* advance time to fill the bucket a little but not enough for one packet */
	mock_pgm_time_now += pgm_usecs(100);
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
/* advance time a lot, should be limited to millisecond fill rate */
	mock_pgm_time_now += pgm_secs(10);
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (TRUE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	fail_unless (FALSE == pgm_rate_check (rate, 1000, TRUE), "rate_check failed");
	pgm_rate_destroy (rate);
}
END_TEST


static
Suite*
make_test_suite (void)
{
	Suite* s;

	s = suite_create (__FILE__);

	TCase* tc_create = tcase_create ("create");
	suite_add_tcase (s, tc_create);
	tcase_add_test (tc_create, test_create_pass_001);
	tcase_add_test_raise_signal (tc_create, test_create_fail_001, SIGABRT);

	TCase* tc_destroy = tcase_create ("destroy");
	suite_add_tcase (s, tc_destroy);
	tcase_add_test (tc_destroy, test_destroy_pass_001);
	tcase_add_test_raise_signal (tc_destroy, test_destroy_fail_001, SIGABRT);

	TCase* tc_check = tcase_create ("check");
	suite_add_tcase (s, tc_check);
	tcase_add_test (tc_check, test_check_pass_001);
	tcase_add_test (tc_check, test_check_pass_002);
	tcase_add_test (tc_check, test_check_pass_003);
	tcase_add_test_raise_signal (tc_check, test_check_fail_001, SIGABRT);
	return s;
}

static
Suite*
make_master_suite (void)
{
	Suite* s = suite_create ("Master");
	return s;
}

int
main (void)
{
	SRunner* sr = srunner_create (make_master_suite ());
	srunner_add_suite (sr, make_test_suite ());
	srunner_run_all (sr, CK_ENV);
	int number_failed = srunner_ntests_failed (sr);
	srunner_free (sr);
	return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* eof */
