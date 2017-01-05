/*******************************************************
 * string16.c
 * 
 * This module duplicates common string routines for arrays
 * of 16-bit characters.
 * 
 * HISTORY:
 * 29Oct07 ink Created.
 * 
 * *****************************************************/
#include <stdlib.h>			// for calloc(), free()
#include <assert.h>			// for assert()
#include <stdio.h>			// for sprintf()
#include "model.h"			// for MAX_STRING_LENGTH definition
#include "string16.h"

char printable_string16[5 * MAX_STRING_LENGTH+1]; 

/* Constructor string16
 * Create a string of 16-bit values; return a pointer to the string 
 * NULL means error.
 * */
STRING16 * string16(int length){
	STRING16 * new_string16;
	SYMBOL_TYPE * ptr_array;
    ptr_array = (SYMBOL_TYPE *) calloc( sizeof( SYMBOL_TYPE ), length);
    if (ptr_array == NULL)
    	return NULL;
    new_string16 = (STRING16 *) calloc(sizeof(STRING16), 1);
    new_string16->max_length = length;
    new_string16->s = ptr_array;
    return(new_string16);
}

/* Deconstructor - Delete string
 */
void delete_string16( STRING16 * str16_to_delete)	{
	free( str16_to_delete->s);	// de-allocate the array
	free (str16_to_delete);		// de-allocate the structure
	str16_to_delete = NULL;
}

/* strlen16 - Return the length of the 16-bit 'string' */
int strlen16( STRING16 * s) {
	return s->length;
}

/* set_strlen16 - set the length of the 16-bit string */
void set_strlen16( STRING16 * s, int len) {
	s->length = len;
}

/* strncpy16 - Copy n elements
 * INPUT:  dest = where to store the n elements (pointer to another STRING16)
 * 			src = source
 * 			offset = offset into src (where to start the cpy)
 * 			n = number of elements to copy
 */
STRING16 * strncpy16( STRING16 *dest, STRING16 *src, int offset, int n)	{
	int i;
	
	assert(offset+n <= src->max_length);
	for (i=0; i < n; i++)
		dest->s[i] = src->s[offset+i];
	dest->length = n;
	return( dest);
}

/* format a STRING16 into a string of printable characters
 * FYI: Currently, this routine stores the result in global
 * memory to avoid memory leakage (if I allocated the string)
 */ 
char * format_string16( STRING16 *s16)	{
	char * dest;
	SYMBOL_TYPE * src;
	int i, j;
	
	dest = printable_string16;
	src = s16->s;

	for (i = 0; i < s16->length && i < 5 * MAX_STRING_LENGTH; ) {
		for (j=0; j < 8 && (s16->length-i > 0); j++, i++)	{
			sprintf( dest, "%04x ",  *src);
			dest += 5;
			src++;
			}
		//*dest++ = '\n';
	}
	*dest = '\0';		// terminate string
	return (printable_string16);
}

/* Remove the first symbol from the string, shortening it by one symbol */
void shorten_string16( STRING16 *s16){
	int i;
	
	// (Use length+1 to copy the terminating null char)
	for (i=0; i < s16->length+1; i++)
		s16->s[i] = s16->s[i+1];
	s16->length--;
}

/* Return the symbol at the given offset of the given string. */
SYMBOL_TYPE get_symbol( STRING16 *s16, int offset){
	assert( offset <= s16->length);
	return(s16->s[offset]);
}

/* Put the symbol at the given offset of the given string. */
void put_symbol( STRING16 *s16, int offset, SYMBOL_TYPE symbol){
	//assert( offset <= s16->length);
	s16->s[offset]=symbol;
}


/* read from a file into a STRING16 structure.
 * It's assumed that the file contains nothing but 16-bit signed
 * integers.
 */
int fread16( STRING16 *dest, int max_length, FILE *src_file){
	int i;
	
	i = fread( dest->s, sizeof( SYMBOL_TYPE), max_length, src_file);
	dest->length = i;
	dest->s[ i ] = 0x00;		// terminate 'string' with a null. (this should 
								// remove the final 0x0A linefeed char.
	return(i);
}

