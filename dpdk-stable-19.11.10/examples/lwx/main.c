/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/queue.h>

#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_log.h>
#include <rte_ethdev.h>
#include <rte_config.h>
#include <rte_mempool.h>
#include <rte_kni.h>
#include <rte_malloc.h>

#define RTE_LOGTYPE_LWX 999

/* Max size of a single packet */
#define MAX_PACKET_SZ           2048

/* Size of the data buffer in each mbuf */
#define MBUF_DATA_SZ (MAX_PACKET_SZ + RTE_PKTMBUF_HEADROOM)

/* Number of mbufs in mempool that is created */
#define NB_MBUF                 (8192 * 16)

/* How many packets to attempt to read from NIC in one go */
#define PKT_BURST_SZ            32

/* How many objects (mbufs) to keep in per-lcore mempool cache */
#define MEMPOOL_CACHE_SZ        PKT_BURST_SZ

#define KNI_MAX_KTHREAD 32

#define PKTMBUF_NAME "lwx_pktmbuf_pool"

struct kni_port_params {
	uint16_t port_id;/* Port ID */
	unsigned lcore_rx; /* lcore ID for RX */
	unsigned lcore_tx; /* lcore ID for TX */
	uint32_t nb_lcore_k; /* Number of lcores for KNI multi kernel threads */
	uint32_t nb_kni; /* Number of KNI devices to be created */
	unsigned lcore_k[KNI_MAX_KTHREAD]; /* lcore ID list for kthreads */
	struct rte_kni *kni[KNI_MAX_KTHREAD]; /* KNI context pointers */
} __rte_cache_aligned;

static struct rte_mempool *pktmbuf_pool = NULL;
static struct kni_port_params *kni_port_param_array[RTE_MAX_ETHPORTS] = {0};

static void kni_init(void)
{	
	memset(kni_port_param_array, 0, sizeof(kni_port_param_array));
	kni_port_param_array[0] = rte_zmalloc("kni_port_param", sizeof(struct kni_port_params), RTE_CACHE_LINE_SIZE);
	kni_port_param_array[0]->port_id = 0;
	kni_port_param_array[0]->lcore_rx = rte_lcore_id();
	kni_port_param_array[0]->lcore_tx = rte_lcore_id();
	kni_port_param_array[0]->lcore_k[0] = rte_lcore_id();
	kni_port_param_array[0]->nb_lcore_k = 1;
	rte_kni_init(1);
}

static int
lcore_hello(__attribute__((unused)) void *arg)
{
	unsigned lcore_id;
	lcore_id = rte_lcore_id();
	printf("hello from core %u\n", lcore_id);
	return 0;
}

static kni_loop(__attribute__((unused)) void *arg)
{
	
	return 0;
}

int
main(int argc, char **argv)
{
	int ret;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");

	pktmbuf_pool = rte_pktmbuf_pool_create(PKTMBUF_NAME, NB_MBUF, PKT_BURST_SZ, 0, MBUF_DATA_SZ, rte_socket_id());
	if (pktmbuf_pool == NULL) {
		rte_panic("pktmbuf pool create fail\n");
	}

	kni_init();
	printf("lwx=avail eth count %d is valid port%d\n", rte_eth_dev_count_avail(), rte_eth_dev_is_valid_port(0));
	
	/* call lcore_hello() on every slave lcore */
	rte_eal_mp_remote_launch(lcore_hello, NULL, SKIP_MASTER);

	rte_eal_mp_wait_lcore();

	/* clean up the EAL */
	rte_eal_cleanup();

	return 0;
}
