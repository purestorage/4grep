#ifndef PACKFILE_INCLUDED
#define PACKFILE_INCLUDED

#include <stdio.h>
#include <stdint.h>

/*--------------------------------------------------------------------*/

#define PACKFILE_NAME "packfile"
#define PACKFILE_INDEX_NAME "packfile_index"
#define TEMP_PACKFILE_INDEX_NAME ".packfile_index.tmp"
#define PACKFILE_LOCK_NAME ".packfile.lock"
#define EMPTY_FILE 1

/*--------------------------------------------------------------------*/

int is_corrupted(FILE* loosefile);

uint8_t *read_from_packfile(char *filename, int64_t mtime, char *store);

int pack_loose_files(char *indexdir);

int pack_loose_files_in_subdir(char *index_subdir);

int remove_if_corrupted(FILE *file, char *file_path);

/*--------------------------------------------------------------------*/

#endif
