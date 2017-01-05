/**************************************************
 * string16.h
 * 
 * Prototypes for routines that are like string routines
 * but operate on strings (arrays) made up of 16-bit elements.
 * 
 * ************************************************/

#ifndef STRING16_H_
#define STRING16_H_

#include <stdio.h>		// for file I/O

typedef signed short int SYMBOL_TYPE;

typedef struct {
	int max_length;		// allocated length of array
	SYMBOL_TYPE *s;		// pointer to allocated memory
	int length;		// length of string
} STRING16;

/* Function Prototypes */
STRING16 * string16(int length);
void delete_string16( STRING16 * str16_to_delete);
int strlen16( STRING16 * s);
void set_strlen16( STRING16 * s, int len);
STRING16 * strncpy16( STRING16 *dest, STRING16 *src, int offset, int n);
char * format_string16( STRING16 *s16);
void shorten_string16( STRING16 *s16);
SYMBOL_TYPE get_symbol( STRING16 *s16, int offset);
int fread16( STRING16 *dest, int max_length, FILE *src_file);
void put_symbol( STRING16 *s16, int offset, SYMBOL_TYPE symbol);



#endif /*STRING16_H_*/
