#include <unistd.h>

#include "../src/bitmap.h"
#include "../src/util.h"

int main(int argc, char **argv){
  FILE *f;

  if (argc == 2) {
    f = fopen(argv[1], "r");
  } else if (argc < 2 && !isatty(fileno(stdin))) {
    f = stdin;
  } else {
    printf("Usage: \n"
           " %s <logfile>\n"
	   " echo <string> | %s\n", argv[0], argv[0]);
    return 1;
  }

  if(f == NULL) {
      perror("Error: File not opened");
      return(-1);
  }

  uint8_t *bitmap = init_bitmap();
  int ret = apply_file_to_bitmap(bitmap, f);
  if (ret == GZ_TRUNCATED) {
    fprintf(stderr, "gzip stream truncated\n");
    return GZ_TRUNCATED;
  }

  write_bitmap(bitmap, stdout);

  return 0;
}
