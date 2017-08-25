#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <zstd.h>
#include <dirent.h>
#include <lockfile.h>

#include "../lib/minunit.h"
#include "../src/filter.h"
#include "../src/bitmap.h"
#include "../src/util.h"
#include "../src/packfile.h"
#include "portable_endian.h"

/*--------------------------------------------------------------------*/

int tests_run = 0;

/*--------------------------------------------------------------------*/

/*
 * Returns a file descriptor pointing towards a pipe containing solely the
 * passed-in string.
 *
 * Works by forking the current process and writing the string to a pipe in the
 * child process.
 */
FILE *get_pipe(char *string) {
  int p[2];
  if (pipe(p) != 0) {
    perror("Error: pipe failed");
    exit(-1);
  }
  if (!fork()) {
    int len = strlen(string);
    for (int written = 0; len > written;) {
      int result = write(p[1], string + written, len - written);
      if (result < 0) {
        perror("Error converting string to pipe\n");
        exit(1);
      }
      written += result;
    }
    exit(0);
  }

  close(p[1]);
  FILE *f = fdopen(p[0], "r");
  return f;
}

/*--------------------------------------------------------------------*/
/**
 * Sets the bits in the bitmap for all the 4grams stored in the string.
 */
void apply_string_to_bitmap(uint8_t *bitmap, char *string) {
  FILE *file = get_pipe(string);
  apply_file_to_bitmap(bitmap, file);
  fclose(file);
}

int bitmaps_are_the_same(uint8_t *bitmap1, uint8_t *bitmap2) {
  for (int i = 0; i < SIZEOF_BITMAP; i++) {
    if (bitmap1[i] != bitmap2[i]) {
      return 0;
    }
  }
  return 1;
}

/*--------------------------------------------------------------------*/

static char *test_init_bitmap() {
  uint8_t *bitmap = init_bitmap();
  for (size_t i = 0; i < SIZEOF_BITMAP; i++) {
    mu_assert("Initialized bitmap has nonzero byte", bitmap[i] == 0);
  }
  free(bitmap);
  return 0;
}

/*--------------------------------------------------------------------*/

static char *test_set_bit() {
  uint8_t *bitmap = init_bitmap();
  set_bit(bitmap, 0);
  mu_assert("Bitmap bit set failed", bitmap[0] == 1);
  set_bit(bitmap, 1);
  mu_assert("Bitmap bit set failed", bitmap[0] == 0b11);
  set_bit(bitmap, 15);
  mu_assert("Bitmap bit set failed", bitmap[1] == 0b10000000);
  set_bit(bitmap, 8 * SIZEOF_BITMAP - 1);
  mu_assert("Bitmap bit set failed", bitmap[SIZEOF_BITMAP - 1] == 0b10000000);
  free(bitmap);
  return 0;
}

static char *test_string_to_bitmap_empty() {
  uint8_t *bitmap = init_bitmap();
  apply_string_to_bitmap(bitmap, "");
  for (size_t i = 0; i < SIZEOF_BITMAP; i++) {
    mu_assert("Extra bits added in string to bitmap", bitmap[i] == 0);
  }
  free(bitmap);
  return 0;
}

static char *test_string_to_bitmap_tiny() {
  uint8_t *bitmap = init_bitmap();
  apply_string_to_bitmap(bitmap, "as");
  for (size_t i = 0; i < SIZEOF_BITMAP; i++) {
    mu_assert("Extra bits added in string to bitmap", bitmap[i] == 0);
  }
  free(bitmap);
  return 0;
}

static char *test_string_to_bitmap_nchars() {
  uint8_t *bitmap = init_bitmap();
  char str[NGRAM_CHARS + 1];
  int n = 0;
  for (int i = 0; i < NGRAM_CHARS; i++) {
    str[i] = 'a';
    n = (n << NGRAM_CHAR_BITS) + ('a' & CHAR_MASK);
  }
  str[NGRAM_CHARS] = '\0';
  apply_string_to_bitmap(bitmap, str);
  mu_assert("test_string_to_bitmap_nchars: bit unset", bitmap[n / 8] == 1 << (n % 8));
  for (size_t i = 0; i < SIZEOF_BITMAP; i++) {
    if (i != n / 8) {
      mu_assert("test_string_to_bitmap_nchars: extra bit set", bitmap[i] == 0);
    }
  }
  free(bitmap);
  return 0;
}

static char *test_string_to_bitmap_long() {
  uint8_t *bitmap = init_bitmap();
  apply_string_to_bitmap(bitmap, "aaaaaaaaaaaaaaaaaaaz");

  int n = 0;
  for (int i = 0; i < NGRAM_CHARS; i++) {
    n = (n << NGRAM_CHAR_BITS) + ('a' & CHAR_MASK);
  }
  int m = 0;
  for (int i = 0; i < NGRAM_CHARS - 1; i++) {
    m = (m << NGRAM_CHAR_BITS) + ('a' & CHAR_MASK);
  }
  m = (m << NGRAM_CHAR_BITS) + ('z' & CHAR_MASK);

  mu_assert("test_string_to_bitmap_long: n unset", bitmap[n / 8] == 1 << (n % 8));
  mu_assert("test_string_to_bitmap_long: m unset", bitmap[m / 8] == 1 << (m % 8));
  for (size_t i = 0; i < SIZEOF_BITMAP; i++) {
    if (i != n / 8 && i != m / 8) {
      mu_assert("test_string_to_bitmap_long: extra bit set", bitmap[i] == 0);
    }
  }
  free(bitmap);
  return 0;
}

static char *test_string_to_bitmap() {
  mu_run_test(test_string_to_bitmap_empty);
  mu_run_test(test_string_to_bitmap_tiny);
  mu_run_test(test_string_to_bitmap_nchars);
  mu_run_test(test_string_to_bitmap_long);
  return 0;
}

static char *test_compress_bitmap() {
  // create a simple bitmap
  uint8_t *bitmap = init_bitmap();
  set_bit(bitmap, 0);
  set_bit(bitmap, 8);

  // pick a file path to pretend we're compressing
  char *fake_tmpfile_path = "/tmp/asdf";
  int64_t fake_mtime = 0;

  char bitmap_tmpfile_path[PATH_MAX] = "/tmp/4gramtmpfile.XXXXXX";
  FILE *bitmap_file = fdopen(mkstemp(bitmap_tmpfile_path), "w");
  // compress to a file in the bitmap store
  mu_assert("Error writing to tmpfile", bitmap_file != NULL);
  compress_to_fp(bitmap, bitmap_file, fake_tmpfile_path, fake_mtime);
  fclose(bitmap_file);

  // try decompressing it
  uint8_t *decompressed = init_bitmap();
  int decompress_ret = decompress_file(decompressed, bitmap_tmpfile_path);
  mu_assert("Decompress error", decompress_ret == 0);

  // check that it decompressed correctly
  for (int i = 0; i < SIZEOF_BITMAP; i++) {
    // printf("%d %d\n", bitmap[i], decompressed[i]);
    mu_assert("Decompressed bitmap not the same", bitmap[i] == decompressed[i]);
  }

  free(bitmap);
  free(decompressed);
  return 0;
}

static char *test_compress_to_file_no_collision() {
  uint8_t *bitmap = init_bitmap();
  char *file_path = "/tmp/nonexistent";
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  mu_assert("Could not create tmpdir", store != NULL);

  int ret = compress_to_file(bitmap, file_path, 0, store);
  mu_assert("Compress to file failed", ret == 0);

  char hashed_filename[21];
  get_hash(file_path, strlen(file_path), hashed_filename);
  strcat(hashed_filename, "_000");
  char *path_to_bitmap_file = add_path_parts(store, hashed_filename);

  mu_assert("Compressed bitmap file doesn't exist",
            access(path_to_bitmap_file, F_OK) == 0);
  uint8_t *decompressed = init_bitmap();
  ret = decompress_file(decompressed, path_to_bitmap_file);
  mu_assert("Error occurred in decompression", ret == 0);
  for (int i = 0; i < SIZEOF_BITMAP; i++) {
    mu_assert("Decompressed bitmap not the same", bitmap[i] == decompressed[i]);
  }
  free(bitmap);
  free(decompressed);
  free(path_to_bitmap_file);
  return 0;
}

static char *test_compress_to_file_with_collision() {
  uint8_t *bitmap = init_bitmap();
  char *file_path = "/tmp/nonexistent";
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  mu_assert("Could not create tmpdir", store != NULL);
  int num_files = 3;

  for (int i = 0; i < num_files; i++) {
    int ret = compress_to_file(bitmap, file_path, 0, store);
    mu_assert("Compress to file failed", ret == 0);

    char cache_file_name[21];
    char num_extension[5];
    sprintf(num_extension, "_%.3d", i);
    get_hash(file_path, strlen(file_path), cache_file_name);
    strcat(cache_file_name, num_extension);
    char *path_to_bitmap_file = add_path_parts(store, cache_file_name);

    mu_assert("Compressed bitmap file doesn't exist",
              access(path_to_bitmap_file, F_OK) == 0);
    uint8_t *decompressed = init_bitmap();
    ret = decompress_file(decompressed, path_to_bitmap_file);
    mu_assert("Error occurred in decompression", ret == 0);
    for (int i = 0; i < SIZEOF_BITMAP; i++) {
      mu_assert("Decompressed bitmap not the same", bitmap[i] == decompressed[i]);
    }
    free(decompressed);
    free(path_to_bitmap_file);
  }
  free(bitmap);
  return 0;
}

static char *test_compress_to_file() {
  mu_run_test(test_compress_to_file_no_collision);
  mu_run_test(test_compress_to_file_with_collision);
  return 0;
}

static char *test_file_packing_single_file() {
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char template2[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  char *tmpfile_dir = mkdtemp(template2);
  mu_assert("Could not create tmpdir", store != NULL);
  char *tmpfile_path = add_path_parts(tmpfile_dir, "1.txt");

  // write a small temporary file
  FILE *tmpfile = fopen(tmpfile_path, "w");
  mu_assert("Could not create tmpfile", tmpfile != NULL);
  fputs("asdf", tmpfile);
  fclose(tmpfile);

  // create a bitmap for the file
  uint8_t *bitmap = init_bitmap();
  tmpfile = fopen(tmpfile_path, "r");
  apply_file_to_bitmap(bitmap, tmpfile);
  fclose(tmpfile);
  int64_t mtime = get_mtime(tmpfile_path);

  // figure out the hash of the file
  char loose_file_name[PATH_MAX];
  get_hash(tmpfile_path, strlen(tmpfile_path), loose_file_name);

  // compress the bitmap to a file in the store
  int ret = compress_to_file(bitmap, tmpfile_path, mtime, store);
  mu_assert("Error compressing", ret == 0);

  // make sure it exists
  mu_assert("Compressed bitmap file doesn't exist",
      access(loose_file_name, F_OK));

  pack_loose_files_in_subdir(store);

  uint8_t *read_bitmap = read_from_packfile(tmpfile_path, mtime, store);
  mu_assert("Could not find bitmap in packfile", read_bitmap != NULL);
  for (size_t i = 0; i < SIZEOF_BITMAP; i++) {
    mu_assert("Wrong bitmap returned", (bitmap[i] == read_bitmap[i]));
  }

  // make sure loose file was removed
  DIR *dir = opendir(store);
  mu_assert("Error opening bitmap store directory", dir != NULL);
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, PACKFILE_NAME) == 0
        || strcmp(entry->d_name, PACKFILE_INDEX_NAME) == 0
        || strcmp(entry->d_name, ".") == 0
        || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    mu_assert("Loose file still in bitmap store directory", 0);
  }
  closedir(dir);

  free(tmpfile_path);
  free(bitmap);
  free(read_bitmap);
  return 0;
}

static char *test_file_packing_multiple_files() {
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char template2[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  char *tmpfile_dir = mkdtemp(template2);
  mu_assert("Could not create tmpdir", store != NULL);
  mu_assert("Could not create tmpdir", tmpfile_dir != NULL);
  int num_files = 10;
  uint8_t *bitmaps[num_files];
  char *tmpfile_paths[num_files];
  for (int i = 0; i < num_files; i++) {
    char name[PATH_MAX];
    sprintf(name, "%d.txt", i);
    tmpfile_paths[i] = add_path_parts(tmpfile_dir, name);
    FILE *tmpfile = fopen(tmpfile_paths[i], "w");
    mu_assert("Could not create tmpfile", tmpfile != NULL);
    fprintf(tmpfile, "%d", i * 1000);
    fclose(tmpfile);
    tmpfile = fopen(tmpfile_paths[i], "r");
    bitmaps[i] = init_bitmap();
    apply_file_to_bitmap(bitmaps[i], tmpfile);
    fclose(tmpfile);
    char loose_file_name[PATH_MAX];
    get_hash(tmpfile_paths[i], strlen(tmpfile_paths[i]), loose_file_name);
    int64_t mtime = get_mtime(tmpfile_paths[i]);
    int ret = compress_to_file(bitmaps[i], tmpfile_paths[i], mtime, store);
    mu_assert("Error compressing", ret == 0);
  }
  pack_loose_files_in_subdir(store);
  for (int i = 0; i < num_files; i++) {
    int64_t mtime = get_mtime(tmpfile_paths[i]);
    uint8_t *read_bitmap = read_from_packfile(tmpfile_paths[i], mtime, store);
    mu_assert("Could not find bitmap in packfile", read_bitmap != NULL);
    for (size_t j = 0; j < SIZEOF_BITMAP; j++) {
      mu_assert("Wrong bitmap returned", bitmaps[i][j] == read_bitmap[j]);
    }
    free(read_bitmap);
    free(bitmaps[i]);
    free(tmpfile_paths[i]);
  }

  // make sure loose file was removed
  DIR *dir = opendir(store);
  mu_assert("Error opening bitmap store directory", dir != NULL);
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, PACKFILE_NAME) == 0
        || strcmp(entry->d_name, PACKFILE_INDEX_NAME) == 0
        || strcmp(entry->d_name, ".") == 0
        || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    mu_assert("Loose file still in bitmap store directory", 0);
  }
  closedir(dir);

  return 0;
}

static char *test_file_packing_existing_packfile() {
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char template2[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  char *tmpfile_dir = mkdtemp(template2);
  mu_assert("Could not create tmpdir", store != NULL);
  mu_assert("Could not create tmpdir", tmpfile_dir != NULL);
  int num_files = 20;
  uint8_t *bitmaps[num_files];
  char *tmpfile_paths[num_files];
  for (int i = 0; i < num_files; i++) {
    char name[PATH_MAX];
    sprintf(name, "%d.txt", i);
    tmpfile_paths[i] = add_path_parts(tmpfile_dir, name);
    FILE *tmpfile = fopen(tmpfile_paths[i], "w");
    mu_assert("Could not create tmpfile", tmpfile != NULL);
    fprintf(tmpfile, "%d", i * 1000);
    fclose(tmpfile);
    tmpfile = fopen(tmpfile_paths[i], "r");
    bitmaps[i] = init_bitmap();
    apply_file_to_bitmap(bitmaps[i], tmpfile);
    fclose(tmpfile);
    char loose_file_name[PATH_MAX];
    get_hash(tmpfile_paths[i], strlen(tmpfile_paths[i]), loose_file_name);
    int64_t mtime = get_mtime(tmpfile_paths[i]);
    int ret = compress_to_file(bitmaps[i], tmpfile_paths[i], mtime, store);
    mu_assert("Error compressing", ret == 0);
    if (i == num_files / 2) {
      pack_loose_files_in_subdir(store);
    }
  }
  pack_loose_files_in_subdir(store);
  for (int i = 0; i < num_files; i++) {
    int64_t mtime = get_mtime(tmpfile_paths[i]);
    uint8_t *read_bitmap = read_from_packfile(tmpfile_paths[i], mtime, store);
    mu_assert("Could not find bitmap in packfile", read_bitmap != NULL);
    for (size_t j = 0; j < SIZEOF_BITMAP; j++) {
      mu_assert("Wrong bitmap returned", bitmaps[i][j] == read_bitmap[j]);
    }
    free(read_bitmap);
    free(bitmaps[i]);
    free(tmpfile_paths[i]);
  }

  // make sure loose file was removed
  DIR *dir = opendir(store);
  mu_assert("Error opening bitmap store directory", dir != NULL);
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, PACKFILE_NAME) == 0
        || strcmp(entry->d_name, PACKFILE_INDEX_NAME) == 0
        || strcmp(entry->d_name, ".") == 0
        || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    mu_assert("Loose file still in bitmap store directory", 0);
  }
  closedir(dir);

  return 0;
}

static char *test_file_packing() {
  mu_run_test(test_file_packing_single_file);
  mu_run_test(test_file_packing_multiple_files);
  mu_run_test(test_file_packing_existing_packfile);
  return 0;
}

static char *test_filter_checks_emptydir() {
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char template2[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  char *tmpfile_dir = mkdtemp(template2);
  mu_assert("Could not create tmpdir", store != NULL);
  mu_assert("Could not create tmpdir", tmpfile_dir != NULL);

  // write a small temporary file
  char *tmpfile_path = add_path_parts(tmpfile_dir, "1.txt");
  FILE *tmpfile = fopen(tmpfile_path, "w");
  mu_assert("Could not create tmpfile", tmpfile != NULL);
  fputs("asdf", tmpfile);
  fclose(tmpfile);
  int64_t mtime = get_mtime(tmpfile_path);

  uint8_t *bitmap = init_bitmap();
  mu_assert("Should not detect loose file",
            check_loose_files(tmpfile_path, mtime, bitmap, store) != 0);
  mu_assert("Should not detect entry in pack file",
            check_pack_files(tmpfile_path, mtime, bitmap, store) != 0);
  free(tmpfile_path);
  free(bitmap);
  return 0;
}

static char *test_filter_checks_loose_file() {
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char template2[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  char *tmpfile_dir = mkdtemp(template2);
  mu_assert("Could not create 4gramtmpdir", store != NULL);
  mu_assert("Could not create 4gramtmpdir", tmpfile_dir != NULL);

  // write a small temporary file
  char *tmpfile_path = add_path_parts(tmpfile_dir, "1.txt");
  FILE *tmpfile = fopen(tmpfile_path, "w");
  mu_assert("Could not create tmpfile", tmpfile != NULL);
  fputs("asdf", tmpfile);
  fclose(tmpfile);
  int64_t mtime = get_mtime(tmpfile_path);

  // compress it to loose file
  uint8_t *bitmap = init_bitmap();
  tmpfile = fopen(tmpfile_path, "r");
  mu_assert("Could not open tmpfile", tmpfile != NULL);
  apply_file_to_bitmap(bitmap, tmpfile);
  fclose(tmpfile);
  compress_to_file(bitmap, tmpfile_path, mtime, store);

  uint8_t *read_bitmap = init_bitmap();
  mu_assert("Should detect loose file",
            check_loose_files(tmpfile_path, mtime, bitmap, store) == 0);
  mu_assert("Should not detect entry in pack file",
            check_pack_files(tmpfile_path, mtime, bitmap, store) != 0);
  free(bitmap);
  free(read_bitmap);
  free(tmpfile_path);
  return 0;
}

static char *test_filter_checks_packfile() {
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char template2[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  char *tmpfile_dir = mkdtemp(template2);
  mu_assert("Could not create 4gramtmpdir", store != NULL);
  mu_assert("Could not create 4gramtmpdir", tmpfile_dir != NULL);

  // write a small temporary file
  char *tmpfile_path = add_path_parts(tmpfile_dir, "1.txt");
  FILE *tmpfile = fopen(tmpfile_path, "w");
  mu_assert("Could not create tmpfile", tmpfile != NULL);
  fputs("asdf", tmpfile);
  fclose(tmpfile);
  int64_t mtime = get_mtime(tmpfile_path);

  // compress it to loose file
  uint8_t *bitmap = init_bitmap();
  tmpfile = fopen(tmpfile_path, "r");
  mu_assert("Could not open tmpfile", tmpfile != NULL);
  apply_file_to_bitmap(bitmap, tmpfile);
  fclose(tmpfile);
  compress_to_file(bitmap, tmpfile_path, mtime, store);

  pack_loose_files_in_subdir(store);

  uint8_t *read_bitmap = init_bitmap();
  mu_assert("Should not detect loose file",
            check_loose_files(tmpfile_path, mtime, bitmap, store) != 0);
  mu_assert("Should detect entry in pack file",
            check_pack_files(tmpfile_path, mtime, bitmap, store) == 0);
  free(bitmap);
  free(read_bitmap);
  free(tmpfile_path);
  return 0;
}

static char *test_filter_checks() {
  mu_run_test(test_filter_checks_emptydir);
  mu_run_test(test_filter_checks_loose_file);
  mu_run_test(test_filter_checks_packfile);
  return 0;
}

static char *test_packfile_locking() {
  uint8_t *bitmap = init_bitmap();
  char *file_path = "/tmp/nonexistent";
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  mu_assert("Could not create tmpdir", store != NULL);

  int ret = compress_to_file(bitmap, file_path, 0, store);
  mu_assert("Compress to file failed", ret == 0);

  char hashed_filename[21];
  get_hash(file_path, strlen(file_path), hashed_filename);
  strcat(hashed_filename, "_000");
  char *path_to_bitmap_file = add_path_parts(store, hashed_filename);
  mu_assert("Loose file not created\n", access(path_to_bitmap_file, F_OK) == 0);

  char *packfile_path = add_path_parts(store, PACKFILE_NAME);

  mu_assert("Could not lock packfile", lockfile_create(packfile_path, 0, 0) == 0);

  if (fork() == 0) {
    pack_loose_files_in_subdir(store);
    exit(0);
  }
  int wait_status;
  wait(&wait_status);

  mu_assert("Loose files were packed despite lock",
      access(path_to_bitmap_file, F_OK) == 0);
  lockfile_remove(packfile_path);
  free(path_to_bitmap_file);
  free(packfile_path);
  free(bitmap);
  return 0;
}

static char *test_get_4gram_indices() {
  char *strings[] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm!@#$%^&*()",
  };
  for (int i = 0; i < 3; i++) {
    uint8_t *bitmap = init_bitmap();
    apply_string_to_bitmap(bitmap, strings[i]);
    int *indices = get_4gram_indices(strings[i]);
    int len = strlen(strings[i]) - NGRAM_CHARS + 1;
    for (int j = 0; j < len; j++) {
      int k = indices[j];
      mu_assert("Invalid 4gram indices", get_bit(bitmap, k));
    }
    free(bitmap);
    free(indices);
  }
  return 0;
}

static char *test_corruption_size() {
  uint8_t *bitmap = init_bitmap();
  apply_string_to_bitmap(bitmap, "hello");
  char *orig_filename = "should be 12";
  FILE *temp = fopen("tmp.txt", "w");

  uint16_t len = strlen(orig_filename);
  void* compressed = malloc(131616);
  uint32_t compressed_size = ZSTD_compress(compressed, 131616,
                                           bitmap, SIZEOF_BITMAP, 3);

  uint16_t len_be = htobe16(len);
  uint32_t compressed_size_be = htobe32(compressed_size);
  fwrite(&len_be, sizeof(uint16_t), 1, temp);
  fwrite(orig_filename, len, 1, temp);
  fwrite(&compressed_size_be, sizeof(uint32_t), 1, temp);
  fwrite(compressed, compressed_size, 1, temp);
  fclose(temp);

  FILE *temp2 = fopen("tmp.txt", "r");
  fseek(temp2, 0, SEEK_END);
  unsigned long written_size = ftell(temp2);
  rewind(temp2);
  fclose(temp2);
  remove("tmp.txt");
  mu_assert("Size of file not same as written size",
            (len + compressed_size + 6 == written_size));
  free(compressed);
  free(bitmap);
  return 0;
}

static char *test_loose_file_locking() {
  uint8_t *bitmap = init_bitmap();
  char *filename = "/tmp/nonexistent";
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  mu_assert("Could not create tmpdir", store != NULL);
  compress_to_file(bitmap, filename, 0, store);

  char hash[21];
  uint16_t len = strlen(filename);
  get_hash(filename, len, hash);
  char loose_file_name[PATH_MAX];
  strcpy(loose_file_name, hash);
  strcat(loose_file_name, "_000");
  char *lockfile_path = get_lock_path(store, loose_file_name);
  mu_assert("Could not lock file", lockfile_create(lockfile_path, 0, 0) == 0);

  if (fork() == 0) {
    pack_loose_files_in_subdir(store);
    exit(0);
  }
  int wait_status;
  wait(&wait_status);

  char *loose_file_path = add_path_parts(store, loose_file_name);
  mu_assert("Loose file was packed despite lock",
      access(loose_file_path, F_OK) == 0);
  free(loose_file_path);
  lockfile_remove(lockfile_path);
  free(lockfile_path);
  free(bitmap);

  return 0;
}

static char *test_strings_to_sorted_indices() {
  char *strings[] = {
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm!@#$%^&*()",
  };
  uint8_t *bitmap = init_bitmap();
  for (int i = 0; i < 3; i++) {
    apply_string_to_bitmap(bitmap, strings[i]);
  }
  struct intarray indices;
  indices = strings_to_sorted_indices(strings, 3);
  for (int i = 0; i < POSSIBLE_NGRAMS; i++) {
    if (get_bit(bitmap, i)) {
      int contained = 0;
      for (int j = 0; j < indices.length; j++) {
        if (indices.data[j] == i) {
          contained = 1;
        }
      }
      mu_assert("strings_to_sorted_indices: Index not found", contained);
    }
  }
  for (int i = 1; i < indices.length; i++) {
    mu_assert("strings_to_sorted_indices: unsorted",
        indices.data[i-1] <= indices.data[i]);
  }
  free(bitmap);
  free_intarray(indices);
  return 0;
}

static char *test_mtime() {
  char template[] = "/tmp/4gramtmpdir.XXXXXX";
  char template2[] = "/tmp/4gramtmpdir.XXXXXX";
  char *store = mkdtemp(template);
  char *tmpfile_dir = mkdtemp(template2);
  mu_assert("Could not create tmpdir", store != NULL);
  char *tmpfile_path = add_path_parts(tmpfile_dir, "1.txt");

  // write a small temporary file
  FILE *tmpfile = fopen(tmpfile_path, "w");
  mu_assert("Could not create tmpfile", tmpfile != NULL);
  fputs("qwertyuiop", tmpfile);
  fclose(tmpfile);
  int64_t mtime = 0;

  // create a bitmap for the file
  uint8_t *bitmap = init_bitmap();
  tmpfile = fopen(tmpfile_path, "r");
  apply_file_to_bitmap(bitmap, tmpfile);
  fclose(tmpfile);

  // figure out the hash of the file
  char loose_file_name[PATH_MAX];
  get_hash(tmpfile_path, strlen(tmpfile_path), loose_file_name);

  // compress the bitmap to a file in the store
  int ret = compress_to_file(bitmap, tmpfile_path, mtime, store);
  mu_assert("Error compressing", ret == 0);

  // make sure it exists
  mu_assert("Compressed bitmap file doesn't exist",
      access(loose_file_name, F_OK));

  uint8_t *bitmap1 = init_bitmap();
  // make sure we can access the file with our mtime
  mu_assert("Could not access loose_file with mtime 0",
      check_loose_files(tmpfile_path, mtime, bitmap1, store) == 0);
  mu_assert("Didn't get same bitmap back",
      bitmaps_are_the_same(bitmap, bitmap1));
  free(bitmap1);
  bitmap1 = init_bitmap();
  mu_assert("Got bitmap with invalid mtime",
      check_loose_files(tmpfile_path, 123, bitmap1, store) != 0);
  free(bitmap1);

  pack_loose_files_in_subdir(store);

  uint8_t *read_bitmap = read_from_packfile(tmpfile_path, mtime, store);
  mu_assert("Could not find bitmap in packfile", read_bitmap != NULL);
  mu_assert("Didn't get same bitmap back",
      bitmaps_are_the_same(bitmap, read_bitmap));
  mu_assert("Got bitmap with invalid mtime",
      read_from_packfile(tmpfile_path, 1, store) == NULL);

  free(tmpfile_path);
  free(bitmap);
  free(read_bitmap);
  return 0;
}

static char *test_get_index_subdirectory() {
  char *indexdir = "/4gram";
  char *subdir = get_index_subdirectory(indexdir, 0);
  mu_assert("get_index_subdirectory epoch failed",
      strcmp(subdir, "/4gram/1970_01") == 0);
  free(subdir);
  subdir = get_index_subdirectory(indexdir, -1);
  mu_assert("get_index_subdirectory negative failed",
      strcmp(subdir, "/4gram/1969_12") == 0);
  free(subdir);
  subdir = get_index_subdirectory(indexdir, 1502920742);
  mu_assert("get_index_subdirectory normal date failed",
      strcmp(subdir, "/4gram/2017_08") == 0);
  free(subdir);
  subdir = get_index_subdirectory(indexdir, 1L << 31);
  mu_assert("get_index_subdirectory overflow test failed",
      strcmp(subdir, "/4gram/2038_01") == 0);
  free(subdir);
  return 0;
}


static char *run_tests() {
  mu_run_test(test_init_bitmap);
  mu_run_test(test_set_bit);
  mu_run_test(test_string_to_bitmap);
  mu_run_test(test_compress_to_file);
  mu_run_test(test_compress_bitmap);
  mu_run_test(test_file_packing);
  mu_run_test(test_filter_checks);
  mu_run_test(test_packfile_locking);
  mu_run_test(test_get_4gram_indices);
  mu_run_test(test_corruption_size);
  mu_run_test(test_loose_file_locking);
  mu_run_test(test_strings_to_sorted_indices);
  mu_run_test(test_mtime);
  mu_run_test(test_get_index_subdirectory);
  return 0;
}

int main() {
  char *result = run_tests();
  if (result != 0) {
    printf("%s\n", result);
  } else {
    printf("All tests passed!\n");
  }
  if (system("rm -rf /tmp/4gramtmpdir.*") != 0) {
    printf("Warning: error cleaning temporary directories.");
  }
  if (system("rm -rf /tmp/4gramtmpfile.*") != 0) {
    printf("Warning: error cleaning temporary files.");
  }
  printf("Tests run: %d\n", tests_run);
  return result != 0;
}
