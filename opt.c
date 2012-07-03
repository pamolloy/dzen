/*
 * opt.c - Table driven command line argument parsing
 *
 * Functions for options with optional arguments must check if the argument is
 * NULL, if so no argument is required
 */

#include "dzen.h"

#include <stdlib.h>	// strtol, exit
#include <errno.h>	// errno

static int strtoi( char *string )	//TODO Further customize for context
/*
 * Convert the initial part of a string to an integer
 * See: www.stackoverflow.com/questions/2729460
 */
{
	char *remainder = NULL;
	int value = 0;

	errno = 0;
	value = strtol(string, &remainder, 10);

	switch (errno)
	{
		case ERANGE:
			eprint("The data could not be represented.\n");
			exit(EXIT_FAILURE);
		case EINVAL:
			eprint("Unsupported base / radix\n");
			exit(EXIT_FAILURE);
	}
	return value;
}

static void set_lines( Dzen *dzen, char *arg )
{
	dzen->slave_win.max_lines = strtoi(arg);
	if (dzen->slave_win.max_lines) {
		if(MIN_BUF_SIZE % dzen->slave_win.max_lines)
			dzen->slave_win.tsize = MIN_BUF_SIZE + (dzen->slave_win.max_lines - (MIN_BUF_SIZE % dzen->slave_win.max_lines));
		else
			dzen->slave_win.tsize = MIN_BUF_SIZE;

		dzen->slave_win.tbuf = emalloc(dzen->slave_win.tsize * sizeof(char *));
	}
}

static void set_geometry( Dzen *dzen, char *arg )
{
	int t;
	int tx, ty;
	unsigned int tw, th;

	t = XParseGeometry(arg, &tx, &ty, &tw, &th);

	if (t & XValue)
		dzen->title_win.x = tx;
	if (t & YValue) {
		dzen->title_win.y = ty;
		if (!ty && (t & YNegative))
			dzen->title_win.y = -1;	// -0 != +0
	}
	if (t & WidthValue)
		dzen->title_win.width = (signed int) tw;
	if (t & HeightValue)
		dzen->line_height = (signed int) th;
}

static void set_update( Dzen *dzen, char *arg )
{
	dzen->tsupdate = True;
}

static void set_expand( Dzen *dzen, char *arg )
{
	switch (arg[0]) {
		case 'l':
			dzen->title_win.expand = left;
			break;
		case 'c':
			dzen->title_win.expand = both;
			break;
		case 'r':
			dzen->title_win.expand = right;
			break;
		default:
			dzen->title_win.expand = noexpand;
	}
}

static void set_persist( Dzen *dzen, char *arg )
{
	char *endptr = NULL;
	dzen->ispersistent = True;
	if (arg != NULL) {
		dzen->timeout = strtoul(arg, &endptr, 10);	//TODO (PM) Replace with strtoi()
		if (*endptr)
			dzen->timeout = 0;
	}
}

static char alignment_from_char( char align )
{
	switch(align) {
		case 'l' :  return ALIGNLEFT;
		case 'r' :  return ALIGNRIGHT;
		case 'c' :  return ALIGNCENTER;
		default  :  return ALIGNCENTER;
	}

}

static void set_title_align( Dzen *dzen, char *arg )
/*
 * Get alignment from character 'l'eft, 'r'ight and 'c'enter
 */
{
	dzen->title_win.alignment = alignment_from_char(arg[0]);
}

static void set_slave_align( Dzen *dzen, char *arg )
{
	dzen->slave_win.alignment = alignment_from_char(arg[0]);
}

static void set_menu( Dzen *dzen, char *arg )
/*
 * Set menu variable in slave window structure to True
 * Default orientation to vertical
 * Check command line argument for orientation
 */
{
	dzen->slave_win.ismenu = True;
	dzen->slave_win.ishmenu = False;	// Default orientation
	if (arg != NULL) {
		if (arg[0] == 'v')
			dzen->slave_win.ishmenu = False;
		else if (arg[0] == 'h')
			dzen->slave_win.ishmenu = True;
		else
			eprint("Invalid input\n");
	}
}

static void set_font( Dzen *dzen, char *arg )
{
	dzen->fnt = arg;
}

static void set_event( Dzen *dzen, char *arg )
{
	action_string = arg;
}

static void set_title_name( Dzen *dzen, char *arg )
{
	dzen->title_win.name = arg;
}

static void set_slave_name( Dzen *dzen, char *arg )
{
	dzen->slave_win.name = arg;
}

static void set_bg( Dzen *dzen, char *arg )
{
	dzen->bg = arg;
}

static void set_fg( Dzen *dzen, char *arg )
{
	dzen->fg = arg;
}

static void set_y( Dzen *dzen, char *arg )
{
	dzen->title_win.y = strtoi(arg);
}

static void set_x( Dzen *dzen, char *arg )
{
	dzen->title_win.x = strtoi(arg);
}

static void set_width( Dzen *dzen, char *arg )
{
	dzen->slave_win.width = strtoi(arg);
}

static void set_height( Dzen *dzen, char *arg )
{
	dzen->line_height = strtoi(arg);
}

static void set_title_width( Dzen *dzen, char *arg )
{
	dzen->title_win.width = strtoi(arg);
}

//TODO The static char* fnpre is initialized with NULL in main.c and then called
// within the main function
static void set_font_preload( Dzen *dzen, char *arg )
{
	fnpre = estrdup(arg);
}

static void set_xin_screen( Dzen *dzen, char *arg )
{
	dzen->xinescreen = strtoi(arg);
}

//TODO The static int use_ewmh_dock is initialized to 0 in main.c and then
// called within the main function
static void set_dock( Dzen *dzen, char *arg )
{
	use_ewmh_dock = 1;
}

static void print_version( Dzen *dzen, char *arg )
{
	printf("dzen-"VERSION", (C)opyright 2007-2009 Robert Manea\n");
	printf("Enabled optional features:"
#ifdef DZEN_XMP
		" XPM"
#endif
#ifdef DZEN_XFT
		" XFT"
#endif
#ifdef DZEN_XINERAMA
		" XINERAMA"
#endif
		"\n");
	exit(EXIT_SUCCESS);
}

struct option
{
	char *name;
	int len;
	int has_arg;	// 0 false, 1 true, 2 optional
	void (*setter)(Dzen *, char *);
} static opts[] = {
	{ "-l", 2, 1, set_lines },
	{ "-geometry", 9, 1, set_geometry },
	{ "-u", 2, 0, set_update },
	{ "-expand", 7, 1, set_expand },
	{ "-p", 2, 2, set_persist },
	{ "-ta", 3, 1, set_title_align },
	{ "-sa", 4, 1, set_slave_align },
	{ "-m", 2, 2, set_menu },
	{ "-fn", 3, 1, set_font },
	{ "-e", 2, 1, set_event },
	{ "-title-name", 11, 1, set_title_name },
	{ "-slave-name", 11, 1, set_slave_name },
	{ "-bg", 3, 1, set_bg },
	{ "-fg", 2, 1, set_fg },
	{ "-y", 2, 1, set_y },
	{ "-x", 2, 1, set_x },
	{ "-w", 2, 1, set_width },
	{ "-h", 2, 1, set_height },
	{ "-tw", 3, 1, set_title_width },
	{ "-fn-preload", 11, 1, set_font_preload },
#ifdef DZEN_XINERAMA
	{ "-xs", 4, 1, set_xin_screen },
#endif
	{ "-dock", 6, 0, set_dock },
	{ "-v", 3, 0, print_version },
	{ NULL, 0, 0, NULL }
};

//TODO Divide into two functions and print usage on return 0
void parse_opts( int ac, char **av, Dzen *dzen )
/*
 * Loop through the command line arguments, and then the built-in flags, calling
 * the appropriate function
 */
{
	int i, j;
	for (i = 1; i < ac; i++) {	// Check command line arguments
		for (j = 0; opts[j].name != NULL; j++) {	// Compare built-in options
			if (!strncmp(av[i], opts[j].name, opts[j].len)) {
				if (opts[j].has_arg == 1) {	// Required argument
					if (++i < ac) {
						
						opts[j].setter(dzen, av[i]);
					}
					else {
						fprintf(stderr, "Missing argument: %s\n", opts[j].name);
						exit(EXIT_FAILURE);
					}
				}
				else if (opts[j].has_arg == 0) {	// Not required argument
					opts[j].setter(dzen, NULL);
					i++;
					/* Unnecessary argument satisfies the
					 * `setter' function declaration in the
					 * `option' structure declaration */
				}
				else if (opts[j].has_arg == 2) {	// Optional argument
					if (i + 1 > ac)	// Last option, no argument
						opts[j].setter(dzen, NULL);
					else if (av[i + 1][0] == '-')	// Followed by option
						opts[j].setter(dzen, NULL);
					else
						opts[j].setter(dzen, av[++i]);
				}
				break;
			}
		}
	}

	
}

