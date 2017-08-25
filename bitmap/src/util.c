#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <cpuid.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>

#include "util.h"

/*--------------------------------------------------------------------*/

/**
 * Allocates a new string consisting of dir + '/' + filename.
 * The combined strings must not exceed PATH_MAX-1 in length.
 */
char *add_path_parts(char *dir, char *filename) {
  char *path = malloc(PATH_MAX);
  strcpy(path, dir);
  strcat(path, "/");
  strcat(path, filename);
  return path;
}

/*--------------------------------------------------------------------*/

/**
 * Returns whether we can read from and write to the given directory.
 */
int is_directory_readwritable(char *path) {
  return (access(path, R_OK) == 0
      && access(path, W_OK) == 0);
}

/*--------------------------------------------------------------------*/

/**
 * Returns directory where bitmaps are currently stored, by checking
 * the first available directory from the list.
 */
char *get_index_directory() {
  static char *indexdir = NULL;
  if (indexdir != NULL) {
    return indexdir;
  }
  if (is_directory_readwritable("/4gram/")) {
    return (indexdir = "/4gram");
  }
  char *home_cache_dir = add_path_parts(getenv("HOME"), ".cache");
  char *home_4gram_dir = add_path_parts(home_cache_dir, "4gram");
  mkdir(home_cache_dir, 0700);
  mkdir(home_4gram_dir, 0777);
  if (is_directory_readwritable(home_4gram_dir)) {
    return (indexdir = home_4gram_dir);
  }
  perror("Could not find readwritable directory to cache 4grams\n");
  return(NULL);
}

/*--------------------------------------------------------------------*/

/**
 * Returns what index subdirectory we should store the index for a file with
 * the given timestamp.
 *
 * These subdirectories are of the form "indexdir/YYYY_MM"
 */
char *get_index_subdirectory(char *indexdir, int64_t timestamp) {
  struct tm *gmt = gmtime(&timestamp);
  char date_string[8];
  strftime(date_string, sizeof(date_string), "%Y_%m", gmt);
  char *index_subdir = add_path_parts(indexdir, date_string);
  mkdir(index_subdir, 0777);
  return index_subdir;
}

/*--------------------------------------------------------------------*/

/**
 * Determines at runtime whether our CPU supports BMI2 instructions.
 */
int supports_bmi2() {
  static int supports_bmi2_cache = -1;
  if (supports_bmi2_cache != -1) {
    return supports_bmi2_cache;
  }
  unsigned int level = 0;
  unsigned int eax = 1;
  unsigned int ebx, ecx, edx;
  __get_cpuid(level, &eax, &ebx, &ecx, &edx);
  supports_bmi2_cache = (ebx >> 8) & 1;
  return supports_bmi2_cache;
}

/*--------------------------------------------------------------------*/

/**
 * Frees the data stored in the given array.
 */
void free_intarray(struct intarray arr) {
  free(arr.data);
}

/**
 * Frees the data stored by the given array array.
 *
 * Recursively frees all sub-arrays.
 */
void free_intarrayarray(struct intarrayarray arr) {
  for (int i = 0; i < arr.num_rows; i++) {
    free_intarray(arr.rows[i]);
  }
  free(arr.rows);
}

/**
 * Like perror, but uses a format string.
 */
void perrorf(char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, ": ");
  perror("");
}

/**
 * Returns the mtime of the file entry at the given path.
 */
int64_t get_mtime(char *path) {
  struct stat s;
  stat(path, &s);
  return s.st_mtime;
}

/**
 * Returns whether path points to a directory or not.
 */
int is_dir(char *path) {
  struct stat s;
  stat(path, &s);
  return S_ISDIR(s.st_mode);
}
