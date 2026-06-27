#include "file.h"

//读取文件内容
uint8_t *read_whole_file(const char *path, size_t *out_size)
{
    FILE *fp;
    long file_size;
    uint8_t *buffer;
    size_t read_size;

    fp = fopen(path, "rb");
    if (fp == NULL)
    {
        printf("fopen failed: %s, errno=%d\n", path, errno);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        printf("fseek end failed\n");
        fclose(fp);
        return NULL;
    }

    file_size = ftell(fp);
    if (file_size <= 0)
    {
        printf("invalid file size: %ld\n", file_size);
        fclose(fp);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        printf("fseek set failed\n");
        fclose(fp);
        return NULL;
    }

    buffer = (uint8_t *)malloc((size_t)file_size);
    if (buffer == NULL)
    {
        printf("malloc failed, size=%ld\n", file_size);
        fclose(fp);
        return NULL;
    }

    read_size = fread(buffer, 1, (size_t)file_size, fp);
    fclose(fp);

    if (read_size != (size_t)file_size)
    {
        printf("fread failed, read=%lu expected=%ld\n",
               (unsigned long)read_size,
               file_size);
        free(buffer);
        return NULL;
    }

    *out_size = read_size;
    return buffer;
}

