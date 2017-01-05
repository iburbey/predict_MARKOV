/*
 * Listing 1 -- coder.h
 *
 * This header file contains the constants, declarations, and
 * prototypes needed to use the arithmetic coding routines.  These
 * declarations are for routines that need to interface with the
 * arithmetic coding stuff in coder.c
 *
 */
#ifndef CODER_H_
#define CODER_H_


#define MAXIMUM_SCALE   16383  /* Maximum allowed frequency count */
#define ESCAPE          0xFFFF	/* was 256 for char version */    /* The escape symbol               */
#define DONE            -1     /* The output stream empty  symbol */
#define FLUSH           -2     /* The symbol to flush the model   */

/*
 * A symbol can either be represented as an int, or as a pair of
 * counts on a scale.  This structure gives a standard way of
 * defining it as a pair of counts.
 */
typedef struct {
                unsigned short int low_count;
                unsigned short int high_count;
                unsigned short int scale;
               } SYMBOL;

extern long underflow_bits;    /* The present underflow count in  */
                               /* the arithmetic coder.           */
/*
 * Function prototypes.
 */
void initialize_arithmetic_decoder( FILE *stream );
void remove_symbol_from_stream( FILE *stream, SYMBOL *s );
void initialize_arithmetic_encoder( void );
void encode_symbol( FILE *stream, SYMBOL *s );
void flush_arithmetic_encoder( FILE *stream );
short int get_current_count( SYMBOL *s );

#endif
