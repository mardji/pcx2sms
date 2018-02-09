/* pcx2sms.c */

/* Compilation instructions:
    $ cc pcx2sms.c -o pcx2sms

   Convert:
    $ ./pcx2sms image.pcx image.raw

   The program prints the width and height of the input image.
*/

#include <stdio.h> /* fread, fwrite */
#include <stdlib.h> /* malloc */
#include <string.h> /* memcpy, memset */

#define FILE_INPUT_BUFFER       0
#define DECODED_SCANLINE_BUFFER 1
#define CONVERTED_OUTPUT_BUFFER 2

struct run_s {
    char ch;
    unsigned int len;
};

struct buffers_s {
    char *buf[3];
    size_t size[3];
    unsigned int pos[3];
    struct run_s run;
};

struct header_info_s {
    unsigned int width;
    unsigned int height;
};

struct conv_s {
    struct buffers_s buffers;
    struct header_info_s header_info;
    FILE *input;
    FILE *output;
};

void read_header(struct conv_s *conv) {
    char *header;

    header = malloc(sizeof(char)*128);
    fread(header, sizeof(char), 128, conv->input);
#define HEADER(x) *((unsigned short *) (header) + (unsigned short) x)
#define HEADER_XMIN HEADER(2)
#define HEADER_YMIN HEADER(3)
#define HEADER_XMAX HEADER(4)
#define HEADER_YMAX HEADER(5)
    conv->header_info.width = HEADER_XMAX - HEADER_XMIN + 1;
    conv->header_info.height = HEADER_YMAX - HEADER_YMIN + 1;
#undef HEADER_XMAX
#undef HEADER_XMIN
#undef HEADER_YMAX
#undef HEADER_YMIN
#undef HEADER
    free(header);

    return;
}

/*
- buf[0] must be a multiple of 8 bytes. This makes things easier on us.
  buf[0] is in the PCX format. Runs are bound by image width.
  (Behavior is undefined if the PCX input file breaks this rule.)
- buf[1] must be a multiple of 8 bytes. Each byte corresponds to a pixel.
  Semantically, buf[1] corresponds to a row of pixels [image width] long.
  (i.e. a single scanline)
  buf[1] is guaranteed to be completely filled, because of the PCX format.
- buf[2] must be a multiple of [image width]*8/2 bytes; [image width] is a
  multiple of 8, therefore buf[2] must be a multiple of 32 bytes. 32 bytes
  in the SMS format correspond to 64 pixels (a 8x8 pixels tile).
  Semantically, buf[2] contains 8 entire scanlines in the SMS format.
  In other words, it contains (exactly!) ([image width]/8) 8x8 pixel tiles.

- The reason size[1] = pos[1] initially, is this is the condition for reading
  in the buffer. We want this to happen immediately at the start and then
  whenever necessary, thereafter. (see next_8_pixels())
*/
void buffers_init(struct conv_s *conv) {
    conv->buffers.buf[FILE_INPUT_BUFFER] = malloc(256);
    conv->buffers.buf[1] = malloc(conv->header_info.width);
    conv->buffers.buf[2] = malloc(conv->header_info.width*8/2);
    conv->buffers.pos[FILE_INPUT_BUFFER] = 0;
    conv->buffers.size[FILE_INPUT_BUFFER] =
        fread(conv->buffers.buf[FILE_INPUT_BUFFER], sizeof(char), 256,
        conv->input);
    conv->buffers.size[1] = conv->buffers.pos[1] = conv->header_info.width;
    conv->buffers.size[2] = conv->buffers.pos[2] = conv->header_info.width*8/2;

    return;
}

void refill_run(struct conv_s *conv) {
    conv->buffers.pos[FILE_INPUT_BUFFER]++;

    if (conv->buffers.pos[FILE_INPUT_BUFFER] ==
        conv->buffers.size[FILE_INPUT_BUFFER])
    {
        conv->buffers.size[FILE_INPUT_BUFFER] =
            fread(
                conv->buffers.buf[FILE_INPUT_BUFFER],
                sizeof(char),
                256,
                conv->input
            );
        conv->buffers.pos[FILE_INPUT_BUFFER] = 0;
    }

    return;
}

// At the start of program: pos[0] == 0.
// At the end of function: ch and len are set appropriately. 1 or 2 characters
// from buf[0] are consumed.
// During function: buffer is filled from file if necessary (see refill_run).
/*
Proof: Assume buf[0] contains at most 1 character.
When it is read from the buffer, pos[0] is incremented by 1: pos[0] = 0 + 1.
Thus, pos[0] == size[0] is true, and the buffer is filled again.
Slightly inelegant: we check to refill the buffer twice (once mandatory, the
second time conditionally).
*/
void next_run(struct conv_s *conv) {
    unsigned char ch;
    unsigned char code;
    unsigned int len;

    ch = *(conv->buffers.buf[0] + conv->buffers.pos[0]);
    len = ((code = (ch & 0xc0)) == 0xc0) ? (ch & 0x3f) : 1;

    refill_run(conv);
    if (code == 0xc0) {
        ch = *(conv->buffers.buf[0] + conv->buffers.pos[0]);
        refill_run(conv);
    }
    conv->buffers.run.ch = ch;
    conv->buffers.run.len = len;

    return;
}

// At the end of function: buf[1] is filled.
void next_8_pixels(struct conv_s *conv) {
#define POS    (conv->buffers.pos[1])
#define SIZE (conv->buffers.size[1])
    /* Condition initially true. Thereafter: depends on input. */
    if (POS == SIZE) {
        POS = 0;
        /* Initially, pos == 0, size == image width in pixels. */
        while (POS < SIZE) {
            /* We set the variables for use in the memset call next stmt. */
            next_run(conv);
            /* If the input file is properly encoded, next_run will never set
            len such that the bounds on buf[1] overflows. */
            memset(conv->buffers.buf[1] + POS, conv->buffers.run.ch, conv->buffers.run.len);
            POS += conv->buffers.run.len;
        }
        POS = 0;
    }
#undef POS

    return;
}

// TODO: PROVE
// At the start of function: buf[1] must be filled to size[1].
// During function: buf[1] is filled to size[1] by call to next_8_pixels().
void next_8_scanlines(struct conv_s *conv) {
    unsigned int row; /* scanline row */
    unsigned int col; /* tile col */
    char tile_row[4];
    int bit;
    int tiles_per_col;

    tiles_per_col = conv->header_info.width/8;

    for (row = 0; row < 8; row++) {
        next_8_pixels(conv); /* fills decoded scanline buffer */
        int ch;
        for (col = 0; col < tiles_per_col; col++) {
            memset(tile_row, 0x00, sizeof(char)*4);
            for (bit = 0; bit < 8; bit++) {
                ch = *(conv->buffers.buf[1] + conv->buffers.pos[1] + bit);
                tile_row[0] += (1 << (7-bit))*((ch & (1 << 0)) >> 0);
                tile_row[1] += (1 << (7-bit))*((ch & (1 << 1)) >> 1);
                tile_row[2] += (1 << (7-bit))*((ch & (1 << 2)) >> 2);
                tile_row[3] += (1 << (7-bit))*((ch & (1 << 3)) >> 3);
            }
            // Write 4 bytes (8 pixels) to buf[2].
            memcpy(conv->buffers.buf[CONVERTED_OUTPUT_BUFFER] + col*32 + row*4,
                tile_row, sizeof(char)*4);
            // We have handled 8 pixels from buf[1]. Mark this.
            conv->buffers.pos[DECODED_SCANLINE_BUFFER] += 8;
        }
    }

    return;
}

int main(int argc, char const *argv[])
{
    if (argc < 3) {
        printf("Insufficient arguments.\n");
        exit(-1);
    }

    struct conv_s conv;
    conv.input = fopen(argv[1], "r");
    conv.output = fopen(argv[2], "w");
    read_header(&conv);

    buffers_init(&conv);

    /* Tiles are 8x8 pixels. Here we calculate the number of tiles in a column.
    Yes, this is a bit of a misnomer. */
    int tile_rows;
    tile_rows = conv.header_info.height/8;

    /* Each iteration, buf[2] should be completely filled. */
    while(tile_rows--) {
        next_8_scanlines(&conv);
        fwrite(conv.buffers.buf[CONVERTED_OUTPUT_BUFFER], sizeof(char),
            conv.buffers.size[CONVERTED_OUTPUT_BUFFER], conv.output);
    }

    return 0;
}
