/*
 * predict.c
 *
 * This module is the driver program for a variable order
 * finite context compression program.  The maximum order is
 * determined by command line option.  This particular version
 * also monitors compression ratios, and flushes the model whenever
 * the local (last 256 symbols) compression ratio hits 90% or higher.
 *
 * This code is based on the program comp-2.c, by Bob Nelson.
 * It has been adapted to do predictions instead of compression.
 * It builds the variable order markov model, but doesn't output
 * an encoded bit stream.
 *
 * To build this program, see similar command lines for comp-2.c.
 *
 * Command line options:
 *
 *  -f text_file_name  [defaults to test.inp]
 *  -o order [defaults to 3 for model-2]
 *  -logloss test_file_name    	# Calculate average-log loss for the given test string.
 *  -p test_file_name			# Run a prediction for each char of given test string.
 *  -v							# verbose mode (prints extra info to stdout
 * -delimiters string_of_delimeters	# characters in this string are ignored in prediction results.
 * (The -delimeters option is not supported in the 16bit version)
 * -input_type representation_type		# denotes type of input.  If verbose is set, outputs change by representation used.
 * -when				        # if this argument is included, the code will flip the sequence from a T1,L1,T2,L2 string 
 *  							# to L1,T1,L2,T2... (where L=location and T=time), train the model.
 * 								# This option ASSUMES that the input_type is binboxstrings.
 * -c confidence_level			# Currently implemented only for the WHEN case.  confidence_level == -1 ignores the
 * 								# the confidence level.  Values between 0 and 100 are used to determine which of the
 * 								# returned predictions to use.  0 means use all predictions.  Any other value means
 * 								# use all predictions whose probabilities sum to greater than or equal to the confidence 
 * 								# level.  Ex. If confidence level is 80% and the predictions returned 75%, 15%, 10%, it would
 * 								# use the first two predictions (sum=90%) but not the third.
 *
 * *
 * 22Apr2010 ink Number of predictions are written to num_pred.xml
 * 				 time deltas (in a wrong prediction, the difference between the right
 *				 time answer (WHEN) and the predicted time (MostProbables) is calculated and written
 * 				 to time_deltas.xml.
 * 26Apr2010 ink Added the confidence_level option.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>		// for log10() function;
#include <assert.h>		// for assert()
#include "coder.h"
#include "model.h"
//include <bitio.h>
#include "predict.h"
#include "string16.h"
#include "mapping.h"	// for ap mapping, ap neighbors, timeslot mapping
char str_representations[][21]={"Unknown","Locstrings","Loctimestrings","Boxstrings","Binboxstrings", "BinDOWts"};
char str_mappings[][6] = {"LOC", "STRT", "DUR", "DELIM"};

#define COUNT_NUMBER_OF_PREDICTIONS_RETURNED		// to write to num_pred.csv file.
/*
 * The file pointers are used throughout this module.
 */
FILE *training_file;		// File containing string to train on.
FILE *test_file;			// File to test against (form future predictions)
FILE *num_pred_file;		// file to write out the number of predictions
							// returned for each test.
FILE *time_deltas_file;		// file to write out the time difference between
							// time predictions andthe right answer.
char test_file_name[ 81 ];

char verbose = FALSE;		// if true, print out lots of info
int confidence_level = -1;	// 0 < value < 100, -1 means don't use it.
							// confidence_level == 0 means use all returned predictions.
							// confidence_level between 0 and 100 means use most probable
							// predictions until sum of probabilities > confidence_level.
							// current implementation uses confidence_level in WHEN case only.
//unsigned int str_delimiters[10];		// delimeters to ignore in prediction tests.
int  representation;		// specified with -input_type argument.
int  research_question;		// WHERE or WHEN will someone be next.

/*
 * Counters used to count up the results of the predictions
 */
int num_tested=0, num_right=0;		// test results
int NumTests = 0;
int FallbackNumCorrect = 0;  // number of times it fell to level 0 and was still right.
int FallbackNum = 0;	// number of times model went to level 0 for a prediction
//Counters for most likely predictions
int MostProb_NumCorrect = 0;		// # times one of the most probably predictions was right
									// (also used to count confidence_level predictions)
int MostProb_NeighborCorrect = 0;	// number of times the most prob prediction was wrong, but one of
									// it's neighboring APs is correct answer (WHERE only)
int MostProb_Within10Minutes = 0;	// (WHERE case) # times right answer within 10 minutes of one
									// of the predictions
int MostProb_Within20Minutes = 0;	// (WHERE case) # times right answer is within 20 minutes of
									// one of the most likely predictions
int MostProb_MultiplePredictions = 0; // # times the Most Probable list included > 1 prediction.
// Counters for the less likely predictions
int LessProb_NumCorrect = 0;		// # times one of the less probable predictions was right
int LessProb_NeighborCorrect = 0;	// number of times one of the less prob prediction was wrong, but one of
									// it's neighboring APs is correct answer (WHERE only)
int LessProb_Within10Minutes = 0;	// (WHERE case) # times right answer within 10 minutes of one
									// of the predictions
int LessProb_Within20Minutes = 0;	// (WHERE case) # times right answer is within 20 minutes of
									// one of the most likely predictions
int LessProb_MultiplePredictions = 0; // # times the Less Probable list included > 1 prediction.

int number_multiple_predictions = 0; //number fo times model made > 1 prediction for a given time.
int number_times_neighbors_are_correct = 0;	// number of times the prediction is a neighbor of actual
STRUCT_PREDICTION pred;			// structure containing predictions

/*
 * The main procedure is similar to the main found in COMP-1.C.
 * It has to initialize the coder, the bit oriented I/O, the
 * standard I/O, and the model.  It then sits in a loop reading
 * input symbols and encoding them.  One difference is that every
 * 256 symbols a compression check is performed.  If the compression
 * ratio exceeds 90%, a flush character is encoded.  This flushes
 * the encoding model, and will cause the decoder to flush its model
 * when the file is being expanded.  The second difference is that
 * each symbol is repeatedly encoded until a succesfull encoding
 * occurs.  When trying to encode a character in a particular order,
 * the model may have to transmit an ESCAPE character.  If this
 * is the case, the character has to be retransmitted using a lower
 * order.  This process repeats until a succesful match is found of
 * the symbol in a particular context.  Usually this means going down
 * no further than the order -1 model.  However, the FLUSH and DONE
 * symbols do drop back to the order -2 model.  Note also that by
 * all rights, add_character_to_model() and update_model() logically
 * should be combined into a single routine.
 */
int main( int argc, char **argv )
{
     SYMBOL_TYPE c, c1, c2;
     int function;		// function to perform
     STRING16 * test_string;

     int i;				// general purpose register

#ifdef THIS_SPACE_RESERVED_FOR_TEST_CODE
     test_timecode();		// Test routines 
     exit( 0 );
#endif     
     
    /* Initialize ********************************************/
    printf("<Run>\n");			// start of XML element
     function = initialize_options( --argc, ++argv );
    initialize_model();
    test_string = string16(MAX_STRING_LENGTH+1);
    
#ifdef COUNT_NUMBER_OF_PREDICTIONS_RETURNED
    /*** Use this code to count the number of predictions returned for each test.
     * They are written into a separate, comma-delimited file.
     */
    num_pred_file = fopen("num_pred.csv", "a");		// should really check for errors
    if (ftell(num_pred_file) == 0)  {
    	//fprintf(num_pred_file, "<?xml version=\"1.0\" standalone=\"yes\" ?>\n");
    	if (confidence_level < 0)
    		fprintf(num_pred_file,"test_file_name, num_best_predictions, num_less_predictions, pred.num_predictions\n");
    	else
    		fprintf(num_pred_file,"test_file_name, confidence_level, num_conf_predictions, total_num_predictions\n");
    }
    /*
   * End of code to count number of predictions.
   *******************/
#endif
    
    /* Train the model on the given input training file ***********/
    if (research_question == WHERE) {
	    for ( ; ; )
	    {
     		if (fread(&c, sizeof(SYMBOL_TYPE),1,training_file) == 0)
    			c = DONE;
    		/* NOTE: fread() seems to skip over whitespace chars, so be careful what's in your bin file. */
    		//printf("Training on 0x%04x\n", c);
            /*** The 16-bit version does not currently support delimiter-removal
            **  if (strchr(str_delimiters, c) != NULL)	{
    		** 	//printf("Skipping training on '%c'\n",c);
    		**	continue;					// This char is a delimiter, go onto the next character
    		** }
    		****************/
	//       if ( c == EOF  || c == '\n' || c == 0x0a)	// skip line feeds
	//            c = DONE;
	       	clear_current_order();
	        if ( c == DONE )
	        	break;
	        update_model( c );		//because current order is 0, this updates the counters in the level-0 table (I THINK)
	        add_character_to_model( c );
	    }
    } else {	// research_question == WHEN
    	// This portion of the code ASSUMES that the input string is a 'binbox' representation
    	// of the format T1,L1,T2,L2... where Tx is the time for pair x, and Lx is the location at 
    	// that time.   For the 'when' question, the pairs need to be flipped so that the sequence
    	// looks like L1,T1,L2,T2, etc.  Instead of re-processing the input data, I'm going to read
    	// the sequence in pairs and flip them.
	    for ( ; ; )
	    {
     		if (fread(&c1, sizeof(SYMBOL_TYPE),1,training_file) == 0)  // Read first char
	    		c = DONE;
	       	clear_current_order();
	        if ( c == DONE )
	        	break;
     		if (fread(&c2, sizeof(SYMBOL_TYPE),1,training_file) == 0)  // Read second char
	    		c = DONE;
	        if ( c == DONE )
	        	break;

	        // Train on second char in pair
	        update_model( c2 );
	        add_character_to_model( c2 );
	        // Train on first char in pair
	       	clear_current_order();
	        update_model( c1 );
	        add_character_to_model( c1 );
	    }
    }

    /*** Print information about the model */
    if (verbose)  {
        //print_model_allocation();
    	//print_model();
    										// for this research question
    }
	//count_model(research_question, verbose);		// print out number of tables and child tables

	/***************************************/

    /* Trying some probabilities....
	printf("PROBABILITIES\n");
	probability( 'r', "ab", verbose);
	probability( 'a', "ac", verbose);
	probability( 'a', "ad", verbose);
	probability( 'a', "br", verbose);
	probability( 'd', "ca", verbose);
	probability( 'b', "da", verbose);
	probability( 'c', "ra", verbose);
	probability( 'b', "a", verbose);
	probability( 'c', "a", verbose);
	probability( 'd', "a", verbose);
	probability( 'r', "b", verbose);
	probability( 'a', "c", verbose);
	probability( 'a', "d", verbose);
	probability( 'a', "r", verbose);
	probability('a',"", verbose);
	probability('r',"ac", verbose);
	probability( 'b', "a", verbose);
	probability( 'r', "ab", verbose);
*8*****/
//	probability( 'V', "01", verbose);

	/* Try some predictions
	printf("PREDICTIONS\n");

	// Try known predictions
	predict_next("ab", & pred);
	predict_next("br", & pred);
	predict_next("a", & pred);
	predict_next("b", & pred);
	predict_next("", & pred);

	// Try unseen context
	predict_next("ar", & pred);

	// Try an unknown symbol in the context
	predict_next("xy", & pred);

	******************************************/

	switch (function)	{
    	case PREDICT_TEST:
    		// read test file
    		i = fread16( test_string, MAX_STRING_LENGTH, test_file);
    		if (i == MAX_STRING_LENGTH)
    			fprintf(stderr,"Test String may be over max length and may have been truncated.\n");
    		predict_test(test_string);
    		break;
    	case LOGLOSS_EVAL:
    		// read test file
    		//fgets(test_string, MAX_STRING_LENGTH, test_file);
    		i = fread16( test_string, MAX_STRING_LENGTH, test_file);
    		if (i == MAX_STRING_LENGTH)
    			fprintf(stderr,"Test String may be over max length and may have been truncated.\n");
    		printf("%d, %f\n", max_order, compute_logloss(test_string, verbose));
    		break;
     	case NO_FUNCTION:
    	default:
    		break;
    	}
    if (!verbose)
    	printf("</Run>\n");	// End of xml element
#ifdef COUNT_NUMBER_OF_PREDICTIONS_RETURNED
    fclose(num_pred_file);
#endif    
    exit( 0 );
}

/*
 * This routine checks for command line options, and opens the
 * input and output files.  The only other command line option
 * besides the input and output file names is the order of the model,
 * which defaults to 3.
 *
 * Returns the function to perform
 */
int initialize_options( int argc, char **argv )
{
    char training_file_name[ 81 ];
    //char test_file_name[ 81 ];
    int function = NO_FUNCTION;
    char str_type[41];
    char str_dir[41], *str_file = str_dir;
    int temp;

    	/* Set Defaults */
#ifdef NOT_USED_IN_16_BIT_VERSION
    str_delimiters[0] = '\0';		// clear delimeter string
#endif
    str_type[0] = '\0';				// clear string_type
    representation = NONE;
    research_question = WHERE;		// 'Where will Bob be at 10:00?' type questions.

//    strcpy( training_file_name, "test.inp" );
    while ( argc > 0 )    {
    	// -f <filename> gives the training file name
        if ( strcmp( *argv, "-f" ) == 0 ) 	{
        	argc--;
        	strcpy( training_file_name, *++argv );
        	if (verbose)
        		printf("Training on file %s\n", training_file_name);
        	}
        // -p <filename> gives the test filename to predict against
       else if ( strcmp( *argv, "-p" ) == 0 ) {
    	   	argc--;
    	   	strcpy( test_file_name, *++argv );
    	    test_file = fopen( test_file_name, "rb");
    	    if ( test_file == NULL )
    	    	{
    	        printf( "Had trouble opening the testing file (option -p)\n" );
    	        exit( -1 );
    	    	}
    	    setvbuf( test_file, NULL, _IOFBF, 4096 );
    	    function = PREDICT_TEST;
    	    if (verbose)
    	    	printf("Testing on file %s\n", test_file_name);
    	    else
    	    	str_file = strrchr(test_file_name,'/')+1;	// pointer to the filename in string
    	    	printf("   <TestFile>%s</TestFile>\n", str_file);
    	    	strcpy(str_dir, test_file_name);
    	    	str_dir[strlen(str_dir) - strlen(str_file)]='\0';
    	    	printf("   <SourceDir>%s</SourceDir>\n", str_dir);
      		}
        // -o <order>
        else if ( strcmp( *argv, "-o" ) == 0 )
        	{
        	argc--;
            max_order = atoi( *++argv );
            // IN this version of the code, where we put time in context and
            // ask the model to predict loc (assuming time,loc pairs), the max_order
            // needs to be an odd number.
            if (max_order%2 == 0)
            	printf("max_order should be an odd value!\n");
        	}
        // -v
        else if ( strcmp( *argv, "-v" ) == 0 )
        	{
            verbose = TRUE;			// print out prediction information
        	}
        // -logloss <test filename>
         else if ( strcmp( *argv, "-logloss" ) == 0 )
         	{
     	   	argc--;
     	   	strcpy( test_file_name, *++argv );
     	    test_file = fopen( test_file_name, "rb");
     	    if ( test_file == NULL )
     	    	{
     	        printf( "Had trouble opening the testing file (option -logloss)\n" );
     	        exit( -1 );
     	    	}
     	    setvbuf( test_file, NULL, _IOFBF, 4096 );
            function = LOGLOSS_EVAL;
         	}
        // -c <level> gives a confidence level. 
        // value needs to be -1 to shut it off or between 0 (use all predictions) and 100.
         else if ( strcmp( *argv, "-c" ) == 0 ) 	{
        	argc--;
            temp = atoi( *++argv );
            if (temp > 100) {
            	fprintf(stderr, "Confidence level %d is out of range.  Should be between 0 and 100 (inclusive) or -1\n", temp);
            	fprintf(stderr, "Ignoring the confidence level argument.\n");
            	temp = -1;
            }
            else if (temp < 0)
            	temp = -1;
            confidence_level = temp;
        	if (verbose)
        		printf("Confidence level is %d\n", confidence_level);
        	}
#ifdef DELIMITER_CODE_NOT_SUPPORTED_IN_16BIT_MODEL
    	// -d <string> ignore prediction of delimeters in string
         else if ( strcmp( *argv, "-d" ) == 0 ) 	{
        	argc--;
        	strcpy( str_delimiters, *++argv );
        	if (verbose)
        		printf("Ignoring delimeters \"%s\"\n", str_delimiters);
        	}
#endif
    	// -input_type <string_type>  Indicate type of input strings used.
        // Choices include "locstrings", "boxstrings", "loctimestrings"
         else if ( strcmp( *argv, "-input_type" ) == 0 ) 	{
        	argc--;
        	strcpy( str_type, *++argv );
        	if (strcmp(str_type, "locstrings") == 0)
        		representation = LOCSTRINGS;
        	else if (strcmp(str_type, "loctimestrings")== 0)
        		representation = LOCTIMESTRINGS;
        	else if (strcmp(str_type, "boxstrings") == 0)
            	representation = BOXSTRINGS;
        	else if (strcmp(str_type, "binboxstrings") == 0)
            	representation = BINBOXSTRINGS;
        	else if (strcmp(str_type, "bindowts") == 0)
            	representation = BINDOWTS;
        	else
        		representation = NONE;
           	if (verbose)
       			printf("Input string type is %d (%s)\n",
        				representation,
        				str_representations[ representation]);
        	}
        // -when
        else if ( strcmp( *argv, "-when" ) == 0 )    	{
            research_question = WHEN;
        	}
        else
        	{
            fprintf( stderr, "\nUsage: predict [-o order] [-v] [-logloss predictfile] " );
            fprintf( stderr, "[-f text file] [-p predictfile] [-input_type string_type] [-when]\n" );
            fprintf( stdout, "\nUsage: predict [-o order] [-v] [-logloss predictfile] " );
            fprintf( stdout, "[-f text file] [-p predictfile] [-input_type string_type] [-when]\n" );
             exit( -1 );
        	}
        argc--;
        argv++;
    	}
    if (verbose)
    	printf("Research question is %s\n", (research_question==WHERE)? "WHERE":"WHEN");
    else
    	printf("   <ResearchQuestion>%s</ResearchQuestion>\n",(research_question==WHERE)? "WHERE":"WHEN");
    training_file = fopen( training_file_name, "rb" );
    if (verbose)
    	fprintf(stdout,"%s\n", training_file_name);
    else
    	printf("   <TrainingFile>%s</TrainingFile>\n", strrchr(training_file_name, '/')+1);
    if ( training_file == NULL  )
    	{
        printf( "Had trouble opening the input training file %s!\n", training_file_name );
        exit( -1 );
    	}
    // Setup full buffering w/ a 4K buffer. (for speed)
    setvbuf( training_file, NULL, _IOFBF, 4096 );
    setbuf( stdout, NULL );
    return( function );
   }

/*******************************************
 * predict_test
 *
 * Given a test string, test each character of the string.
 * For example, if the test string is "abc", call
 * predict_next() for each substring:
 * 		predict_next("", &pred);
 * 		predict_next("a", &pred);
 * 		predict_next("ab", & pred);
 *
 * This version (predict_MELT) has been modified to use the
 * first character in each pair as context and the second to predict.
 * (Of course, this assumes 1st order, and a representation of
 * <time,loc> pairs (aka binboxstrings).)
 * INPUTS:
 * 	  test_string = pointer to string to test.
 *
 * RETURNS: nothing
 * *********************************************/
void predict_test( STRING16 * test_string){
	int i;			// index into test string
	int length;		// string length
	SYMBOL_TYPE predicted_char;
    STRING16 *str_sub;		// sub-strings (chunks of context)
	
    //printf("Original test string %s\n", format_string16(test_string));
    // initialize
    str_sub = string16(max_order);			// allocate memory for sub-string
    length = strlen16( test_string);
    build_test_string( test_string );		// if WHEN, flip string
    
    /***********
     * LOOP
     ***********/
    // Go through test string, and try to predict every other symbol
    // using the context of the preceding symbol.
    // note that the loop starts with 1 because we are using char 0 for context only,
    // not prediction.  (This loop works for higher orders.)
  
    for (i=max_order; i < length; i+= 2, NumTests++)	{	
		// Copy the symbols preceding the test-symbol into str_sub.
		if (i < max_order)
			strncpy16( str_sub, test_string,0, i);	// create test string
		else
			strncpy16( str_sub, test_string, i-max_order, max_order);

		/****************************************
		 * DO THE PREDICTION
		 ***************************************/
		predicted_char = predict_next(str_sub, &pred);
		//printf("%s!\n\n", predicted_char == get_symbol( test_string, i) ?  "RIGHT" : "WRONG");
		
		/****************************************
		 * Analyze the results
		 ****************************************/
		analyze_pred_results( get_symbol(test_string,i), get_symbol(test_string,i-1));
    }
    /***********************************************
     * Output the results
     ***********************************************/
    output_pred_results();
	return;
}	// end of predict_test


/********************************************************************
 * get_char_type
 *
 * Return the type of character just predicted.
 * INPUTS: representation
 * 			expected symbol
 * 			index into input string
 * OUTPUTS: next_type_index is changed for loctimestrings
 * RETURNS: 0 = Location
 * 			1 = Starting Time
 * 			2 = Duration
 * 			3 = Delimiter
 ************************************************************************/
int get_char_type( SYMBOL_TYPE symbol, int index_into_input_string)
{
	switch (representation)	{
	case LOCSTRINGS:
		return( get_locstring_type( symbol));
	case BOXSTRINGS:
		return( get_boxstring_type( index_into_input_string));
	case LOCTIMESTRINGS:
		return( get_loctimestring_type( symbol));
	case BINBOXSTRINGS:
		return( get_binboxstring_type( symbol));
	case BINDOWTS:
		return( get_bindowts_type( symbol));
	default:	// If we don't know the string type, we can't figure out the character type
		return DELIM;
	}
}
/**************************************************************************
 * get_locstring_type
 * Given the character, return the character type (delimiter or location)
 *
 * INPUTS: symbol in input string
 * OUTPUTS: None
 * RETURNS: DELIM for a delimiter
 * 			LOC for a location char
 **************************************************************************/
int get_locstring_type( SYMBOL_TYPE symbol)
{
	if (symbol == (SYMBOL_TYPE) ':')
		return (DELIM);
	else
		return (LOC);
}

/**************************************************************************
 * get_boxstring_type
 * Given the index into the test string, return the character type (delimiter or location)
 *
 * INPUTS:  index into input string
 * OUTPUTS: None
 * RETURNS:
 * 			LOC for a location char
 * 			STRT for a starting time character
 * 			DUR	for a duration character
 **************************************************************************/
int get_boxstring_type( int index_into_input_string)
{
	switch (index_into_input_string % 6)
	{
	case 0: case 1:		return( STRT);
	case 2: case 3:		return( LOC);
	case 4: case 5:		return( DUR);
	default:	// Error!
		printf("Error in get_box_string_type\n");
		return -1;
	}
}

/**************************************************************************
 * get_loctimestring_type
 * Given the character (symbol),
 * return the character type (delimiter, location, etc)
 *
 * Loctimestrings look like this:
 *    L}tt:tt~dd:dd
 * where L is a location, tt:tt is the starting time and dd:dd is the duration.
 * INPUTS: 	symbol in input string
 *
 * OUTPUTS: None
 * RETURNS: DELIM for a delimiter
 * 			LOC for a location char
 **************************************************************************/
int get_loctimestring_type( SYMBOL_TYPE symbol)
{
	static int next_type_index = 0;
	static int types[] = {LOC, STRT, STRT, STRT, STRT, DUR, DUR, DUR, DUR};
	int result;

	if (symbol == (SYMBOL_TYPE) '}' || symbol == (SYMBOL_TYPE) ':' ||
			symbol ==  (SYMBOL_TYPE)'~' || symbol == (SYMBOL_TYPE) ';')
		return (DELIM);

	else {
		result = types[ next_type_index];
		next_type_index = (next_type_index + 1) % 9;	// point to the next type
		return(result);
	}
}

/**************************************************************************
 * get_binboxstring_type
 * Given the character, return the character type (delimiter or location)
 *
 * INPUTS: symbol in input string
 * OUTPUTS: None
 * RETURNS: DELIM for a delimiter
 * 			LOC for a location char
 **************************************************************************/
int get_binboxstring_type( SYMBOL_TYPE symbol)
{

	if (symbol >= INITIAL_START_TIME && symbol <= FINAL_START_TIME)
		return(STRT);
	if (symbol >= INITIAL_DURATION && symbol <= FINAL_DURATION)
		return(DUR);
	if (symbol >= INITIAL_LOCATION && symbol <= FINAL_LOCATION)
		return(LOC);

	// This is an error. We should never get here.
	return(DELIM);
}
/**************************************************************************
 * get_bindowts_type
 * Given the character, return the character type (delimiter or location)
 *
 * * The range of times is different for the DOWTS (day-of-week timeslot)
 * symbols.
 * INPUTS: symbol in input string
 * OUTPUTS: None
 * RETURNS: DELIM for a delimiter
 * 			LOC for a location char
 **************************************************************************/
int get_bindowts_type( SYMBOL_TYPE symbol)
{

	if (symbol >= INITIAL_START_TIME && symbol <= 0x25FF)
		return(STRT);
	if (symbol >= 0x2620 && symbol <= 0x26FF)
		return(LOC);

	// This is an error. We should never get here.
	return(DELIM);
}

#ifdef THIS_IS_THE_CODE_TO_TEST_THE_STRING16_ROUTINES

// TEST STRING16 routines
int main( int argc, char **argv )
{
	STRING16 * str16;
	int i;
	FILE * test_file;
	SYMBOL_TYPE c;

	str16 = string16(MAX_STRING_LENGTH);		// allocate new struct
	// Fill the string
/*	for (i = 0; i < 5; i++)
		str16->s[i] = (SYMBOL_TYPE) i + 0x1000;
	str16->s[i] = 0x0;
	str16->length = i+1;

	printf("The string is: %s\n", format_string16( str16));

	for (i=0; i < 5; i++)	{
		printf("The symbol at offset %d is 0x%04x.\n", i, get_symbol( str16, i));
	}

	for (i=0; i < 5; i++)	{
		shorten_string16( str16);
		printf("After shortening, the string is: %s\n", format_string16( str16));
	}
*/
	test_file = fopen( "108wks01_05.dat", "rb");
	/* This is one way to read in a file:  */
	i = fread16( str16, MAX_STRING_LENGTH, test_file);
	printf("The string is: %s\n", format_string16( str16));
	fclose( test_file);
	/**/

	/** And this is another **/
	test_file = fopen( "108wks01_05.dat", "r");
	do {
		//fscanf( test_file, "%x", &c);	// read an unsigned short integer (2 bytes)
		i = fread(&c, sizeof(SYMBOL_TYPE),1,test_file);
		if (i>0)
			printf("Training on 0x%04x\n", c);
	} while (i > 0);

	delete_string16( str16);
	exit(0);
}
#endif	// STRING16 Test Code

/***********************************************************
 *	neighboring_ap
 * 
 * Given two symbols for locations (AP) return true if
 * the first one is a neighbor of the second.
 * 
 ***********************************************************/
unsigned char neighboring_ap( SYMBOL_TYPE predicted_ap, SYMBOL_TYPE actual_ap)
{
	unsigned int i;
	unsigned int actual_ap_number=0;
	
	// Translate from the ap symbol value to the actual ap number (1-524)
	// mappings are in the ap_map[] in mapping.h
	for (i=0;i < 525; i++)
		if (ap_map[i] == actual_ap) {
			actual_ap_number = i;
			break;
		}
	if (i == 525)  {
		printf("Error: hit end of ap_map looking for 0x%x\n", actual_ap);
		return( FALSE );
	}
	
	// Look in the ap_neighbors array to see if the predicted_ap is a 
	// neighbor of the actual_ap.
	i = 0;
	while (ap_neighbors[ actual_ap_number][i] != 0) {
		//printf("compare 0x%x to 0x%x - ", ap_neighbors[ actual_ap_number][i], predicted_ap);
		if (ap_neighbors[ actual_ap_number][i] == predicted_ap) {
			//printf("return TRUE\n");
			return (TRUE);
		}
		//else printf("return FALSE\n");
		i++;
	}
	return(FALSE);
	
}	// end of neighboring_ap




/*******************************************************
** get_hhmm_from_code 
*  
*  Given a time code (ex. 0x2621), return the time ("00:00")
*  INPUTS: code to convert, dest of where to put string
*  OUTPUT: dest = string ex "10:23"
*  RETURNS: TRUE for success, FALSE for failure (code not found)
********************************************************/
int get_hhmm_from_code( SYMBOL_TYPE code, char * dest) {
	int hours, minutes;
	int i;
	// Find code in timeslot_map
	for (i=0; i < 1441; i++)
		if (timeslot_map[i] == code) 
			break;
	if (i==1441) { 	// code not found
		printf("mapping.c: timecode 0x%4x not found.\n", code);
		return (FALSE);
		}
	else {			// legit value found
		// calculate time: time="00:00" + index
		hours = i/60;
		minutes = i % 60;
		sprintf(dest, "%02d:%02d", hours, minutes);
		return (TRUE);
		}
	}

/***************************************
 * test_timecode
 * 
 * routine to test time routines
 ***************************************/
void test_timecode() {
	SYMBOL_TYPE i;
	char s[8];		// time string
	
	for (i=0x2621;i <= 0x2d94; i++)	
		if (get_hhmm_from_code(i, s))
			printf("%s\n", s);
}

/*
 * get_str_mappings
 * Given a mapping (LOC,etc.), return pointer to
 * the string to print.
 * 
 * INPUTS: mapping (LOC, STRT...)
 * OUTPUT: pointer to string
 */
char * get_str_mappings(int mapping)
{
	return (str_mappings[mapping]);

}
/************************************************************************
*
*	build_test_string
*
* Build test string for testing.  If predicting WHERE, leave it alone.
* If predicting WHEN, flip it to go LTLTLTL instead of TLTLTLTLTL
* where L=location and T=time
*
* INPUTS: test_string = input test string
* OUTPUTS: If WHEN, test_string has been flipped.
* RETURNS: nothing
*************************************************************************/
void build_test_string(STRING16 * test_string)
{
	STRING16 * str_dest;
	int length;
	int i;
	
	length = strlen16(test_string);
   	if (research_question == WHEN)	{
        str_dest = string16( length);			// allocate memory for actual test string
    	for (i=0; i < length; i++)
    		if (i%2) {	// odd
    			//Do this (16bit version):str_dest[i-1] = test_string[i];
    			put_symbol(str_dest, i-1, get_symbol(test_string,i));
    		}
    		else	{	// even
    			//Do this (16bit version): str_dest[i+1] = test_string[i];
    			put_symbol(str_dest, i+1, get_symbol(test_string,i));
    		}
    	set_strlen16(str_dest, i);			// terminate the string
    	strncpy16( test_string, str_dest, 0, length);	// copy into final location
    }
    if (verbose) {
    	printf("Testing on string %s\n", format_string16(test_string));
    }
}	// end of build_test_string()
/********************************************************
 * 
 * analyze_results
 * Look at results and add to counters
 * INPUT: correct_answer is the actual result from the test string
 * All other inputs come from global variables.
 * OUTPUTS: Global counters are incremented.
 * RETURNS: void
 * 
 * The pred structure returns ALL predictions (unless the
 * code in model-2.c predict_next ()has been changed per the comment).
 * If no predictions were made, the model may have resorted
 * to falling back to order 0.
 * The predictions that were made are listed in order from 
 * the most likely to the least likely.  I want to first look
 * at only the most likely results and count their stats and
 * then look at the other, less likely results.
 * GLOBAL: num_pred_file is file to output the number of predictions returned for each test.
 * 	confidence_level is used to determine which predictions to use (WHEN case only)
 ********************************************************/
void analyze_pred_results( SYMBOL_TYPE correct_answer, SYMBOL_TYPE context)
{		
	int j;				// counter into number of predictions
	unsigned char predicted_correctly;	// true if one of the predictions is correct
	unsigned char is_neighbor = FALSE;		// true if two APs are neighbors
	char str_time[8], str_time2[8];	// space to format time strings
	unsigned char bool_MultipleBest = FALSE;	// true if > 1 most likely predictions
	unsigned char bool_MultipleLess = FALSE;	// true if > 1 less likely predictions
	unsigned char is_within_10min = FALSE;		// true if one of the predictions is within 10 minutes
	unsigned char is_within_20min = FALSE;		// true if one of the predictions is within 20 minutes
	int num_best_predictions = 0;			// number of most likely predictions
	int num_less_predictions = 0;			// number of less than best predictions
	int index_last_best = 0;				// number of predictions -1  that are 'most likely'
	int best_count;							// numerator for most likely predictions
	// Variables used for confidence_level approach
	float f_confidence;						// confidence_level expressed as a value between 0 and 1.
	float f_prob_sum;						// sum of prediction probabiliies
	float f_previous_prob;					// probability of previous prediction
	float f_current_prob;					// probability of current prediction
	int i_current_numerator, i_prev_numerator;  // numerator of current and previous probabilities
	unsigned char bool_done = FALSE;		// true to break out of confidence_level loop.	
	
	if (verbose) {		// Print output
       	printf("context,expected symbol, predicted symbol, # predictions, order, probability\n");

		for (j=0; j < pred.num_predictions; j++)  {	
			if (research_question == WHERE) {
				get_hhmm_from_code(context, str_time );
				printf("%s, 0x%04x, 0x%04x, %d, %d, %f, %s\n",
					str_time,						// context time.
					correct_answer,					// expected symbol
					pred.sym[j].symbol,				// predicted symbol
					pred.num_predictions,
					pred.depth,						// depth
					(float) pred.sym[j].prob_numerator/pred.prob_denominator,
					(correct_answer == pred.sym[j].symbol)? "CORRECT" : "--");
			}
			else //research_question == WHEN
				{
				// If the prediction is from falling back to 0, don't bother with it
				// if it's returning a location instead of a time.  (Remember, the model doesn't
				// know there's a diff)
				if ((pred.depth==0)) 
					if (get_char_type( pred.sym[j].symbol, 0) == LOC)
							continue;						// this is a LOC, go onto the next TIME prediction.
				get_hhmm_from_code(correct_answer, str_time );	// convert expected symbol to time
				get_hhmm_from_code(pred.sym[j].symbol, str_time2);			// convert prediction into a time
				printf("0x%04x, %s, %s, %d, %d, %f, %s\n",
					context,		// location
					str_time,		// expected symbol
					str_time2,		// predicted symbol
					pred.num_predictions,
					pred.depth,						// depth
					(float) pred.sym[j].prob_numerator/pred.prob_denominator,
					(correct_answer == pred.sym[j].symbol)? "CORRECT" : "--");
				}
			}
		} // end if verbose
	/*****
	 * Check if any predictions were returned.
	 ******/
	if (pred.num_predictions == 0)	// no predictions were returned!
		return;
	
	/******
	 * Check for fallback to 0 order.
	 *******/
	// Check for the fallback situation, where the model fell back to order 0
	if (pred.depth == 0)	{	//Model fell back to 0th-order
		FallbackNum++;
		//  See if one of the fallback predictions is correct.
		for (j=1; j < pred.num_predictions; j++)
			if (correct_answer == pred.sym[j].symbol)	{
				FallbackNumCorrect++;
				break;
			}
		return;	// Don't need to check any farther.
	}	// End of fallback case.
	
	/****************
	 * Check types of predictions (MostLikely or LessLikely)
	 ****************/
	// The first element in the pred structure is the most likely.
	// It's count value is the numerator of it's probability, so
	// any entries with the same count have the same probability.
	best_count = pred.sym[0].prob_numerator;	// highest count (probability)
	index_last_best = 0;						// assume only one prediction.
	num_best_predictions = 1;
	
	// Do a quick check to see if more than one prediction was returned
	// as the most likely.
	for (j=1; j < pred.num_predictions; j++) {
		if (pred.sym[j].prob_numerator == best_count) {
			bool_MultipleBest = TRUE;
			index_last_best = j;
			num_best_predictions++;
		}
		else if (j > (index_last_best+1)) {
			bool_MultipleLess = TRUE;	//found > 1 less likely prediction.
			num_less_predictions++;
		}
		else // this case for the first less likely
			num_less_predictions++;
	}
	if (bool_MultipleBest)
		MostProb_MultiplePredictions++;
	if (bool_MultipleLess)
		LessProb_MultiplePredictions++;
	
	if (confidence_level == -1 || research_question == WHERE) {
#ifdef COUNT_NUMBER_OF_PREDICTIONS_RETURNED
		/*****
		 * Output the number of predictions to a file.
		 ******/
		//fprintf(num_pred_file,"  <Test value=\"%d\">\n", num_tested);
		//fprintf(num_pred_file,"     <NumBestPred>%d</NumBestPred>\n", num_best_predictions);
		//fprintf(num_pred_file,"     <NumLessPred>%d</NumLessPred>\n", num_less_predictions);
		//fprintf(num_pred_file,"  </Test>\n");
		fprintf(num_pred_file,"%s, %d, %d, %d\n", test_file_name, num_best_predictions, num_less_predictions,pred.num_predictions);
#endif
		/*************
		 * Check predictions for correctness or if they are close to correct.
		 *************/	
		// Check the most likely predictions first.
		predicted_correctly = FALSE;		// Assume they are all wrong.
		for (j=0; (j < index_last_best+1) && (!predicted_correctly); j++)  {
			if (correct_answer == pred.sym[j].symbol) {
				predicted_correctly = TRUE;
				MostProb_NumCorrect++;
			}
		}
		if (!predicted_correctly)  {
			is_neighbor = FALSE;
			is_within_10min = FALSE;
			is_within_20min = FALSE;
			// All the most likely predictions are wrong.  See if some were close.
			for (j=0; j < index_last_best; j++)  {
				// this prediction is wrong.  See if it is close.
				if (research_question == WHERE) {
					is_neighbor = neighboring_ap( pred.sym[j].symbol, correct_answer);
					if (is_neighbor)
						break;
				}
				else {  //research_question is WHEN
					is_within_10min = within_time_window( pred.sym[j].symbol, correct_answer, 10);
					if (!is_within_20min)
						// stop checking to see if one is within 20 minutes if you already found one.
						is_within_20min = within_time_window( pred.sym[j].symbol, correct_answer, 20);
					if (is_within_10min)
						break;
				}
			}
			// Update the counters for the MostProbable predictions
			if (is_neighbor)
				MostProb_NeighborCorrect++;
			if (is_within_10min)
				MostProb_Within10Minutes++;
			if (is_within_20min)
				MostProb_Within20Minutes++;
		}
			
		//Now check less likely predictions
		predicted_correctly = FALSE;	// assume they are all wrong.
		for (j=index_last_best+1; (j < pred.num_predictions) && !predicted_correctly; j++)  {
			if (correct_answer == pred.sym[j].symbol) {
				predicted_correctly = TRUE;
				LessProb_NumCorrect++;
			}
		}
		if (!predicted_correctly) {	
			// All the less likely predictions are wrong.  See if some were close.
			is_neighbor = FALSE;
			is_within_10min = FALSE;
			is_within_20min = FALSE;
			for (j=index_last_best+1; j < pred.num_predictions; j++)  {
				// this prediction is wrong.  See if it is close.
				if (research_question == WHERE) {
					is_neighbor = neighboring_ap( pred.sym[j].symbol, correct_answer);
					if (is_neighbor)
						break;
				}
				else {  //research_question is WHEN
					is_within_10min = within_time_window( pred.sym[j].symbol, correct_answer, 10);
					if (!is_within_20min)
						// stop checking to see if one is within 20 minutes if you already found one.
						is_within_20min = within_time_window( pred.sym[j].symbol, correct_answer, 20);
					if (is_within_10min)
						break;
				}
			}
		// Update the counters for the LessProbable predictions
		if (is_neighbor)
			LessProb_NeighborCorrect++;
		if (is_within_10min)
			LessProb_Within10Minutes++;
		if (is_within_20min)
			LessProb_Within20Minutes++;
		}
	}		// end of checking predictions without using confidence level
	else	// use confidence level to determine which predictions to use.
	{

		f_confidence = (float) confidence_level/100.0;			// convert to a value between 0 and 1 (inclusive)
		assert (f_confidence >= 0 && f_confidence <= 1);	// check range
		predicted_correctly = FALSE;				// Assume they are all wrong.
		bool_done = FALSE;							// true if confidence-level has been reached.
		f_previous_prob = 0.0;						// probability of previous prediction
		f_prob_sum = 0.0;							// total probability of the predictions used.
		i_prev_numerator = 0;

		/*********************
		 * The predictions are returned in order of highest probability, where the probability
		 * is calculated by dividing the numerator by the denominator.
		 * We want to check all predictions until we hit the confidence level specified by the user.
		 * For example, if the confidence level is 75% and three predictions are returned with probabilities
		 * of 85%, 15% and 5%, we would use the first two (to get at least the top 75% of predictions) and not the third.
		 * If there are a bunch of predictions with the same probability, we check them all, even if that takes us
		 * over the threshold.
		 ***********************************************************************/
		j=0;
		//printf("correct, prediction, prob, total_prob, confidence = %f\n", f_confidence);
		// go through each prediction, stopping if we hit the confidence level or
		// if we hit a correct prediction.
		while ((j < pred.num_predictions) && (!bool_done)){
			// calculate probability of this prediction
			f_current_prob = (float) pred.sym[j].prob_numerator/ (float) pred.prob_denominator;
			i_current_numerator = pred.sym[j].prob_numerator;
			// if its the same as the previous prediction or if we are below the
			// confidence threshold, check to see if its correct.
			if ((i_current_numerator == i_prev_numerator) || (f_prob_sum <= f_confidence))	{
			//if ((f_current_prob == f_previous_prob) || (f_prob_sum <= f_confidence))	{
				//printf(">>>");
				if (correct_answer == pred.sym[j].symbol) {
					predicted_correctly = TRUE;
					MostProb_NumCorrect++;
				}
			}
			//else
				//printf("   ");
			// add this prediction to the totals
			f_prob_sum += f_current_prob;
			if ((f_prob_sum > f_confidence) && (i_current_numerator != i_prev_numerator)) {
				//printf("bool_done = TRUE\n");
				bool_done = TRUE;
			}
			f_previous_prob = f_current_prob;
			i_prev_numerator = i_current_numerator;
			//printf("0x%x, 0x%x, %f, %f, %f, %s\n", correct_answer, pred.sym[j].symbol, f_current_prob, f_prob_sum, f_confidence, (correct_answer == pred.sym[j].symbol)? "CORRECT" : "-----");
			j++;
		}
#ifdef COUNT_NUMBER_OF_PREDICTIONS_RETURNED
		/*****
		 * Output the number of predictions to a file.
		 ******/
		//fprintf(num_pred_file,"  <Test value=\"%d\">\n", num_tested);
		//fprintf(num_pred_file,"     <NumConfPred>%d</NumConfPred>\n", j-1);
		//fprintf(num_pred_file,"  </Test>\n");
		fprintf(num_pred_file,"%s, %d, %d, %d\n", test_file_name, confidence_level, j, pred.num_predictions);
#endif

	}	// end of confidence level tests.

}	// end of analyze_results
/*************************************************
 * output_pred_results
 * if verbose, display the results in words
 * else in XML.
 * INPUTS: global counters
 * OUTPUTS: none
 * RETURNS: voide
 ************************************************/
void output_pred_results()
{
	if (verbose)	{
		/* Print only the percentage of pairs correct & percentage when time is correct */
		printf("NumTests=%d, FallbackNumCorrect=%d, Number_fallbacks_to_zero=%d\n",
			NumTests,			// number of tests
			FallbackNumCorrect,		// number of times it fell back to level 0
									// but was still correct
			FallbackNum);	// number of times it fell back to 0, wrong or right prediction
	
		}
	else {
		// Overall Results
		//printf("   <MaxOrder>%d</MaxOrder>\n", max_order);
		printf("   <NumTests>%d</NumTests>\n", NumTests);
		// Results for predictions that went to Fallback
		printf("   <FallbackNum>%d</FallbackNum>\n", FallbackNum);
		printf("   <FallbackNumCorrect>%d</FallbackNumCorrect>\n", FallbackNumCorrect);
		if (confidence_level < 0)	{	// normal output
			/****
			 *  Results for most likely predictions
			******/
			printf("   <MostProb_NumCorrect>%d</MostProb_NumCorrect>\n", MostProb_NumCorrect);
			if (research_question == WHERE)
				printf("   <MostProb_NeighborCorrect>%d</MostProb_NeighborCorrect>\n", MostProb_NeighborCorrect);
			else // research_question == WHEN
			{
				printf("   <MostProb_Within10Minutes>%d</MostProb_Within10Minutes>\n", MostProb_Within10Minutes);
				printf("   <MostProb_Within20Minutes>%d</MostProb_Within20Minutes>\n", MostProb_Within20Minutes);			
			}
			printf("   <MostProb_MultiplePredictions>%d</MostProb_MultiplePredictions>\n", MostProb_MultiplePredictions);
			/****
			 *  Results for less likely predictions
			******/
			printf("   <LessProb_NumCorrect>%d</LessProb_NumCorrect>\n", LessProb_NumCorrect);
			if (research_question == WHERE)
				printf("   <LessProb_NeighborCorrect>%d</LessProb_NeighborCorrect>\n", LessProb_NeighborCorrect);
			else // research_question == WHEN
			{
				printf("   <LessProb_Within10Minutes>%d</LessProb_Within10Minutes>\n", LessProb_Within10Minutes);
				printf("   <LessProb_Within20Minutes>%d</LessProb_Within20Minutes>\n", LessProb_Within20Minutes);			
			}
			printf("   <LessProb_MultiplePredictions>%d</LessProb_MultiplePredictions>\n", LessProb_MultiplePredictions);
		}	// end of normal output
		else {				// using confidence level
			/****
			 *  Results for most likely predictions
			******/
			printf("   <ConfidenceLevel>%d</ConfidenceLevel>\n", confidence_level);
			printf("   <ConfidenceLevel_NumCorrect>%d</ConfidenceLevel_NumCorrect>\n", MostProb_NumCorrect);
		}
	
	}
}	// end of output_pred_results
/*********************************************************************
 * within_time_window
 * 
 * Compare two time codes (symbols) and see if they are within 
 * the given range.
 * INPUTS: time1 = 16bit code for a time
 * 		time2 = 16bit code for another time
 * 		range = number of MINUTES to see if they are in range.
 * RETURNS: TRUE if they are in range (|symbol2 - symbol1| <= range)
 * 	FALSE if they are not or an error occurred.
 *********************************************************************/
unsigned char within_time_window( SYMBOL_TYPE time1, SYMBOL_TYPE time2, int range)
{
	int ts1=0, ts2=0, i;
	// Find codes in timeslot_map
	for (i=0; i < 1441; i++)
		if (timeslot_map[i] == time1) {
			ts1 = i;
			break;
		}
	if (i==1441) { 	// code not found
		printf("within_time_window: timecode 0x%4x not found.\n", time1);
		return (FALSE);
		}
	for (i=0; i < 1441; i++)
		if (timeslot_map[i] == time2) {
			ts2 = i;
			break;
		}
	if (i==1441) { 	// code not found
		printf("within_time_window: timecode 0x%4x not found.\n", time2);
		return (FALSE);
		}
	
	return (abs(ts2-ts1)<= range);
}
