/* Rename the PSRAM-songs copy of iterator.c so it links next to the desktop
   one in libscisound. -include'd on the command line. */
#ifndef SONGPREFIX_H
#define SONGPREFIX_H
#define new_cleanup_iterator               pico_new_cleanup_iterator
#define new_fast_forward_iterator          pico_new_fast_forward_iterator
#define print_tabs_id                      pico_print_tabs_id
#define _reset_synth_channels              pico__reset_synth_channels
#define sfx_iterator_combine               pico_sfx_iterator_combine
#define songit_clone                       pico_songit_clone
#define song_iterator_add_death_listener   pico_song_iterator_add_death_listener
#define song_iterator_remove_death_listener pico_song_iterator_remove_death_listener
#define songit_free                        pico_songit_free
#define songit_handle_message              pico_songit_handle_message
#define songit_make_message                pico_songit_make_message
#define songit_make_ptr_message            pico_songit_make_ptr_message
#define songit_new                         pico_songit_new
#define songit_new_tee                     pico_songit_new_tee
#define songit_next                        pico_songit_next
#endif
