#include <inttypes.h>
#include <stdlib.h>
#include "bridger.h"

static struct uloop_timeout flow_update_timer;

static int flow_key_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, sizeof(struct bridger_flow_key));
}

static int flow_sort_key_cmp(const void *k1, const void *k2, void *ptr)
{
	return memcmp(k1, k2, sizeof(uint64_t));
}

static AVL_TREE(flows, flow_key_cmp, false, NULL);
static AVL_TREE(sorted_flows, flow_sort_key_cmp, true, NULL);

static void
__bridger_flow_delete(struct bridger_flow *flow)
{
	list_del(&flow->fdb_in_list);
	list_del(&flow->fdb_out_list);
	avl_delete(&sorted_flows, &flow->sort_node);
}

void bridger_flow_delete(struct bridger_flow *flow)
{
	__bridger_flow_delete(flow);
	avl_delete(&flows, &flow->node);
	bridger_bpf_flow_delete(flow);
}

void bridger_check_pending_flow(struct bridger_flow_key *key,
				struct bridger_pending_flow *val)
{
	struct fdb_entry *fdb_in, *fdb_out;
	struct bridger_flow *flow;
	struct bridge *br;
	char src[20], dest[20];
	struct device *dev;
	struct fdb_key fkey = {};

	dev = device_get(key->ifindex);
	if (dev->br)
		return;

	if (!dev->master || !dev->master->br)
		return;

	if (!memcmp(key->src, dev->addr, ETH_ALEN) ||
	    !memcmp(key->src, dev->master->addr, ETH_ALEN) ||
	    !memcmp(key->dest, dev->addr, ETH_ALEN) ||
	    !memcmp(key->dest, dev->master->addr, ETH_ALEN))
		return;

	br = dev->master->br;

	memcpy(fkey.addr, key->src, ETH_ALEN);
	fkey.vlan = device_vlan_get_input(dev, key->vlan);
	fdb_in = fdb_get(br, &fkey);

	memcpy(fkey.addr, key->dest, ETH_ALEN);
	fdb_out = fdb_get(br, &fkey);

	D("Pending flow on %s: %s -> %s @%d num_packets=%"PRIu64" -> %s\n",
	  dev ? dev->ifname : "(unknown)",
	  strcpy(src, format_macaddr(key->src)),
	  strcpy(dest, format_macaddr(key->dest)),
	  key->vlan & BRIDGER_VLAN_ID, val->packets,
	  fdb_out ? fdb_out->dev->ifname : "(unknown)");

	if (!fdb_in || !fdb_out)
		return;

	if (fdb_out->dev->br)
		return;

	flow = avl_find_element(&flows, key, flow, node);
	if (!flow) {
		flow = calloc(1, sizeof(*flow));
		flow->node.key = &flow->key;
		flow->sort_node.key = &flow->avg_packets;
		memcpy(&flow->key, key, sizeof(flow->key));
		avl_insert(&flows, &flow->node);
	} else {
		__bridger_flow_delete(flow);
	}

	flow->fdb_in = fdb_in;
	list_add(&flow->fdb_in_list, &fdb_in->flows_in);

	flow->fdb_out = fdb_out;
	list_add(&flow->fdb_out_list, &fdb_out->flows_out);

	flow->offload.target_port = device_ifindex(fdb_out->dev);
	flow->offload.vlan = device_vlan_get_output(fdb_out->dev, fkey.vlan);

	bridger_bpf_flow_upload(flow);

	avl_insert(&sorted_flows, &flow->sort_node);
}

static void
bridger_flow_update_cb(struct uloop_timeout *timeout)
{
	struct bridger_flow *flow;
	char src[20], dest[20];

	uloop_timeout_set(timeout, 1000);

	avl_for_each_element(&flows, flow, node) {
		avl_delete(&sorted_flows, &flow->sort_node);
		bridger_bpf_flow_update(flow);

		D("Update flow %s@%s -> %s@%s vlan=%d cur_packets=%"PRIu64" avg_packets=%"PRIu64"\n",
		  strcpy(src, format_macaddr(flow->key.src)),
		  flow->fdb_in->dev->ifname,
		  strcpy(dest, format_macaddr(flow->key.dest)),
		  flow->fdb_out->dev->ifname,
		  flow->key.vlan & BRIDGER_VLAN_ID,
		  flow->cur_packets,
		  flow->avg_packets >> BRIDGER_EWMA_SHIFT);

		avl_insert(&sorted_flows, &flow->sort_node);
	}
}

int bridger_flow_init(void)
{
	flow_update_timer.cb = bridger_flow_update_cb;
	bridger_flow_update_cb(&flow_update_timer);

	return 0;
}