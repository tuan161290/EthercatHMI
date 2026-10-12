/*
 * SOEM NIC driver for ESP-IDF esp_eth.
 * Derived from SOEM oshw/rtk/nicdrv.c. Differences from the rt-kernel port:
 *  - single port only (the board has one RJ45); ecx_setupnic() refuses a secondary port;
 *  - mutexes go through the OSAL and are created once;
 *  - ecx_waitinframe() blocks on the RX queue between polls instead of spinning;
 *  - a failed transmit releases the buffer index, as the Linux port does.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief
 * EtherCAT raw frame driver.
 *
 * Low level interface functions to send and receive EtherCAT packets.
 * EtherCAT has the property that packets are only send by the master,
 * and the send packets always return in the receive buffer.
 * There can be multiple packets "on the wire" before they return.
 * To combine the received packets with the original send packets a buffer
 * system is installed. The identifier is put in the index item of the
 * EtherCAT header. The index is stored and compared when a frame is received.
 * If there is a match the packet can be combined with the transmit packet
 * and returned to the higher level function.
 */

#include <string.h>

#include "osal.h"
#include "oshw.h"

#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

/** Redundancy modes */
enum
{
   /** No redundancy, single NIC mode */
   ECT_RED_NONE,
   /** Double redundant NIC connection */
   ECT_RED_DOUBLE
};

/** Primary source MAC address used for EtherCAT.
 * This address is not the MAC address used from the NIC.
 * EtherCAT does not care about MAC addressing, but it is used here to
 * differentiate the route the packet traverses through the EtherCAT
 * segment. This is needed to find out the packet flow in redundant
 * configurations. */
const uint16 priMAC[3] = EC_PRIMARY_MAC_ARRAY;
/** Secondary source MAC address used for EtherCAT. */
const uint16 secMAC[3] = EC_SECONDARY_MAC_ARRAY;

static void ecx_clear_rxbufstat(int *rxbufstat)
{
   int i;
   for (i = 0; i < EC_MAXBUF; i++)
   {
      rxbufstat[i] = EC_BUF_EMPTY;
   }
}

/** Basic setup to connect NIC to the attached esp_eth interface.
 * @param[in] port        = port context struct
 * @param[in] ifname      = ignored, there is only one EMAC
 * @param[in] secondary   = must be 0, redundancy is not supported
 * @return >0 if succeeded
 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
   int i;

   if (secondary)
   {
      return 0;
   }

   if (oshw_mac_init((const uint8_t *)priMAC) != 0)
   {
      return 0;
   }

   port->getindex_mutex = osal_mutex_create();
   port->tx_mutex = osal_mutex_create();
   port->rx_mutex = osal_mutex_create();
   if (!port->getindex_mutex || !port->tx_mutex || !port->rx_mutex)
   {
      ecx_closenic(port);
      return 0;
   }

   port->lastidx = 0;
   port->redstate = ECT_RED_NONE;
   port->redport = NULL;
   port->stack.txbuf = &(port->txbuf);
   port->stack.txbuflength = &(port->txbuflength);
   port->stack.tempbuf = &(port->tempinbuf);
   port->stack.rxbuf = &(port->rxbuf);
   port->stack.rxbufstat = &(port->rxbufstat);
   port->stack.rxsa = &(port->rxsa);
   ecx_clear_rxbufstat(&(port->rxbufstat[0]));

   /* setup ethernet headers in tx buffers so we don't have to repeat it */
   for (i = 0; i < EC_MAXBUF; i++)
   {
      ec_setupheader(&(port->txbuf[i]));
   }
   ec_setupheader(&(port->txbuf2));

   return 1;
}

/** Release port resources. The esp_eth interface stays installed.
 * @param[in] port        = port context struct
 * @return 0
 */
int ecx_closenic(ecx_portt *port)
{
   osal_mutex_destroy(port->getindex_mutex);
   osal_mutex_destroy(port->tx_mutex);
   osal_mutex_destroy(port->rx_mutex);
   port->getindex_mutex = NULL;
   port->tx_mutex = NULL;
   port->rx_mutex = NULL;
   return 0;
}

/** Fill buffer with ethernet header structure.
 * Destination MAC is always broadcast.
 * Ethertype is always ETH_P_ECAT.
 * @param[out] p = buffer
 */
void ec_setupheader(void *p)
{
   ec_etherheadert *bp;
   bp = p;
   bp->da0 = oshw_htons(0xffff);
   bp->da1 = oshw_htons(0xffff);
   bp->da2 = oshw_htons(0xffff);
   bp->sa0 = oshw_htons(priMAC[0]);
   bp->sa1 = oshw_htons(priMAC[1]);
   bp->sa2 = oshw_htons(priMAC[2]);
   bp->etype = oshw_htons(ETH_P_ECAT);
}

/** Get new frame identifier index and allocate corresponding rx buffer.
 * @param[in] port        = port context struct
 * @return new index.
 */
uint8 ecx_getindex(ecx_portt *port)
{
   uint8 idx;
   uint8 cnt;

   osal_mutex_lock(port->getindex_mutex);

   idx = port->lastidx + 1;
   /* index can't be larger than buffer array */
   if (idx >= EC_MAXBUF)
   {
      idx = 0;
   }
   cnt = 0;
   /* try to find unused index */
   while ((port->rxbufstat[idx] != EC_BUF_EMPTY) && (cnt < EC_MAXBUF))
   {
      idx++;
      cnt++;
      if (idx >= EC_MAXBUF)
      {
         idx = 0;
      }
   }
   port->rxbufstat[idx] = EC_BUF_ALLOC;
   port->lastidx = idx;

   osal_mutex_unlock(port->getindex_mutex);

   return idx;
}

/** Set rx buffer status.
 * @param[in] port     = port context struct
 * @param[in] idx      = index in buffer array
 * @param[in] bufstat  = status to set
 */
void ecx_setbufstat(ecx_portt *port, uint8 idx, int bufstat)
{
   port->rxbufstat[idx] = bufstat;
}

/** Transmit buffer (non blocking).
 * @param[in] port        = port context struct
 * @param[in] idx         = index in tx buffer array
 * @param[in] stacknumber = must be 0 (primary)
 * @return number of bytes sent, or -1 on error
 */
int ecx_outframe(ecx_portt *port, uint8 idx, int stacknumber)
{
   int lp, rval;
   ec_stackT *stack;

   if (stacknumber)
   {
      return -1;
   }
   stack = &(port->stack);
   lp = (*stack->txbuflength)[idx];
   (*stack->rxbufstat)[idx] = EC_BUF_TX;
   rval = oshw_mac_send((*stack->txbuf)[idx], lp);
   if (rval < 0)
   {
      (*stack->rxbufstat)[idx] = EC_BUF_EMPTY;
   }

   return rval;
}

/** Transmit buffer (non blocking). Single port: same as ecx_outframe().
 * @param[in] port        = port context struct
 * @param[in] idx = index in tx buffer array
 * @return number of bytes sent, or -1 on error
 */
int ecx_outframe_red(ecx_portt *port, uint8 idx)
{
   ec_etherheadert *ehp;

   ehp = (ec_etherheadert *)&(port->txbuf[idx]);
   /* rewrite MAC source address 1 to primary */
   ehp->sa1 = oshw_htons(priMAC[1]);
   return ecx_outframe(port, idx, 0);
}

/** Non blocking read of the RX queue. Put frame in temporary buffer.
 * @param[in] port        = port context struct
 * @return >0 if frame is available and read
 */
static int ecx_recvpkt(ecx_portt *port)
{
   int bytesrx;

   bytesrx = oshw_mac_recv(*port->stack.tempbuf, sizeof(port->tempinbuf));
   port->tempinbufs = bytesrx;

   return (bytesrx > 0);
}

/** Non blocking receive frame function. Uses RX buffer and index to combine
 * read frame with transmitted frame. To compensate for received frames that
 * are out-of-order all frames are stored in their respective indexed buffer.
 * If a frame was placed in the buffer previously, the function retrieves it
 * from that buffer index without calling ec_recvpkt. If the requested index
 * is not already in the buffer it calls ec_recvpkt to fetch it. There are
 * three options now, 1 no frame read, so exit. 2 frame read but other
 * than requested index, store in buffer and exit. 3 frame read with matching
 * index, store in buffer, set completed flag in buffer status and exit.
 *
 * @param[in] port        = port context struct
 * @param[in] idx         = requested index of frame
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME or EC_OTHERFRAME.
 */
static int ecx_inframe(ecx_portt *port, uint8 idx)
{
   uint16 l;
   int rval;
   uint8 idxf;
   ec_etherheadert *ehp;
   ec_comt *ecp;
   ec_stackT *stack;
   ec_bufT *rxbuf;

   stack = &(port->stack);
   rval = EC_NOFRAME;
   rxbuf = &(*stack->rxbuf)[idx];
   /* check if requested index is already in buffer ? */
   if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
   {
      l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
      /* return WKC */
      rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
      /* mark as completed */
      (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
   }
   else
   {
      osal_mutex_lock(port->rx_mutex);
      /* check again if requested index is already in buffer ?
       * other task might have received it before we grabbed mutex */
      if ((idx < EC_MAXBUF) && ((*stack->rxbufstat)[idx] == EC_BUF_RCVD))
      {
         l = (*rxbuf)[0] + ((uint16)((*rxbuf)[1] & 0x0f) << 8);
         /* return WKC */
         rval = ((*rxbuf)[l] + ((uint16)(*rxbuf)[l + 1] << 8));
         /* mark as completed */
         (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
      }
      /* non blocking call to retrieve frame from the RX queue */
      else if (ecx_recvpkt(port))
      {
         rval = EC_OTHERFRAME;
         ehp = (ec_etherheadert *)(stack->tempbuf);
         /* check if it is an EtherCAT frame */
         if (ehp->etype == oshw_htons(ETH_P_ECAT))
         {
            stack->rxcnt++;
            ecp = (ec_comt *)(&(*stack->tempbuf)[ETH_HEADERSIZE]);
            l = etohs(ecp->elength) & 0x0fff;
            idxf = ecp->index;
            /* found index equals requested index ? */
            if (idxf == idx)
            {
               /* yes, put it in the buffer array (strip ethernet header) */
               memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idx] - ETH_HEADERSIZE);
               /* return WKC */
               rval = ((*rxbuf)[l] + ((uint16)((*rxbuf)[l + 1]) << 8));
               /* mark as completed */
               (*stack->rxbufstat)[idx] = EC_BUF_COMPLETE;
               /* store MAC source word 1 for redundant routing info */
               (*stack->rxsa)[idx] = oshw_ntohs(ehp->sa1);
            }
            else
            {
               /* check if index exist and someone is waiting for it */
               if (idxf < EC_MAXBUF && (*stack->rxbufstat)[idxf] == EC_BUF_TX)
               {
                  rxbuf = &(*stack->rxbuf)[idxf];
                  /* put it in the buffer array (strip ethernet header) */
                  memcpy(rxbuf, &(*stack->tempbuf)[ETH_HEADERSIZE], (*stack->txbuflength)[idxf] - ETH_HEADERSIZE);
                  /* mark as received */
                  (*stack->rxbufstat)[idxf] = EC_BUF_RCVD;
                  (*stack->rxsa)[idxf] = oshw_ntohs(ehp->sa1);
               }
               else
               {
                  /* stale frame (its requester already timed out): drop it */
               }
            }
         }
      }
      osal_mutex_unlock(port->rx_mutex);
   }

   /* WKC if matching frame found */
   return rval;
}

/** Microseconds left on timer, 0 if expired. */
static uint32 timer_remaining_us(osal_timert *timer)
{
   ec_timet now, left;

   osal_get_monotonic_time(&now);
   if (!osal_timespeccmp(&now, &timer->stop_time, <))
   {
      return 0;
   }
   osal_timespecsub(&timer->stop_time, &now, &left);
   return (uint32)(left.tv_sec * 1000000 + left.tv_nsec / 1000);
}

/** Blocking receive frame function.
 *
 * @param[in] port        = port context struct
 * @param[in] idx = requested index of frame
 * @param[in] timer = absolute timeout time
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
static int ecx_waitinframe_red(ecx_portt *port, uint8 idx, osal_timert *timer)
{
   int wkc;
   uint32 left;

   for (;;)
   {
      wkc = ecx_inframe(port, idx);
      if (wkc > EC_NOFRAME)
      {
         break;
      }
      left = timer_remaining_us(timer);
      if (left == 0)
      {
         break;
      }
      /* Sleep until the EMAC delivers something instead of spinning on the RT core. */
      oshw_mac_wait_rx(left);
   }
   /* report other-frame results as "no frame" like the reference ports */
   return (wkc > EC_NOFRAME) ? wkc : EC_NOFRAME;
}

/** Blocking receive frame function. Calls ec_waitinframe_red().
 * @param[in] port        = port context struct
 * @param[in] idx       = requested index of frame
 * @param[in] timeout   = timeout in us
 * @return Workcounter if a frame is found with corresponding index, otherwise
 * EC_NOFRAME.
 */
int ecx_waitinframe(ecx_portt *port, uint8 idx, int timeout)
{
   osal_timert timer;

   osal_timer_start(&timer, timeout);
   return ecx_waitinframe_red(port, idx, &timer);
}

/** Blocking send and receive frame function. Used for non processdata frames.
 * A datagram is build into a frame and transmitted via this function. It waits
 * for an answer and returns the workcounter. The function retries if time is
 * left and the result is WKC=0 or no frame received.
 *
 * The function calls ec_outframe_red() and ec_waitinframe_red().
 *
 * @param[in] port        = port context struct
 * @param[in] idx      = index of frame
 * @param[in] timeout  = timeout in us
 * @return Workcounter or EC_NOFRAME
 */
int ecx_srconfirm(ecx_portt *port, uint8 idx, int timeout)
{
   int wkc = EC_NOFRAME;
   osal_timert timer;

   osal_timer_start(&timer, timeout);
   do
   {
      osal_timert read_timer;

      /* tx frame on primary */
      ecx_outframe_red(port, idx);
      osal_timer_start(&read_timer, MIN(timeout, EC_TIMEOUTRET));
      /* get frame from primary */
      wkc = ecx_waitinframe_red(port, idx, &read_timer);
      /* wait for answer with WKC>0 or otherwise retry until timeout */
   } while ((wkc <= EC_NOFRAME) && (osal_timer_is_expired(&timer) == FALSE));

   return wkc;
}
