#ifndef FILTER_INCLUDED
#define FILTER_INCLUDED

/*--------------------------------------------------------------------*/

#include <stdint.h>
#include <stdio.h>
#include <util.h>

/*--------------------------------------------------------------------*/

int check_pack_files(char *filename, int64_t mtime, uint8_t *bitmap, char *dir);

int check_loose_files(char *filename, int64_t mtime, uint8_t *bitmap, char *directory);

int *get_4gram_indices(char *string);

struct intarray strings_to_sorted_indices(char **index_strings,
                                          int num_index_strings);

struct intarrayarray strings_to_filter_anded(char **index_strings,
                                        int num_index_strings);

struct intarrayarray strings_to_filter_orred(char **index_strings,
                                        int num_index_strings);

int should_filter_out_file(uint8_t *file_bitmap, struct intarrayarray filter);
/*--------------------------------------------------------------------*/

#endif
