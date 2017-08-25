#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <immintrin.h>
#include <zstd.h>
#include <zlib.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <lockfile.h>

#include "bitmap.h"
#include "xxhash.h"
#include "util.h"
#include "portable_endian.h"

/*--------------------------------------------------------------------*/

#define ESTIMATED_ZSTD_SIZE (ZSTD_compressBound(SIZEOF_BITMAP))

/*--------------------------------------------------------------------*/

/**
 * Initalizes memory for bitmap
 */
uint8_t *init_bitmap(){
  uint8_t *bitmap = calloc(SIZEOF_BITMAP, 1);
  if (bitmap == NULL){
  	perror("Error: Bitmap not initialized");
    return(NULL);
  }
  return bitmap;
}

/*--------------------------------------------------------------------*/

void set_bit(uint8_t *bitmap, int bit_index){
  bitmap[bit_index / 8] |= (1 << bit_index % 8);
}

/*--------------------------------------------------------------------*/

uint8_t get_bit(uint8_t *bitmap, int bit_index) {
  return (bitmap[bit_index / 8] >> (bit_index % 8)) & 1;
}

/*--------------------------------------------------------------------*/

void write_bitmap(uint8_t *bitmap, FILE *file){
  fwrite(bitmap, 1, SIZEOF_BITMAP, file);
}

/*--------------------------------------------------------------------*/

char *get_lock_path(char *directory, char *filename) {
  char lock_filename[PATH_MAX];
  sprintf(lock_filename, ".%s.lock", filename);
  return add_path_parts(directory, lock_filename);
}

/*--------------------------------------------------------------------*/

/**
 * Finds the first path of the form "directory/filename_XXX" that doesn't
 * already exist, counting up from 000 to 999.
 *
 * When found, it creates a new file with mode 0666 and returns a file
 * descriptor.
 */
int available_name(char *filename, char *directory){
  char tmp[21];
  int i = 0;
  //Max hash collision will be _999
  while(i < 1000){
    sprintf(tmp, "%s_%.3d", filename, i);
    char *full_path = add_path_parts(directory, tmp);
    int fd = open(full_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    free(full_path);
    if(fd != -1) {//exists
      char *lock_path = get_lock_path(directory, tmp);
      int a = lockfile_create(lock_path, 0, 0);
      free(lock_path);
      if(a != 0){
        i++;
        continue;
      }
      strcpy(filename, tmp);
      return fd;
    }
    i++;
  }
  return(-1);
}

/*--------------------------------------------------------------------*/

/**
 * Fucnction gets the xxhash of the filename and stores it
 * in hash_hex_str
 */
int get_hash(char *filename, size_t len, char *hash_hex_str){
  XXH64_canonical_t* dst = malloc(sizeof(XXH64_canonical_t));
  if (dst == NULL){
    perror("Error: Memory not allocated");
    return(-1);
  }
  uint64_t hashed = XXH64(filename, len, HASH_SEED);
  XXH64_canonicalFromHash(dst, hashed);
  for(int i = 0; i < 8; i++){
    sprintf((hash_hex_str+2*i), "%02X", dst->digest[i]);
  }
  free(dst);
  return 0;
}

/*--------------------------------------------------------------------*/

/**
 * Function will write the bitmap that has been compressed in filename to
 * decompressed.
 * Saved data comprises of length of filename, filename, compressed data,
 * decompressed size.
 */
int decompress_file(uint8_t *decompressed, char *full_path){
  uint16_t len;
  uint32_t compressed_size;
  int ret_val = -1;
  FILE *f = fopen(full_path, "r");
  if(f == NULL) {
    if (errno != ENOENT) {
      perrorf("Error: File not opened: %s", full_path);
    }
    return ret_val;
  }
  if (fread(&len, sizeof(uint16_t), 1, f) != 1) {
    perrorf("Error in reading file size: %s", full_path);
    fclose(f);
    return ret_val;
  }
  len = be16toh(len);
  char orig_filename[len+1];
  if (fread(orig_filename, len, 1, f) != 1){
    perrorf("Error in reading filename: %s", full_path);
    fclose(f);
    return ret_val;
  }
  int64_t mtime;
  if (fread(&mtime, sizeof(int64_t), 1, f) != 1) {
    perrorf("Error in reading mtime: %s", full_path);
    fclose(f);
    return ret_val;
  }
  mtime = be64toh(mtime);
  if (fread(&compressed_size, sizeof(uint32_t), 1, f) != 1){
    perrorf("Error in reading decompressed size: %s", full_path);
    fclose(f);
    return ret_val;
  }
  compressed_size = be32toh(compressed_size);
  char stream[compressed_size+1];
  if (fread(&stream, compressed_size, 1, f) != 1){
    perrorf("Error in reading decompressed file: %s", full_path);
    goto OUT1;
  }
  size_t decompressed_size = ZSTD_decompress(decompressed, SIZEOF_BITMAP,
                                             stream,compressed_size);
  if(ZSTD_isError(decompressed_size) == 1){
    perrorf("Error in decompression of %s: %s",
            full_path, ZSTD_getErrorName(decompressed_size));
  	goto OUT1;
  }
  ret_val = 0;
  goto OUT1;

  OUT1:
    fclose(f);
    return ret_val;
}

/*--------------------------------------------------------------------*/

/**
 * Compresses the bitmap into the file described by fp using ZSTD
 * The original filename's length is stored followed by the filename, followed
 * by the compressed size, followed by the actual compressed data.
 */
int compress_to_fp(uint8_t *bitmap, FILE *fp, char *orig_filename,
    int64_t mtime) {
  uint16_t len = strlen(orig_filename);
  void* compressed = malloc(ESTIMATED_ZSTD_SIZE);
  int ret_val = -1;
  if (compressed == NULL){
  	perror("Error: Memory not allocated");
    return ret_val;
  }

  uint32_t compressed_size = ZSTD_compress(compressed, ESTIMATED_ZSTD_SIZE,
                                           bitmap, SIZEOF_BITMAP, 8);

  if(ZSTD_isError(compressed_size) == 1) {
  	perror("Error in compression");
    goto OUT2;
  }
  uint16_t len_be = htobe16(len);
  if (fwrite(&len_be, sizeof(uint16_t), 1, fp) != 1){
    goto OUT2;
  }
  if (fwrite(orig_filename, len, 1, fp) != 1){
    perror("Error: Filename not written");
    goto OUT2;
  }
  int64_t mtime_be = htobe64(mtime);
  if (fwrite(&mtime_be, sizeof(int64_t), 1, fp) != 1){
    perror("Error: mtime not written");
    goto OUT2;
  }
  uint32_t compressed_size_be = htobe32(compressed_size);
  if (fwrite(&compressed_size_be, sizeof(uint32_t), 1, fp) != 1){
    perror("Error: Compressed size not written");
    goto OUT2;
  }
  if (fwrite(compressed, compressed_size, 1, fp) != 1){
    perror("Error: Compressed file not written");
    goto OUT2;
  }
  ret_val = 0;
  goto OUT2;

  OUT2:
    free(compressed);
    return ret_val;
}

/*--------------------------------------------------------------------*/

/**
 * Function will compress the bitmap into a loosefile which is
 * named after the filename's hash and number of occurences.
 */
int compress_to_file(uint8_t *bitmap, char *filename, int64_t mtime,
    char *indexdir) {
  char hashed_filename[21], lock[27];
  uint16_t len = strlen(filename);
  get_hash(filename, len, hashed_filename);
  int fd = available_name(hashed_filename, indexdir);
  FILE *fp = fdopen(fd, "wb");
  if(fp == NULL) {
    perrorf("Error: File not opened: %s", hashed_filename);
    return(-1);
  }
  int ret = compress_to_fp(bitmap, fp, filename, mtime);
  fflush(fp);
  fsync(fd);
  fclose(fp);
  sprintf(lock, ".%s.lock", hashed_filename);
  char *lock_path = add_path_parts(indexdir, lock);
  lockfile_remove(lock_path);
  free(lock_path);
  return ret;
}

/*--------------------------------------------------------------------*/

__attribute__ ((target("bmi2")))
int init_4gram_state_bmi2(char *text) {
  int n = 0;
  for (int i = 0; i < NGRAM_CHARS; i++){
    int tmp = text[i] & CHAR_MASK;
    n = _pdep_u32(n, NGRAM_SHIFT_LEFT_MASK) + tmp;
  }
  return n;
}

/*--------------------------------------------------------------------*/

int init_4gram_state_slow(char *text) {
  int n = 0;
  for (int i = 0; i < NGRAM_CHARS; i++){
    int tmp = text[i] & CHAR_MASK;
    n = ((n << NGRAM_CHAR_BITS) & NGRAM_MASK) + tmp;
  }
  return n;
}

/*--------------------------------------------------------------------*/

/**
 * Returns the ngram index of the first ngram in text.
 */
int init_4gram_state(char *text) {
  if (supports_bmi2()) {
    return init_4gram_state_bmi2(text);
  } else {
    return init_4gram_state_slow(text);
  }
}

/*--------------------------------------------------------------------*/

__attribute__ ((target("bmi2")))
int apply_to_bitmap_bmi2(uint8_t *bitmap, char *buf, int len, int n) {
  for (int i = 0; i < len / sizeof(char); i++) {
    int tmp = buf[i] & CHAR_MASK;
    n = _pdep_u32(n, NGRAM_SHIFT_LEFT_MASK) + tmp;
    set_bit(bitmap, n);
  }
  return n;
}

/*--------------------------------------------------------------------*/

int apply_to_bitmap_slow(uint8_t *bitmap, char *buf, int len, int n) {
  for (int i = 0; i < len / sizeof(char); i++) {
    int tmp = buf[i] & CHAR_MASK;
    n = ((n << NGRAM_CHAR_BITS) & NGRAM_MASK) + tmp;
    set_bit(bitmap, n);
  }
  return n;
}

/*--------------------------------------------------------------------*/
/**
 * Applies all of the ngrams in buf to bitmap.
 *
 * Checks to see if system supports bmi2 instructions and calls relevant
 * functions
 */
int apply_to_bitmap(uint8_t *bitmap, char *buf, int len, int n) {
  if (supports_bmi2()) {
    return apply_to_bitmap_bmi2(bitmap, buf, len, n);
  } else {
    return apply_to_bitmap_slow(bitmap, buf, len, n);
  }
}

/*--------------------------------------------------------------------*/
/**
 * Scans the file at filename and writes bits for its 4grams to bitmap.
 * Decompresses the file to read it if the file is gzip-compressed.
 * Returns GZ_TRUNCATED if the given file was gzip-compressed and the
 * last read ended in the middle of the gzip stream.
 */
int apply_file_to_bitmap(uint8_t *bitmap, FILE *f){
  int n = 0;
  int ret_val = -1;
  int fd = fileno(f);
  char buf[BUFSIZE];
  int read_amount;

  // open file with a dup fd so closing gzf doesn't close the file descriptor
  int dup_fd = dup(fd);
  if (dup_fd < 0) {
    perror("Error duplicating fd");
    return ret_val;
  }
  gzFile gzf = gzdopen(dup_fd, "r");
  if (gzf == NULL) {
    perror("Error opening gzip stream");
    goto OUT1;
  }

  // read first four characters to initialize 4gram
  read_amount = gzread(gzf, buf, NGRAM_CHARS * sizeof(char));
  if (read_amount < 0) {
    fprintf(stderr, "gzread error: %s\n", gzerror(gzf, &read_amount));
    gzclose(gzf);
    goto OUT1;
  }
  if (read_amount == NGRAM_CHARS) {
    n = init_4gram_state(buf);
    set_bit(bitmap, n);
  }

  // read rest of file
  do {
    read_amount = gzread(gzf, buf, BUFSIZE);
    if (read_amount < 0) {
      fprintf(stderr, "gzread error: %s\n", gzerror(gzf, &read_amount));
      gzclose(gzf);
      goto OUT1;
    }
    n = apply_to_bitmap(bitmap, buf, read_amount, n);
  } while (read_amount > 0 || (read_amount < 0 && errno == EINTR));

  int gzclose_ret = gzclose(gzf);
  if (gzclose_ret == Z_BUF_ERROR) {
    return GZ_TRUNCATED;
  } else if (gzclose_ret != Z_OK) {
    perror("Error closing .gz file");
    goto OUT1;
  }
  ret_val = 0;
  goto OUT1;

  OUT1:
    close(dup_fd);
    return ret_val;
}

/*--------------------------------------------------------------------*/

uint8_t *b_or_b(uint8_t *bitmap1, uint8_t *bitmap2){
  uint8_t *b1_or_b2 = init_bitmap();
  for (int i = 0 ; i < SIZEOF_BITMAP; i++) {
    uint8_t b1 = get_bit(bitmap1, i);
    uint8_t b2 = get_bit(bitmap2, i);
    set_bit(b1_or_b2, (b1 | b2));
  }
  return b1_or_b2;
}

/*--------------------------------------------------------------------*/





