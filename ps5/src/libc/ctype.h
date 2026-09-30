#ifndef GBAC0RE_CTYPE_H
#define GBAC0RE_CTYPE_H

/* Freestanding ctype: plain declarations; definitions live in
   src/gba_runtime.c. */

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

#endif
