/* Rename the second copy of decompress0.c so both the stock and the streaming
   implementation can be linked into one binary and run against the same bytes.
   Same trick as tests/picodiff/picoprefix.h.

   -include'd on the command line, so it lands before any declaration. */
#ifndef STREAMPREFIX_H
#define STREAMPREFIX_H

#define decompress0  stream_decompress0
#define decrypt1     stream_decrypt1
#define decrypt2     stream_decrypt2
#define getc2        stream_getc2
#define sci0_get_compression_method stream_sci0_get_compression_method

#endif
