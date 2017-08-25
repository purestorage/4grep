#ifndef BITMAP_INCLUDED
#define BITMAP_INCLUDED

/*--------------------------------------------------------------------*/

#include <stdint.h>
#include <stdio.h>

/*--------------------------------------------------------------------*/

uint8_t *init_bitmap();

void set_bit(uint8_t *bitmap, int bit_index);

uint8_t get_bit(uint8_t *bitmap, int bit_index);

void write_bitmap(uint8_t *bitmap, FILE *file);

int get_hash(char *filename, size_t len, char *hash_hex_str);

int decompress_file(uint8_t *decompressed, char *full_path);

int compress_to_fp(uint8_t *bitmap, FILE *fp, char *orig_filename, int64_t mtime);

int compress_to_file(uint8_t *bitmap, char *filename, int64_t mtime, char *indexdir);

int apply_file_to_bitmap(uint8_t *bitmap, FILE *f);

uint8_t *b_or_b(uint8_t *bitmap1, uint8_t *bitmap2);

/*--------------------------------------------------------------------*/

#endif
