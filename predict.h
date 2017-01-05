#ifndef PREDICT_H_
#define PREDICT_H_

#define FALSE	0
#define TRUE ~FALSE
/*
 * Declarations for local procedures.
 */
int initialize_options( int argc, char **argv );
int check_compression( void );
//void print_compression( void );
void predict_test( STRING16 * test_string);
#ifdef NOTUSEDIN16BITVERSION
void remove_delimiters( char * str_input, char * str_purge);
void strpurge( char * str_in, char ch_purge);
#endif
int get_char_type( SYMBOL_TYPE symbol, int index_into_input_string);
int get_locstring_type( SYMBOL_TYPE symbol);
int get_boxstring_type( int index_into_input_string);
int get_loctimestring_type( SYMBOL_TYPE symbol);
int get_binboxstring_type( SYMBOL_TYPE symbol);
int get_bindowts_type( SYMBOL_TYPE symbol);
unsigned char neighboring_ap( SYMBOL_TYPE predicted_ap, SYMBOL_TYPE actual_ap);
int get_hhmm_from_code( SYMBOL_TYPE code, char * dest);
void test_timecode();
char * get_str_mappings(int mapping);
void build_test_string(STRING16 * test_string);
void analyze_pred_results( SYMBOL_TYPE correct_answer, SYMBOL_TYPE context);
void output_pred_results(void);
unsigned char within_time_window( SYMBOL_TYPE time1, SYMBOL_TYPE time2, int range);

/* Function Types */
#define NO_FUNCTION		0
#define PREDICT_TEST	1
#define LOGLOSS_EVAL	2

/* String Types (types of input strings) */
#define NONE			0
#define LOCSTRINGS		1
#define LOCTIMESTRINGS	2
#define BOXSTRINGS		3
#define BINBOXSTRINGS	4
#define BINDOWTS		5


/* Character (Symbol) Types */
#define LOC			0		// location
#define STRT		1		// starting time
#define DUR			2		// duration
#define DELIM		3		// delimiter


/* Research Question Types */
/* These values are used in 'research_question' variable. */
#define	WHERE		1		// "Where will Bob be at 10:00?"
#define WHEN		2		// "When will Bob be at location x?"
#endif /*PREDICT_H_*/
