/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * Simple send/reply ping tool using the PGM transport.
 *
 * Copyright (c) 2006-2007 Miru Limited.
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


#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include <glib.h>

#include <pgm/backtrace.h>
#include <pgm/log.h>
#include <pgm/transport.h>
#include <pgm/gsi.h>
#include <pgm/signal.h>
#include <pgm/timer.h>
#include <pgm/if.h>
#include <pgm/http.h>
#include <pgm/snmp.h>


/* typedefs */

struct idle_source {
	GSource		source;
	guint64		expiration;
};

/* globals */

static int g_port = 7500;
static const char* g_network = "";
static int g_udp_encap_port = 0;

static int g_odata_rate = 10;					/* 10 per second */
static int g_odata_interval = (1000 * 1000) / 10;	/* 100 ms */
static guint32 g_payload = 0;
static int g_max_tpdu = 1500;
static int g_max_rte = 400*1000;
static int g_sqns = 100 * 1000;

static gboolean g_fec = FALSE;
static int g_k = 64;
static int g_n = 255;

static gboolean g_send_mode = TRUE;

static pgm_transport_t* g_transport = NULL;
static GIOChannel* g_io_channel = NULL;

static GMainLoop* g_loop = NULL;


static void on_signal (int);
static gboolean on_startup (gpointer);
static gboolean on_shutdown (gpointer);
static gboolean on_mark (gpointer);

static void send_odata (void);
static gboolean on_io_data (GIOChannel*, GIOCondition, gpointer);
static int on_data (gpointer, guint, gpointer);

static gboolean idle_prepare (GSource*, gint*);
static gboolean idle_check (GSource*);
static gboolean idle_dispatch (GSource*, GSourceFunc, gpointer);


G_GNUC_NORETURN static void
usage (const char* bin)
{
	fprintf (stderr, "Usage: %s [options]\n", bin);
	fprintf (stderr, "  -n <network>    : Multicast group or unicast IP address\n");
	fprintf (stderr, "  -s <port>       : IP port\n");
        fprintf (stderr, "  -p <port>       : Encapsulate PGM in UDP on IP port\n");
	fprintf (stderr, "  -d <seconds>    : Terminate transport after duration.\n");
	fprintf (stderr, "  -m <frequency>  : Number of message to send per second\n");
	fprintf (stderr, "  -l              : Listen mode (default send mode)\n");
        fprintf (stderr, "  -r <rate>       : Regulate to rate bytes per second\n");
        fprintf (stderr, "  -f <type>       : Enable FEC with either proactive or ondemand parity\n");
        fprintf (stderr, "  -k <k>          : Configure Reed-Solomon code (n, k)\n");
        fprintf (stderr, "  -g <n>\n");
        fprintf (stderr, "  -t              : Enable HTTP administrative interface\n");
        fprintf (stderr, "  -x              : Enable SNMP interface\n");
	exit (1);
}

int
main (
	int	argc,
	char   *argv[]
	)
{
	g_message ("pgmping");
	gboolean enable_http = FALSE;
	gboolean enable_snmpx = FALSE;

	int timeout = 0;

/* parse program arguments */
	const char* binary_name = strrchr (argv[0], '/');
	int c;
	while ((c = getopt (argc, argv, "s:n:p:m:ld:r:e:k:g:txh")) != -1)
	{
		switch (c) {
		case 'n':	g_network = optarg; break;
		case 's':	g_port = atoi (optarg); break;
		case 'p':	g_udp_encap_port = atoi (optarg); break;
		case 'r':	g_max_rte = atoi (optarg); break;

		case 'f':	g_fec = TRUE; break;
		case 'k':	g_k = atoi (optarg); break;
		case 'g':	g_n = atoi (optarg); break;

		case 't':	enable_http = TRUE; break;
		case 'x':	enable_snmpx = TRUE; break;

		case 'm':	g_odata_rate = atoi (optarg);
				g_odata_interval = (1000 * 1000) / g_odata_rate; break;
		case 'd':	timeout = 1000 * atoi (optarg); break;

		case 'l':	g_send_mode = FALSE; break;

		case 'h':
		case '?': usage (binary_name);
		}
	}

	if (g_fec && ( !g_k || !g_n )) {
		puts ("Invalid Reed-Solomon parameters.");
		usage (binary_name);
	}

	log_init ();
	pgm_init ();

	if (enable_http)
		pgm_http_init(PGM_HTTP_DEFAULT_SERVER_PORT);
	if (enable_snmpx)
		pgm_snmp_init();

	g_loop = g_main_loop_new (NULL, FALSE);

/* setup signal handlers */
	signal (SIGSEGV, on_sigsegv);
	pgm_signal_install (SIGINT, on_signal);
	pgm_signal_install (SIGTERM, on_signal);
	pgm_signal_install (SIGHUP, SIG_IGN);

/* delayed startup */
	g_message ("scheduling startup.");
	g_timeout_add (0, (GSourceFunc)on_startup, NULL);

	if (timeout) {
		g_message ("scheduling shutdown.");
		g_timeout_add (timeout, (GSourceFunc)on_shutdown, NULL);
	}

/* dispatch loop */
	g_message ("entering main event loop ... ");
	g_main_loop_run (g_loop);

	g_message ("event loop terminated, cleaning up.");

/* cleanup */
	g_main_loop_unref(g_loop);
	g_loop = NULL;

	if (g_transport) {
		g_message ("destroying transport.");

		pgm_transport_destroy (g_transport, TRUE);
		g_transport = NULL;
	}

	if (enable_http)
		pgm_http_shutdown();
	if (enable_snmpx)
		pgm_snmp_shutdown();

	g_message ("finished.");
	return 0;
}

static void
on_signal (
	G_GNUC_UNUSED int signum
	)
{
	g_message ("on_signal");

	g_main_loop_quit(g_loop);
}

static gboolean
on_shutdown (
	G_GNUC_UNUSED gpointer data
	)
{
	g_message ("shutdown");
	g_main_loop_quit(g_loop);
	return FALSE;
}

static gboolean
on_startup (
	G_GNUC_UNUSED gpointer data
	)
{
	g_message ("startup.");
	g_message ("create transport.");

	pgm_gsi_t gsi;
#if 0
	char hostname[NI_MAXHOST];
	struct addrinfo hints, *res = NULL;

	gethostname (hostname, sizeof(hostname));
	memset (&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_flags = AI_ADDRCONFIG;
	getaddrinfo (hostname, NULL, &hints, &res);
	int e = pgm_create_ipv4_gsi (((struct sockaddr_in*)(res->ai_addr))->sin_addr, &gsi);
	g_assert (e == 0);
	freeaddrinfo (res);
#else
	int e = pgm_create_md5_gsi (&gsi);
	g_assert (e == 0);
#endif

	struct pgm_sock_mreq recv_smr, send_smr;
	int smr_len = 1;
	e = pgm_if_parse_transport (g_network, AF_INET, &recv_smr, &send_smr, &smr_len);
	g_assert (e == 0);
	g_assert (smr_len == 1);

	if (g_udp_encap_port) {
		((struct sockaddr_in*)&send_smr.smr_multiaddr)->sin_port = g_htons (g_udp_encap_port);
		((struct sockaddr_in*)&recv_smr.smr_interface)->sin_port = g_htons (g_udp_encap_port);
	}

	e = pgm_transport_create (&g_transport, &gsi, g_port, &recv_smr, 1, &send_smr);
	g_assert (e == 0);

	pgm_transport_set_sndbuf (g_transport, 1024 * 1024);
	pgm_transport_set_rcvbuf (g_transport, 1024 * 1024);
	pgm_transport_set_max_tpdu (g_transport, g_max_tpdu);
	pgm_transport_set_txw_sqns (g_transport, g_sqns);
	pgm_transport_set_txw_max_rte (g_transport, g_max_rte);
	pgm_transport_set_rxw_sqns (g_transport, g_sqns);
	pgm_transport_set_hops (g_transport, 16);
	pgm_transport_set_ambient_spm (g_transport, 8192*1000);
	guint spm_heartbeat[] = { 1*1000, 1*1000, 2*1000, 4*1000, 8*1000, 16*1000, 32*1000, 64*1000, 128*1000, 256*1000, 512*1000, 1024*1000, 2048*1000, 4096*1000, 8192*1000 };
	pgm_transport_set_heartbeat_spm (g_transport, spm_heartbeat, G_N_ELEMENTS(spm_heartbeat));
	pgm_transport_set_peer_expiry (g_transport, 5*8192*1000);
	pgm_transport_set_spmr_expiry (g_transport, 250*1000);
	pgm_transport_set_nak_bo_ivl (g_transport, 50*1000);
	pgm_transport_set_nak_rpt_ivl (g_transport, 200*1000);
	pgm_transport_set_nak_rdata_ivl (g_transport, 500*1000);
	pgm_transport_set_nak_data_retries (g_transport, 2);
	pgm_transport_set_nak_ncf_retries (g_transport, 5);

#if 0
	if (g_send_mode)
		pgm_transport_set_txw_preallocate (g_transport, g_sqns);
	else {
		pgm_transport_set_rxw_preallocate (g_transport, g_sqns);
		pgm_transport_set_event_preallocate (g_transport, g_odata_rate * 2);
	}
#endif

	if (g_fec) {
		pgm_transport_set_fec (g_transport, 0, TRUE, TRUE, g_n, g_k);
	}

	e = pgm_transport_bind (g_transport);
	if (e < 0) {
		if      (e == -1)
			g_critical ("pgm_transport_bind failed errno %i: \"%s\"", errno, strerror(errno));
		else if (e == -2)
			g_critical ("pgm_transport_bind failed h_errno %i: \"%s\"", h_errno, hstrerror(h_errno));
		else
			g_critical ("pgm_transport_bind failed e %i", e);
		g_main_loop_quit(g_loop);
		return FALSE;
	}
	g_assert (e == 0);

/* period timer to indicate some form of life */
// TODO: Gnome 2.14: replace with g_timeout_add_seconds()
	g_timeout_add(10 * 1000, (GSourceFunc)on_mark, NULL);

	if (g_send_mode)
	{
		if ((g_odata_interval / 1000) > 0)
			g_message ("scheduling ODATA broadcasts every %i ms.", g_odata_interval / 1000);
		else
			g_message ("scheduling ODATA broadcasts every %i us.", g_odata_interval);

// create an idle source with non-glib timing
		static GSourceFuncs idle_funcs = {
			.prepare	= idle_prepare,
			.check		= idle_check,
			.dispatch	= idle_dispatch,
			.finalize	= NULL,
			.closure_callback = NULL
		};
		GSource* source = g_source_new (&idle_funcs, sizeof(struct idle_source));
		struct idle_source* idle_source = (struct idle_source*)source;
		idle_source->expiration = pgm_time_update_now() + g_odata_interval;
		g_source_set_priority (source, G_PRIORITY_LOW);
		/* guint id = */ g_source_attach (source, NULL);
		g_source_unref (source);
	}
	else
	{
		g_message ("adding PGM receiver watch");
		g_io_channel = g_io_channel_unix_new (g_transport->recv_sock);
		/* guint event = */ g_io_add_watch (g_io_channel, G_IO_IN, on_io_data, NULL);
	}

	g_message ("startup complete.");
	return FALSE;
}

static gboolean
idle_prepare (
	GSource*	source,
	gint*		timeout
	)
{
	struct idle_source* idle_source = (struct idle_source*)source;

	guint64 now = pgm_time_update_now();
	glong msec = ((gint64)idle_source->expiration - (gint64)now) / 1000;
	if (msec < 0)
		msec = 0;
	else
		msec = MIN (G_MAXINT, (guint)msec);
	*timeout = (gint)msec;
	return (msec == 0);
}

static gboolean
idle_check (
	GSource*	source
	)
{
	struct idle_source* idle_source = (struct idle_source*)source;
	guint64 now = pgm_time_update_now();
	gboolean retval = ( pgm_time_after_eq(now, idle_source->expiration) );
	if (!retval) g_thread_yield();
	return retval;
}

static gboolean
idle_dispatch (
	GSource*	source,
	G_GNUC_UNUSED GSourceFunc	callback,
	G_GNUC_UNUSED gpointer	user_data
	)
{
	struct idle_source* idle_source = (struct idle_source*)source;

	send_odata ();
	idle_source->expiration += g_odata_interval;
	return TRUE;
}

/* we send out a stream of ODATA packets with basic changing payload
 */

static void
send_odata (void)
{
	gssize e;
	char b[100];
	sprintf (b, "%" G_GUINT32_FORMAT, g_payload);

	e = pgm_transport_send (g_transport, &b, sizeof(b), 0);
        if (e < 0) {
		g_warning ("pgm_transport_send failed: %i/%s.", errno, strerror(errno));
                return;
        }

	g_payload++;
}

/* this can significantly starve the event loop if everything is running in parallel.
 */

static gboolean
on_io_data (
	G_GNUC_UNUSED GIOChannel*	source,
	G_GNUC_UNUSED GIOCondition	condition,
	G_GNUC_UNUSED gpointer	data
	)
{
	gssize len = 0;
	do {
		char buffer[4096];
		len = pgm_transport_recv (g_transport, buffer, sizeof(buffer), MSG_DONTWAIT /* non-blocking */);
		if (len > 0)
		{
			on_data (buffer, len, NULL);
		}
	} while (len > 0);

	return TRUE;
}

static int
on_data (
	gpointer	data,
	guint		len,
	G_GNUC_UNUSED gpointer	user_data
	)
{
	if (len == 100)
		g_payload = strtol ((char*)data, NULL, 10);
	else
		g_warning ("payload size %i bytes", len);

//	g_message ("payload: %s", (char*)data);

	return 0;
}

/* idle log notification
 */

static gboolean
on_mark (
	G_GNUC_UNUSED gpointer data
	)
{
	g_message ("counter: %i", g_payload);

	return TRUE;
}

/* eof */
