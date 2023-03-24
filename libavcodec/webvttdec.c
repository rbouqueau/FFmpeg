/*
 * Copyright (c) 2012 Clément Bœsch
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
 * WebVTT subtitle decoder
 * @see http://dev.w3.org/html5/webvtt/
 * @todo need to support extended markups and cue settings
 */

#include "avcodec.h"
#include "ass.h"
#include "codec_internal.h"
#include "libavutil/bprint.h"

#define WEBVTT_TAG_REPLACE_NUM 14
#define WEBVTT_ROOT_TAG "__ffmpeg_root_tag__"

static const struct WebVTTTagReplace {
    const char *from;
    const char *to;
} webvtt_tag_replace_default[WEBVTT_TAG_REPLACE_NUM] = {
    {"<i>", "{\\i1}"}, {"</i>", "{\\i0}"},
    {"<b>", "{\\b1}"}, {"</b>", "{\\b0}"},
    {"<u>", "{\\u1}"}, {"</u>", "{\\u0}"},
    {"{", "\\{"}, {"}", "\\}"}, // escape to avoid ASS markup conflicts
    {"&gt;", ">"}, {"&lt;", "<"},
    {"&lrm;", ""}, {"&rlm;", ""}, // FIXME: properly honor bidi marks
    {"&amp;", "&"}, {"&nbsp;", "\\h"},
};
typedef struct WebVTTTagReplace WebVTTTagReplace;

static void webvtt_tag_replace_free(WebVTTTagReplace *webvtt_tag_replace, int webvtt_tag_replace_num_entries)
{
    if (webvtt_tag_replace != webvtt_tag_replace_default) {
        for (int i=WEBVTT_TAG_REPLACE_NUM; i<webvtt_tag_replace_num_entries; ++i) {
            av_free((void *)webvtt_tag_replace[i].from);
            av_free((void *)webvtt_tag_replace[i].to);
        }

        av_free((void *)webvtt_tag_replace);
    }
}

static WebVTTTagReplace* parse_style(const char *p, WebVTTTagReplace *webvtt_tag_replace, int *webvtt_tag_replace_num_entries)
{
    while (p && *p) {
        char *name = NULL, *color = NULL;
        size_t len = 0;

#define REMOVE_SPACES() while (*p == ' ') p++;
#define SKIP_NEWLINE() { \
            len = strcspn(p, "\r\n"); \
            p += len; \
            if (*p == '\r') \
                p++; \
            if (*p == '\n') \
                p++; \
        }

        //skip header
        SKIP_NEWLINE();
        if (strncmp(p, "::cue", 5)) {
            av_log(NULL, AV_LOG_WARNING, "Stop parsing Style. Expecting \"::cue\", got: \"%s\"\n", p);
            goto exit;
        }

        p += 5;

        REMOVE_SPACES();
        if (*p == '(') {
            p += 1;
            len = strcspn(p, ")");
            if (len < 1 || len > strlen(p)) {
                av_log(NULL, AV_LOG_WARNING, "Stop parsing Style. Missing closing parenthesis, got: \"%s\"\n", p);
                goto exit;
            }

            name = av_strndup(p, len);
            p += len + 1;
        }

        REMOVE_SPACES();
        if (*p == '{') {
            len = strcspn(p, "}");
            if (len < 1 || len > strlen(p)) {
                av_log(NULL, AV_LOG_WARNING, "Stop parsing Style. Missing closing bracket, got: \"%s\"\n", p);
                goto exit;
            }

            p += 1;
            SKIP_NEWLINE();

            while ((len = strcspn(p, "\r\n"))) {
                REMOVE_SPACES();
                if (!strncmp(p, "color:", 6)) {
                    p += 6;
                    REMOVE_SPACES();
                    len = strcspn(p, ";");

                    if (!name)
                        name = av_strdup(WEBVTT_ROOT_TAG);

                    if (webvtt_tag_replace == webvtt_tag_replace_default) {
                        size_t sz = (*webvtt_tag_replace_num_entries + 1) * sizeof(WebVTTTagReplace);
                        webvtt_tag_replace = av_malloc(sz);
                        memcpy(webvtt_tag_replace, webvtt_tag_replace_default, WEBVTT_TAG_REPLACE_NUM * sizeof(WebVTTTagReplace));
                    } else {
                        webvtt_tag_replace = av_realloc(webvtt_tag_replace, (*webvtt_tag_replace_num_entries + 1) * sizeof(WebVTTTagReplace));
                    }

                    p += 1; // '#'
                    len = strcspn(p, ";"); // expecting len=6 for RGB
                    color = av_malloc(14);
                    sprintf(color, "{\\c&H%.6s&}", p);
                    webvtt_tag_replace[*webvtt_tag_replace_num_entries] = (WebVTTTagReplace){name, color};
                    (*webvtt_tag_replace_num_entries)++;
                }
                SKIP_NEWLINE();
            }
            SKIP_NEWLINE();
        } else {
            av_log(NULL, AV_LOG_WARNING, "Stop parsing Style. Missing opening bracket, got: \"%s\"\n", p);
            goto exit;
        }
    }

exit:
    return webvtt_tag_replace;
}

static int webvtt_event_to_ass(AVBPrint *buf, const char *p, const WebVTTTagReplace *webvtt_tag_replace, int webvtt_tag_replace_num_entries)
{
    int i, again = 0, skip = 0;

    for (i = WEBVTT_TAG_REPLACE_NUM; i < webvtt_tag_replace_num_entries; i++)
        if (!strcmp(webvtt_tag_replace[i].from, WEBVTT_ROOT_TAG))
            av_bprintf(buf, "%s", webvtt_tag_replace[i].to);

    while (*p) {
        for (i = 0; i < webvtt_tag_replace_num_entries; i++) {
            const char *from = webvtt_tag_replace[i].from;
            const size_t len = strlen(from);
            if (!strncmp(p, from, len)) {
                av_bprintf(buf, "%s", webvtt_tag_replace[i].to);
                p += len;
                again = 1;
                break;
            }
        }
        if (!*p)
            break;

        if (again) {
            again = 0;
            skip = 0;
            continue;
        }
        if (*p == '<')
            skip = 1;
        else if (*p == '>')
            skip = 0;
        else if (p[0] == '\n' && p[1])
            av_bprintf(buf, "\\N");
        else if (!skip && *p != '\r')
            av_bprint_chars(buf, *p, 1);
        p++;
    }

    for (i = WEBVTT_TAG_REPLACE_NUM; i < webvtt_tag_replace_num_entries; i++)
        if (!strcmp(webvtt_tag_replace[i].from, WEBVTT_ROOT_TAG))
            av_bprintf(buf, "</"WEBVTT_ROOT_TAG">");

    return 0;
}

static int webvtt_decode_frame(AVCodecContext *avctx, AVSubtitle *sub,
                               int *got_sub_ptr, const AVPacket *avpkt)
{
    int ret = 0;
    const char *ptr = avpkt->data;
    FFASSDecoderContext *s = avctx->priv_data;
    AVBPrint buf;

    WebVTTTagReplace *webvtt_tag_replace = (WebVTTTagReplace *)webvtt_tag_replace_default;
    int webvtt_tag_replace_num_entries = WEBVTT_TAG_REPLACE_NUM;
    uint8_t *styling = NULL;
    size_t styling_size = 0;

    styling = av_packet_get_side_data(avpkt, AV_PKT_DATA_WEBVTT_STYLING, &styling_size);

    if (styling_size > INT_MAX)
        return AVERROR(EINVAL);

    if (styling)
        webvtt_tag_replace = parse_style(styling, webvtt_tag_replace, &webvtt_tag_replace_num_entries);

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);
    if (ptr && avpkt->size > 0 && !webvtt_event_to_ass(&buf, ptr, webvtt_tag_replace, webvtt_tag_replace_num_entries))
        ret = ff_ass_add_rect(sub, buf.str, s->readorder++, 0, NULL, NULL);

    av_bprint_finalize(&buf, NULL);
    if (ret < 0)
        return ret;
    webvtt_tag_replace_free(webvtt_tag_replace, webvtt_tag_replace_num_entries);
    *got_sub_ptr = sub->num_rects > 0;
    return avpkt->size;
}

const FFCodec ff_webvtt_decoder = {
    .p.name         = "webvtt",
    CODEC_LONG_NAME("WebVTT subtitle"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_WEBVTT,
    FF_CODEC_DECODE_SUB_CB(webvtt_decode_frame),
    .init           = ff_ass_subtitle_header_default,
    .flush          = ff_ass_decoder_flush,
    .priv_data_size = sizeof(FFASSDecoderContext),
};
