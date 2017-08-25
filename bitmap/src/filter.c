#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <immintrin.h>
#include <lockfile.h>
#include <errno.h>

#include "bitmap.h"
#include "filter.h"
#include "packfile.h"
#include "util.h"
#include "xxhash.h"
#include "portable_endian.h"

/*--------------------------------------------------------------------*/

#define BITMAP_CREATED 2

/*--------------------------------------------------------------------*/

/**
 * Checks packfiles for filename.
 *
 * If an entry with the given filename and mtime is found in a packfile, it is
 * applied to the given bitmap.
 */
int check_pack_files(char *filename, int64_t mtime, uint8_t *bitmap,
    char *dir){
  errno = 0;
  uint8_t *read_bitmap = read_from_packfile(filename, mtime, dir);
  if(read_bitmap == NULL) {
    if (errno == ESTALE) {
      // retry once on stale NFS file handle
      read_bitmap = read_from_packfile(filename, mtime, dir);
      if (read_bitmap == NULL) {
        if (errno == ESTALE) {
          perrorf("Error checking packfile for %s", filename);
        }
        return(-1);
      }
    }
    return(-1);
  }
  memcpy(bitmap, read_bitmap, SIZEOF_BITMAP);
  free(read_bitmap);
  return 0;
}

/*--------------------------------------------------------------------*/

/**
 * Checks the loosefiles in the directory to see if the bitmap exists.
 *
 * If an entry with the given filename and mtime is found in a loose file, it
 * is applied to the given bitmap.
 */
int check_loose_files(char *filename, int64_t mtime, uint8_t *bitmap, char *directory){
  int ret_val = -1;
  uint16_t orig_len;
  char hashed_filename[21];
  char tmp[27];
  DIR *dir;
  FILE *possible;

  uint16_t len = strlen(filename);
  get_hash(filename, len, hashed_filename);

  if ((dir = opendir(directory)) == NULL) {
    perrorf("Error opening directory: %s", directory);
    return ret_val;
  }

  int i = 0;
  char *tmp_real_path;

  while(i < 1000){
    sprintf(tmp, "%s_%.3d", hashed_filename, i);
    tmp_real_path = add_path_parts(directory, tmp);

    possible = fopen(tmp_real_path, "r");
    if (possible == NULL) {
      free(tmp_real_path);
      break;
    }

    char *lock_path = get_lock_path(directory, tmp);
    int ret = lockfile_check(lock_path, 0);
    free(lock_path);
    if(ret == 0){
      free(tmp_real_path);
      break;
    }

    if(remove_if_corrupted(possible, tmp_real_path)) {
      i++;
      free(tmp_real_path);
      fclose(possible);
      continue;
    }

    if (fread(&orig_len, 2, 1, possible) != 1) {
      perrorf("Error in reading file size: %s", tmp_real_path);
      goto OUT1;
    }
    orig_len = be16toh(orig_len);
    char orig_filename[orig_len];
    if (fread(orig_filename, orig_len, 1, possible) != 1){
      perrorf("Error in reading filename: %s", tmp_real_path);
      goto OUT1;
      
    }
    if(strncmp(orig_filename, filename, len) != 0){
      goto OUT1;
    }

    int64_t loose_mtime;
    if (fread(&loose_mtime, sizeof(int64_t), 1, possible) != 1) {
      perrorf("Error in reading mtime: %s", tmp_real_path);
      goto OUT1;
    }
    loose_mtime = be64toh(loose_mtime);
    if (loose_mtime != mtime) {
      goto OUT1;
    }

    if(decompress_file(bitmap, tmp_real_path) == 0){
      ret_val = 0;
      goto OUT1;
    }
    
    fclose(possible);
    free(tmp_real_path);
    i++;
  }
  closedir(dir);
  return ret_val;

  OUT1:
    closedir(dir);
    free(tmp_real_path);
    fclose(possible);
    return ret_val;

}

/*--------------------------------------------------------------------*/

/**
 * Scans the file at filename and writes bits for its 4grams to bitmap.
 * Decompresses the file to read it if the file is gzip-compressed.
 *
 * If the bitmap is cached in the index directory, the bitmap is read from the
 * cache and the file at filename is ignored.
 *
 * Returns 0 upon success.
 * Returns GZ_TRUNCATED if the given file was gzip-compressed and the
 * last read ended in the middle of the gzip stream.
 * Returns 3 if the given file does not exist.
 */
int get_bitmap_for_file(uint8_t *bitmap, char *filename, char *indexdir) {
  char *real_path = realpath(filename, NULL);
  if (real_path == NULL) {
    return 3;
  }
  int64_t mtime = get_mtime(real_path);
  char *index_subdir = get_index_subdirectory(indexdir, mtime);
  int ret_val = 0;
  //check loosefiles
  if (check_loose_files(real_path, mtime, bitmap, index_subdir) == 0){
    goto OUT2;
  }
  // not in loosefiles so check packfiles
  if(check_pack_files(real_path, mtime, bitmap, index_subdir) == 0){
    goto OUT2;
  }

  FILE *file = fopen(real_path, "r");
  if (file == NULL) {
    perrorf("Could not open file %s", real_path);
    ret_val = 1;
    goto OUT2;
  }

  int ret = apply_file_to_bitmap(bitmap, file);
  fclose(file);
  if (ret != 0) {
    ret_val = ret;
    goto OUT2;
  }
  compress_to_file(bitmap, real_path, mtime, index_subdir);
  return BITMAP_CREATED;

  OUT2:
    free(real_path);
    return ret_val;
}

/*--------------------------------------------------------------------*/

int *get_4gram_indices_slow(char *string) {
  int len = strlen(string);
  int n = 0;
  if (len <= 0)
    return NULL;

  if (len < NGRAM_CHARS) {
    int *indices = malloc(sizeof(int));
    for (int i = 0; i < len; i++){
      int tmp = string[i] & CHAR_MASK;
      n = ((n << NGRAM_CHAR_BITS) & NGRAM_MASK) + tmp;
    }
    indices[0] = n;
    return indices;
  }
  int *indices = malloc((strlen(string) - NGRAM_CHARS + 1) * sizeof(int));
  for (int i = 0; i < NGRAM_CHARS - 1; i++){
    int tmp = string[i] & CHAR_MASK;
    n = ((n << NGRAM_CHAR_BITS) & NGRAM_MASK) + tmp;
  }
  for (int i = NGRAM_CHARS - 1; i < len; i++) {
    int tmp = string[i] & CHAR_MASK;
    n = ((n << NGRAM_CHAR_BITS) & NGRAM_MASK) + tmp;
    indices[i - NGRAM_CHARS + 1] = n;
  }
  return indices;
}

/*--------------------------------------------------------------------*/

__attribute__ ((target("bmi2")))
int *get_4gram_indices_bmi2(char *string) {
  int len = strlen(string);
  int n = 0;
  if (len <= 0)
    return NULL;
  if (len < NGRAM_CHARS) {
    int *indices = malloc(sizeof(int));
    for (int i = 0; i < len; i++){
      int tmp = string[i] & CHAR_MASK;
      n = ((n << NGRAM_CHAR_BITS) & NGRAM_MASK) + tmp;
    }
    indices[0] = n;
    return indices;
  }
  int *indices = malloc((strlen(string) + 1 - NGRAM_CHARS) * sizeof(int));
  for (int i = 0; i < NGRAM_CHARS - 1; i++){
    int tmp = string[i] & CHAR_MASK;
    n = _pdep_u32(n, NGRAM_SHIFT_LEFT_MASK) + tmp;
  }
  for (int i = NGRAM_CHARS - 1; i < len; i++) {
    int tmp = string[i] & CHAR_MASK;
    n = _pdep_u32(n, NGRAM_SHIFT_LEFT_MASK) + tmp;
    indices[i - NGRAM_CHARS + 1] = n;
  }
  return indices;
}

/*--------------------------------------------------------------------*/

/**
 * Returns an array of ngram indices found in the provided string.
 */
int *get_4gram_indices(char *string) {
  if (supports_bmi2()) {
    return get_4gram_indices_bmi2(string);
  } else {
    return get_4gram_indices_slow(string);
  }
}

/*--------------------------------------------------------------------*/

int compare_ints(const void *a, const void *b) {
  const int *ia = (const int *) a;
  const int *ib = (const int *) b;
  return (*ia > *ib) - (*ia < *ib);
}

/*--------------------------------------------------------------------*/

/**
 * Merges the two sorted arrays of ints arr1 and arr2 and stores the result in
 * result.
 */
void two_finger_merge_int(int *arr1, int arr1size,
                          int *arr2, int arr2size,
                          int *result) {
  int i1 = 0;
  int i2 = 0;
  for (int r = 0; r < arr1size + arr2size; r++) {
    if (i1 < arr1size
        && (i2 >= arr2size || arr1[i1] < arr2[i2])) {
      result[r] = arr1[i1];
      i1++;
    } else {
      result[r] = arr2[i2];
      i2++;
    }
  }
}

/*--------------------------------------------------------------------*/

/**
 * Gets the indices of the grams in the given index string, puts them in an
 * array, sorts them, and returns them.
 */
struct intarray string_to_sorted_indices(char *index_string){
  int len_index_string = strlen(index_string);
  int *index_string_4gram_indices = get_4gram_indices(index_string);
  struct intarray arr = {
    .length = len_index_string - NGRAM_CHARS + 1,
    .data = index_string_4gram_indices,
  };
  qsort(arr.data, arr.length, sizeof(int), compare_ints);
  return arr;
}

/*--------------------------------------------------------------------*/
/**
 * Returns a sorted list of all ngram indices found in the index strings.
 */
struct intarray strings_to_sorted_indices(char **index_strings,
                                          int num_index_strings) {

  struct intarray indices;
  indices = string_to_sorted_indices(index_strings[0]);
  for (int i = 1; i < num_index_strings; i++) {
    struct intarray old_indices = indices;
    struct intarray new_indices;
    new_indices = string_to_sorted_indices(index_strings[i]);
    indices.length = old_indices.length + new_indices.length;
    indices.data = malloc(indices.length * sizeof(int));
    two_finger_merge_int(old_indices.data, old_indices.length,
        new_indices.data, new_indices.length, indices.data);
    free_intarray(old_indices);
    free_intarray(new_indices);
  }
  return indices;
}

/**
 * Returns 1 if file_bitmap does not match filter.
 *
 * filter is a 'sum of products' array of arrays of ngram indices. The indices
 * in each subarray are anded together, and each subarray is orred together.
 * Put another way, we filter out files that don't contain all the ngrams in at
 * least one subarray.
 *
 */
int should_filter_out_file(uint8_t *file_bitmap, struct intarrayarray filter) {
  int contained = 0;
  for (int i = 0; i < filter.num_rows; i++) {
    int ngrams_in_subarray_all_present = 1;
    for (int j = 0; j < filter.rows[i].length; j++) {
      if (!get_bit(file_bitmap, filter.rows[i].data[j])) {
        ngrams_in_subarray_all_present = 0;
        break;
      }
    }
    if (ngrams_in_subarray_all_present) {
      contained = 1;
      break;
    }
  }
  return !contained;
}

/*--------------------------------------------------------------------*/
/**
 * Function that is called by 4grep to start filtering using search strings
 *
 * See should_filter_out_file for details on ngram_filter.
 *
 * Returns -1 upon failure, 1 if bitmap is found and indices
 * match, 2 if bitmap found but does not match, 3 if no bitmap found and
 * matches, 4 if did not have bitmap and has no match.
 */
int start_filter(struct intarrayarray ngram_filter,
                 char *filename, char *indexdir){

  int ret = -1, MTCH = 1, NO_MTCH = 2;
  mode_t old_umask = umask(0);

  // now start filtering files
  uint8_t *file_bitmap = init_bitmap();

  int bitmap_ret = get_bitmap_for_file(file_bitmap, filename,
                                       indexdir);
  if (bitmap_ret != 0 && bitmap_ret != 2) {
    goto OUT1;
  }

  int filtered = should_filter_out_file(file_bitmap, ngram_filter);

  if (!filtered)
    ret = MTCH;
  else
    ret = NO_MTCH;

  if (bitmap_ret == BITMAP_CREATED)
    ret += BITMAP_CREATED;

  OUT1:
    free(file_bitmap);
    umask(old_umask);
    return ret;
}
