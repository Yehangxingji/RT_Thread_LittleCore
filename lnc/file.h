#ifndef FILE_H
#define FILE_H  

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

uint8_t *read_whole_file(const char *path, size_t *out_size);

#endif
