#include <pthread.h>

#include "packet_queue.h"

void avpacket_queue_init(AVPacketQueue *q, int64_t max_size, AVFormatContext *avctx)
{
    memset(q, 0, sizeof(AVPacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->max_size = max_size;
    q->avctx = avctx;
}

void avpacket_queue_flush(AVPacketQueue *q)
{
    PacketListEntry *pkt, *pkt1;
    pthread_mutex_lock(&q->mutex);
    for (pkt = q->pkt_list.head; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->pkt_list.head = NULL;
    q->pkt_list.tail = NULL;
    q->nb_packets = 0;
    q->size       = 0;
    pthread_mutex_unlock(&q->mutex);
}

void avpacket_queue_end(AVPacketQueue *q)
{
    avpacket_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

uint64_t avpacket_queue_size(AVPacketQueue *q)
{
    unsigned long long size;
    pthread_mutex_lock(&q->mutex);
    size = q->size;
    pthread_mutex_unlock(&q->mutex);
    return size;
}

int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt)
{
    PacketListEntry *pkt1;
    // Drop Packet if queue size is > maximum queue size
    if (avpacket_queue_size(q) > (uint64_t)q->max_size) {
        av_packet_unref(pkt);
        av_log(q->avctx, AV_LOG_WARNING,  "Queue buffer overrun.\n");
        return -1;
    }
    /* ensure the packet is reference counted */
    if (av_packet_make_refcounted(pkt) < 0) {
        av_packet_unref(pkt);
        return -1;
    }
    pkt1 = (PacketListEntry *)av_malloc(sizeof(*pkt1));
    if (!pkt1) {
        av_packet_unref(pkt);
        return -1;
    }
    av_packet_move_ref(&pkt1->pkt, pkt);
    pkt1->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (!q->pkt_list.tail) {
        q->pkt_list.head = pkt1;
    } else {
        q->pkt_list.tail->next = pkt1;
    }
    q->pkt_list.tail = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block)
{
    int ret;
    pthread_mutex_lock(&q->mutex);
    while (1) {
        PacketListEntry *pkt1 = q->pkt_list.head;
        if (pkt1) {
            q->pkt_list.head = pkt1->next;
            if (!q->pkt_list.head) {
                q->pkt_list.tail = NULL;
            }
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            *pkt     = pkt1->pkt;
            av_free(pkt1);
            ret = 0;
            break;
        } else if (!block) {
            ret = 1;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}
