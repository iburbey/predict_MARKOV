/*
 * Listing 8 -- model.h
 *
 * This file contains all of the function prototypes and
 * external variable declarations needed to interface with
 * the modeling code found in model-1.c or model-2.c.
 */
#ifndef MODEL_H_
#define MODEL_H_

/*
 * Eternal variable declarations.
 */
extern int max_order;
extern int flushing_enabled;

#include "string16.h"
#include "coder.h"

/*
 * Definitions
 */
#define MAX_NUM_PREDICTIONS	1500	// Maximum number of predictions
#define MAX_DEPTH	3				// Maximum depth of model (which is also the longest context)
#define MAX_MODEL_COUNT	500		// used in count_model
#define true 1
#define false 0
#define MAX_STRING_LENGTH	30000

// The symbols rangs for the binary box strings
// (from build_16bit_boxstrings.py)
// 27Apr2009 Changed for 1 minute thresholds!!!
#define INITIAL_START_TIME	0x2620			// was 0x2120
#define FINAL_START_TIME	0x2DFF			// was 0x21FF
#define INITIAL_DURATION	0x2220			// not used for MELT version
#define FINAL_DURATION		0x22FF
#define INITIAL_LOCATION	0x2320
#define FINAL_LOCATION		0x25FF
#define LOWEST_SYMBOL		INITIAL_LOCATION	// was INITIAL_START_TIME
#define RANGE_OF_SYMBOLS	FINAL_START_TIME-LOWEST_SYMBOL 	// was FINAL_LOCATION-LOWEST_SYMBOL

/*
 * This program consumes massive amounts of memory.  One way to
 * handle large amounts of memory is to use Zortech's __handle
 * pointer type.  So that my code will run with other compilers
 * as well, the handle stuff gets redefined when using other
 * compilers.
 */
#ifdef __ZTC__
#include <handle.h>
#else
#define __handle
#define handle_calloc( a )        calloc( (a), 1 )
#define handle_realloc( a, b )    realloc( (a), (b) )
#define handle_free( a )          free( (a) )
#endif

/* A context table contains a list of the counts for all symbols
 * that have been seen in the defined context.  For example, a
 * context of "Zor" might have only had 2 different characters
 * appear.  't' might have appeared 10 times, and 'l' might have
 * appeared once.  These two counts are stored in the context
 * table.  The counts are stored in the STATS structure.  All of
 * the counts for a given context are stored in and array of STATS.
 * As new characters are added to a particular contexts, the STATS
 * array will grow.  Sometimes, the STATS array will shrink
 * after flushing the model.
 */
typedef struct {
                SYMBOL_TYPE symbol;	// was unsigned char
                int counts;			// was 'unsigned char', but rescaling set some level 0 counts to 0
               } STATS;

/*
 * Each context has to have links to higher order contexts.  These
 * links are used to navigate through the context tables.  For example,
 * to find the context table for "ABC", I start at the order 0 table,
 * then find the pointer to the "A" context table by looking through
 * then LINKS array.  At that table, we find the "B" link and go to
 * that table.  The process continues until the destination table is
 * found.  The table pointed to by the LINKS array corresponds to the
 * symbol found at the same offset in the STATS table.  The reason that
 * LINKS is in a separate structure instead of being combined with
 * STATS is to save space.  All of the leaf context nodes don't need
 * next pointers, since they are in the highest order context.  In the
 * leaf nodes, the LINKS array is a NULL pointers.
 */
typedef struct {
                 struct context *next;
               } LINKS;

/*
 * The CONTEXT structure holds all of the known information about
 * a particular context.  The links and stats pointers are discussed
 * immediately above here.  The max_index element gives the maximum
 * index that can be applied to the stats or link array.  When the
 * table is first created, and stats is set to NULL, max_index is set
 * to -1.  As soon as single element is added to stats, max_index is
 * incremented to 0.
 *
 * The lesser context pointer is a navigational aid.  It points to
 * the context that is one less than the current order.  For example,
 * if the current context is "ABC", the lesser_context pointer will
 * point to "BC".  The reason for maintaining this pointer is that
 * this particular bit of table searching is done frequently, but
 * the pointer only needs to be built once, when the context is
 * created.
 */
typedef struct context {
                         int max_index;
                         LINKS __handle *links;
                         STATS __handle *stats;
                         struct context *lesser_context;
                       } CONTEXT;

/*
 * This structure holds the results of a prediction, including the predicted
 * next symbol and it's probability (prob_numerator/prob_denominator).
*/
typedef struct {
                unsigned short int symbol;		// The symbol
                int prob_numerator;  // numerator of the probability
                				// the denominator is in the containing structure
               } STRUCT_PREDICTED_SYMBOL;

/*
 * This structure holds a prediction, including a list of predicted symbols, with
 * their associated probabilities, the context string used to find the context,
 * and the context level at which the context was found.  (context_level = length
 * of the context string)  We are returning the context string in case (in the future)
 * we manipulate
 * the given context string to find a similar string with better probabilities.
 */
typedef struct {
	struct {
		SYMBOL_TYPE symbol;		// The symbol
	    int prob_numerator;  // numerator of the probability
	          				// the denominator is in the containing structure
	    } sym[MAX_NUM_PREDICTIONS];
	//unsigned char context_string_used[MAX_DEPTH];
	int depth;				// context level at which this prediction was made
	int num_predictions;	// number of elements in sym[] array.
    int prob_denominator;	// denominator of the probability
} STRUCT_PREDICTION;


/*
 * Prototypes for routines that can be called from MODEL-X.C
 */
void initialize_model( void );
void update_model( SYMBOL_TYPE symbol );
void clear_current_order(void);
int convert_int_to_symbol( SYMBOL_TYPE c, SYMBOL *s );
//void get_symbol_scale( SYMBOL *s );
//int convert_symbol_to_int( int count, SYMBOL *s );
void add_character_to_model( SYMBOL_TYPE c );
void flush_model( void );
void recursive_count(int depth,  CONTEXT *table, int research_question, int verbose );
void count_model(int research_question, int verbose);
void print_model(void);
void recursive_print( int depth, CONTEXT * table);
float probability( SYMBOL_TYPE c, STRING16 * context_string, char verbose);
unsigned char predict_next(STRING16 * context_string, STRUCT_PREDICTION * results);
void print_model_allocation();
void traverse_tree( STRING16 * context_string);
void clear_scoreboard(void);
float compute_logloss( STRING16 * test_string, int verbose);






#endif
