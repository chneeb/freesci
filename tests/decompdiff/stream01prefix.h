/* Rename the second (streaming) copy of decompress01.c so it links next to the
   stock one in the resource library -- as streamprefix.h does for decompress0.c.
   pico_decompress01_stream exists only in this copy and needs no prefix.
   -include'd on the command line. */
#ifndef STREAM01PREFIX_H
#define STREAM01PREFIX_H

#define decompress01      stream_decompress01
#define decrypt3          stream_decrypt3
#define decryptinit3      stream_decryptinit3
#define gbits             stream_gbits
#define decode_rle        stream_decode_rle
#define rle_size          stream_rle_size
#define pic_reorder       stream_pic_reorder
#define view_reorder      stream_view_reorder
#define build_cel_headers stream_build_cel_headers

#endif
