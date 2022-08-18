#ifndef AVDEVICE_PACKET_QUEUE_H
#define AVDEVICE_PACKET_QUEUE_H

extern "C" {
#include "libavformat/avformat.h"
}

#include <DeckLinkAPI.h>
#include "decklink_common.h"

typedef struct AVPacketQueue {
    PacketList pkt_list;
    int nb_packets;
    int nb_video_packets;
    unsigned long long size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    AVFormatContext *avctx;
    int64_t max_q_size;
} AVPacketQueue;

void avpacket_queue_init(AVFormatContext *avctx, AVPacketQueue *q);
void avpacket_queue_flush(AVPacketQueue *q);
void avpacket_queue_end(AVPacketQueue *q);
uint64_t avpacket_queue_size(AVPacketQueue *q);
int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt);
int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block);

#endif /* AVDEVICE_PACKET_QUEUE_H */
