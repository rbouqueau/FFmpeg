/*
 * Newfor/Nufor/Nu4 protocol
 * Copyright (c) 2023 Motion Spell - Romain Bouqueau
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavcodec/dvbtxt.h"
#include "avformat.h" //av_url_split
#include "url.h"

typedef struct NewforContext {
    const AVClass *class;
    URLContext *tcp_conn;
} NewforContext;

#define OFFSET(x) offsetof(NewforContext, x)
#define D AV_OPT_FLAG_DECODING_PARAM
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { NULL }
};

static const AVClass newfor_class = {
    .class_name = "newfor",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

/* return non zero if error */
static int newfor_open(URLContext *h, const char *uri, int flags)
{
    NewforContext *s = h->priv_data;
    char tcp_uri[1024] = "tcp";
    AVDictionary *opts = NULL;
    int err;

    av_log(h, AV_LOG_TRACE, "newfor open \"%s\"\n", uri);

    strcpy(tcp_uri + 3, uri + 6);
    err = ffurl_open_whitelist(&s->tcp_conn, tcp_uri, h->flags,
                                &h->interrupt_callback, &opts,
                                h->protocol_whitelist, h->protocol_blacklist, h);

    av_dict_free(&opts);

    return err;
}

static int newfor_read(URLContext *h, uint8_t *buf, int size)
{
    //NewforContext *s = h->priv_data;
    av_log(h, AV_LOG_TRACE, "newfor read size %d\n", size);
    return 0;
}

static int newfor_write(URLContext *h, const uint8_t *buf, int size)
{
    NewforContext *s = h->priv_data;
    const int teletext_pkt_size = 40/*teletext_page_size*/ + 3/*header*/;
    const unsigned n = size / teletext_pkt_size;
    uint8_t page_init[5] = { 0x0E, 0x15, 0, 0 , 0 };
    uint8_t pages[2/*header*/ + 7/*@n max val*/ * (2/*RH RL*/ + 40/*data*/)] = {0};
    const uint8_t off_air[] = { 0x18 };
    const uint8_t on_air[] = { 0x10 };
    int written = 0;
    //FIXME: input: WE MISS THE PAGE NUM... IS THIS WHY THE HOME PAGE IS SENT OVER AND OVER?
    //of should we rewrite it here? Is it shared from the encoding?
    const int page_num = 888; //TODO?: pageWrMng->pages[0]->pageNumber & 0x0700)>>8
    int row_num = 1;

    av_log(h, AV_LOG_TRACE, "newfor write off air + data + on air\n");
    av_assert0(size % teletext_pkt_size == 0);
    av_assert0(n <= 7);

    // off air
    written = s->tcp_conn->prot->url_write(s->tcp_conn, off_air, sizeof(off_air));
    if (written != sizeof(off_air)) {
        av_log(s, AV_LOG_ERROR, "Unable to write off-air command\n");
        return AVERROR(EIO);
    }

    //page init
    page_init[2] = hamming_8_4_coding(page_num / 100);       //hundreds
    page_init[3] = hamming_8_4_coding((page_num / 10) % 10); //tens
    page_init[4] = hamming_8_4_coding(page_num % 10);        //units
    written = s->tcp_conn->prot->url_write(s->tcp_conn, page_init, sizeof(page_init));
    if (written != sizeof(page_init)) {
        av_log(s, AV_LOG_ERROR, "Unable to write page init command (page num=%d)\n", page_num);
        return AVERROR(EIO);
    }

    // send data
    pages[0] = 0x0F;
    pages[1] = hamming_8_4_coding(n); //TODO: clear bits = 8?
    for(unsigned i=0; i<n; ++i) {
        uint8_t *page = pages + 2 + i * (2 + 40);
        page[0] = hamming_8_4_coding((row_num & 0xF0) >> 4);
        page[1] = hamming_8_4_coding(row_num & 0x0F);
        memcpy(page + 2, buf + i * teletext_pkt_size + 3, 40);
        row_num++;
    }
    written = s->tcp_conn->prot->url_write(s->tcp_conn, pages, 2 + n * (2 + 40));
    if (written != 2 + n * (2 + 40)) {
        av_log(s, AV_LOG_ERROR, "Unable to send subtitles\n");
        return AVERROR(EIO);
    }

    // on air
    written = s->tcp_conn->prot->url_write(s->tcp_conn, on_air, sizeof(on_air));
    if (written != sizeof(on_air)) {
        av_log(s, AV_LOG_ERROR, "Unable to write on-air command\n");
        return AVERROR(EIO);
    }

    return 0;
}

static int newfor_close(URLContext *h)
{
    //NewforContext *s = h->priv_data;
    av_log(h, AV_LOG_TRACE, "newfor close\n");
    return 0;
}

const URLProtocol ff_newfor_protocol = {
    .name                = "newfor",
    .url_open            = newfor_open,
    .url_read            = newfor_read,
    .url_write           = newfor_write,
    .url_close           = newfor_close,
    .priv_data_size      = sizeof(NewforContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class     = &newfor_class,
};
