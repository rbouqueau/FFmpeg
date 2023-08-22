/*
 * Teletext subtitle encoder shared functionality
 * Copyright (c) 2023 Motion Spell - Romain Bouqueau
 * based on BSD2-licensed Teletext encoder source code written by:
 *   Copyright (c) 2022 Benjamin Bricard
 *   Copyright (c) 2022 Leandre Moudar
 *   Copyright (c) 2022 Florian Mahieu
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
 * Teletext subtitle encoder
 * @see https://www.etsi.org/deliver/etsi_en/300700_300799/300706/01.02.01_60/en_300706v010201p.pdf
 */

#include "avcodec.h"
#include "codec_internal.h"
#include "put_bits.h"
#include "libavutil/internal.h"
#include "dvbtxt.h"
#include "ass_split.h"

//We use the MPEG2-TS payload format as the reference format.

#define CHARACTER_PER_ROW 40 //Max number of characters per row
#define NB_ROW 25 //Max number of rows
//FIXME: Not enabled by default because FFmpeg doesn't allow subtitles to expose extradata and output several packets from one
//#define ENABLE_HOME_PAGE

#define MAX_PACKETS 3
#define NB_ENHANCEMENT_PACKET MAX_PACKETS

//////////////////////////////////////////////////////////////////////
// Teletext Packet
//////////////////////////////////////////////////////////////////////

/**
 * @struct TeletextPacket
 * @brief Defines a Teletext Packet
 * 43 bytes long
 */
static struct __teletext_packet {
    uint8_t framing_code;   /**Byte 4 | value = 0xE4 */
    uint8_t magazine;       /**Byte 5 | (X/or M/) | Hamming 8/4 coding. Data bits 2, 4 and 6. !! Bit 8 is used by the first bit of the packet number !! */
    uint8_t packet_number;  /**Byte 6 | (Y) | Hamming 8/4 coding. Data bits 2, 4, 6 and 8*/
    uint8_t data_block[40]; /**Depends on data packet type (either Page Header Packet (Y = 0) | Normal Packet (Y = 1 to 25) or Non-Displayable Packet (Y = 26 to 31) )*/
} const TeletextPacket_default = {
    .framing_code   = 0xE4,      
    .magazine       = 0xA8,     //0x00 before hamming 8/4 ==> 0x15 before the swap ==> 0xA8
    .packet_number  = 0xA8      //0x00
};

typedef struct __teletext_packet TeletextPacket;

//Page Header Packet (40 bytes)
//Use these macro to fill the PageHeaderPacket structure (ETSI EN 300 706 => 9.3.1.3 Control bits)
#define CONCAT_BITS_SUBCODE_S2_C4(subcode_S2,control_bit_C4_erase_page) control_bit_C4_erase_page << 3 | subcode_S2
#define CONCAT_BITS_SUBCODE_S4_C5_C6(subcode_S4,control_bit_C5_news_flash,control_bit_C6_subtitle) control_bit_C6_subtitle << 3 | control_bit_C5_news_flash << 2 | subcode_S4
#define CONCAT_BITS_C7_C8_C9_C10(control_bit_C7_suppr_header,control_bit_C8_update_indicator,control_bit_C9_interrupted_sequence,control_bit_C10_inhibit_display) control_bit_C10_inhibit_display << 3 | control_bit_C9_interrupted_sequence << 2 | control_bit_C8_update_indicator << 1 | control_bit_C7_suppr_header
#define CONCAT_BITS_C11_C12_C13_C14(control_bit_C11_magazine_serial,control_bit_C12_national_opt,control_bit_C13_national_opt,control_bit_C14_national_opt)  control_bit_C14_national_opt << 3 |  control_bit_C13_national_opt << 2 |  control_bit_C12_national_opt << 1 | control_bit_C11_magazine_serial

//All fields are Hamming 8/4 coded and bit swapped. Hence they have only 4 bits used to carry data. Before hamming 8/4 coding we can represent those bits as 0x00 to 0x0F.
//The page number 0xFF is transmitted, when no particular page number is specified.
//Page subcode lentght is 13 bits, default value = 0x3F7F , S1 is the least significant and S4 is the most significant part.
static struct __page_header_packet {
    uint8_t page_number_units; 
    uint8_t page_number_tens; 
    uint8_t subcode_S1;
    uint8_t subcode_S2_C4;
    uint8_t subcode_S3;
    uint8_t subcode_S4_C5_C6;
    uint8_t control_bits_C7__C10;       /**Control bits C7 to C10*/
    uint8_t control_bits_C11__C14;      /**Control bits C11 to C14*/
    uint8_t data_bytes[32];             /**Odd parity coded*/
} const PageHeaderPacket_default = {    //Default initialization of the struct
    .page_number_units      = 0x57,     //0x0F before hamming 8/4 ==> 0xEA before the swap ==> 0x57
    .page_number_tens       = 0x57,     //0x0F 
    .subcode_S1             = 0x57,     //0x0F
    .subcode_S2_C4          = 0xE4,     //0x07 before hamming 8/4 ==> 0x27 before the swap ==> 0xE4
    .subcode_S3             = 0x57,     //0x0F
    .subcode_S4_C5_C6       = 0x7A,     //0x03 before hamming 8/4 ==> 0x5E before the swap ==> 0x7A
    .control_bits_C7__C10   = 0xA8,     //0x00 before hamming 8/4 ==> 0x15 before the swap ==> 0xA8
    .control_bits_C11__C14  = 0xA8,     //0x00
};

typedef struct __page_header_packet PageHeaderPacket;

//Normal Packet (40 bytes)
typedef struct {
    uint8_t data_bytes[40];             /**Odd parity coded*/
} NormalPacket;

//Page enhancement data packets (Packets X/26, X/28 and M/29 can carry data to enhance a basic Level 1 Teletext page)
//ETSI EN 300 706 => 9.4
typedef struct { //To be completed
    uint8_t designationCode; //Hamming 8/4
} PageEnhancementDataPacket;

//Page Linking 
//ETSI EN 300 706 => 9.6 Packets for Page Linking 
typedef struct {  //to be completed
    uint8_t designationCode; //Hamming 8/4
    //  data 
} PageLinking;

//Other packet types:
// - Magazine-Related Page Enhancement Data Packets
// - Packets for Page Linking
// - Broadcast Service Data Packets

//Configuration & Customization
/**
 * @enum SpacingAttributes
 * @brief Spacing attributes for configuration & customization
 * ETSI EN 300 706 => 12.2 Spacing attributes
 */
typedef enum {
    SPAC_ATTR_ALPHA_BLACK       = 0x00,
    SPAC_ATTR_ALPHA_RED         = 0x01,
    SPAC_ATTR_ALPHA_GREEN       = 0x02,
    SPAC_ATTR_ALPHA_YELLOW      = 0x03,
    SPAC_ATTR_ALPHA_BLUE        = 0x04,
    SPAC_ATTR_ALPHA_MAGENTA     = 0x05,
    SPAC_ATTR_ALPHA_CYAN        = 0x06,
    SPAC_ATTR_ALPHA_WHITE       = 0x07, /*default color of a character (reset at each new row)*/
    SPAC_ATTR_FLASH             = 0x08,
    SPAC_ATTR_STEADY            = 0x09,
    SPAC_ATTR_END_BOX           = 0x0A, /*End a box of characters*/
    SPAC_ATTR_START_BOX         = 0x0B, /*Start a box of characters, have to doubled to start a box (Protection against false operation)*/
    SPAC_ATTR_NORMAL_SIZE       = 0x0C,
    SPAC_ATTR_DOUBLE_HEIGHT     = 0x0D,
    SPAC_ATTR_DOUBLE_WIDTH      = 0x0E,
    SPAC_ATTR_DOUBLE_SIZE       = 0x0F,
    SPAC_ATTR_MOSAICS_BLACK     = 0x10,
    SPAC_ATTR_MOSAICS_RED       = 0x11,
    SPAC_ATTR_MOSAICS_GREEN     = 0x12,
    SPAC_ATTR_MOSAICS_YELLOW    = 0x13,
    SPAC_ATTR_MOSAICS_BLUE      = 0x14,
    SPAC_ATTR_MOSAICS_MAGENTA   = 0x15,
    SPAC_ATTR_MOSAICS_CYAN      = 0x16,
    SPAC_ATTR_MOSAICS_WHITE     = 0x17,
    SPAC_ATTR_CONCEAL           = 0x18,
    SPAC_ATTR_CONTIGUOUS_MOSAIC_GRAPHICS    = 0x19,
    SPAC_ATTR_SEPARATED_MOSAIC_GRAPHICS     = 0x1A,
    SPAC_ATTR_ESC               = 0x1B,
    SPAC_ATTR_BLACK_BACKGROUND  = 0x1C,
    SPAC_ATTR_NEW_BACKGROUND    = 0x1D,
    SPAC_ATTR_HOLD_MOSAICS      = 0x1E,
    SPAC_ATTR_RELEASE_MOSAICS   = 0x1F,

    SPAC_ATTR_SPACE             = 0x20 /**Not part of 12.2 Spacing attributes*/
} SpacingAttributes;

//Control bits
//ETSI EN 300 706 => 9.3.1.3 Control bits
static struct __control_bits{
    uint8_t/*bool*/ C4_erasePage;
    uint8_t/*bool*/ C5_newFlash;
    uint8_t/*bool*/ C6_subtitle;
    uint8_t/*bool*/ C7_suppressHeader;
    uint8_t/*bool*/ C8_updateIndicator;
    uint8_t/*bool*/ C9_interruptedSequence;
    uint8_t/*bool*/ C10_inhibitDisplay;
    uint8_t/*bool*/ C11_magazineSerial;
    uint8_t/*bool*/ C12_C13_C14_nationalOption[3]; //Defined as {C12,C13,C14}
} const ControlBits_default = {
    .C4_erasePage               = 0,
    .C5_newFlash                = 0,
    .C6_subtitle                = 0,
    .C7_suppressHeader          = 0,
    .C8_updateIndicator         = 0,
    .C9_interruptedSequence     = 0,
    .C10_inhibitDisplay         = 0,
    .C11_magazineSerial         = 0,
    .C12_C13_C14_nationalOption = {0,0,0}
};

typedef struct __control_bits ControlBits;

/**
 * @brief Set the Control Bits 
 * Used to set the contol bits of an input ControlBits struct
 * @param control_bits ControlBits struct
 * @param C4__C11_bits Bits C4 to C11 with C4 the LSb and C11 the MSb of a byte.
 * @param C12__C14bis Bits C12 to C14 with C12 at the index 0 and C14 at the last
 */
static void setControlBits(ControlBits *control_bits, uint8_t C4__C11_bits, uint8_t/*bool*/ C12__C14bits[3]) {
    control_bits->C4_erasePage              = C4__C11_bits & 0x01;
    control_bits->C5_newFlash               = (C4__C11_bits & 0x02) >> 1;
    control_bits->C6_subtitle               = (C4__C11_bits & 0x04) >> 2;
    control_bits->C7_suppressHeader         = (C4__C11_bits & 0x08) >> 3;
    control_bits->C8_updateIndicator        = (C4__C11_bits & 0x10) >> 4;
    control_bits->C9_interruptedSequence    = (C4__C11_bits & 0x20) >> 5;
    control_bits->C10_inhibitDisplay        = (C4__C11_bits & 0x40) >> 6;
    control_bits->C11_magazineSerial        = (C4__C11_bits & 0x80) >> 7;

    control_bits->C12_C13_C14_nationalOption[0] = C12__C14bits[0];
    control_bits->C12_C13_C14_nationalOption[1] = C12__C14bits[1];
    control_bits->C12_C13_C14_nationalOption[2] = C12__C14bits[2];
}

/**
 * @brief Fill a teletext packet with stuffing bytes 0xFF
 * 
 * @param ttxPacket Input Teletext packet
 */
static void ttxPacketStuffing(TeletextPacket *ttxPacket) {
    ttxPacket->framing_code = 0xFF;
    ttxPacket->magazine = 0xFF;
    ttxPacket->packet_number = 0xFF;
    for(int i=0; i<40; i++) {
        ttxPacket->data_block[i] = 0xFF;
    }
}

/**
 * @brief Set the Magazine PacketNumber 
 * This functions facilitate the inserting of magazine and packet number into Teletext structure
 * @param ttxPacket Teletext Packet to be modified
 * @param magazine Magazine number of the Teletext packet (range 0 - 7)
 * @param packetNumber Packet number of the Teletext packet
 */
static void setMagazine_PacketNumber(TeletextPacket *ttxPacket, uint8_t magazine, uint8_t packetNumber) {
    //clear magazine and insert the LSb of the packet Number into the magazine
    magazine = (magazine & 0x07) | (packetNumber & 0x01)<<3;

    //shift packet number and clear
    packetNumber = (packetNumber >> 1) & 0x0F;

    //hamming coding and swap
    ttxPacket->magazine = swap_byte(hamming_8_4_coding(magazine));
    ttxPacket->packet_number = swap_byte(hamming_8_4_coding(packetNumber));   
}

//Latin National Option Sub-sets, ETSI EN 300 706 ==> Table 32 and 15.6.2
static const char *latinNationalOptionSub_set[7][13] = {
    {"£","$","@","←","½","→","↑","#","—","¼","║","¾","÷"},//English
    {"#","$","§","Ä","Ö","Ü","^","_","°","ä","ö","ü","ß"},//German
    {"#","¤","É","Ä","Ö","Å","Ü","_","é","ä","ö","å","ü"},//Swedish/Finnish
    {"£","$","é","°","ç","→","↑","#","ù","à","ò","è","ì"},//Italian
    {"é","ï","à","ë","ê","ù","î","#","è","â","ô","û","ç"},//French
    {"ç","$","i","á","é","í","ó","ú","¿","ü","ñ","è","à"},//Portuguese/Spanish
};

//Latin National Option Sub-sets values
static const uint8_t natoptValues[13] = {
    0x23,0x24,0x40,0x5B,0x5C,0x5D,0x5E,0x5F,0x60,0x7B,0x7C,0x7D,0x7E
};

/**
 * @struct TeletextPage
 * @brief Struct that define a Teletext Page with its various packets
 * A page must have a header packet to be displayed.
 */
typedef struct {
    PageHeaderPacket headerPacket; /**header packet of page*/
    NormalPacket displayablePackets[NB_ROW]; /**displayable packet of packet (contains text)*/
    PageEnhancementDataPacket pageEnhancementPackets[NB_ENHANCEMENT_PACKET]; /**packet used to enhance the page aspect. Can contain X/27, X/28 and M/29 packets in that order*/
    PageLinking linkPage; /**packet used to link other page*/

    uint16_t pageNumber; /**Page number*/
    uint8_t hasHeaderPacket; /**Boolean used to know if there is a page linking.*/
    uint8_t hasDisplayablePacket[NB_ROW]; /**Boolean used to know if a displayable packet is set (true) or not (false). Set to false during the creation of the first header page. Index 0 means rows 1 of a Page and so on.*/
    uint8_t hasPageEnhancementPackets[NB_ENHANCEMENT_PACKET]; /**Boolean used to know if there are page enchancement packets (true) or not (false). Set to false during the creation of the first header page.*/
    uint8_t hasPageLinking; /**Boolean used to know if there is a page linking. Set to false during the creation of the first header page.*/
} TeletextPage;

/////////////////////////////////////////////////////////////////////////////////////////////////////////

//Struct used to manage the display of Teletext page
typedef struct {
    TeletextPage **pages;            //Array of Teletext pages to be displayed
    uint8_t nbPages;                 //Number of pages to be displayed
    uint8_t firstPageTotalPackets;   //Total number of packets to be displayed for the page in first position of the array
    uint8_t firstPageWrittenPackets; //Number of packet already written for the page in first position of the array
} PageWriterManager;

/////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * @struct TeletextDispText
 * @brief Struct that contains formatted subtitle 
 * It contains also various informations for ths display like the line position, the row Span or the number of lines used
 */
typedef struct {
    uint8_t row;   /**range 0 - 24 (means lines 1 to 25 on display because the first line is for the header)*/
    uint8_t rowSpan; /**Row Span*/
    uint8_t nbRowsUsed; /**number of rows used*/
    uint8_t *formattedText; /**Text formatted for teletext, have to free between uses*/
} TeletextDispText;

/**
 * @brief text alignement
 * @enum TextAlign
 */
typedef enum {
  LEFT,
  CENTER,
  RIGHT
} TextAlign;

/**
 * @struct TeletextAspect
 * @brief Struct that defines Teletext style
 * It contains a color, a size, a vertical position and a text alignement
 */
typedef struct {
    SpacingAttributes color;    /**color ot the subtitle (see SpacingAttributes for more details)*/
    SpacingAttributes textSize; /**text size (normal, double height, double width or double size)*/
    float verticalPadding;      /**vertical padding should be between 0 and 1*/
    TextAlign align;            /**text alignement*/
} TeletextAspect;

/////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct {
    AVCodecContext *avctx;
    ASSSplitContext *ass_ctx;
    PageWriterManager pageWRMng;
    PutBitContext pb;

    int home_page_num;
    TeletextPage *home_page;
    int subtitle_page_num;
    TeletextPage *subtitle_page;
    TeletextAspect textAspect;
    ControlBits controlbitSubtitlePage;
    int nb_rows;
} TeletextContext;

/////////////////////////////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Set a header Packet in a Magazine
 * This function will initilize a Page Header Packet with the parameters given
 * ETSI EN 300 706 => 9.3.1 Page header
 * @param ttxPage Structure of a Teletext Page
 * @param pageNumber Number associated to a page. Consits of 3 values : M Pt Pu. Where M = magazine (range 1 - 8), Pt = page number tens (range 0 - F), Pu = page number units (range 0 - F). Note that magazine value of 0 is referred to as belonging to magazine 8
 * @param subcode The subcode is used to define the last part of a page address. Consits of 4 values : S4 S3 S2 S1. Where S1 = LSB digit (range 0 - F), S2 = LSB+1 digit (range 0 - 7), S3 = LSB+2 digit (range 0 - F) , S4 = MSB digit (range 0 - 3).
 * @param control_bits Those bits are used to set the behaviour/parameters of a page and its packets. ETSI EN 300 706 => 9.3.1.3 Control bits 
 * @param dataByte Data of the header row (can be text characters). To be displayed or not (depending on control bits). The last 8 bytes are commonly used to display a real time clock (hh:mm:ss). All bytes have to be initialized, thus fill unused bytes by using spaces
 */
static void setHeaderPacket(TeletextPage *ttxPage, uint16_t pageNumber, uint16_t subcode, ControlBits control_bits, uint8_t dataByte[32]) {
    ttxPage->pageNumber = (pageNumber & 0x07FF); //This mask allows only the necessary data to be retained
    //Page Address & Control Bits field of the header are coded in Hamming8/4 and swapped
    ttxPage->headerPacket.page_number_units = swap_byte(hamming_8_4_coding(pageNumber & 0x000F));
    ttxPage->headerPacket.page_number_tens = swap_byte(hamming_8_4_coding((pageNumber & 0x00F0)>>4));
    ttxPage->headerPacket.subcode_S1 = swap_byte(hamming_8_4_coding(subcode & 0x000F)); //get S1 subcode
    ttxPage->headerPacket.subcode_S2_C4 = swap_byte(hamming_8_4_coding(CONCAT_BITS_SUBCODE_S2_C4((subcode & 0x0070)>>4, control_bits.C4_erasePage)));
    ttxPage->headerPacket.subcode_S3 =  swap_byte(hamming_8_4_coding((subcode & 0x0F00)>>8)); //get S3 subcode
    ttxPage->headerPacket.subcode_S4_C5_C6 = swap_byte(hamming_8_4_coding(CONCAT_BITS_SUBCODE_S4_C5_C6((subcode & 0x3000)>>12, control_bits.C5_newFlash, control_bits.C6_subtitle)));
    ttxPage->headerPacket.control_bits_C7__C10 = swap_byte(hamming_8_4_coding(CONCAT_BITS_C7_C8_C9_C10(control_bits.C7_suppressHeader, control_bits.C8_updateIndicator, control_bits.C9_interruptedSequence, control_bits.C10_inhibitDisplay)));
    ttxPage->headerPacket.control_bits_C11__C14 = swap_byte(hamming_8_4_coding(CONCAT_BITS_C11_C12_C13_C14(control_bits.C11_magazineSerial, control_bits.C12_C13_C14_nationalOption[0], control_bits.C12_C13_C14_nationalOption[1], control_bits.C12_C13_C14_nationalOption[2])));

    //Handling control bits
    if(control_bits.C4_erasePage) { ///reset the displayable packet
        for(uint8_t row=0; row<NB_ROW; row++) {
            ttxPage->hasDisplayablePacket[row] = 0;
        }
    }

    //Data Bytes of the header are coded in odd parity and swapped
    for(uint8_t i=0; i<32; i++) {
        ttxPage->headerPacket.data_bytes[i] = swap_byte(odd_parity_coding(dataByte[i]));
    }

    ttxPage->hasHeaderPacket = 1;  
}

/**
 * @brief Set a Displayable Packet 
 * This function can be used to create or update the data of a displayable packet
 * @param ttxPage Structure of a Teletext Page
 * @param rowNumber Row number of the displayable packet (range 0 - 24, means lines 1 to 25 on display)
 * @param dataByte Data of the header row (can be text characters). All bytes have to be initialized, thus fill unused bytes by using spaces (0x20). Byte will be coded and swapped by the function before saving the results the structure
 * @return true The page is successfully created
 * @return false An error occured during page creation
 */
static uint8_t/*bool*/ setDisplayablePacket(TeletextPage *ttxPage, uint8_t rowNumber, uint8_t dataByte[40]) {
    if(/*rowNumber >= 0 &&*/ rowNumber <= 24 && ttxPage->hasHeaderPacket) {
        //Data Bytes of the header are coded in odd parity and swapped
        for(uint8_t i=0; i<40; i++) {
            ttxPage->displayablePackets[rowNumber].data_bytes[i] = swap_byte(odd_parity_coding(dataByte[i]));
        }

        //set to true the right row
        ttxPage->hasDisplayablePacket[rowNumber] = 1;

        return 1;
    } else {
        return 0;
    } 
}

/**
 * @brief Format the text of a header packet
 * 
 * @param inputText Text to be formatted (max length 32 or 18)
 * @param outputText Return the formatted text
 * @param addDayTime Adds the current date and time at the end of the text, this will erase the last 14 bytes.
 */
static void formatHeaderText(const char *inputText, uint8_t outputText[32]) {
    uint8_t maxTextSize = 32;
    const uint8_t textSize = strlen(inputText);

    for(int i = 0; i<maxTextSize; i++) {
        if(textSize <= i) {
            outputText[i] = SPAC_ATTR_SPACE; //Adding space
        } else {
            outputText[i] = inputText[i]; //Copy
        }
    }
}

/**
 * @brief Detects specials characters in a string and convert it according to the national option
 * 
 * @param inputText Input subtitle string
 * @param C12_C13_C14_nationalOption Control bits for the national option (language)
 * @return char* String with encoded special characters according to the national option
 */
static char *applyNationalOption(TeletextContext *s, const char *inputText, uint16_t inputTextSize, uint8_t/*bool*/ C12_C13_C14_nationalOption[3]) {
    char substr[5];
    uint8_t reduceSize = 0;
    uint8_t/*bool*/ speCharFound = 0;
    uint8_t nationalOptionVal = C12_C13_C14_nationalOption[0] << 2 | C12_C13_C14_nationalOption[1] << 1 | C12_C13_C14_nationalOption[2];
    char *outputText = av_malloc(inputTextSize + 1/*terminal '\0'*/);
    if(!outputText) {
        av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
        return NULL;
    }

    for(uint16_t i = 0; i<inputTextSize; i++) {
        for(uint8_t k = 0; k<13; k++) {//Check if the character is in the latin national option subset table 
            uint8_t speCharSize = strlen(latinNationalOptionSub_set[nationalOptionVal][k]);
            substr[speCharSize] = '\0'; //Reset the length of the character
            strncpy(substr, inputText+i, speCharSize);
            if(!strcmp(latinNationalOptionSub_set[nationalOptionVal][k], substr)) {
                speCharFound = 1; //We found a special character/string to be replaced
                outputText[i-reduceSize] = natoptValues[k];
                reduceSize += speCharSize - 1; //Compute the space gained by replacing this special character
                i += speCharSize - 1 ;

                outputText = av_realloc(outputText, sizeof(char) * ((inputTextSize+1) - reduceSize)); //Reduce the memory size, +1 to get the space to put an \0 at the end
                if(!outputText) {
                    av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
                    return NULL;
                }
            }
        }
        if(!speCharFound) {
            outputText[i-reduceSize] = inputText[i];
        }
        speCharFound = 0;
    }
    outputText[inputTextSize - reduceSize] = '\0';

    return outputText;
}

/**
 * @brief Format one line of Teletext
 * 
 * @param outputText Contains the formatted line of Teletext
 * @param index Index into the input subtitle string (inputText)
 * @param rowCharacterUsage Number of characters used on this Teletext line
 * @param inputText Input subtitle string
 * @param textAspect Struct that defines the Teletext aspect (color, padding)
 * @param startOffset Number of spacings attributes before the text 
 * @param endOffset Number of spacings attributes after the text
 * @param C6_subtitle Control bits that manages the national option (language)
 */
static void insertFormattedSub(TeletextDispText *outputText, int index, uint8_t rowCharacterUsage, char *inputText, TeletextAspect *textAspect, uint8_t startOffset, uint8_t endOffset, uint8_t/*bool*/ C6_subtitle) {
    uint8_t j_buff = 0;
    uint8_t currentRow = outputText->nbRowsUsed - 1;
    uint8_t linebuff[CHARACTER_PER_ROW];

    int paddingLeftOffset = 0;

    //Copy all selected characters into a buffer
    for(int j = index - rowCharacterUsage; j < index; j++) {
        linebuff[j_buff] = inputText[j]; //converting character to hexa //Add national option here to handle conversion
        j_buff++;
    }

    if(textAspect->align == LEFT) {
        for(int fill = startOffset + rowCharacterUsage + CHARACTER_PER_ROW * currentRow + endOffset; fill <CHARACTER_PER_ROW * (currentRow+1); fill++) {
            outputText->formattedText[fill] = SPAC_ATTR_SPACE; //fill with space
        }
    }
    if(textAspect->align == RIGHT) {
        paddingLeftOffset = CHARACTER_PER_ROW - rowCharacterUsage - startOffset - endOffset; //Compute the left offset
        for(int fill = CHARACTER_PER_ROW * currentRow; fill < paddingLeftOffset + CHARACTER_PER_ROW * currentRow; fill++) {
            outputText->formattedText[fill] = SPAC_ATTR_SPACE; //fill with space
        }
    }

    if(textAspect->align == CENTER) {
        paddingLeftOffset = (CHARACTER_PER_ROW - rowCharacterUsage - startOffset - endOffset) / 2;
        //Set the padding by taking into account spacing attribute from start offset and endoffest  but check if the val if negative because it can't be
        paddingLeftOffset = (paddingLeftOffset - (startOffset - endOffset)/2) < 0 ? 0 : (paddingLeftOffset - (startOffset - endOffset)/2);
          
        for(int fill_left = CHARACTER_PER_ROW * currentRow; fill_left < paddingLeftOffset + CHARACTER_PER_ROW * currentRow ;fill_left++) {
            outputText->formattedText[fill_left] = SPAC_ATTR_SPACE; //fill with space
        }
        for(int fill_right = (CHARACTER_PER_ROW * currentRow) + paddingLeftOffset + endOffset + startOffset + rowCharacterUsage; fill_right <((CHARACTER_PER_ROW * (currentRow+1))) ;fill_right++) {
            outputText->formattedText[fill_right] = SPAC_ATTR_SPACE; //fill with space
        }
    }

    //Go in first (affect the whole line)
    if(textAspect->color != SPAC_ATTR_ALPHA_WHITE) {
        outputText->formattedText[paddingLeftOffset + CHARACTER_PER_ROW * currentRow] = textAspect->color;
    }

    if(C6_subtitle) { //Put start and end box
        outputText->formattedText[paddingLeftOffset + startOffset-3 + CHARACTER_PER_ROW * currentRow] = SPAC_ATTR_DOUBLE_HEIGHT;
        outputText->formattedText[paddingLeftOffset + startOffset-2 + CHARACTER_PER_ROW * currentRow] = SPAC_ATTR_START_BOX;
        outputText->formattedText[paddingLeftOffset + startOffset-1 + CHARACTER_PER_ROW * currentRow] = SPAC_ATTR_START_BOX;
        outputText->formattedText[paddingLeftOffset + startOffset + rowCharacterUsage+0 + CHARACTER_PER_ROW * currentRow] = SPAC_ATTR_END_BOX;
    }

    //Write text
    for(int k=0; k<rowCharacterUsage; k++) {
        outputText->formattedText[paddingLeftOffset + startOffset + k + CHARACTER_PER_ROW * currentRow] = linebuff[k];
    }
}

/**
 * @brief Format the text of a displayable packet
 * This function have to modified to take in charge more parameters
 * @param inputText Text to be formatted
 * @param outputText Structure that contains formatted text and display info
 * @param textAspect Aspect of the Teletext subtitles (color, padding)
 * @param C6_subtitle Control bit used when the associated page is for subtitling
 * @param C12_C13_C14_nationalOption Control bits for the national option (language)
 */
static int formatDisplayableText(TeletextContext *s, const char *inputText, uint16_t inputTextSize, TeletextDispText *outputText, TeletextAspect *textAspect, uint8_t/*bool*/ C6_subtitle, uint8_t/*bool*/ C12_C13_C14_nationalOption[3]) {
    char *inputTextNatOpt;
    uint16_t textSize;

    uint8_t startOffset = 0; //Spacing attribute before the text
    uint8_t endOffset = 0; //Spacing attribute after the text

    uint8_t numberSpacingAttrib; //Compute the total number of added spacing attribute
    uint8_t rowCharacterUsage;
    uint8_t lastSpacePos;
    int i;

    outputText->nbRowsUsed = 1;
    //Check the vertical padding
    if(textAspect->verticalPadding > 1.0 || textAspect->verticalPadding < 0.0) {
        textAspect->verticalPadding = 0.0;
    }
    outputText->row = s->nb_rows + (NB_ROW-1) * (textAspect->verticalPadding);
    av_log(s->avctx, AV_LOG_TRACE, "Display line : %d | text : %s  color : %d  padding top %f\n", outputText->row, inputText, textAspect->color, textAspect->verticalPadding);

    outputText->rowSpan = 2; //double height

    //Detects special characters in a string and convert it according to the national option
    inputTextNatOpt = applyNationalOption(s, inputText, inputTextSize, C12_C13_C14_nationalOption);
    if(!inputTextNatOpt)
        return AVERROR(ENOMEM);

    textSize = strlen(inputTextNatOpt); //length of string
   
    //Apply the color for each rows
    if(textAspect->color != SPAC_ATTR_ALPHA_WHITE) {
        startOffset += 1;
    }

    //Add offset due to subtitle box
    if(C6_subtitle) {
        startOffset += 3; //double height + 2x start box
        endOffset += 1; //end box
    }

    numberSpacingAttrib = startOffset + endOffset; //Compute the total number of added spacing attributes
    rowCharacterUsage = 0;
    lastSpacePos = 0;
    outputText->formattedText = av_malloc(CHARACTER_PER_ROW); //Allocate for 1 row
    if(!outputText->formattedText) {
        av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
        return AVERROR(ENOMEM);
    }
    for(i=0; i<textSize; i++) {
        if(inputTextNatOpt[i] == ' ') {//Check for space
            lastSpacePos = rowCharacterUsage; //Save the index of a space
        }

        //Check if the next character will exceed the number of character per row, if it the case go back to the last space to cut the string.
        if((rowCharacterUsage + numberSpacingAttrib + 1) >= CHARACTER_PER_ROW ) {
            i = i +1 - (rowCharacterUsage - lastSpacePos); //set i and keep the space
            rowCharacterUsage = lastSpacePos +1; //rowCharacterUsage and keep the space
            //Format and insert subtitle into the outputText struct
            insertFormattedSub(outputText, i, rowCharacterUsage, inputTextNatOpt, textAspect, startOffset, endOffset, C6_subtitle);
            outputText->nbRowsUsed++;
            //Add a new row
            outputText->formattedText = av_realloc(outputText->formattedText,sizeof(uint8_t) * CHARACTER_PER_ROW * outputText->nbRowsUsed);
            if(!outputText->formattedText) {
                av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
                return AVERROR(ENOMEM);
            }
            rowCharacterUsage = 0;
            i--; //To avoid index problem with the loop for increment
        } else {
            rowCharacterUsage++;
        }

        //The text can't use more than the number max of available rows
        if(outputText->nbRowsUsed >= NB_ROW) {
            break;
        }
    }

    //Format and insert subtitle into the outputText struct
    insertFormattedSub(outputText, i, rowCharacterUsage, inputTextNatOpt, textAspect, startOffset, endOffset, C6_subtitle);

    outputText->row++; //to be in range 1 - NB_ROW(25)

    //Check if all rows can be displayed properly
    if(outputText->row + ((outputText->nbRowsUsed-1) * outputText->rowSpan) > NB_ROW) {
        outputText->row = NB_ROW - ((outputText->nbRowsUsed-1) * outputText->rowSpan);
    }

    //Free input text with national option
    av_free(inputTextNatOpt);

    return 0;
}

static void compute_nb_packet_first_page(PageWriterManager *pageWrMng) {
    pageWrMng->firstPageTotalPackets = 1; //header mandatory
    pageWrMng->firstPageWrittenPackets = 0;

    //Scan for all packets to be displayed
    //Linking page
    if(pageWrMng->pages[0]->hasPageLinking) { 
        pageWrMng->firstPageTotalPackets++;
    }

    //Number of enhancement packets
    for(uint8_t i=0; i<NB_ENHANCEMENT_PACKET; i++) {
        if(pageWrMng->pages[0]->hasPageEnhancementPackets[i]) {
            pageWrMng->firstPageTotalPackets++;
        }
    }

    //Number of displayable packets
    for(uint8_t i=0; i<NB_ROW; i++) {
        if(pageWrMng->pages[0]->hasDisplayablePacket[i]) {
            pageWrMng->firstPageTotalPackets++;

            //FIXME: we truncate after header + 2 lines
            if (pageWrMng->firstPageTotalPackets == MAX_PACKETS)
                for(uint8_t j=i+1; j<NB_ROW; j++)
                    pageWrMng->pages[0]->hasDisplayablePacket[j] = 0;
        }
    }
}

static uint8_t/*bool*/ addPageToWriter(TeletextContext *s, PageWriterManager *pageWrMng, TeletextPage *page) {
    if(!page->hasHeaderPacket) { //mandatory
        return 0;
    } else {
        if(pageWrMng->nbPages == 0) {
            pageWrMng->nbPages++;
            pageWrMng->pages = av_malloc(sizeof(TeletextPage*));
            if(!pageWrMng->pages) {
                av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
                return AVERROR(ENOMEM);
            }
            pageWrMng->pages[0] = av_malloc(sizeof(TeletextPage));
            if(!pageWrMng->pages[0]) {
                av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
                return AVERROR(ENOMEM);
            }
            memcpy(pageWrMng->pages[0], page, sizeof(TeletextPage));

            //Compute the number of packet that will be displayed, for the first page
            compute_nb_packet_first_page(pageWrMng);          
        } else {
            pageWrMng->nbPages++;
            pageWrMng->pages = av_realloc(pageWrMng->pages, sizeof(TeletextPage*) * pageWrMng->nbPages);
            if(!pageWrMng->pages) {
                av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
                return AVERROR(ENOMEM);
            }
            pageWrMng->pages[pageWrMng->nbPages - 1] = av_malloc(sizeof(TeletextPage));
            if(!pageWrMng->pages[pageWrMng->nbPages - 1]) {
                av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
                return AVERROR(ENOMEM);
            }
            memcpy(pageWrMng->pages[pageWrMng->nbPages - 1], page, sizeof(TeletextPage)); //add the page 
        }

        return 1;
    }
}
//Use this macro to fill the line_offset_params field of the PESDataField structure
#define CONCAT_BITS_LINE_OFFSET_PARAM(field_parity,line_offset) (0x3 << 6) | (field_parity << 5) | line_offset

enum DataUnitID {
    DATA_UNIT_EBU_TELETEXT_NON_SUBTITLE = 0x02,
    DATA_UNIT_EBU_TELETEXT_SUBTITLE = 0x03,
    DATA_UNIT_STUFFING = 0xFF
};
//3 teletext packets in one PES Data field
//Length of the data field => 46 bytes * 3 + 1 byte => 139 bytes
//PES data field : 139 bytes
static struct __pes_data_field {
    uint8_t data_identifier;
    uint8_t data_unit_id[MAX_PACKETS];
    uint8_t data_unit_length[MAX_PACKETS];
    uint8_t line_offset_params[MAX_PACKETS]; // (reserved_future_use << 5, field_parity << 4, line_offset ) => 8 bits
    TeletextPacket teletext_packet[MAX_PACKETS];
} const PESDataField_default = {
    .data_identifier = 0x10, 
    .data_unit_id = {DATA_UNIT_STUFFING,DATA_UNIT_STUFFING,DATA_UNIT_STUFFING}, 
    .data_unit_length = {0x2C,0x2C,0x2C},
    .line_offset_params = {CONCAT_BITS_LINE_OFFSET_PARAM(0x1,0x1F),CONCAT_BITS_LINE_OFFSET_PARAM(0x1,0x1F),CONCAT_BITS_LINE_OFFSET_PARAM(0x1,0x1F)},
};
typedef struct __pes_data_field PESDataField;

static int pageWritingManagement(TeletextContext *s, PageWriterManager *pageWrMng, PutBitContext *pb) {
    PESDataField dataField;
    TeletextPacket ttxPacket;

    //init a data field and teletext packet
    dataField = PESDataField_default; //initialize data field structure
    ttxPacket = TeletextPacket_default; //initialize teletext packet structure

    if(pageWrMng->nbPages > 0) { //check if there is a page to write
        if(pageWrMng->firstPageWrittenPackets == 0) { // header to be written
            dataField.data_unit_id[0] = DATA_UNIT_EBU_TELETEXT_SUBTITLE;
            dataField.line_offset_params[0] = CONCAT_BITS_LINE_OFFSET_PARAM(0x1,0xA);

//FIXME: X/0 "acts as both a page identifier and a page terminating packet" so it shall be set last. Not an issue for NewFor validation.
            //fill header packet
            setMagazine_PacketNumber(&ttxPacket, (pageWrMng->pages[0]->pageNumber & 0x0700)>>8, 0);
            memcpy(ttxPacket.data_block, &pageWrMng->pages[0]->headerPacket.page_number_units, 40);
            dataField.teletext_packet[0] = ttxPacket;
            ttxPacketStuffing(&ttxPacket);
            dataField.teletext_packet[1] = ttxPacket;
            dataField.teletext_packet[2] = ttxPacket;
            pageWrMng->firstPageWrittenPackets++;

            put_bits(pb, 8, dataField.data_identifier);
        } else if(pageWrMng->pages[0]->hasPageLinking) {
            //Write linking page
            pageWrMng->pages[0]->hasPageLinking = 0; //clear
            pageWrMng->firstPageWrittenPackets++;
        } else if(pageWrMng->pages[0]->hasPageEnhancementPackets[0] || pageWrMng->pages[0]->hasPageEnhancementPackets[1] || pageWrMng->pages[0]->hasPageEnhancementPackets[2]) {
            //Write enhancement packet
            //set to 0
            pageWrMng->firstPageWrittenPackets++;
        } else { //displayable packets
            //Write displayable
            uint8_t nb_packet = 0;
            for(int nb_disp_pack = 0; nb_disp_pack<NB_ROW; nb_disp_pack++) {
                if(pageWrMng->pages[0]->hasDisplayablePacket[nb_disp_pack]) { //check for displayable packets (in range 0 to 24)                    
                    dataField.data_unit_id[nb_packet] = DATA_UNIT_EBU_TELETEXT_SUBTITLE;
                    dataField.line_offset_params[nb_packet] = CONCAT_BITS_LINE_OFFSET_PARAM(0x1,0xA);

                    //fill display packet
                    setMagazine_PacketNumber(&ttxPacket, (pageWrMng->pages[0]->pageNumber & 0x0700)>>8, nb_disp_pack);
                    memcpy(ttxPacket.data_block, pageWrMng->pages[0]->displayablePackets[nb_disp_pack].data_bytes, 40);
                    dataField.teletext_packet[nb_packet] = ttxPacket;

                    pageWrMng->pages[0]->hasDisplayablePacket[nb_disp_pack] = 0;
                    pageWrMng->firstPageWrittenPackets++;
                    
                    nb_packet++;
                    if(nb_packet == MAX_PACKETS) {//Max number of packets carriable by a data field
                        break;
                    }
                }
            }
            if(nb_packet < MAX_PACKETS) { //Add stuffing byte
                ttxPacketStuffing(&ttxPacket);
                for(int pac = nb_packet; pac<MAX_PACKETS; pac++) {
                    dataField.teletext_packet[pac] = ttxPacket;
                    dataField.data_unit_id[pac] = DATA_UNIT_STUFFING;
                    dataField.data_unit_length[pac] = 0x2C;
                    dataField.line_offset_params[pac] = CONCAT_BITS_LINE_OFFSET_PARAM(0x1,0x1F);
                }
            } 
        }
        
        //Delete the page and move down other pages
        if(pageWrMng->firstPageWrittenPackets >= pageWrMng->firstPageTotalPackets) {
            av_free(pageWrMng->pages[0]); //Free the page
            pageWrMng->nbPages--;
            //Move down
            for(int pageInd = 0; pageInd < pageWrMng->nbPages; pageInd++) {
                pageWrMng->pages[pageInd] = pageWrMng->pages[pageInd + 1];
            }

            if(pageWrMng->nbPages > 0) {
                //Shrink the memory 
                pageWrMng->pages = av_realloc(pageWrMng->pages, sizeof(TeletextPage*) * pageWrMng->nbPages);
                if(!pageWrMng->pages) {
                    av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
                    return AVERROR(ENOMEM);
                }
                compute_nb_packet_first_page(pageWrMng);
            }
            if(pageWrMng->nbPages == 0) { //if there isn't anymore pages
                av_free(pageWrMng->pages);
            }
        }
    } else { 
        return 0; //nothing more to process
    }

    //writing data
    for(int packIndex=0; packIndex<MAX_PACKETS; packIndex++) { //go through the data field
        uint8_t *ptrTtx;

        if (dataField.data_unit_id[packIndex] == DATA_UNIT_STUFFING)
            continue;

        put_bits(pb, 8, dataField.data_unit_id[packIndex]);
        put_bits(pb, 8, dataField.data_unit_length[packIndex]);
        put_bits(pb, 8, dataField.line_offset_params[packIndex]);
        ptrTtx = (uint8_t*)&dataField.teletext_packet[packIndex];
        for(int ttxIndex=0; ttxIndex<43; ttxIndex++) {
            put_bits(pb, 8, *(ptrTtx + ttxIndex));
        }
    }

    return 1;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////

static int color_distance(unsigned a, unsigned b)
{
    unsigned char *color_a = (unsigned char*)&a, *color_b = (unsigned char*)&b;
    return FFABS(color_a[0] - color_b[0]) + FFABS(color_a[1] - color_b[1]) + FFABS(color_a[2] - color_b[2]) + FFABS(color_a[3] - color_b[3]);
}

static void find_closest_color(TeletextAspect *textAspect, unsigned color)
{
    static const struct Color {
        unsigned ass_color;
        SpacingAttributes teletext_color;
    } colors[] = {
        { 0xFFFFFF, SPAC_ATTR_ALPHA_WHITE },
        { 0x000000, SPAC_ATTR_ALPHA_BLACK },
        { 0x0000FF, SPAC_ATTR_ALPHA_RED },
        { 0x00FF00, SPAC_ATTR_ALPHA_GREEN },
        { 0x00FFFF, SPAC_ATTR_ALPHA_YELLOW },
        { 0xFF0000, SPAC_ATTR_ALPHA_BLUE },
        { 0xFF00FF, SPAC_ATTR_ALPHA_MAGENTA },
    };

    int dist = INT_MAX;

    for (int i=0; i<FF_ARRAY_ELEMS(colors); ++i) {
        int local_dist = color_distance(color, colors[i].ass_color);
        if (local_dist < dist) {
            dist = local_dist;
            textAspect->color = colors[i].teletext_color;
        }
    }
}

static void teletext_color_cb(void *priv, unsigned int color, av_unused unsigned int color_id) {
    TeletextContext *s = priv;
    find_closest_color(&s->textAspect, color);
}

static void teletext_sendpage_cb(void *priv) {
    TeletextContext *s = priv;

    //Add the subtitle page to the writer
    addPageToWriter(s, &s->pageWRMng, s->subtitle_page);

    //Add a new page header to display the subtitle page
    if (s->home_page)
        addPageToWriter(s, &s->pageWRMng, s->home_page);
}

static void teletext_addline_cb(void *priv, const char *text, int len) {
    TeletextContext *s = priv;
    TeletextDispText dispTextSubtitlePage = {0}; // dispText.formattedText have to be freed after each uses
    int ret;
    uint8_t/*bool*/ lang[3] = {1,0,0};
    setControlBits(&s->controlbitSubtitlePage, 0xBD/*1011 1101*/, lang);

    ret = formatDisplayableText(s, text, len, &dispTextSubtitlePage, &s->textAspect, s->controlbitSubtitlePage.C6_subtitle, s->controlbitSubtitlePage.C12_C13_C14_nationalOption);
    if(ret < 0) {
        av_log(s->avctx, AV_LOG_ERROR, "Aborting. Error when formatting text: %s.\n", av_err2str(ret));
        return;
    }

    for(uint8_t nb_row = 0; nb_row < dispTextSubtitlePage.nbRowsUsed; nb_row++) {
        uint8_t buff[40];
        memcpy(buff, dispTextSubtitlePage.formattedText+(CHARACTER_PER_ROW * nb_row), CHARACTER_PER_ROW);
        if (!setDisplayablePacket(s->subtitle_page, dispTextSubtitlePage.row - 1 + (dispTextSubtitlePage.rowSpan * nb_row), buff))
            av_log(s->avctx, AV_LOG_WARNING, "Warning: text won't be encoded because it is beyond the displayable area: \"%s\".\n", text);
    }
    s->nb_rows += dispTextSubtitlePage.nbRowsUsed;
    av_free(dispTextSubtitlePage.formattedText);
}

static const ASSCodesCallbacks teletext_callbacks = {
    .text          = teletext_addline_cb,
    .end           = teletext_sendpage_cb,
    .color         = teletext_color_cb,
};

/////////////////////////////////////////////////////////////////////////////////////////////////////////

static int teletext_encode_frame(AVCodecContext *avctx, uint8_t *buf,
                             int bufsize, const AVSubtitle *sub)
{
    TeletextContext *s = avctx->priv_data;
    ASSDialog *dialog;
    int i;

    uint8_t dataHeaderSubtitlePage[32] = {0};
    formatHeaderText("Teletext Page", dataHeaderSubtitlePage);
    setHeaderPacket(s->subtitle_page, s->subtitle_page_num, 0x0000, s->controlbitSubtitlePage, dataHeaderSubtitlePage);

    s->pageWRMng = (PageWriterManager){0};
    s->textAspect.color = SPAC_ATTR_ALPHA_WHITE;
    s->textAspect.align = CENTER;
    s->textAspect.verticalPadding = 0.85;
    s->textAspect.textSize = SPAC_ATTR_NORMAL_SIZE;
    s->controlbitSubtitlePage = ControlBits_default;
    s->nb_rows = 0;

    init_put_bits(&s->pb, buf, bufsize);

    for(i=0; i<sub->num_rects; i++) {
        int ret;
        const char *ass = sub->rects[i]->ass;

        if(sub->rects[i]->type != SUBTITLE_ASS) {
            av_log(avctx, AV_LOG_ERROR, "Only SUBTITLE_ASS type supported.\n");
            return AVERROR(EINVAL);
        }

        dialog = ff_ass_split_dialog(s->ass_ctx, ass);
        if(!dialog)
            return AVERROR(ENOMEM);

        ret = ff_ass_split_override_codes(&teletext_callbacks, s, dialog->text);
        if(ret < 0) {
            int log_level = (ret != AVERROR_INVALIDDATA ||
                            avctx->err_recognition & AV_EF_EXPLODE) ?
                            AV_LOG_ERROR : AV_LOG_WARNING;
            av_log(avctx, log_level,
                "Splitting received ASS dialog text %s failed: %s\n",
                dialog->text,
                av_err2str(ret));

            if(log_level == AV_LOG_ERROR) {
                ff_ass_free_dialog(&dialog);
                return ret;
            }
        }

        //write pages
        while (pageWritingManagement(s, &s->pageWRMng, &s->pb)) {}

        ff_ass_free_dialog(&dialog);
    }

    /* FIXME: set extradata when FFmpeg allows it (needed for MPEG-TS transport) - same for the language
     * This 5-bit field indicates the type of Teletext page indicated. (0x01 Initial Teletext page)
     * teletext_magazine_number: This is a 3-bit field which identifies the magazine number.
     * teletext_page_number: This is an 8-bit field giving two 4-bit hex digits identifying the page number.
     */

    flush_put_bits(&s->pb);
    return put_bytes_output(&s->pb);
}

static av_cold int teletext_encode_close(AVCodecContext *avctx) {
    TeletextContext *s = avctx->priv_data;

    av_free(s->home_page);
    av_free(s->subtitle_page);

    ff_ass_split_free(s->ass_ctx);

    return 0;
}

static av_cold int teletext_encode_init(AVCodecContext *avctx) {
    TeletextContext *s = avctx->priv_data;
    s->avctx = avctx;

    //Home Page
#ifdef ENABLE_HOME_PAGE
    s->home_page_num = 0x111;
    s->home_page = av_calloc(1, sizeof(TeletextPage));
    if(!s->home_page) {
        av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
        return AVERROR(ENOMEM);
    }
#endif

    //Subtitle Page
    s->subtitle_page_num = 0x100;
    s->subtitle_page = av_calloc(1, sizeof(TeletextPage));
    if(!s->subtitle_page) {
        av_log(s->avctx, AV_LOG_ERROR, "Cannot allocate memory.\n");
        return AVERROR(ENOMEM);
    }

    //Compute Home Page static data
    if(s->home_page) {
        int ret;
        const char *header_text = "Teletext";
        const char *subtitle_text = "Teletext Page";
        ControlBits controlbitHomePage = ControlBits_default;
        uint8_t dataHeaderHomePage[32];
        uint8_t byteDispHomePage[40];
        TeletextAspect textAspectHomePage = {SPAC_ATTR_ALPHA_WHITE, SPAC_ATTR_NORMAL_SIZE, 0.5, CENTER};
        TeletextDispText dispTextHomePage = {0};

        controlbitHomePage.C11_magazineSerial = 1;
        controlbitHomePage.C12_C13_C14_nationalOption[0] = 1;

        formatHeaderText(header_text, dataHeaderHomePage);
        setHeaderPacket(s->home_page, s->home_page_num, 0x0000, controlbitHomePage, dataHeaderHomePage);
        ret = formatDisplayableText(s, subtitle_text, strlen(subtitle_text), &dispTextHomePage, &textAspectHomePage, controlbitHomePage.C6_subtitle, controlbitHomePage.C12_C13_C14_nationalOption);
        if(ret < 0)
            return ret;
        memcpy(byteDispHomePage, dispTextHomePage.formattedText, 40);
        setDisplayablePacket(s->home_page, dispTextHomePage.row, byteDispHomePage); //Will use only 1 line

        av_free(dispTextHomePage.formattedText); //Formatted text needs to be freed
    }

    return 0;
}

const FFCodec ff_teletext_encoder = {
    .p.name         = "teletext",
    CODEC_LONG_NAME("Teletext subtitle"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_DVB_TELETEXT,
    .priv_data_size = sizeof(TeletextContext),
    .init           = teletext_encode_init,
    FF_CODEC_ENCODE_SUB_CB(teletext_encode_frame),
    .close          = teletext_encode_close,
    .caps_internal  = FF_CODEC_CAP_INIT_CLEANUP,
};
