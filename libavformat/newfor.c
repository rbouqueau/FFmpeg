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

#define NEWFOR_MAX_PKT_PER_PAGE 7
#define NEWFOR_SAFE(a) { int ret=a; if(ret<0) return ret; }
#define NEWFOR_WAIRFORANSWER 40000 // needed to avoid TCP agregation, despite putting the TCP_NODELAY option

//The reference format is the MPEG2-TS payload format.
const int teletext_pkt_size = 3/*pes fields*/ + 40/*teletext_page_size*/ + 3/*header*/;
const int pes_data_identifier = 0x10;

static uint8_t hamming_8_4_decode(uint8_t a) {
	static const uint8_t hamming_8_4_decode_table[256] = {
		0x01, 0xff, 0x01, 0x01, 0xff, 0x00, 0x01, 0xff, 0xff, 0x02, 0x01, 0xff, 0x0a, 0xff, 0xff, 0x07,
		0xff, 0x00, 0x01, 0xff, 0x00, 0x00, 0xff, 0x00, 0x06, 0xff, 0xff, 0x0b, 0xff, 0x00, 0x03, 0xff,
		0xff, 0x0c, 0x01, 0xff, 0x04, 0xff, 0xff, 0x07, 0x06, 0xff, 0xff, 0x07, 0xff, 0x07, 0x07, 0x07,
		0x06, 0xff, 0xff, 0x05, 0xff, 0x00, 0x0d, 0xff, 0x06, 0x06, 0x06, 0xff, 0x06, 0xff, 0xff, 0x07,
		0xff, 0x02, 0x01, 0xff, 0x04, 0xff, 0xff, 0x09, 0x02, 0x02, 0xff, 0x02, 0xff, 0x02, 0x03, 0xff,
		0x08, 0xff, 0xff, 0x05, 0xff, 0x00, 0x03, 0xff, 0xff, 0x02, 0x03, 0xff, 0x03, 0xff, 0x03, 0x03,
		0x04, 0xff, 0xff, 0x05, 0x04, 0x04, 0x04, 0xff, 0xff, 0x02, 0x0f, 0xff, 0x04, 0xff, 0xff, 0x07,
		0xff, 0x05, 0x05, 0x05, 0x04, 0xff, 0xff, 0x05, 0x06, 0xff, 0xff, 0x05, 0xff, 0x0e, 0x03, 0xff,
		0xff, 0x0c, 0x01, 0xff, 0x0a, 0xff, 0xff, 0x09, 0x0a, 0xff, 0xff, 0x0b, 0x0a, 0x0a, 0x0a, 0xff,
		0x08, 0xff, 0xff, 0x0b, 0xff, 0x00, 0x0d, 0xff, 0xff, 0x0b, 0x0b, 0x0b, 0x0a, 0xff, 0xff, 0x0b,
		0x0c, 0x0c, 0xff, 0x0c, 0xff, 0x0c, 0x0d, 0xff, 0xff, 0x0c, 0x0f, 0xff, 0x0a, 0xff, 0xff, 0x07,
		0xff, 0x0c, 0x0d, 0xff, 0x0d, 0xff, 0x0d, 0x0d, 0x06, 0xff, 0xff, 0x0b, 0xff, 0x0e, 0x0d, 0xff,
		0x08, 0xff, 0xff, 0x09, 0xff, 0x09, 0x09, 0x09, 0xff, 0x02, 0x0f, 0xff, 0x0a, 0xff, 0xff, 0x09,
		0x08, 0x08, 0x08, 0xff, 0x08, 0xff, 0xff, 0x09, 0x08, 0xff, 0xff, 0x0b, 0xff, 0x0e, 0x03, 0xff,
		0xff, 0x0c, 0x0f, 0xff, 0x04, 0xff, 0xff, 0x09, 0x0f, 0xff, 0x0f, 0x0f, 0xff, 0x0e, 0x0f, 0xff,
		0x08, 0xff, 0xff, 0x05, 0xff, 0x0e, 0x0d, 0xff, 0xff, 0x0e, 0x0f, 0xff, 0x0e, 0x0e, 0xff, 0x0e
	};

	uint8_t val = hamming_8_4_decode_table[a];
	if (val == 0xff) {
		val = 0; //error
	}
	return (val & 0x0f);
}

typedef struct NewforContext {
    const AVClass *class;
    URLContext *tcp_conn;
    int page_num;
} NewforContext;

#define OFFSET(x) offsetof(NewforContext, x)
static const AVOption options[] = {
    { NULL }
};

static const AVClass newfor_class = {
    .class_name = "newfor",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static int newfor_open(URLContext *h, const char *uri, int flags)
{
    NewforContext *s = h->priv_data;
    char tcp_uri[1024] = "tcp";
    AVDictionary *opts = NULL;
    int err;

    av_log(h, AV_LOG_TRACE, "newfor open \"%s\"\n", uri);

    snprintf(tcp_uri+3, sizeof(tcp_uri)-3, "%s?tcp_nodelay=1&timeout=%d", uri+6, NEWFOR_WAIRFORANSWER);
    h->flags = AVIO_FLAG_READ_WRITE;
    err = ffurl_open_whitelist(&s->tcp_conn, tcp_uri, h->flags,
                                &h->interrupt_callback, &opts,
                                h->protocol_whitelist, h->protocol_blacklist, h);

    av_dict_free(&opts);

    return err;
}

static int newfor_get_page_num(const uint8_t *buf)
{
    const uint8_t *address_ptr = buf + 2;
    const uint8_t *data = address_ptr + 2;
	uint8_t address = (hamming_8_4_decode(swap_byte(address_ptr[1])) << 4) | hamming_8_4_decode(swap_byte(address_ptr[0]));
	uint8_t m = address & 0x7;
	uint8_t y = (address >> 3) & 0x1f;

	if(m == 0)
		m = 8;

	if(y == 0)
		return (m << 8) | (hamming_8_4_decode(swap_byte(data[1])) << 4) | hamming_8_4_decode(swap_byte(data[0]));
    else
        return 0;
}

static int newfor_get_lang(const uint8_t *buf)
{
    const uint8_t *address_ptr = buf + 2;
    const uint8_t *data = address_ptr + 2;
	uint8_t address = (hamming_8_4_decode(swap_byte(address_ptr[1])) << 4) | hamming_8_4_decode(swap_byte(address_ptr[0]));
	uint8_t y = (address >> 3) & 0x1f;

	if(y == 0) {
		uint8_t val = hamming_8_4_decode(swap_byte(data[7]));
        return ((val & 0x8) >> 3) | ((val & 0x4) >> 1) | ((val & 0x2) << 1);
    } else
        return 0;
}

static int newfor_write_page_off_air_internal(NewforContext *s)
{
    int written;
    const uint8_t off_air[] = { odd_parity_coding(0x18) };

    av_log(s, AV_LOG_TRACE, "newfor write page (off-air)\n");

    written = s->tcp_conn->prot->url_write(s->tcp_conn, off_air, sizeof(off_air));
    if(written != sizeof(off_air)) {
        av_log(s, AV_LOG_ERROR, "Unable to write off-air command\n");
        return AVERROR(EIO);
    }
    if (NEWFOR_WAIRFORANSWER > 0) {
        unsigned char res;
        s->tcp_conn->prot->url_read(s->tcp_conn, &res, 1);
    }

    return 0;
}

static int newfor_connect_internal(NewforContext *s, int page_num)
{
    int written;
    uint8_t page_init[5] = {
        odd_parity_coding(0x0E),
        hamming_8_4_coding(0x00),
        hamming_8_4_coding(page_num / 0x100),         //hundreds
        hamming_8_4_coding((page_num / 0x10) % 0x10), //tens
        hamming_8_4_coding(page_num % 0X10)           //units
    };

    written = s->tcp_conn->prot->url_write(s->tcp_conn, page_init, sizeof(page_init));
    if(written != sizeof(page_init)) {
        av_log(s, AV_LOG_ERROR, "Unable to write page init command (page num=0x%X)\n", page_num);
        return AVERROR(EIO);
    }
    if (NEWFOR_WAIRFORANSWER > 0) {
        unsigned char res;
        s->tcp_conn->prot->url_read(s->tcp_conn, &res, 1);
    }

    return 0;
}

static int newfor_write_page_init(URLContext *h, const uint8_t *buf, int size, int *read)
{
    NewforContext *s = h->priv_data;
    int page_num = 0, lang = 0;
    const unsigned n_pkt = size / teletext_pkt_size;

    av_log(h, AV_LOG_TRACE, "newfor write page (init)\n");

    for(unsigned i=0; i<n_pkt; ++i) {
        if(*(buf + *read) == pes_data_identifier) {
            if (i>0 && page_num)
                break; //new page

            page_num = newfor_get_page_num(buf + *read + 1/*skip data_identifier*/ + 2);

            if(page_num != 0) {
                lang = newfor_get_lang(buf + *read + 1/*skip data_identifier*/ + 2);
                *read += 1; //add data_identifier byte
            } else
                av_log(s, AV_LOG_DEBUG, "page num not located in packet %d/%d\n", i, n_pkt);
        }

        *read += teletext_pkt_size;
    }
    if(!page_num)
        return 0;
    if(page_num == 0x111) //home page //FIXME: find a more reliable way to identify it
        return 0;
    if (s->page_num != page_num) {
        NEWFOR_SAFE(newfor_write_page_off_air_internal(s));
        NEWFOR_SAFE(newfor_connect_internal(s, page_num));
        NEWFOR_SAFE(newfor_connect_internal(s, lang));
        s->page_num = page_num;
    }

    return 0;
}

static int newfor_write_page_on_air(URLContext *h)
{
    NewforContext *s = h->priv_data;
    int written;
    const uint8_t on_air[] = { odd_parity_coding(0x10) };

    av_log(h, AV_LOG_TRACE, "newfor write page (on air)\n");

    written = s->tcp_conn->prot->url_write(s->tcp_conn, on_air, sizeof(on_air));
    if(written != sizeof(on_air)) {
        av_log(s, AV_LOG_ERROR, "Unable to write on-air command\n");
        return AVERROR(EIO);
    }
    if (NEWFOR_WAIRFORANSWER > 0) {
        unsigned char res;
        s->tcp_conn->prot->url_read(s->tcp_conn, &res, 1);
    }

    return 0;
}

static int newfor_write_page_send_data(URLContext *h, const uint8_t *buf, int size, int page_num)
{
    NewforContext *s = h->priv_data;
    int written;
    int read = 0;
    unsigned n = (size - (size % teletext_pkt_size)/*skip header*/) / teletext_pkt_size;
    int row_num = 24 - n * 2;
    uint8_t pages[2/*header*/ + NEWFOR_MAX_PKT_PER_PAGE * (2/*RH RL*/ + 40/*data*/)] = {0};

    av_log(h, AV_LOG_TRACE, "newfor write page (send data)\n");

    if (n > NEWFOR_MAX_PKT_PER_PAGE) {
        av_log(s, AV_LOG_ERROR, "More than %d packets for page 0x%X. Truncating.\n", NEWFOR_MAX_PKT_PER_PAGE, page_num);
        n = NEWFOR_MAX_PKT_PER_PAGE;
    }

    pages[0] = odd_parity_coding(0x0F);
    pages[1] = hamming_8_4_coding(n); //TODO: clear bits = 8? instead of erasing pages, see the other comment below about the optimization

    for(unsigned i=0; i<n; ++i) {
        uint8_t *page = pages + 2 + i * (2 + 40);
        page[0] = hamming_8_4_coding((row_num & 0xF0) >> 4);
        page[1] = hamming_8_4_coding( row_num & 0x0F);
        for (int j=0; j<40; ++j) {
            *(page + 2 + j) = swap_byte(*(buf + 1 + i * teletext_pkt_size + 3 + j));
        }
        if (0) {
            fprintf(stdout, "NEWFOR[%02u]: ", row_num);
            for (uint8_t col = 0; col < 40; col++) fprintf(stdout, "%02x ", *(page + 2 + col));
            fprintf(stdout, "\n");
        }
        row_num++;
    }
    read = 1 + n * teletext_pkt_size;

    written = s->tcp_conn->prot->url_write(s->tcp_conn, pages, 2 + n * (2 + 40));
    if(written != 2 + n * (2 + 40)) {
        av_log(s, AV_LOG_ERROR, "Unable to send subtitle packets\n");
        return AVERROR(EIO);
    }
    if (NEWFOR_WAIRFORANSWER > 0) {
        unsigned char res;
        s->tcp_conn->prot->url_read(s->tcp_conn, &res, 1);
    }

    NEWFOR_SAFE(newfor_write_page_on_air(h));

    return read;
}

static int newfor_write_page(URLContext *h, const uint8_t *buf, int size)
{
    NewforContext *s = h->priv_data;
    int total_read = 0;

    while (total_read < size) {
        int read = 0;
        NEWFOR_SAFE(newfor_write_page_init(h, buf, size - total_read, &read));

        //skip line 0 that signals the page_num
        buf += teletext_pkt_size;
        total_read += teletext_pkt_size;
        read -= teletext_pkt_size;

        if (s->page_num != 0 && read > 0)
            NEWFOR_SAFE(newfor_write_page_send_data(h, buf, read, s->page_num));

        buf += read;
        total_read += read;
    }

    if (total_read != size)
        av_log(h, AV_LOG_WARNING, "write size mismatch: total_read=%d , size=%d\n", total_read, size);

    return total_read;
}

static int newfor_write(URLContext *h, const uint8_t *buf, int size)
{
    int remaining = size;

    // The dataField.data_identifier(8) is sent once per page
    const int num_pages = size % teletext_pkt_size;

    av_log(h, AV_LOG_TRACE, "newfor write\n");

    for(unsigned p=0; p<num_pages; ++p) {
        int read = newfor_write_page(h, buf, remaining);
        buf += read;
        remaining -= read;
    }

    if (remaining != 0)
        av_log(h, AV_LOG_WARNING, "page write size mismatch of %d bytes (out of %d bytes)\n", remaining, size);

    return size - remaining;
}

static int newfor_close(URLContext *h)
{
    NewforContext *s = h->priv_data;
    av_log(h, AV_LOG_TRACE, "newfor close\n");
    newfor_write_page_off_air_internal(s);
    newfor_connect_internal(s, 999);
    return 0;
}

const URLProtocol ff_newfor_protocol = {
    .name                = "newfor",
    .url_open            = newfor_open,
    .url_write           = newfor_write,
    .url_close           = newfor_close,
    .priv_data_size      = sizeof(NewforContext),
    .flags               = URL_PROTOCOL_FLAG_NETWORK,
    .priv_data_class     = &newfor_class,
};
