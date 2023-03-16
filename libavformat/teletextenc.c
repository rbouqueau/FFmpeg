/*
 * Teletext subtitle muxer
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

/**
 * @file
 * Teletext subtitle muxer
 * @see https://www.etsi.org/deliver/etsi_en/300700_300799/300706/01.02.01_60/en_300706v010201p.pdf
 */

//TODO: transport teletext data here in a canonical format, and move the MPEG-TS code here to prepare for MPEG-TS muxing

#include "avformat.h"
#include "internal.h"

typedef struct TeletextMuxContext {
} TeletextMuxContext;

static int teletext_write_header(AVFormatContext *ctx)
{
    (void)ctx;
    av_log(ctx, AV_LOG_TRACE, "telx write header\n");
    return 0;
}

static int teletext_write_packet(AVFormatContext *ctx, AVPacket *pkt)
{
    AVIOContext *pb = ctx->pb;
    av_log(ctx, AV_LOG_TRACE, "telx write packet\n");
    avio_write(pb, pkt->data, pkt->size);
    return 0;
}

static int teletext_write_trailer(AVFormatContext *ctx)
{
    (void)ctx;
    av_log(ctx, AV_LOG_TRACE, "telx write trailer\n");
    return 1;
}

const AVOutputFormat ff_teletext_muxer = {
    .name              = "teletext",
    .long_name         = NULL_IF_CONFIG_SMALL("Teletext subtitle"),
    .priv_data_size    = sizeof(TeletextMuxContext),
    .flags             = AVFMT_GLOBALHEADER | AVFMT_VARIABLE_FPS |
                         AVFMT_TS_NONSTRICT,
    .subtitle_codec    = AV_CODEC_ID_DVB_TELETEXT,
    .write_header      = teletext_write_header,
    .write_packet      = teletext_write_packet,
    .write_trailer     = teletext_write_trailer,
};
