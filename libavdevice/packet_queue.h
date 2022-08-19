#ifndef AVDEVICE_PACKET_QUEUE_H
#define AVDEVICE_PACKET_QUEUE_H

#include "libavformat/avformat.h"
#include "libavcodec/packet_internal.h"

typedef struct AVPacketQueue {
    PacketList pkt_list;
    int nb_packets;
    int nb_video_packets;
    unsigned long long size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    AVFormatContext *avctx;
    int64_t max_size;
} AVPacketQueue;

void avpacket_queue_init(AVPacketQueue *q, int64_t max_size, AVFormatContext *avctx);
void avpacket_queue_flush(AVPacketQueue *q);
void avpacket_queue_end(AVPacketQueue *q);
uint64_t avpacket_queue_size(AVPacketQueue *q);
int avpacket_queue_put(AVPacketQueue *q, AVPacket *pkt);
int avpacket_queue_get(AVPacketQueue *q, AVPacket *pkt, int block);

#endif /* AVDEVICE_PACKET_QUEUE_H */
