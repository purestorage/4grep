#ifndef UTIL_INCLUDED
#define UTIL_INCLUDED

#include <stdint.h>

/*--------------------------------------------------------------------*/

#define NGRAM_CHARS 5
#define NGRAM_CHAR_BITS 4
#define POSSIBLE_NGRAMS ((1u) << (NGRAM_CHARS * NGRAM_CHAR_BITS))
#define SIZEOF_BITMAP (POSSIBLE_NGRAMS / 8)

#define BUFSIZE 2048
#define CHAR_MASK ((1 << NGRAM_CHAR_BITS) - 1)
#define NGRAM_MASK (POSSIBLE_NGRAMS - 1)
#define NGRAM_SHIFT_LEFT_MASK (NGRAM_MASK - CHAR_MASK)
#define HASH_SEED 0xfe5000 //purestorage color

#define GZ_TRUNCATED 1

/*--------------------------------------------------------------------*/

char *add_path_parts(char *dir, char *filename);

char *get_bitmap_store_directory();

int supports_bmi2();

struct intarray {
  int length;
  int *data;
};

void free_intarray(struct intarray arr);

struct intarrayarray {
  int num_rows;
  struct intarray *rows;
};

void free_intarrayarray(struct intarrayarray arr);

void perrorf(char *fmt, ...)
__attribute__((format (printf, 1, 2)));

int64_t get_mtime(char *path);

char *get_lock_path(char *directory, char *filename);

char *get_index_subdirectory(char *indexdir, int64_t timestamp);

int is_dir(char *path);

/*--------------------------------------------------------------------*/

#endif
