/**
 * Copyright (c) 2015 Runtime Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <errno.h>
#include <assert.h>
#include "os/os.h"
#include "nimble/ble.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "ble_hs_conn.h"
#include "ble_l2cap.h"

_Static_assert(sizeof (struct ble_l2cap_hdr) == BLE_L2CAP_HDR_SZ,
               "struct ble_l2cap_hdr must be 4 bytes");

#define BLE_L2CAP_CHAN_MAX              32

static struct os_mempool ble_l2cap_chan_pool;

static void *ble_l2cap_chan_mem;

struct ble_l2cap_chan *
ble_l2cap_chan_alloc(void)
{
    struct ble_l2cap_chan *chan;

    chan = os_memblock_get(&ble_l2cap_chan_pool);
    if (chan == NULL) {
        return NULL;
    }

    memset(chan, 0, sizeof *chan);

    return chan;
}

void
ble_l2cap_chan_free(struct ble_l2cap_chan *chan)
{
    int rc;

    if (chan == NULL) {
        return;
    }

    rc = os_memblock_put(&ble_l2cap_chan_pool, chan);
    assert(rc == 0);
}

uint16_t
ble_l2cap_chan_mtu(struct ble_l2cap_chan *chan)
{
    uint16_t mtu;

    /* If either side has not exchanged MTU size, use the default.  Otherwise,
     * use the lesser of the two exchanged values.
     */
    if (!(chan->blc_flags & BLE_L2CAP_CHAN_F_TXED_MTU) ||
        chan->blc_peer_mtu == 0) {

        mtu = chan->blc_default_mtu;
    } else {
        mtu = min(chan->blc_my_mtu, chan->blc_peer_mtu);
    }

    assert(mtu >= chan->blc_default_mtu);

    return mtu;
}

int
ble_l2cap_parse_hdr(struct os_mbuf *om, int off,
                    struct ble_l2cap_hdr *l2cap_hdr)
{
    int rc;

    rc = os_mbuf_copydata(om, off, sizeof *l2cap_hdr, l2cap_hdr);
    if (rc != 0) {
        return EMSGSIZE;
    }

    l2cap_hdr->blh_len = le16toh(&l2cap_hdr->blh_len);
    l2cap_hdr->blh_cid = le16toh(&l2cap_hdr->blh_cid);

    return 0;
}

struct os_mbuf *
ble_l2cap_prepend_hdr(struct os_mbuf *om, uint16_t cid)
{
    struct ble_l2cap_hdr hdr;

    htole16(&hdr.blh_len, OS_MBUF_PKTHDR(om)->omp_len);
    htole16(&hdr.blh_cid, cid);

    om = os_mbuf_prepend(om, sizeof hdr);
    if (om == NULL) {
        return NULL;
    }

    memcpy(om->om_data, &hdr, sizeof hdr);

    return om;
}

static int
ble_l2cap_rx_payload(struct ble_hs_conn *conn, struct ble_l2cap_chan *chan,
                     struct os_mbuf *om)
{
    int rc;

    assert(chan->blc_rx_buf == NULL);
    chan->blc_rx_buf = om;

    rc = chan->blc_rx_fn(conn, chan, &chan->blc_rx_buf);
    os_mbuf_free_chain(chan->blc_rx_buf);
    chan->blc_rx_buf = NULL;
    if (rc != 0) {
        return rc;
    }

    return 0;
}

int
ble_l2cap_rx(struct ble_hs_conn *conn,
             struct hci_data_hdr *hci_hdr,
             struct os_mbuf *om)
{
    struct ble_l2cap_chan *chan;
    struct ble_l2cap_hdr l2cap_hdr;
    int rc;

    /* XXX: HCI-fragmentation unsupported. */
    assert(BLE_HCI_DATA_PB(hci_hdr->hdh_handle_pb_bc) == BLE_HCI_PB_FULL);

    rc = ble_l2cap_parse_hdr(om, 0, &l2cap_hdr);
    if (rc != 0) {
        return rc;
    }

    /* Strip L2CAP header from the front of the mbuf. */
    os_mbuf_adj(om, BLE_L2CAP_HDR_SZ);

    if (l2cap_hdr.blh_len != hci_hdr->hdh_len - BLE_L2CAP_HDR_SZ) {
        return EMSGSIZE;
    }

    chan = ble_hs_conn_chan_find(conn, l2cap_hdr.blh_cid);
    if (chan == NULL) {
        return ENOENT;
    }

    rc = ble_l2cap_rx_payload(conn, chan, om);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/**
 * Transmits the L2CAP payload contained in the specified mbuf.  The supplied
 * mbuf is consumed, regardless of the outcome of the function call.
 *
 * @param chan                  The L2CAP channel to transmit over.
 * @param om                    The data to transmit.
 *
 * @return                      0 on success; nonzero on error.
 */
int
ble_l2cap_tx(struct ble_l2cap_chan *chan, struct os_mbuf *om)
{
    int rc;

    om = ble_l2cap_prepend_hdr(om, chan->blc_cid);
    if (om == NULL) {
        rc = ENOMEM;
        goto err;
    }

    rc = ble_hs_tx_data(om);
    if (rc != 0) {
        goto err;
    }

    return 0;

err:
    os_mbuf_free_chain(om);
    return rc;
}

static void
ble_l2cap_free_mem(void)
{
    free(ble_l2cap_chan_mem);
    ble_l2cap_chan_mem = NULL;
}

int
ble_l2cap_init(void)
{
    int rc;

    ble_l2cap_free_mem();

    ble_l2cap_chan_mem = malloc(
        OS_MEMPOOL_BYTES(BLE_L2CAP_CHAN_MAX,
                         sizeof (struct ble_l2cap_chan)));
    if (ble_l2cap_chan_mem == NULL) {
        rc = ENOMEM;
        goto err;
    }

    rc = os_mempool_init(&ble_l2cap_chan_pool, BLE_L2CAP_CHAN_MAX,
                         sizeof (struct ble_l2cap_chan),
                         ble_l2cap_chan_mem, "ble_l2cap_chan_pool");
    if (rc != 0) {
        rc = EINVAL; // XXX
        goto err;
    }

    return 0;

err:
    ble_l2cap_free_mem();
    return rc;
}
