/* Copyright 2014 Freescale Semiconductor Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *	 notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *	 notice, this list of conditions and the following disclaimer in the
 *	 documentation and/or other materials provided with the distribution.
 *     * Neither the name of Freescale Semiconductor nor the
 *	 names of its contributors may be used to endorse or promote products
 *	 derived from this software without specific prior written permission.
 *
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY Freescale Semiconductor ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Freescale Semiconductor BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "../../fsl-mc/include/dpni.h"	/* DPNI_LINK_OPT_* */
#include "ldpaa_eth.h"

/* To be kept in sync with 'enum dpni_counter' */
char ldpaa_ethtool_stats[][ETH_GSTRING_LEN] = {
	"rx frames",
	"rx bytes",
	"rx frames dropped",
	"rx err frames",
	"rx mcast frames",
	"rx mcast bytes",
	"rx bcast frames",
	"rx bcast bytes",
	"tx frames",
	"tx bytes",
	"tx err frames",
};
/* To be kept in sync with 'struct ldpaa_eth_stats' */
char ldpaa_ethtool_extras[][ETH_GSTRING_LEN] = {
	/* per-cpu stats */

	"tx conf frames",
	"tx conf bytes",
	"tx sg frames",
	"tx sg bytes",
	"rx sg frames",
	"rx sg bytes",
	/* how many times we had to retry the enqueue command */
	"tx portal busy",

	/* per-FQ stats */

	/* How many times we had to retry the volatile dequeue command */
	"rx portal busy",
	"rx fqdan",
	"tx conf fqdan",
#ifdef CONFIG_FSL_QBMAN_DEBUG
	"rx pending frames",
	"rx pending bytes",
	"tx conf pending frames",
	"tx conf pending bytes",
#endif
};
#define LDPAA_ETH_NUM_EXTRA_STATS	ARRAY_SIZE(ldpaa_ethtool_extras)

static void __cold ldpaa_get_drvinfo(struct net_device *net_dev,
				     struct ethtool_drvinfo *drvinfo)
{
	strlcpy(drvinfo->driver, KBUILD_MODNAME, sizeof(drvinfo->driver));
	strlcpy(drvinfo->version, VERSION, sizeof(drvinfo->version));
	strlcpy(drvinfo->fw_version, "N/A", sizeof(drvinfo->fw_version));
	strlcpy(drvinfo->bus_info, dev_name(net_dev->dev.parent->parent),
		sizeof(drvinfo->bus_info));
}

static uint32_t __cold ldpaa_get_msglevel(struct net_device *net_dev)
{
	return ((struct ldpaa_eth_priv *)netdev_priv(net_dev))->msg_enable;
}

static void __cold ldpaa_set_msglevel(struct net_device *net_dev,
				      uint32_t msg_enable)
{
	((struct ldpaa_eth_priv *)netdev_priv(net_dev))->msg_enable =
					msg_enable;
}

static int __cold ldpaa_get_settings(struct net_device *net_dev,
				     struct ethtool_cmd *cmd)
{
	struct dpni_link_state state = {0};
	int err = 0;
	struct ldpaa_eth_priv *priv = netdev_priv(net_dev);

	err = dpni_get_link_state(priv->mc_io, priv->mc_token, &state);
	if (unlikely(err)) {
		netdev_err(net_dev, "ERROR %d getting link state", err);
		goto out;
	}

	if (state.options & DPNI_LINK_OPT_AUTONEG)
		cmd->autoneg = AUTONEG_ENABLE;
	if (!(state.options & DPNI_LINK_OPT_HALF_DUPLEX))
		cmd->duplex = DUPLEX_FULL;
	ethtool_cmd_speed_set(cmd, state.rate);

out:
	return err;
}

static int __cold ldpaa_set_settings(struct net_device *net_dev,
				     struct ethtool_cmd *cmd)
{
	struct dpni_link_cfg cfg = {0};
	struct ldpaa_eth_priv *priv = netdev_priv(net_dev);
	int err = 0;

	netdev_info(net_dev, "Setting link parameters...");

	cfg.rate = ethtool_cmd_speed(cmd);
	if (cmd->autoneg == AUTONEG_ENABLE)
		cfg.options |= DPNI_LINK_OPT_AUTONEG;
	else
		cfg.options &= ~DPNI_LINK_OPT_AUTONEG;
	if (cmd->duplex  == DUPLEX_HALF)
		cfg.options |= DPNI_LINK_OPT_HALF_DUPLEX;
	else
		cfg.options &= ~DPNI_LINK_OPT_HALF_DUPLEX;

	err = dpni_set_link_cfg(priv->mc_io, priv->mc_token, &cfg);
	if (unlikely(err))
		netdev_err(net_dev, "ERROR %d setting link cfg", err);

	return err;
}

static void ldpaa_get_strings(struct net_device *netdev, u32 stringset,
			      u8 *data)
{
	u8 *p = data;
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < DPNI_CNT_NUM_STATS; i++) {
			strlcpy(p, ldpaa_ethtool_stats[i], ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		for (i = 0; i < LDPAA_ETH_NUM_EXTRA_STATS; i++) {
			strlcpy(p, ldpaa_ethtool_extras[i], ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}
		break;
	}
}

static int ldpaa_get_sset_count(struct net_device *net_dev, int sset)
{
	switch (sset) {
	case ETH_SS_STATS: /* ethtool_get_stats(), ethtool_get_drvinfo() */
		return DPNI_CNT_NUM_STATS + LDPAA_ETH_NUM_EXTRA_STATS;
	default:
		return -EOPNOTSUPP;
	}
}

/** Fill in hardware counters, as returned by the MC firmware.
 */
static void ldpaa_get_ethtool_stats(struct net_device *net_dev,
				    struct ethtool_stats *stats,
				    u64 *data)
{
	int i; /* Current index in the data array */
	int j, k, err;

#ifdef CONFIG_FSL_QBMAN_DEBUG
	uint32_t fcnt, bcnt;
	uint32_t fcnt_rx_total = 0, fcnt_tx_total = 0;
	uint32_t bcnt_rx_total = 0, bcnt_tx_total = 0;
#endif
	struct ldpaa_eth_priv *priv = netdev_priv(net_dev);
	struct ldpaa_eth_stats *extras;
	struct ldpaa_eth_fq_stats *fq_stats;

	memset(data, 0,
	       sizeof(u64) * (DPNI_CNT_NUM_STATS + LDPAA_ETH_NUM_EXTRA_STATS));

	/* Print standard counters, from DPNI statistics */
	for (i = 0; i < DPNI_CNT_NUM_STATS; i++) {
		err = dpni_get_counter(priv->mc_io, priv->mc_token, i,
				       data + i);
		if (err != 0)
			netdev_warn(net_dev, "Err %d getting DPNI counter %d",
				    err, i);
	}

	/* Print per-cpu extra stats */
	for_each_online_cpu(k) {
		extras = per_cpu_ptr(priv->percpu_extras, k);
		for (j = 0; j < sizeof(*extras) / sizeof(__u64); j++)
			*((__u64 *)data + i + j) += *((__u64 *)extras + j);
	}
	i += j;

	for (j = 0; j < priv->num_fqs; j++) {
		fq_stats = &priv->fq[j].stats;
		for (k = 0; k < sizeof(*fq_stats) / sizeof(__u64); k++)
			*((__u64 *)data + i + k) += *((__u64 *)fq_stats + k);
	}
	i += k;

#ifdef CONFIG_FSL_QBMAN_DEBUG
	for (j = 0; j < priv->num_fqs; j++) {
		/* Print FQ instantaneous counts */
		err = dpaa_io_query_fq_count(NULL, priv->fq[j].fqid,
					     &fcnt, &bcnt);
		if (unlikely(err)) {
			netdev_warn(net_dev, "FQ query error %d", err);
			return;
		}

		if (priv->fq[j].type == LDPAA_TX_CONF_FQ) {
			fcnt_tx_total += fcnt;
			bcnt_tx_total += bcnt;
		} else {
			fcnt_rx_total += fcnt;
			bcnt_rx_total += bcnt;
		}
	}
	*(data + i++) = fcnt_rx_total;
	*(data + i++) = bcnt_rx_total;
	*(data + i++) = fcnt_tx_total;
	*(data + i++) = bcnt_tx_total;
#endif
}

const struct ethtool_ops ldpaa_ethtool_ops = {
	.get_drvinfo = ldpaa_get_drvinfo,
	.get_msglevel = ldpaa_get_msglevel,
	.set_msglevel = ldpaa_set_msglevel,
	.get_link = ethtool_op_get_link,
	.get_settings = ldpaa_get_settings,
	.set_settings = ldpaa_set_settings,
	.get_sset_count = ldpaa_get_sset_count,
	.get_ethtool_stats = ldpaa_get_ethtool_stats,
	.get_strings = ldpaa_get_strings,
};
