#include <stddef.h>

void *memset(void *dst, int value, size_t length)
{
    unsigned char *out = dst;

    while (length--)
        *out++ = (unsigned char)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t length)
{
    unsigned char *out = dst;
    const unsigned char *in = src;

    while (length--)
        *out++ = *in++;
    return dst;
}

int strcmp(const char *left, const char *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t length)
{
    while (length && *left && *left == *right) {
        ++left;
        ++right;
        --length;
    }
    if (!length)
        return 0;
    return (unsigned char)*left - (unsigned char)*right;
}

char *strncpy(char *dst, const char *src, size_t length)
{
    char *out = dst;

    while (length && *src) {
        *out++ = *src++;
        --length;
    }
    while (length--)
        *out++ = '\0';
    return dst;
}
