/* The HAVE_PICO copy of decompress01.c, renamed to link next to the desktop
   one (libscicore) and the tests/decompdiff streaming copy. */
#ifndef DECOMP01PREFIX_H
#define DECOMP01PREFIX_H
#define decompress01              vd_decompress01
#define decrypt3                  vd_decrypt3
#define decryptinit3              vd_decryptinit3
#define gbits                     vd_gbits
#define decode_rle                vd_decode_rle
#define rle_size                  vd_rle_size
#define pic_reorder               vd_pic_reorder
#define view_reorder              vd_view_reorder
#define build_cel_headers         vd_build_cel_headers
#define pico_decompress01_stream  vd_pico_decompress01_stream
#endif
