/*
 * Listing 12 -- model-2.c
 *
 * This module contains all of the modeling functions used with
 * comp-2.c and expand-2.c.  This modeling unit keeps track of
 * all contexts from 0 up to max_order, which defaults to 3.
 * In addition, there is a special context -1 which is a fixed model
 * used to encode previously unseen characters, and a context -2
 * which is used to encode EOF and FLUSH messages.
 *
 * Each context is stored in a special CONTEXT structure, which is
 * documented below.  Context tables are not created until the
 * context is seen, and they are never destroyed.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>	// for isprint() declaration
#include <math.h>		// for log10() function;
#include "coder.h"
#include "model.h"
#include "string16.h"	// for handling 16-bit char 'strings'
#include "predict.h"	// for printing symbol type
/*
 * max_order is the maximum order that will be maintained by this
 * program.  EXPAND-2 and COMP-2 both will modify this int based
 * on command line parameters.
 */
int max_order=3;
/*
 * *contexts[] is an array of current contexts.  If I want to find
 * the order 0 context for the current state of the model, I just
 * look at contexts[0].  This array of context pointers is set up
 * every time the model is updated.
 */
CONTEXT **contexts;
/*
 * current_order contains the current order of the model.  It starts
 * at max_order, and is decremented every time an ESCAPE is sent.  It
 * will only go down to -1 for normal symbols, but can go to -2 for
 * EOF and FLUSH.
 */
int current_order;
/*
 * This variable tells COMP-2.C that the FLUSH symbol can be
 * sent using this model.
 */
int flushing_enabled=0;
/*
 * This table contains the cumulative totals for the current context.
 * Because this program is using exclusion, totals has to be calculated
 * every time a context is used.  The scoreboard array keeps track of
 * symbols that have appeared in higher order models, so that they
 * can be excluded from lower order context total calculations.
 */
short int totals[ RANGE_OF_SYMBOLS+2 ];
char scoreboard[ RANGE_OF_SYMBOLS ];
int alloc_count=0;		// number of CONTEXT structs allocated.

/*
 * Global variables for routine that counts the tables once
 * the model is built.  (I know, they shouldn't be global, but
 * it's the quickest way to implement, and this is grad student
 * code, not a professional release.
 */
int num_context_tables=0;
int num_children[1500];	// number of children in each context table

/*
 * Local procedure declarations.
 */
void error_exit( char *message );
void update_table( CONTEXT *table, SYMBOL_TYPE symbol );
void rescale_table( CONTEXT *table );
void totalize_table( CONTEXT *table );
CONTEXT *shift_to_next_context( CONTEXT *table, SYMBOL_TYPE c, int order);
CONTEXT *allocate_next_order_table( CONTEXT *table,
                                    SYMBOL_TYPE symbol,
                                    CONTEXT *lesser_context );

/*
 * This routine has to get everything set up properly so that
 * the model can be maintained properly.  The first step is to create
 * the *contexts[] array used later to find current context tables.
 * The *contexts[] array indices go from -2 up to max_order, so
 * the table needs to be fiddled with a little.  This routine then
 * has to create the special order -2 and order -1 tables by hand,
 * since they aren't quite like other tables.  Then the current
 * context is set to \0, \0, \0, ... and the appropriate tables
 * are built to support that context.  The current order is set
 * to max_order, the scoreboard is cleared, and the system is
 * ready to go.
 */

void initialize_model()
{
    int i;
    CONTEXT *null_table;
    CONTEXT *control_table;
    
    current_order = max_order;
    contexts = (CONTEXT **) calloc( sizeof( CONTEXT * ), 10 );
    alloc_count += 10;
    if ( contexts == NULL )
        error_exit( "Failure #1: allocating context table!" );
    contexts += 2;
    null_table = (CONTEXT *) calloc( sizeof( CONTEXT ), 1 );
    alloc_count++;
    if ( null_table == NULL )
        error_exit( "Failure #2: allocating null table!" );
    null_table->max_index = -1;
    contexts[ -1 ] = null_table;
    for ( i = 0 ; i <= max_order ; i++ )
        contexts[ i ] = allocate_next_order_table( contexts[ i-1 ],
                                               0,
                                               contexts[ i-1 ] );
    alloc_count += max_order;
    handle_free( (char __handle *) null_table->stats );
    null_table->stats =
         (STATS __handle *) handle_calloc( sizeof( STATS ) * 500 );  // HERE was 256
    if ( null_table->stats == NULL )
        error_exit( "Failure #3: allocating null table!" );
    null_table->max_index = 499;  // TEST TEST TEST  was 256
    for ( i=0 ; i < 500 ; i++ )  // TEST TEST was 255
    {
        null_table->stats[ i ].symbol = (unsigned char) i;
        null_table->stats[ i ].counts = 1;
    }

    control_table = (CONTEXT *) calloc( sizeof(CONTEXT), 1 );
    if ( control_table == NULL )
        error_exit( "Failure #4: allocating null table!" );
    alloc_count++;
    control_table->stats =
         (STATS __handle *) handle_calloc( sizeof( STATS ) * 2 );
    if ( control_table->stats == NULL )
        error_exit( "Failure #5: allocating null table!" );
    contexts[ -2 ] = control_table;
    control_table->max_index = 1;
    control_table->stats[ 0 ].symbol = -FLUSH;
    control_table->stats[ 0 ].counts = 1;
    control_table->stats[ 1 ].symbol =- DONE;
    control_table->stats[ 1 ].counts = 1;

    clear_scoreboard();
}
/*
 * This is a utility routine used to create new tables when a new
 * context is created.  It gets a pointer to the current context,
 * and gets the symbol that needs to be added to it.  It also needs
 * a pointer to the lesser context for the table that is to be
 * created.  For example, if the current context was "ABC", and the
 * symbol 'D' was read in, add_character_to_model would need to
 * create the new context "BCD".  This routine would get called
 * with a pointer to "BC", the symbol 'D', and a pointer to context
 * "CD".  This routine then creates a new table for "BCD", adds it
 * to the link table for "BC", and gives "BCD" a back pointer to
 * "CD".  Note that finding the lesser context is a difficult
 * task, and isn't done here.  This routine mainly worries about
 * modifying the stats and links fields in the current context.
 */

CONTEXT *allocate_next_order_table( CONTEXT *table,
                                    SYMBOL_TYPE symbol,
                                    CONTEXT *lesser_context )
{
    CONTEXT *new_table;
    int i;
    unsigned int new_size;
    
    for ( i = 0 ; i <= table->max_index ; i++ )
        if ( table->stats[ i ].symbol == symbol )
            break;
    if ( i > table->max_index )
    {
        table->max_index++;
        new_size = sizeof( LINKS );
        new_size *= table->max_index + 1;
        if ( table->links == NULL )
            table->links = (LINKS __handle *) handle_calloc( new_size );
        else
            table->links = (LINKS __handle *)
                 handle_realloc( (char __handle *) table->links, new_size );
        new_size = sizeof( STATS );
        new_size *= table->max_index + 1;
        if ( table->stats == NULL )
            table->stats = (STATS __handle *) handle_calloc( new_size );
        else
            table->stats = (STATS __handle *)
                handle_realloc( (char __handle *) table->stats, new_size );
        if ( table->links == NULL )
            error_exit( "Failure #6: allocating new table" );
        if ( table->stats == NULL )
            error_exit( "Failure #7: allocating new table" );
        table->stats[ i ].symbol = symbol;
        table->stats[ i ].counts = 0;
    }
    new_table = (CONTEXT *) calloc( sizeof( CONTEXT ), 1 );
    alloc_count++;
    if ( new_table == NULL )
        error_exit( "Failure #8: allocating new table" );
    new_table->max_index = -1;
    table->links[ i ].next = new_table;
    new_table->lesser_context = lesser_context;
    return( new_table );
}

/*
 * This routine is called to increment the counts for the current
 * contexts.  It is called after a character has been encoded or
 * decoded.  All it does is call update_table for each of the
 * current contexts, which does the work of incrementing the count.
 * This particular version of update_model() practices update exclusion,
 * which means that if lower order models weren't used to encode
 * or decode the character, they don't get their counts updated.
 * This seems to improve compression performance quite a bit.
 * To disable update exclusion, the loop would be changed to run
 * from 0 to max_order, instead of current_order to max_order.
 ***
 *** Ingrid:  For example, with his code and the training string
 * 'abracadabra' the counts end up as 'a'=4, 'b'=1, 'r'=1, 'c'=1, 'd'=1
 *
 */
void update_model( SYMBOL_TYPE symbol )
{
    int local_order;

    if ( current_order < 0 )
        local_order = 0;
    else
        local_order = current_order;

    //Ingrid: Override his code
    local_order = 0;		// Ingrid
    // End of override changes


    if ( symbol >= 0 )
    {
        while ( local_order <= max_order )
        {
            if ( symbol >= 0 )
                update_table( contexts[ local_order ], symbol );
            local_order++;
        }
    }
    current_order = max_order;
    clear_scoreboard();
}
/*
 * This routine is called to update the count for a particular symbol
 * in a particular table.  The table is one of the current contexts,
 * and the symbol is the last symbol encoded or decoded.  In principle
 * this is a fairly simple routine, but a couple of complications make
 * things a little messier.  First of all, the given table may not
 * already have the symbol defined in its statistics table.  If it
 * doesn't, the stats table has to grow and have the new guy added
 * to it.  Secondly, the symbols are kept in sorted order by count
 * in the table so as that the table can be trimmed during the flush
 * operation.  When this symbol is incremented, it might have to be moved
 * up to reflect its new rank.  Finally, since the counters are only
 * bytes, if the count reaches 255, the table absolutely must be rescaled
 * to get the counts back down to a reasonable level.
 */
void update_table( CONTEXT *table, SYMBOL_TYPE symbol )
{
    int i;
    int index;
    SYMBOL_TYPE temp;
    CONTEXT *temp_ptr;
    unsigned int new_size;
    
/*
 * First, find the symbol in the appropriate context table.  The first
 * symbol in the table is the most active, so start there.
 */
    index = 0;
    while ( index <= table->max_index &&
            table->stats[index].symbol != symbol )
        index++;
    if ( index > table->max_index )
    {
        table->max_index++;
        new_size = sizeof( LINKS );
        new_size *= table->max_index + 1;
        if ( current_order < max_order )
        {
            if ( table->max_index == 0 )
                table->links = (LINKS __handle *) handle_calloc( new_size );
            else
                table->links = (LINKS __handle *)
                   handle_realloc( (char __handle *) table->links, new_size );
            if ( table->links == NULL )
                error_exit( "Error #9: reallocating table space!" );
            table->links[ index ].next = NULL;
        }
        new_size = sizeof( STATS );
        new_size *= table->max_index + 1;
        if (table->max_index==0)
            table->stats = (STATS __handle *) handle_calloc( new_size );
        else
            table->stats = (STATS __handle *)
                handle_realloc( (char __handle *) table->stats, new_size );
        if ( table->stats == NULL )
            error_exit( "Error #10: reallocating table space!" );
        table->stats[ index ].symbol = symbol;
        table->stats[ index ].counts = 0;
    }
/*
 * Now I move the symbol to the front of its list.
 */
    i = index;
    while ( i > 0 &&
            table->stats[ index ].counts == table->stats[ i-1 ].counts )
        i--;
    if ( i != index )
    {
        temp = table->stats[ index ].symbol;
        table->stats[ index ].symbol = table->stats[ i ].symbol;
        table->stats[ i ].symbol = temp;
        if ( table->links != NULL )
        {
            temp_ptr = table->links[ index ].next;
            table->links[ index ].next = table->links[ i ].next;
            table->links[ i ].next = temp_ptr;
        }
        index = i;
    }
/*
 * The switch has been performed, now I can update the counts
 */
    table->stats[ index ].counts++;
    //if ( table->stats[ index ].counts == 255 )	// Ingrid: removed this - it sets level 0 counts to 0
        //rescale_table( table );    // HERE these two lines were commented out until I hit a large file.
}

/*********************************************
 * clear_current_order
 *
 * Used for during training.
 *
 * INPUTS: None
 * OUTPUTS: current_order set to 0
 * RETURNS: void
 ************************************************/
void clear_current_order(){
	current_order = 0;
	return;
}
/*
 * This routine is called when a given symbol needs to be encoded.
 * It is the job of this routine to find the symbol in the context
 * table associated with the current table, and return the low and
 * high counts associated with that symbol, as well as the scale.
 * Finding the table is simple.  Unfortunately, once I find the table,
 * I have to build the table of cumulative counts, which is
 * expensive, and is done elsewhere.  If the symbol is found in the
 * table, the appropriate counts are returned.  If the symbols is
 * not found, the ESCAPE symbol probabilities are returned, and
 * the current order is reduced.  Note also the kludge to support
 * the order -2 character set, which consists of negative numbers
 * instead of unsigned chars.  This insures that no match will every
 * be found for the EOF or FLUSH symbols in any "normal" table.
 */
int convert_int_to_symbol( SYMBOL_TYPE c, SYMBOL *s )
{
    int i;
    CONTEXT *table;

    table = contexts[ current_order ];
    totalize_table( table );
    s->scale = totals[ 0 ];
    if ( current_order == -2 )
        c = -c;
    for ( i = 0 ; i <= table->max_index ; i++ )
    {
        if ( c == table->stats[ i ].symbol )
        {
            if ( table->stats[ i ].counts == 0 )
                break;
            s->low_count = totals[ i+2 ];
            s->high_count = totals[ i+1 ];
            return( 0 );
        }
    }

    s->low_count = totals[ 1 ];
    s->high_count = totals[ 0 ];

    current_order--;
    return( 1 );
}

/*
 * This routine is called when decoding an arithmetic number.  In
 * order to decode the present symbol, the current scale in the
 * model must be determined.  This requires looking up the current
 * table, then building the totals table.  Once that is done, the
 * cumulative total table has the symbol scale at element 0.
 */
void get_symbol_scale( SYMBOL *s )
{
    CONTEXT *table;

    table = contexts[ current_order ];
    totalize_table( table );
    s->scale = totals[ 0 ];
}

/*
 * This routine is called during decoding.  It is given a count that
 * came out of the arithmetic decoder, and has to find the symbol that
 * matches the count.  The cumulative totals are already stored in the
 * totals[] table, form the call to get_symbol_scale, so this routine
 * just has to look through that table.  Once the match is found,
 * the appropriate character is returned to the caller.  Two possible
 * complications.  First, the character might be the ESCAPE character,
 * in which case the current_order has to be decremented.  The other
 * complication is that the order might be -2, in which case we return
 * the negative of the symbol so it isn't confused with a normal
 * symbol.
 */
/**** not used in prediction ************************
int convert_symbol_to_int( int count, SYMBOL *s)
{
    int c;
    CONTEXT *table;

    table = contexts[ current_order ];
    for ( c = 0; count < totals[ c ] ; c++ )
        ;
    s->high_count = totals[ c-1 ];
    s->low_count = totals[ c ];
    if ( c == 1 )
    {
        current_order--;
        return( ESCAPE );
    }
    if ( current_order < -1 )
        return( (int) -table->stats[ c-2 ].symbol );
    else
        return( table->stats[ c-2 ].symbol );
}
************************************************/

/*
 * After the model has been updated for a new character, this routine
 * is called to "shift" into the new context.  For example, if the
 * last context was "ABC", and the symbol 'D' had just been processed,
 * this routine would want to update the context pointers to that
 * contexts[1]=="D", contexts[2]=="CD" and contexts[3]=="BCD".  The
 * potential problem is that some of these tables may not exist.
 * The way this is handled is by the shift_to_next_context routine.
 * It is passed a pointer to the "ABC" context, along with the symbol
 * 'D', and its job is to return a pointer to "BCD".  Once we have
 * "BCD", we can follow the lesser context pointers in order to get
 * the pointers to "CD" and "C".  The hard work was done in
 * shift_to_context().
 */
void add_character_to_model( SYMBOL_TYPE c )
{
    int i;
    
    if ( max_order < 0 || c < 0 )
       return;
    contexts[ max_order ] =
       shift_to_next_context( contexts[ max_order ],
                              c, max_order );
    for ( i = max_order-1 ; i > 0 ; i-- )
        contexts[ i ] = contexts[ i+1 ]->lesser_context;
}

/*
 * This routine is called when adding a new character to the model. From
 * the previous example, if the current context was "ABC", and the new
 * symbol was 'D', this routine would get called with a pointer to
 * context table "ABC", and symbol 'D', with order max_order.  What this
 * routine needs to do then is to find the context table "BCD".  This
 * should be an easy job, and it is if the table already exists.  All
 * we have to in that case is follow the back pointer from "ABC" to "BC".
 * We then search the link table of "BC" until we find the linke to "D".
 * That link points to "BCD", and that value is then returned to the
 * caller.  The problem crops up when "BC" doesn't have a pointer to
 * "BCD".  This generally means that the "BCD" context has not appeared
 * yet.  When this happens, it means a new table has to be created and
 * added to the "BC" table.  That can be done with a single call to
 * the allocate_new_table routine.  The only problem is that the
 * allocate_new_table routine wants to know what the lesser context for
 * the new table is going to be.  In other words, when I create "BCD",
 * I need to know where "CD" is located.  In order to find "CD", I
 * have to recursively call shift_to_next_context, passing it a pointer
 * to context "C" and they symbol 'D'.  It then returns a pointer to
 * "CD", which I use to create the "BCD" table.  The recursion is guaranteed
 * to end if it ever gets to order -1, because the null table is
 * guaranteed to have a for every symbol to the order 0 table.  This is
 * the most complicated part of the modeling program, but it is
 * necessary for performance reasons.
 */
CONTEXT *shift_to_next_context( CONTEXT *table, SYMBOL_TYPE c, int order)
{
    int i;
    CONTEXT *new_lesser;
    
/*
 * First, try to find the new context by backing up to the lesser
 * context and searching its link table.  If I find the link, we take
 * a quick and easy exit, returning the link.  Note that their is a
 * special Kludge for context order 0.  We know for a fact that
 * the lesser context pointer at order 0 points to the null table,
 * order -1, and we know that the -1 table only has a single link
 * pointer, which points back to the order 0 table.
 */
    table = table->lesser_context;
    if ( order == 0 )
        return( table->links[ 0 ].next );
    for ( i = 0 ; i <= table->max_index ; i++ )
        if ( table->stats[ i ].symbol == c )  {
            if ( table->links[ i ].next != NULL )
                return( table->links[ i ].next );
            else
                break;
        }
/*
 * If I get here, it means the new context did not exist.  I have to
 * create the new context, add a link to it here, and add the backwards
 * link to *his* previous context.  Creating the table and adding it to
 * this table is pretty easy, but adding the back pointer isn't.  Since
 * creating the new back pointer isn't easy, I duck my responsibility
 * and recurse to myself in order to pick it up.
 */
    new_lesser = shift_to_next_context( table, c, order-1 );
/*
 * Now that I have the back pointer for this table, I can make a call
 * to a utility to allocate the new table.
 */
    table = allocate_next_order_table( table, c, new_lesser );
    return( table );
}

/*
 * Rescaling the table needs to be done for one of three reasons.
 * First, if the maximum count for the table has exceeded 16383, it
 * means that arithmetic coding using 16 and 32 bit registers might
 * no longer work.  Secondly, if an individual symbol count has
 * reached 255, it will no longer fit in a byte.  Third, if the
 * current model isn't compressing well, the compressor program may
 * want to rescale all tables in order to give more weight to newer
 * statistics.  All this routine does is divide each count by 2.
 * If any counts drop to 0, the counters can be removed from the
 * stats table, but only if this is a leaf context.  Otherwise, we
 * might cut a link to a higher order table.
 */
void rescale_table( CONTEXT *table )
{
    int i;

    if ( table->max_index == -1 )
        return;
    for ( i = 0 ; i <= table->max_index ; i++ )
        table->stats[ i ].counts /= 2;
    if ( table->stats[ table->max_index ].counts == 0 &&
         table->links == NULL )
    {
        while ( table->stats[ table->max_index ].counts == 0 &&
                table->max_index >= 0 )
            table->max_index--;
        if ( table->max_index == -1 )
        {
            handle_free( (char __handle *) table->stats );
            table->stats = NULL;
        }
        else
        {
            table->stats = (STATS __handle *)
                handle_realloc( (char __handle *) table->stats,
                                 sizeof( STATS ) * ( table->max_index + 1 ) );
            if ( table->stats == NULL )
                error_exit( "Error #11: reallocating stats space!" );
        }
    }
}

/*
 * This routine has the job of creating a cumulative totals table for
 * a given context.  The cumulative low and high for symbol c are going to
 * be stored in totals[c+2] and totals[c+1].  Locations 0 and 1 are
 * reserved for the special ESCAPE symbol.  The ESCAPE symbol
 * count is calculated dynamically, and changes based on what the
 * current context looks like.  Note also that this routine ignores
 * any counts for symbols that have already showed up in the scoreboard,
 * and it adds all new symbols found here to the scoreboard.  This
 * allows us to exclude counts of symbols that have already appeared in
 * higher order contexts, improving compression quite a bit.
 */
void totalize_table( CONTEXT *table )
{
    int i;
    unsigned char max;
//    int num_excluded_symbols = 0;	// Ingrid - count excluded symbols

    for ( ; ; )
    {
        max = 0;
        i = table->max_index + 2;
        totals[ i ] = 0;
        for ( ; i > 1 ; i-- )
        {
            totals[ i-1 ] = totals[ i ];
            if ( table->stats[ i-2 ].counts )
                if ( ( current_order == -2 ) ||
                     scoreboard[ table->stats[ i-2 ].symbol - LOWEST_SYMBOL ] == 0 )
                     totals[ i-1 ] += table->stats[ i-2 ].counts;
            if ( table->stats[ i-2 ].counts > max )
                max = table->stats[ i-2 ].counts;
        }
/*
 * Here is where the escape calculation needs to take place.
 */

        if ( max == 0 )
            totals[ 0 ] = 1;
        else
        {
        	/* Ingrid - I commented out this code because I'm not sure what he's doing!
        	 * (and it gives me incorrect restults for logloss numbers)
        	 *
*            totals[ 0 ] = (short int) ( 256 - table->max_index );
*            totals[ 0 ] *= table->max_index;
*            totals[ 0 ] /= 256;
*            totals[ 0 ] /= max;
*            totals[ 0 ]++;
*            totals[ 0 ] += totals[ 1 ];
********    end of original code	*/
        	/* Start of Ingrid's code */
        	if (current_order == 0)
        		totals[0] = totals[1] + table->max_index;
        	else
           		totals[0] = totals[1] + table->max_index + 1;
        	/* End of Ingrid's changes  */
        }
        if ( totals[ 0 ] < MAXIMUM_SCALE )
            break;
        rescale_table( table );			// This should never get called. (IT will print a message if it does.)
    }
    for ( i = 0 ; i < table->max_index ; i++ )		// Ingrid - changed to <= (was <)
    		// Careful: if it runs through the whole loop it will cause an ACCESS_VIOLATION
    	if (table->stats[i].counts != 0) {
    		// This is a bug fix hack -- don't know why we can sometimes get a table where this is not true:
    		if (table->stats[i].symbol >= LOWEST_SYMBOL)
    			scoreboard[ table->stats[ i ].symbol - LOWEST_SYMBOL ] = 1;
            printf("i=%d, max_index=%d, brackets=%d, max=%d\n", i, table->max_index, table->stats[ i ].symbol - LOWEST_SYMBOL, RANGE_OF_SYMBOLS);
    		}	
}

/*
 * This routine is called when the entire model is to be flushed.
 * This is done in an attempt to improve the compression ratio by
 * giving greater weight to upcoming statistics.  This routine
 * starts at the given table, and recursively calls itself to
 * rescale every table in its list of links.  The table itself
 * is then rescaled.
 */
void recursive_flush( CONTEXT *table )
{
    int i;
    
    if ( table->links != NULL )
        for ( i = 0 ; i <= table->max_index ; i++ )
            if ( table->links[ i ].next != NULL )
                recursive_flush( table->links[ i ].next );
    rescale_table( table );
}

/*
 * This routine is called to flush the whole table, which it does
 * by calling the recursive flush routine starting at the order 0
 * table.
 */
void flush_model()
{
    recursive_flush( contexts[ 0 ] );
}

void error_exit( char *message)
{
    putc( '\n', stdout );
    puts( message );
    exit( -1 );
}
/*
 * count_model
 * 
 * This routine is called to count the number of tables in the model.
 * research_question = WHEN or WHERE (so we know what to count)
 * verbose = TRUE to printf verbose output, rather than comma-sep values.
 *
 */
void count_model(int research_question, int verbose)
{
	int i, min, max;
	float total;
	float average, std_dev, median, var;
#ifdef ADD_THIS_IN_IF_YOU_WANT_IT
	int jj,max_count, max_index;
	int count[MAX_MODEL_COUNT];		//working array to hold codes for calculating mode.
	int done;
	int multimodal;		// true if number of children has more than one mode.
#endif	
	// count tables and children for this research_question.
	recursive_count(0, contexts[0], research_question, verbose);
	//printf("num_context_tables = %d\n", num_context_tables);
	//for (i=0; i <= num_context_tables; i++)
		//printf("num_children[%d]=%d\n", i, num_children[i]);
	
	// Find min, max
	min=10000;
	max = 0;
	for (i=1; i <= num_context_tables; i++){
		if (num_children[i] < min)
			min = num_children[i];
		if (num_children[i] > max)
			max = num_children[i];
	}
	// The tables are sorted by count, so the max number of children is always 
	// the first value.
	if (verbose)
		printf("max num_children=%d\n", max);
	else
		printf("   <MaxNumChildren>%d</MaxNumChildren>\n", max);
	
	if (verbose)
		printf("min num_children=%d\n", min);
	else
		printf("   <MinNumChildren>%d</MinNumChildren>\n", min);
		
	
	
	// Calculate average number of children
	for (i=1, total=0; i <= num_context_tables; i++)
		total += num_children[i];
	average = total/num_context_tables;
	if (verbose)
		printf("average num_children=%f\n", average);
	else
		printf("   <AveNumChildren>%.2f</AveNumChildren>\n", average);
	
	// Calculate standard deviation
	for (i=1, total=0; i <= num_context_tables; i++ ) {
		var = num_children[i] - average;
		var *= var;				//square
		total += var;
	}
	std_dev = sqrt(total/num_context_tables);
	if (verbose)
		printf("standard deviation=%f\n", std_dev);
	else
		printf("   <StdDevChildren>%.2f</StdDevChildren>\n", std_dev);
	
	// Calculate median (middle value or arithmetic mean of two middle values)
	if (num_context_tables % 2)
		// odd number of entries, median = middle value
		median = num_children[(num_context_tables+1)/2];
	else
		// even number of entries, take ave of middle two values
		median = (num_children[num_context_tables/2] + 
				num_children[num_context_tables/2+1])/2;
	if (verbose)
		printf("median = %f\n", median);
	else
		printf("   <MedianNumChildren>%.2f</MedianNumChildren>\n", median);
	
	
#ifdef ADD_THIS_IN_IF_YOU_WANT_IT
	// Calculate mode (most common value).
	// This part of the code assumes that the values are in order.
	i=0;
	max_count=0;
	max_index = 0;
	jj=1;
	done = FALSE;
	while (! done) {
		// count the number of times each value appears and 
		// stick the count in the corresponding box in the count[] array.
		//(ex. if the first 5 values are the same, count[1]=count[2]=count[3]=count[4]=0
		// count[5] = 5
		while (++i <= num_context_tables &&
				(i < MAX_MODEL_COUNT) &&
				num_children[i] == num_children[i-1])
		{
			count[i-1]=0;
			jj++;			// increment counter of how many times this value appears
		}
		if (i == MAX_MODEL_COUNT) {
			fprintf(stderr, "Hit an error calculating mode.\n");
			break;
		}
		// hit a spot where the value is different
		count[i-1] = jj;		// store count at index where value can be found.
		// Is this the max we've seen so far?
		if (jj > max_count)	{
			max_index = i-1;
			max_count = jj;
		}
		jj=1;
		if (i > num_context_tables)
			done = TRUE;
	}
	if (verbose)
		printf("Mode is %d.\n", num_children[ max_index ]);	
	else
		printf("   <ModeChildren>%d</ModeChildren>\n", num_children[max_index]);
	// TODO: Add check for bimodal!
	// Go back through table.  if you find another value with the same count,
	// output 'T' for 'multimodal?' variable, else output FALSE
	for (multimodal=FALSE, i=1; i <= num_context_tables; i++) {
		// if there's a value with the same count and a different index,
		// then there is more than one mode.
		if (count[i] == max_count && i != max_index)
			multimodal = TRUE;
	}
	if (verbose)
		printf("Multimodal = %s\n", multimodal == TRUE? "true" : "false");
	else
		printf("   <MultimodalChildren>%s</MultimodalChildren>\n", multimodal == TRUE? "True" : "False");
#endif
		
}

/*
 * print_model
 *
 * This routine is called when the entire model is to be printed.
 */
void print_model()
{
    recursive_print( 0, contexts[ 0 ] );
}
/*
 * recursive_count
 * Walk through the tables, and print what you find.
 * Count the given table, and then count it's children.
 *
 * Inputs: depth = level (how many tabs to print)
 * 		   table = points to the table to print
 *
 * NOte that this routine assumes only 1st order tables
 * and a binboxstrings representation.
 */
void recursive_count(int depth,  CONTEXT *table, int research_question, int verbose )
{
    int i, type;
	char tabs[] = "\t\t\t\t";
	char str_time[8];	// room to format time as hh:mm
	// the following variables are now globals.
	//static int num_context_tables=0;
	//static int num_children[200];	// number of children in each context table

	/* print tabs to create nested table */
	tabs[ depth+1 ] = '\0';	/* create tab string */

	/* Print this table's information */
	if (verbose)
		printf("Depth: %d, %d child(ren)\n",
		depth,
		table->max_index+1);
	else
		if (depth==0)
			printf("   <TotalNumChildren>%d</TotalNumChildren>\n",table->max_index+1);

	if (table->max_index == -1)
	{
		return;
	}
	if (depth > 0)
		num_children[ num_context_tables] = table->max_index+1;

	for (i=0; i <= table->max_index; i++)  
	{
		type = get_binboxstring_type( table->stats[i].symbol);
		/* If this table has links, print them */
    	if (( table->links != NULL && depth < max_order ) &&
    		((research_question == WHEN && type == LOC) ||
    		 (research_question == WHERE && type == STRT))){
    		if (verbose)  {
    			if (type == STRT)
    			{
    				get_hhmm_from_code(table->stats[i].symbol, str_time );
    				printf("%sContext %s %s: ", tabs, str_time, get_str_mappings(type));
    			}
    			else
    				printf("%sContext 0x%04x %s: ", tabs, table->stats[i].symbol, get_str_mappings(type));
    		}
   			num_context_tables++;
    		recursive_count( depth+1, table->links[i].next, research_question, verbose);
    	}
	}
	//printf("num_context_tables=%d\n", num_context_tables);
}

/*
 * recursive_print
 * Walk through the tables, and print what you find.
 * Print the given table, and then print it's children.
 *
 * Inputs: depth = level (how many tabs to print)
 * 		   table = points to the table to print
 *
 */
void recursive_print(int depth,  CONTEXT *table )
{
    int i;
	char tabs[] = "\t\t\t\t";

	/* print tabs to create nested table */
	tabs[ depth ] = '\0';	/* create tab string */

	if (table->max_index == -1)
		return;

	/* Print this table's information */
	for (i=0; i <= table->max_index; i++)  {
		printf("%sSymbol: 0x%04x, counts: %d\n",
			tabs,
			table->stats[i].symbol,
			table->stats[i].counts);

		/* If this table has links, print them */
    	if ( table->links != NULL && depth < max_order )
			recursive_print( depth+1, table->links[i].next);
		}
}
/**************************
** probability
**
** Given a context and a char, return the probability of that char.
**
**
** INPUTS:  string context (assumed to be shorter than max_order!)
**			character
**			pointer to symbol where results will be stored
**			verbose = true to print out results
** OUTPUTS: symbol is filled in with interval information
** RETURNS: probability (as a number between 0 and 1)
**  Note that ESCAPE probability is NOT included.
*/
float probability( SYMBOL_TYPE c, STRING16 * context_string, char verbose)
{
	int i;
	CONTEXT *table;
	int prob_numerator = 1, prob_denominator = 1;
#ifdef TOOK_THIS_OUT_TO_BE_CONSISTENT
	int local_order=0;
#endif
	float fl_prob;		// final probability calculation
	int done = false;

	while (!done){
		// Traverse the tree, trying to find the context string.
		if (current_order >= 0)
			traverse_tree(context_string);
		table = contexts[current_order];	// point to best context

		// now find the character we want the probability of
		i = 0;
		while (i <= table->max_index &&
				table->stats[i].symbol != c)
			i++;

		if (i > table->max_index)	{
			// If you got here, it means that you found the context string (or part of it)
			// in the table, but can't find the test character c anywhere.  You can try a shorter
			// context or stop at level -1.
			if (current_order > 0)
				shorten_string16( context_string );	// remove the first character from the context string and try again
			else {
				// This character wasn't found in the training data, so fall back to level -1
				current_order = -1;
				}
			}
		else
			done = true;		// found the test character in the current context table
		}
	// Brute force calculation of the probability

	// Numerator = counts for this character c
	prob_numerator = table->stats[i].counts;

#ifdef TOOK_THIS_OUT_TO_BE_CONSISTENT
	// Demoninator has two parts, it's the sum of all the counts + the number of elements in the table
	// (His context[0] table has an extra entry in it, so don't add the extra '1'
	if (local_order == 0)
		prob_denominator = table->max_index;
	else
		prob_denominator = table->max_index + 1;	// this is the number of elements in the table.
#else
	prob_denominator = 0;
#endif
	
	for (i=0; i <= table->max_index; i++)
		prob_denominator += table->stats[i].counts;

	fl_prob = (float) prob_numerator/(float) prob_denominator;
	printf("Pr( 0x%04x | %s) = %d/%d = %f\n", c, format_string16(context_string),
		prob_numerator,
		prob_denominator,
		fl_prob);
	return( fl_prob);
}


/**************************
** predict_next
**
** Given a context, return:
**	  the most likely next chars and their probabilities
* 	  the context level at which you found those chars (to indicate
* 		(if you had to fall back to a lower level of context)
**
**
** INPUTS:  string context (only the last max_order bytes are used)
**			pointer to where results will be stored
** OUTPUTS: symbol is filled in with interval information
** RETURNS:
********************************************************************/
unsigned char predict_next( STRING16 * context_string, STRUCT_PREDICTION * results)
{
	int i;
	CONTEXT *table;
	int max_counts;		// maximum value for 'counts' found

	// Traverse the tree, trying to find the context string.
	// If the entire context string can't be found,
	// trim the first char off of the context string and try again.
	// At the end of this section,
	//  the context[] array points to the current context
	//	(example: context[1] points to the "a" context and
	//  context[2] points to the "ab" context.
	//  local_order is set to the depth where the context was found
	traverse_tree( context_string);
	if (current_order < 0)		// if the last char wasn't found at all, don't back down all the way to -1
		current_order = 0;
	table = contexts[ current_order ];

	/* At this point, we have traversed the tree and we are
	 * pointing to the best context that we can find.
	 * table points to this context table
	 * local_order is the depth (or order) of this model
	 */
//	strcpy( results->context_string_used, &context_string[start_of_string]);
	results->depth = current_order;

	/* Find the symbols with the highest probability, given
	 * this context.
	 * Fortunately, this implementation of the model stores
	 * the symbols in sorted order, so we can start at the
	 * top and stop when we hit a max count or when the
	 * counts (used to calc the probability) diminish.
	 */

	// Demoninator has two parts, it's the sum of all the counts + the number of elements in the table
	// (His context[0] table has an extra entry in it, so don't add the extra '1'
#ifdef REMOVED_FOR_CONFIDENCE_LEVEL_TESTING
	if (current_order == 0)
		results->prob_denominator = table->max_index;
	else
		results->prob_denominator = table->max_index + 1;	// this is the number of elements in the table.
#else
		results->prob_denominator = 0;				// initialize denominator
#endif
	max_counts = table->stats[0].counts;	// max value for 'counts' is in first element
	// go through the table and store all symbols with the same value for 'counts' as the
	// first element.
	
	// the clause below [table->stats[i].counts== max_counts] is causing the model to only 
	// return the most popular predictions, not all of the predictions.
	for (i=0;
		i <= table->max_index &&
		//table->stats[i].counts == max_counts &&				// comment this out to return ALL predictions instead of just the most popular.
		i < MAX_NUM_PREDICTIONS;
		i++)	{
		// store information about this symbol
		results->sym[i].symbol = table->stats[i].symbol;
		results->sym[i].prob_numerator = table->stats[i].counts;
		results->prob_denominator += table->stats[i].counts;
		}
	results->num_predictions = i;

	// Add up the rest of the counts in the table for the denominator
	for ( ; i <= table->max_index; i++)	{
		results->prob_denominator += table->stats[i].counts;
		}
#ifdef DEBUG_MODEL
	/* print results
	 */
	/*** TEST TEST TEST TEST *****************************/
	printf("predict_next: given context_string = \"%s\".\n", format_string16(context_string));
	printf("\tdepth = %d, denominator=%d, number of predictions = %d\n",
			results->depth,
			results->prob_denominator,
			results->num_predictions);
	for (i=0; i < results->num_predictions; i++)
		printf("\tSymbol 0x%04x, numerator=%d\n", results->sym[i].symbol, results->sym[i].prob_numerator);
	printf("\n");
#endif


	return( results->sym[0].symbol);
}

/** print_model_allocation
 *  print out the statistics on memory usage
 */
void print_model_allocation()
{
	printf("%d CONTEXT tables allocated.\n", alloc_count);
}

/*************************************************************
 * traverse_tree
 * Given a context string, traverse the tree.
 * Assumes context_string length < max_order
 * INPUTS: context string (it may be empty, that's oK)
 * OUTPUTS: current_order is set to largest order where context
 * 		was found
 * 		contexts[] pointers set up (valid from 0 to current_order)
 * RETURNS: void
 *
 *************************************************************/
void traverse_tree( STRING16 * context_string) {
	int i;
	CONTEXT *table;
	SYMBOL_TYPE test_char;	// char we are currently looking for in tree
	int local_order;
	int index_into_string;
	int done = false;

	// Traverse the tree, trying to find the context string.
	// If the entire context string can't be found,
	// trim the first char off of the context string and try again.
	// At the end of this section,
	//  the context[] array points to the current context
	//	(example: context[1] points to the "a" context and
	//  context[2] points to the "ab" context.
	//  local_order is set to the depth where the context was found
	local_order= 0;		// Start with the order-0 model

	// initialize our moving index into the string.  This
	// index is used to pull characters out of the string.
	index_into_string = 0;
	//printf("traverse_tree: context_string=\"%s\"\n", format_string16(context_string);

	// Slimy test for the blank string
	if (strlen16(context_string) == 0)
		done = true;

	while (!done)	{		// traversing tree
		test_char = get_symbol(context_string, index_into_string );
		table = contexts[ local_order ];
		// Search this table for this character
		i = 0;
		while (i <= table->max_index &&
			table->stats[i].symbol != test_char &&
			table->max_index >= 0)
			{
			i++;
			}
		if ((i > table->max_index) ||			// didn't find this symbol in the table
				((table->links[i].next)->max_index == -1)) // there is no further symbols for this context
												// (this second case only happens for
												// the very end of the training
												// string.  You could have a branch
												// of the tree that is only a few
												// links long, so you can't assume you
												// can always traverse down to
												// a depth of max-order.
			{
			// didn't find it

			// if we are at the end of the string, we are out of luck
			if (strlen16( context_string) == 1)	{
	//			printf("prob: couldn't find this char, going to level -1\n");
				local_order = -1;
				break;					// stop traversing tree
				}
			else {
				shorten_string16( context_string);
				index_into_string = 0;		// start over at beginning of shorter string
				local_order= 0;
				}
			continue;
			}

		// Found this character, go onto the next char
		if (++index_into_string == strlen16( context_string))
			done = true;
		local_order++;
		contexts[local_order] = table->links[i].next;
		}
	table = contexts[ local_order ];
	current_order = local_order;

	/* At this point, we have traversed the tree and we are
	 * pointing to the best context that we can find.
	 * table points to this context table
	 * current_order is the depth (or order) of this model
	 */
	return;
}
/****************************************************8
 * clear_scoreboard
 * Clear the scoreboard
 ******************************************************/
void clear_scoreboard() {
	int i;

    for ( i = 0 ; i < RANGE_OF_SYMBOLS ; i++ )
        scoreboard[ i ] = 0;
}


/*******************************************
 * compute_logloss
 *
 * Given a test string, calculate the average
 * log-loss for encoding the string.
 *
 * INPUTS:
 * 	  test_string = pointer to string to test.
 *
 * RETURNS: nothing
 * *********************************************/
float compute_logloss( STRING16 * test_string, int verbose){
	int i;			// index into test string
	int length=0;		// string length
    SYMBOL s;		// interval information
    int escaped;	// true if we hit ESCAPE situation.
    double prob_numerator, prob_denominator;	// for calculating probabilities for each char
    float fl_prob;	// probability as a float
    float summation = 0.0;	// summation of the log-base-2(P())
    STRING16 * str_sub;

    if (verbose)	{
 //   	printf("compute_logloss: Testing on string \"%s\"\n", format_string_16(test_string));
    	}

    // Create working substring
    str_sub = string16(max_order);

	// Calculate the probability of each character in the test string.
	// Since this calculation has to do with encoding, we need to include
	// the ESCAPE probabilities and the EXCLUSION mechanism, which
	// are handled by the convert_int_to_symbol routine.

	for (i=0; i < strlen16( test_string) ; i++)	{

		// Create the context string, which is the max_order characters
		// before the character in question.
		// Ex.  If the test_string is "abcdef" and i is 5, the
		// test character will be 'f' and the context string (for
		// a model order of 2) is the 2 characters before the 'f', which
		// are "de".

		if (i < max_order)  {
			strncpy16( str_sub, test_string, 0, i);	// create test string
			}
		else {
			strncpy16( str_sub, test_string, i-max_order, max_order);
			}
		prob_numerator = 1;
		prob_denominator = 1;
		clear_scoreboard();
		if (verbose)
			printf("\t%d: log2(P(0x%04x|\"%s\")",
					i, get_symbol(test_string, i), format_string16(str_sub));	// print first part of line

		do {
			//printf("\ttraverse for \"%s\"\n", format_string16(str_sub));
			traverse_tree( str_sub);		// set pointers to best context
			escaped = convert_int_to_symbol( get_symbol(test_string,i), &s);
			//printf("\thigh=%d, low=%d, scale=%d\n", s.high_count, s.low_count, s.scale);
			if (s.scale != 0) {
				prob_numerator *= (s.high_count - s.low_count);
				prob_denominator *= s.scale;
				//printf("\tnum=%f,denom=%f\n", prob_numerator, prob_denominator);
				}
			if (escaped){
				/* If the test char isn't found in this table, shorten the context and try again. */
				//printf("escaped..");
				// was ==>  if (strlen16(str_sub)== 0) {   	// can't shorten anymore
				if (strlen16(str_sub)<= 1) {   	// can't shorten anymore
					//printf(" abort\n");
					escaped=false;				// abort if not found
					}
				else
					shorten_string16( str_sub);	// remove first char from context
				}
		} while (escaped);

		fl_prob = (float) prob_numerator/(float) prob_denominator;
		//printf("fl_prob (%c)= %f\n", test_string[i], fl_prob);
		summation += log10(fl_prob);
		if (verbose)
			printf("= %f\n", log10(fl_prob)/log10(2.0));
	}

	// Convert logbase10 to log base 2 by diving by log-base-10(2)
	summation /= log10(2.0);
	// Take the average and change the sign
	summation /= length;
	summation *= -1.0;
	if (verbose)
		printf("average log-loss is %f\n", summation);
	return (summation);
}	// end of compute_logloss

