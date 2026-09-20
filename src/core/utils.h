#ifndef UTILS_H
#define UTILS_H
#include <stddef.h>
#include <stdint.h>

void puts(const char* str);
void putc(char c);
void cls();
void beep();
int getkey_block();
void set_attr(int attr);
void int_to_str(uint32_t n, char *buf);
uint32_t rand(void);

size_t strlen(const char *str);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *ptr, int value, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

#endif