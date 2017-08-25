#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <dirent.h>
#include <stdint.h>
#include <assert.h>
#include <zstd.h>
#include <errno.h>
#include <lockfile.h>
#include <time.h>
#include <pthread.h>

#include "bitmap.h"
#include "util.h"
#include "xxhash.h"
#include "packfile.h"
#include "portable_endian.h"

/*--------------------------------------------------------------------*/

/** An entry in the index.
 * Note: in order to allow mmaping the index file, this struct stores
 * packfile_offset as big-endian!
 */
struct index_entry {
  uint64_t hash;
  uint64_t packfile_offset;
};

/*--------------------------------------------------------------------*/

/**
 * Checks to see if loosefile was interrupted when writing by checking
 * actual size vs expected size
 * Returns 0 if no error, returns EMPTY_FILE if loose file is empty,
 * returns -1 if other error or file corrupted.
 */
int is_corrupted(FILE* loosefile) {
  uint16_t len;
  uint32_t compressed_size;
  int64_t mtime;

  struct stat loosefile_stat;
  fstat(fileno(loosefile), &loosefile_stat);
  off_t loosefile_size = loosefile_stat.st_size;

  if (loosefile_size == 0) {
    return EMPTY_FILE;
  }

  if (fread(&len, sizeof(uint16_t), 1, loosefile) != 1) {
    perror("Error in reading filename size");
    return(-1);
  }
  len = be16toh(len);
  if (fseek(loosefile, len, SEEK_CUR) != 0) {
    perror("Error reading loose file");
    return(-1);
  }
  if (fread(&mtime, sizeof(int64_t), 1, loosefile) != 1) {
    perror("Error in reading mtime");
    return(-1);
  }
  mtime = be64toh(mtime);
  if (fread(&compressed_size, sizeof(uint32_t), 1, loosefile) != 1){
    perror("Error in reading decompressed size");
    return(-1);
  }
  compressed_size = be32toh(compressed_size);

  if((len + compressed_size + sizeof(uint16_t) + sizeof(uint32_t) +
        sizeof(int64_t)) != loosefile_size){
    fprintf(stderr, "Corrupted file: l:%u, cs:%u, filesize:%lld\n",
            len, compressed_size, (long long) loosefile_size);
    return(-1);
  }

  rewind(loosefile);
  return 0;
}

/*--------------------------------------------------------------------*/

/**
 * Returns 1 if file was detected as corrupted, deleted and should be skipped.
 * Returns 2 if file was empty and should be skipped, but wasn't deleted.
 * Returns 0 otherwise.
 */
int remove_if_corrupted(FILE *file, char *file_path) {
  int corrupt_status = is_corrupted(file);
  if(corrupt_status != 0){
    // ignore empty files because they could mean we acquired a read lock
    // on the file before the writing process could acquire a write lock
    // TODO: remove empty files that are very old
    if (corrupt_status != EMPTY_FILE) {
      remove(file_path);
      return 1;
    }
    return 2;
  }
  return 0;
}

/*--------------------------------------------------------------------*/

/**
 * Returns the index into the packfile index entries where the first entry with
 * the given hash is located, or -1 if the hash does not exist in the index.
 */
size_t find_hash_in_index(struct index_entry *index,
    size_t num_entries, uint64_t hash) {

  size_t left = 0;
  size_t right = num_entries - 1;
  while (left != right) {
    size_t middle = (right + left) / 2;
    if (index[middle].hash < hash) {
      left = middle + 1;
    } else {
      right = middle;
    }
  }
  if (index[left].hash != hash) {
    return -1;
  } else {
    return left;
  }
}

/*--------------------------------------------------------------------*/

/**
 * Calculates the number of existing entries in the packfile index based on the
 * size of the packfile index file.
 *
 * Returns -1 on error.
 */
size_t get_num_index_entries(FILE *packfile_index) {
  if (fseek(packfile_index, 0, SEEK_END) != 0) {
    return(-1);
  }
  long index_size = ftell(packfile_index);
  rewind(packfile_index);
  return index_size / sizeof(struct index_entry);
}

/*--------------------------------------------------------------------*/

/**
 * Reads the data stored in the packfile with the given name.
 * Assumes the data is (the size of a) bitmap when decompressed.
 *
 * filename: name of file to search for in the packfile
 * mtime: mtime of file to search for in the packfile
 * indexdir: index directory
 */
uint8_t *read_from_packfile(char *filename, int64_t mtime, char *indexdir) {
  
  char *packfile_path = add_path_parts(indexdir, PACKFILE_NAME);
  FILE *packfile = fopen(packfile_path, "r");
  uint8_t *packed_file = NULL;
  free(packfile_path);
  if(packfile == NULL) {
    if (errno != ENOENT) {
      perror("Error: could not open packfile");
    }
    return(NULL);
  }
  char *packfile_index_path = add_path_parts(indexdir, PACKFILE_INDEX_NAME);
  FILE *packfile_index = fopen(packfile_index_path, "r");
  free(packfile_index_path);
  if(packfile_index == NULL) {
    fclose(packfile);
    if (errno != ENOENT)
      perror("Error: could not open packfile index");
    return(NULL);
  }

  uint64_t hashed = XXH64(filename, strlen(filename), HASH_SEED);
  size_t num_index_entries = get_num_index_entries(packfile_index);
  if (num_index_entries <= 0)
    goto OUT2;
  size_t index_filesize = num_index_entries * sizeof(struct index_entry);
  struct index_entry *index = mmap(NULL, index_filesize, PROT_READ,
                                   MAP_PRIVATE, fileno(packfile_index), 0);
  if(index == MAP_FAILED){
    perror("Error: could not mmap packfile index");
    goto OUT1;
  }
  size_t first_identical_hash_loc = find_hash_in_index(
      index, num_index_entries, hashed);
  if (first_identical_hash_loc == -1) {
    goto OUT1;
  }
  // now to see if any of the identical hashes map to the same filename
  // we need to read the packfile for this
  for (size_t i = first_identical_hash_loc; index[i].hash == hashed; i++) {
    size_t offset = be64toh(index[i].packfile_offset);
    uint16_t name_len;
    fseek(packfile, offset, SEEK_SET);
    if (fread(&name_len, sizeof(uint16_t), 1, packfile) != 1) {
      perror("Error in packfile fread");
      goto OUT1;
    }
    name_len = be16toh(name_len);
    char packed_filename[name_len];
    if (fread(packed_filename, name_len, 1, packfile) != 1) {
      perror("Error in packfile fread");
      goto OUT1;
    }
    if (strncmp(packed_filename, filename, name_len) != 0) {
      continue;
    }
    int64_t packed_mtime;
    if (fread(&packed_mtime, sizeof(int64_t), 1, packfile) != 1) {
      perror("Error in packfile fread");
      goto OUT1;
    }
    packed_mtime = be64toh(packed_mtime);
    if (packed_mtime != mtime) {
      continue;
    }
    uint32_t packed_file_len;
    // we found an entry with the same filename!
    // now we may read the file
    if (fread(&packed_file_len, sizeof(uint32_t), 1, packfile) != 1) {
      perror("Error in packfile fread");
      goto OUT1;
    }
    packed_file_len = be32toh(packed_file_len);
    uint8_t compressed_file[packed_file_len];
    if (fread(&compressed_file, packed_file_len, 1, packfile) != 1) {
      perror("Error in packfile fread");
      goto OUT1;
    }
    // now decompress it
    packed_file = malloc(SIZEOF_BITMAP);
    if (packed_file == NULL){
      perror("Error: Memory not allocated");
      goto OUT1;
    }

    size_t s = ZSTD_decompress(packed_file, SIZEOF_BITMAP,
        compressed_file, packed_file_len);
    if(ZSTD_isError(s) == 1){
      fprintf(stderr, "Error in packfile decompression: %s\n",
              ZSTD_getErrorName(s));
      free(packed_file);
      packed_file = NULL;
      goto OUT1;
    }
  }

  OUT1:
    if (munmap(index, index_filesize) == -1) {
      perror("Error in packfile index munmap");
    }
  OUT2:
    fclose(packfile);
    fclose(packfile_index);
    return packed_file;

}

/*--------------------------------------------------------------------*/

/**
 * If the file at the given path does not exist, it is created with permissions
 * 0666.
 */
int create_file_if_nonexistent(char *path) {
  int fd = open(path, O_CREAT, 0666);
  if (fd == -1) {
    perrorf("Error creating file: %s", path);
    return(-1);
  }
  close(fd);
  return 0;
}

/*--------------------------------------------------------------------*/

/**
 * Appends the given data to the end of the packfile, returning the offset at
 * which it is added.
 */
long write_data_to_packfile(void *data, size_t size, FILE *packfile) {
  long packfile_offset = ftell(packfile);
  int write_amount = fwrite(data, size, 1, packfile);
  if (write_amount != 1) {
      perror("Error writing to packfile");
      return -1;
  }
  return packfile_offset;
}

/**
 * Adds the file at the given path to the packfile opened in append-mode.
 * Returns the offset into the packfile at which the new file is written.
 */
long add_file_to_packfile(char *filename, char *indexdir, FILE *packfile) {
  char tmp[27];
  int ret_val = -1;
  sprintf(tmp, ".%s.lock", filename);
  char *lock_path = add_path_parts(indexdir, tmp);
  int ret = lockfile_check(lock_path, 0);
  free(lock_path);
  if(ret == 0){
    return ret_val;
  }

  char *file_path = add_path_parts(indexdir, filename);
  FILE *f = fopen(file_path, "r");
  if (f == NULL) {
    perrorf("Could not open %s", file_path);
    goto OUT1;
  }

  if (remove_if_corrupted(f, file_path)) {
    fclose(f);
    goto OUT1;
  }
  char buf[BUFSIZE];
  long packfile_offset = ftell(packfile);

  int read_amount;
  while ((read_amount = fread(buf, 1, BUFSIZE * sizeof(char), f)) > 0) {
    int write_amount = fwrite(buf, 1, read_amount, packfile);
    if (write_amount != read_amount) {
      perror("Error writing to packfile");
    }
  }
  fclose(f);
  if (read_amount < 0) {
    perrorf("Error reading from %s", file_path);
    goto OUT1;
  }
  ret_val = packfile_offset;
  goto OUT1;

  OUT1:
    free(file_path);
    return ret_val;

}

/*--------------------------------------------------------------------*/

/**
 * Comparison function for sorting index entries.
 */
int compare_index_entries(const void *a, const void *b) {
  const struct index_entry *iea = (const struct index_entry *)a;
  const struct index_entry *ieb = (const struct index_entry *)b;
  return (iea->hash > ieb->hash) - (iea->hash < ieb->hash);
}

/*--------------------------------------------------------------------*/

/**
 * Counts files in the directory to be added to packfile
 */
int count_loose_files(char *dir_path) {
  DIR *dir = opendir(dir_path);
  if (dir == NULL){
    perrorf("Error in opening directory: %s", dir_path);
    return(-1);
  }
  int num_loose = 0;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (strcmp(entry->d_name, PACKFILE_NAME) == 0
        || strcmp(entry->d_name, PACKFILE_INDEX_NAME) == 0
        || entry->d_name[0] == '.' ) {
      continue;
    }
    num_loose++;
  }
  closedir(dir);
  return num_loose;
}

/*--------------------------------------------------------------------*/

/**
 * Writes new_index to a temporary file, then renames it to the index file,
 * replacing the old one atomically.
 */
int write_new_index(struct index_entry *new_index,
                     int new_index_length, char *file_path) {
  // write the new index to a tmp file
  if(create_file_if_nonexistent(file_path) == -1) {
    return -1;
  }
  FILE *file = fopen(file_path, "w");
  if (file == NULL) {
    perror("Error creating tempfile");
    return(-1);
  }
  int write_amount = fwrite(new_index, new_index_length,
      sizeof(struct index_entry), file);
  fclose(file);
  if (write_amount < 0) {
    perror("Error writing tempfile");
    return(-1);
  }
  return 0;
}

/*--------------------------------------------------------------------*/

/**
 * Gets hash from the saved string
 */
uint64_t string_to_hash(char *filename){
  XXH64_canonical_t *canonical = malloc(sizeof(XXH64_canonical_t));
  char *hash_str = filename;
  for(int i = 0; i < 8; i++){
    sscanf((hash_str+2*i), "%02hhx", &(canonical->digest[i]));
  }
  uint64_t ret_hash = XXH64_hashFromCanonical(canonical);
  free(canonical);
  return ret_hash;
}

/*--------------------------------------------------------------------*/

struct read_file_result {
  int error;
  void *data;
  size_t length;
};

struct read_file_args {
  char *filename;
  char *indexdir;
};

void *read_file(void *args) {
  struct read_file_args *real_args = args;
  char *filename = real_args->filename;
  char *indexdir = real_args->indexdir;
  free(args);
  struct read_file_result *result = malloc(sizeof(struct read_file_result));
  result->error = 0;

  char *path = add_path_parts(indexdir, filename);
  char tmp[27];
  sprintf(tmp, ".%s.lock", filename);
  char *lock_path = add_path_parts(indexdir, tmp);
  int ret = lockfile_check(lock_path, 0);
  free(lock_path);
  if(ret == 0){
    goto OUT4;
  }

  FILE *file = fopen(path, "r");
  if (file == NULL) {
    result->error = errno;
    goto OUT4;
  }
  int corrupt = remove_if_corrupted(file, path);
  if (corrupt) {
    result->error = -corrupt;
    goto OUT3;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    result->error = errno;
    goto OUT3;
  }
  long size = ftell(file);
  if (size < 0) {
    result->error = errno;
    goto OUT3;
  }
  rewind(file);
  void *buff = malloc(size);
  if (buff == NULL) {
    result->error = errno;
    goto OUT3;
  }
  int read_amount = fread(buff, size, 1, file);
  if (read_amount != 1) {
    result->error = errno;
    free(buff);
    goto OUT3;
  } else {
    result->data = buff;
    result->length = size;
  }
  fclose(file);
  free(path);
  return result;

  OUT3:
  fclose(file);
  OUT4:
  free(path);
  result->data = NULL;
  result->length = 0;
  return result;
}

/**
 * Reads many files in parallel, starting a separate thread per file.
 */
struct read_file_result *read_files_in_parallel(char **filenames, int num,
    char *indexdir) {
  struct read_file_result *results =
    malloc(num * sizeof(struct read_file_result));
  pthread_t threads[num];
  int threads_created = 0;
  for (int i = 0; i < num; i++) {
    struct read_file_args *args = malloc(sizeof(*args));
    args->filename = filenames[i];
    args->indexdir = indexdir;
    if (pthread_create(&threads[i], NULL, read_file, args) != 0) {
      perror("Could not create thread");
      break;
    }
    threads_created++;
  }
  for (int t = 0; t < threads_created; t++) {
    struct read_file_result *result;
    pthread_join(threads[t], (void **)&result);
    results[t] = *result;
    free(result);
  }
  if (threads_created < num) {
    for (int i = 0; i < num; i++) {
      free(results[i].data);
    }
    free(results);
    return NULL;
  }
  return results;
}

/**
 * Appends all file data from results to the packfile, writing the new index
 * entries to new_entries.
 */
int write_to_packfile(
    struct read_file_result *results,
    int num_results,
    struct index_entry *new_entries,
    char *added_file_paths[],
    FILE *packfile,
    char *indexdir,
    char *filenames[]) {
  int files_added = 0;
  for (int i = 0; i < num_results; i++) {
    struct read_file_result result = results[i];
    if (result.length > 0) {
      long offset = write_data_to_packfile(
          result.data, result.length, packfile);
      if (offset < 0) {
        continue;
      }
      new_entries[files_added].hash = string_to_hash(filenames[i]);
      new_entries[files_added].packfile_offset = htobe64(offset);
      added_file_paths[files_added] = add_path_parts(indexdir, filenames[i]);
      files_added++;
    } else {
      if (result.error > 0 && result.error != EACCES) {
        fprintf(stderr, "Error reading file %s: %s\n", filenames[i],
            strerror(result.error));
      } else if (result.error == -1) {
        char *path = add_path_parts(indexdir, filenames[i]);
        fprintf(stderr, "File was corrupted and removed: %s", path);
        free(path);
      }
    }
  }
  return files_added;
}

/**
 * Adds num_loose loose files to the packfile.
 * Returns a pointer to index entries for the now-packed files.
 */
struct index_entry *add_loose_files_to_packfile(
    int *num_loose, char *indexdir, char *file_paths[],
    FILE *packfile, char* lock_path) {
  static const int parallel_reads = 50;

  struct index_entry *new_entries = malloc(
      sizeof(struct index_entry) * *num_loose);
  if (new_entries == NULL){
    perror("Error: Memory not allocated");
    return(NULL);
  }
  time_t last_lockfile_touch = time(NULL);

  DIR *dir = opendir(indexdir);
  if (dir == NULL){
    perror("Error in opening directory");
    free(new_entries);
    return NULL;
  }

  struct dirent *entry;
  int files_added = 0;
  char *filenames_buffer[parallel_reads];
  int buffer_size = 0;
  while (1) {
    entry = readdir(dir);
    if (entry != NULL) {
      if (strcmp(entry->d_name, PACKFILE_NAME) == 0
          || strcmp(entry->d_name, PACKFILE_INDEX_NAME) == 0
          || entry->d_name[0] == '.') {
        continue;
      }

      filenames_buffer[buffer_size] = malloc(strlen(entry->d_name) + 1);
      strcpy(filenames_buffer[buffer_size], entry->d_name);
      buffer_size++;
    }
    time_t curr_time = time(NULL);
    if (curr_time > last_lockfile_touch + 60) {
      lockfile_touch(lock_path);
      last_lockfile_touch = curr_time;
    }
    int buffer_full = buffer_size == parallel_reads;
    int enough_files = entry == NULL || files_added + buffer_size == *num_loose;
    if (buffer_full || enough_files) {
      // read some files
      struct read_file_result *results = read_files_in_parallel(
          filenames_buffer, buffer_size, indexdir);
      files_added += write_to_packfile(
          results, buffer_size, new_entries + files_added, file_paths +
          files_added, packfile, indexdir, filenames_buffer);
      for (int i = 0; i < buffer_size; i++) {
        free(filenames_buffer[i]);
        free(results[i].data);
      }
      free(results);
      buffer_size = 0;
    }
    if (enough_files) {
      break;
    }
  }
  *num_loose = files_added;
  closedir(dir);
  return new_entries;
}

/*--------------------------------------------------------------------*/

/**
 * Merges the two sorted arrays arr1 and arr2 and stores the result in result.
 */
void two_finger_merge(struct index_entry *arr1, int arr1size,
                      struct index_entry *arr2, int arr2size,
                      struct index_entry *result) {
  int i1 = 0;
  int i2 = 0;
  for (int r = 0; r < arr1size + arr2size; r++) {
    if (i1 < arr1size
        && (i2 >= arr2size || arr1[i1].hash < arr2[i2].hash)) {
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
 * Adds all of the new index entries to the packfile index.
 * Re-sorts as needed.
 */
int add_entries_to_index(struct index_entry *new_entries,
    int num_new_entries, char *indexdir) {
  // read old index into new index buffer
  int ret_val = -1;
  char *packfile_index_path = add_path_parts(
      indexdir, PACKFILE_INDEX_NAME);
  create_file_if_nonexistent(packfile_index_path);
  FILE *packfile_index = fopen(packfile_index_path, "r");
  if (packfile_index == NULL) {
    perror("Error opening packfile index");
    free(packfile_index_path);
    return ret_val;
  }

  size_t num_existing = get_num_index_entries(packfile_index);
  size_t new_index_length = num_existing + num_new_entries;
  struct index_entry *new_index = malloc(new_index_length *
                                         sizeof(struct index_entry));
  struct index_entry *old_index = malloc(num_existing *
                                         sizeof(struct index_entry));
  int read_amount = fread(old_index, sizeof(struct index_entry),
      num_existing, packfile_index);
  fclose(packfile_index);

  if (read_amount < 0) {
    perror("Error reading index file");
    goto OUT1;
  }
  assert(read_amount == num_existing);
  
  qsort(new_entries, num_new_entries, sizeof(struct index_entry),
      compare_index_entries);
  two_finger_merge(old_index, num_existing, new_entries, num_new_entries,
                   new_index);
  char *tmpfile_path = add_path_parts(indexdir,
                                      TEMP_PACKFILE_INDEX_NAME);
  write_new_index(new_index, new_index_length, tmpfile_path);
  rename(tmpfile_path, packfile_index_path);

  free(tmpfile_path);
  ret_val = 0;
  goto OUT1;

  OUT1:
    free(packfile_index_path);
    free(new_index);
    free(old_index);
    return ret_val;
}

/*--------------------------------------------------------------------*/

struct delete_files_thread_args {
  char **file_paths;
  int num_to_delete;
};

void *delete_files_thread_work(void *args) {
  struct delete_files_thread_args *actual_args = args;
  for (int i = 0; i < actual_args->num_to_delete; i++) {
    remove(actual_args->file_paths[i]);
  }
  free(actual_args);
  return 0;
}

/**
 * Delete the files that were in directory but now in packfile.
 *
 * Files are deleted in parallel across 50 threads.
 */
void delete_loose_files(char *file_paths[], int num_loose){
  if (num_loose == 0) {
    return;
  }
  int num_threads = num_loose > 50 ? 50 : num_loose;
  pthread_t threads[num_threads];
  int base_files_per_thread = num_loose / num_threads;
  int deletes_delegated = 0;
  int threads_created = 0;
  while (deletes_delegated < num_loose) {
    struct delete_files_thread_args *args = malloc(sizeof(*args));
    int num_to_delete;
    if (threads_created < num_loose % num_threads) {
      num_to_delete = base_files_per_thread + 1;
    } else {
      num_to_delete = base_files_per_thread;
    }
    args->file_paths = &file_paths[deletes_delegated];
    args->num_to_delete = num_to_delete;
    if (pthread_create(&threads[threads_created], NULL, delete_files_thread_work, args)) {
      free(args);
      perror("Error starting deletion thread");
      goto OUT2;
    }
    deletes_delegated += num_to_delete;
    threads_created++;
  }

  OUT2:
  for (int t = 0; t < threads_created; t++) {
    pthread_join(threads[t], NULL);
  }
}

/*--------------------------------------------------------------------*/

/**
 * Scans the index directory for files not in the packfile.
 * Each found file is read, inserted into the packfile, and deleted.
 * The packfile index is updated as well.
 */
int pack_loose_files_in_subdir(char *index_subdir) {
  // add all the loose files to the packfile and
  // create index entries for them
  int ret_val = -1;
  mode_t old_umask = umask(0);

  char *packfile_path = add_path_parts(index_subdir, PACKFILE_NAME);
  create_file_if_nonexistent(packfile_path);

  char *packfile_lock = add_path_parts(index_subdir, PACKFILE_LOCK_NAME);
  int ret = lockfile_create(packfile_lock, 0, 0);
  if(ret != 0){
    free(packfile_lock);
    free(packfile_path);
    return(ret_val);
  }

  FILE *packfile = fopen(packfile_path, "a");
  free(packfile_path);
  if (packfile == NULL) {
    lockfile_remove(packfile_lock);
    free(packfile_lock);
    perror("Error opening packfile");
    return(ret_val);
  }

  // figure our how many loose files there are
  int num_loose = count_loose_files(index_subdir);
  char *file_paths[num_loose];

  if (num_loose == 0) {
    goto OUT1;
  }
  struct index_entry *new_entries = add_loose_files_to_packfile(
      &num_loose, index_subdir, file_paths, packfile, packfile_lock);

  if (new_entries == NULL){
    goto OUT1;
  } else if (num_loose == 0) {
    free(new_entries);
    goto OUT1;
  }

  fflush(packfile);
  int fd = fileno(packfile);
  fsync(fd);

  add_entries_to_index(new_entries, num_loose, index_subdir);
  free(new_entries);
  delete_loose_files(file_paths, num_loose);
  for (int i = 0; i < num_loose; i++) {
    free(file_paths[i]);
  }

  ret_val = 0;
  goto OUT1;

  OUT1:
    fclose(packfile);
    lockfile_remove(packfile_lock);
    free(packfile_lock);
    umask(old_umask);
    return(ret_val);
}

int pack_loose_files(char *indexdir) {
  DIR *dir = opendir(indexdir);
  if (dir == NULL){
    perrorf("Error in opening directory: %s", indexdir);
    return(-1);
  }
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (entry->d_name[0] == '.' ) {
      continue;
    }
    char *path = add_path_parts(indexdir, entry->d_name);
    if (is_dir(path)) {
      pack_loose_files_in_subdir(path);
    }
    free(path);
  }
  closedir(dir);

  return 0;
}
