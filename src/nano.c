/* $Id$ */
/**************************************************************************
 *   nano.c                                                               *
 *                                                                        *
 *   Copyright (C) 1999-2004 Chris Allegretta                             *
 *   This program is free software; you can redistribute it and/or modify *
 *   it under the terms of the GNU General Public License as published by *
 *   the Free Software Foundation; either version 2, or (at your option)  *
 *   any later version.                                                   *
 *                                                                        *
 *   This program is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 *   GNU General Public License for more details.                         *
 *                                                                        *
 *   You should have received a copy of the GNU General Public License    *
 *   along with this program; if not, write to the Free Software          *
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.            *
 *                                                                        *
 **************************************************************************/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <locale.h>
#include <limits.h>
#include <assert.h>
#include "proto.h"
#include "nano.h"

#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif

#ifdef HAVE_TERMIO_H
#include <termio.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#ifndef NANO_SMALL
#include <setjmp.h>
#endif

#ifndef DISABLE_WRAPJUSTIFY
static int fill = 0;	/* Fill - where to wrap lines, basically */
#endif
#ifndef DISABLE_WRAPPING
static int same_line_wrap = 0;	/* Whether wrapped text should be
				   prepended to the next line */
#endif

static struct termios oldterm;	/* The user's original term settings */
static struct sigaction act;	/* For all our fun signal handlers */

#ifndef NANO_SMALL
static sigjmp_buf jmpbuf;	/* Used to return to mainloop after SIGWINCH */
#endif

/* What we do when we're all set to exit. */
void finish(void)
{
    if (!ISSET(NO_HELP))
	blank_bottombars();
    else
	blank_statusbar();

    wrefresh(bottomwin);
    endwin();

    /* Restore the old terminal settings. */
    tcsetattr(0, TCSANOW, &oldterm);

#if !defined(NANO_SMALL) && defined(ENABLE_NANORC)
    if (!ISSET(NO_RCFILE) && ISSET(HISTORYLOG))
	save_history();
#endif

#ifdef DEBUG
    thanks_for_all_the_fish();
#endif

    exit(0);
}

/* Die (gracefully?) */
void die(const char *msg, ...)
{
    va_list ap;

    endwin();
    curses_ended = TRUE;

    /* Restore the old terminal settings. */
    tcsetattr(0, TCSANOW, &oldterm);

    va_start(ap, msg);
    vfprintf(stderr, msg, ap);
    va_end(ap);

    /* save the currently loaded file if it's been modified */
    if (ISSET(MODIFIED))
	die_save_file(filename);

#ifdef ENABLE_MULTIBUFFER
    /* then save all of the other modified loaded files, if any */
    if (open_files != NULL) {
	openfilestruct *tmp;

	tmp = open_files;

	while (open_files->prev != NULL)
	    open_files = open_files->prev;

	while (open_files->next != NULL) {

	    /* if we already saved the file above (i.e, if it was the
	       currently loaded file), don't save it again */
	    if (tmp != open_files) {
		/* make sure open_files->fileage and fileage, and
		   open_files->filebot and filebot, are in sync; they
		   might not be if lines have been cut from the top or
		   bottom of the file */
		fileage = open_files->fileage;
		filebot = open_files->filebot;
		/* save the file if it's been modified */
		if (open_files->file_flags & MODIFIED)
		    die_save_file(open_files->filename);
	    }
	    open_files = open_files->next;
	}
    }
#endif

    exit(1); /* We have a problem: exit w/ errorlevel(1) */
}

void die_save_file(const char *die_filename)
{
    char *ret;
    int i = -1;

    /* If we're using restricted mode, don't write any emergency backup
     * files, since that would allow reading from or writing to files
     * not specified on the command line. */
    if (ISSET(RESTRICTED))
	return;

    /* If we can't save, we have REAL bad problems, but we might as well
       TRY. */
    if (die_filename[0] == '\0')
	ret = get_next_filename("nano.save");
    else {
	char *buf = charalloc(strlen(die_filename) + 6);

	strcpy(buf, die_filename);
	strcat(buf, ".save");
	ret = get_next_filename(buf);
	free(buf);
    }
    if (ret[0] != '\0')
	i = write_file(ret, TRUE, FALSE, TRUE);

    if (i != -1)
	fprintf(stderr, _("\nBuffer written to %s\n"), ret);
    else
	fprintf(stderr, _("\nNo %s written (too many backup files?)\n"), ret);

    free(ret);
}

/* Die with an error message that the screen was too small if, well, the
 * screen is too small. */
void die_too_small(void)
{
    die(_("Window size is too small for nano...\n"));
}

void print_view_warning(void)
{
    statusbar(_("Key illegal in VIEW mode"));
}

/* Initialize global variables -- no better way for now.  If
 * save_cutbuffer is nonzero, don't set cutbuffer to NULL. */
void global_init(int save_cutbuffer)
{
    current_x = 0;
    current_y = 0;

    editwinrows = LINES - 5 + no_help();
    if (editwinrows < MIN_EDITOR_ROWS || COLS < MIN_EDITOR_COLS)
	die_too_small();

    fileage = NULL;
    if (!save_cutbuffer)
	cutbuffer = NULL;
    current = NULL;
    edittop = NULL;
    totlines = 0;
    totsize = 0;
    placewewant = 0;

#ifndef DISABLE_WRAPJUSTIFY
    fill = wrap_at;
    if (fill <= 0)
	fill += COLS;
    if (fill < 0)
	fill = 0;
#endif

    hblank = charalloc(COLS + 1);
    memset(hblank, ' ', COLS);
    hblank[COLS] = '\0';
}

void window_init(void)
{
    editwinrows = LINES - 5 + no_help();
    if (editwinrows < MIN_EDITOR_ROWS)
	die_too_small();

    if (topwin != NULL)
	delwin(topwin);
    if (edit != NULL)
	delwin(edit);
    if (bottomwin != NULL)
	delwin(bottomwin);

    /* Set up the windows. */
    topwin = newwin(2, COLS, 0, 0);
    edit = newwin(editwinrows, COLS, 2, 0);
    bottomwin = newwin(3 - no_help(), COLS, LINES - 3 + no_help(), 0);

    /* Turn the keypad back on. */
    keypad(edit, TRUE);
    keypad(bottomwin, TRUE);
}

#ifndef DISABLE_MOUSE
void mouse_init(void)
{
    if (ISSET(USE_MOUSE)) {
	mousemask(BUTTON1_RELEASED, NULL);
	mouseinterval(50);
    } else
	mousemask(0, NULL);
}
#endif

#ifndef DISABLE_HELP
/* This function allocates help_text, and stores the help string in it. 
 * help_text should be NULL initially. */
void help_init(void)
{
    size_t allocsize = 1;	/* space needed for help_text */
    char *ptr = NULL;
    const shortcut *s;
#ifndef NANO_SMALL
    const toggle *t;
#endif

    /* First set up the initial help text for the current function */
    if (currshortcut == whereis_list || currshortcut == replace_list
	     || currshortcut == replace_list_2)
	ptr = _("Search Command Help Text\n\n "
		"Enter the words or characters you would like to search "
		"for, then hit enter.  If there is a match for the text you "
		"entered, the screen will be updated to the location of the "
		"nearest match for the search string.\n\n "
		"The previous search string will be shown in brackets after "
		"the Search: prompt.  Hitting Enter without entering any text "
		"will perform the previous search.\n\n The following function "
		"keys are available in Search mode:\n\n");
    else if (currshortcut == goto_list)
	ptr = _("Go To Line Help Text\n\n "
		"Enter the line number that you wish to go to and hit "
		"Enter.  If there are fewer lines of text than the "
		"number you entered, you will be brought to the last line "
		"of the file.\n\n The following function keys are "
		"available in Go To Line mode:\n\n");
    else if (currshortcut == insertfile_list)
	ptr = _("Insert File Help Text\n\n "
		"Type in the name of a file to be inserted into the current "
		"file buffer at the current cursor location.\n\n "
		"If you have compiled nano with multiple file buffer "
		"support, and enable multiple buffers with the -F "
		"or --multibuffer command line flags, the Meta-F toggle, or "
		"a nanorc file, inserting a file will cause it to be "
		"loaded into a separate buffer (use Meta-< and > to switch "
		"between file buffers).\n\n If you need another blank "
		"buffer, do not enter any filename, or type in a "
		"nonexistent filename at the prompt and press "
		"Enter.\n\n The following function keys are "
		"available in Insert File mode:\n\n");
    else if (currshortcut == writefile_list)
	ptr = _("Write File Help Text\n\n "
		"Type the name that you wish to save the current file "
		"as and hit Enter to save the file.\n\n If you have "
		"selected text with Ctrl-^, you will be prompted to "
		"save only the selected portion to a separate file.  To "
		"reduce the chance of overwriting the current file with "
		"just a portion of it, the current filename is not the "
		"default in this mode.\n\n The following function keys "
		"are available in Write File mode:\n\n");
#ifndef DISABLE_BROWSER
    else if (currshortcut == browser_list)
	ptr = _("File Browser Help Text\n\n "
		"The file browser is used to visually browse the "
		"directory structure to select a file for reading "
		"or writing.  You may use the arrow keys or Page Up/"
		"Down to browse through the files, and S or Enter to "
		"choose the selected file or enter the selected "
		"directory.  To move up one level, select the directory "
		"called \"..\" at the top of the file list.\n\n The "
		"following function keys are available in the file "
		"browser:\n\n");
    else if (currshortcut == gotodir_list)
	ptr = _("Browser Go To Directory Help Text\n\n "
		"Enter the name of the directory you would like to "
		"browse to.\n\n If tab completion has not been disabled, "
		"you can use the TAB key to (attempt to) automatically "
		"complete the directory name.\n\n The following function "
		"keys are available in Browser Go To Directory mode:\n\n");
#endif
#ifndef DISABLE_SPELLER
    else if (currshortcut == spell_list)
	ptr = _("Spell Check Help Text\n\n "
		"The spell checker checks the spelling of all text "
		"in the current file.  When an unknown word is "
		"encountered, it is highlighted and a replacement can "
		"be edited.  It will then prompt to replace every "
		"instance of the given misspelled word in the "
		"current file.\n\n The following other functions are "
		"available in Spell Check mode:\n\n");
#endif
#ifndef NANO_SMALL
    else if (currshortcut == extcmd_list)
	ptr = _("External Command Help Text\n\n "
		"This menu allows you to insert the output of a command "
		"run by the shell into the current buffer (or a new "
		"buffer in multibuffer mode).\n\n The following keys are "
		"available in this mode:\n\n");
#endif
    else /* Default to the main help list */
	ptr = _(" nano help text\n\n "
	  "The nano editor is designed to emulate the functionality and "
	  "ease-of-use of the UW Pico text editor.  There are four main "
	  "sections of the editor: The top line shows the program "
	  "version, the current filename being edited, and whether "
	  "or not the file has been modified.  Next is the main editor "
	  "window showing the file being edited.  The status line is "
	  "the third line from the bottom and shows important messages. "
	  "The bottom two lines show the most commonly used shortcuts "
	  "in the editor.\n\n "
	  "The notation for shortcuts is as follows: Control-key "
	  "sequences are notated with a caret (^) symbol and can be "
	  "entered either by using the Control (Ctrl) key or pressing the "
	  "Esc key twice.  Escape-key sequences are notated with the Meta "
	  "(M) symbol and can be entered using either the Esc, Alt or "
	  "Meta key depending on your keyboard setup.  Also, pressing Esc "
	  "twice and then typing a three-digit number from 000 to 255 "
	  "will enter the character with the corresponding ASCII code.  "
	  "The following keystrokes are available in the main editor "
	  "window.  Alternative keys are shown in parentheses:\n\n");

    allocsize += strlen(ptr);

    /* The space needed for the shortcut lists, at most COLS characters,
     * plus '\n'. */
    allocsize += (COLS + 1) * length_of_list(currshortcut);

#ifndef NANO_SMALL
    /* If we're on the main list, we also count the toggle help text. 
     * Each line has "M-%c\t\t\t", which fills 24 columns, plus at most
     * COLS - 24 characters, plus '\n'.*/
    if (currshortcut == main_list) {
	size_t endislen = strlen(_("enable/disable"));

	for (t = toggles; t != NULL; t = t->next)
	    allocsize += 8 + strlen(t->desc) + endislen;
    }
#endif /* !NANO_SMALL */

    /* help_text has been freed and set to NULL unless the user resized
     * while in the help screen. */
    free(help_text);

    /* Allocate space for the help text */
    help_text = charalloc(allocsize);

    /* Now add the text we want */
    strcpy(help_text, ptr);
    ptr = help_text + strlen(help_text);

    /* Now add our shortcut info */
    for (s = currshortcut; s != NULL; s = s->next) {
	/* true if the character in s->metaval is shown in first column */
	int meta_shortcut = 0;

	if (s->ctrlval != NANO_NO_KEY) {
#ifndef NANO_SMALL
	    if (s->ctrlval == NANO_HISTORY_KEY)
		ptr += sprintf(ptr, "%.2s", _("Up"));
	    else
#endif
	    if (s->ctrlval == NANO_CONTROL_SPACE)
		ptr += sprintf(ptr, "^%.5s", _("Space"));
	    else if (s->ctrlval == NANO_CONTROL_8)
		ptr += sprintf(ptr, "^?");
	    else
		ptr += sprintf(ptr, "^%c", s->ctrlval + 64);
	}
#ifndef NANO_SMALL
	else if (s->metaval != NANO_NO_KEY) {
	    meta_shortcut = 1;
	    if (s->metaval == NANO_ALT_SPACE)
		ptr += snprintf(ptr, 8, "M-%.5s", _("Space"));
	    else
		ptr += sprintf(ptr, "M-%c", toupper(s->metaval));
	}
#endif

	*(ptr++) = '\t';

	if (s->funcval != NANO_NO_KEY)
	    ptr += sprintf(ptr, "(F%d)", s->funcval - KEY_F0);

	*(ptr++) = '\t';

	if (!meta_shortcut && s->metaval != NANO_NO_KEY)
	    ptr += sprintf(ptr, "(M-%c)", toupper(s->metaval));
	else if (meta_shortcut && s->miscval != NANO_NO_KEY)
	    ptr += sprintf(ptr, "(M-%c)", toupper(s->miscval));

	*(ptr++) = '\t';

	assert(s->help != NULL);
	ptr += sprintf(ptr, "%.*s\n", COLS > 24 ? COLS - 24 : 0, s->help);
    }

#ifndef NANO_SMALL
    /* And the toggles... */
    if (currshortcut == main_list) {
	for (t = toggles; t != NULL; t = t->next) {
	    assert(t->desc != NULL);
	    ptr += sprintf(ptr, "M-%c\t\t\t%s %s\n", toupper(t->val), t->desc,
				_("enable/disable"));
	}
    }
#endif /* !NANO_SMALL */

    /* If all went well, we didn't overwrite the allocated space for
       help_text. */
    assert(strlen(help_text) < allocsize);
}
#endif

/* Create a new filestruct node.  Note that we specifically do not set
 * prevnode->next equal to the new line. */
filestruct *make_new_node(filestruct *prevnode)
{
    filestruct *newnode = (filestruct *)nmalloc(sizeof(filestruct));

    newnode->data = NULL;
    newnode->prev = prevnode;
    newnode->next = NULL;
    newnode->lineno = prevnode != NULL ? prevnode->lineno + 1 : 1;

    return newnode;
}

/* Make a copy of a node to a pointer (space will be malloc()ed). */
filestruct *copy_node(const filestruct *src)
{
    filestruct *dst = (filestruct *)nmalloc(sizeof(filestruct));

    assert(src != NULL);

    dst->data = charalloc(strlen(src->data) + 1);
    dst->next = src->next;
    dst->prev = src->prev;
    strcpy(dst->data, src->data);
    dst->lineno = src->lineno;

    return dst;
}

/* Splice a node into an existing filestruct. */
void splice_node(filestruct *begin, filestruct *newnode, filestruct *end)
{
    if (newnode != NULL) {
	newnode->next = end;
	newnode->prev = begin;
    }
    if (begin != NULL)
	begin->next = newnode;
    if (end != NULL)
	end->prev = newnode;
}

/* Unlink a node from the rest of the filestruct. */
void unlink_node(const filestruct *fileptr)
{
    assert(fileptr != NULL);

    if (fileptr->prev != NULL)
	fileptr->prev->next = fileptr->next;

    if (fileptr->next != NULL)
	fileptr->next->prev = fileptr->prev;
}

/* Delete a node from the filestruct. */
void delete_node(filestruct *fileptr)
{
    if (fileptr != NULL) {
	if (fileptr->data != NULL)
	    free(fileptr->data);
	free(fileptr);
    }
}

/* Okay, now let's duplicate a whole struct! */
filestruct *copy_filestruct(const filestruct *src)
{
    filestruct *head;	/* copy of src, top of the copied list */
    filestruct *prev;	/* temp that traverses the list */

    assert(src != NULL);

    prev = copy_node(src);
    prev->prev = NULL;
    head = prev;
    src = src->next;
    while (src != NULL) {
	prev->next = copy_node(src);
	prev->next->prev = prev;
	prev = prev->next;

	src = src->next;
    }

    prev->next = NULL;
    return head;
}

/* Frees a filestruct. */
void free_filestruct(filestruct *src)
{
    if (src != NULL) {
	while (src->next != NULL) {
	    src = src->next;
	    delete_node(src->prev);
#ifdef DEBUG
	    fprintf(stderr, "%s: free'd a node, YAY!\n", "delete_node()");
#endif
	}
	delete_node(src);
#ifdef DEBUG
	fprintf(stderr, "%s: free'd last node.\n", "delete_node()");
#endif
    }
}

void renumber_all(void)
{
    filestruct *temp;
    int i = 1;

    assert(fileage == NULL || fileage != fileage->next);
    for (temp = fileage; temp != NULL; temp = temp->next)
	temp->lineno = i++;
}

void renumber(filestruct *fileptr)
{
    if (fileptr == NULL || fileptr->prev == NULL || fileptr == fileage)
	renumber_all();
    else {
	int lineno = fileptr->prev->lineno;

	assert(fileptr != fileptr->next);
	for (; fileptr != NULL; fileptr = fileptr->next)
	    fileptr->lineno = ++lineno;
    }
}

/* Print one usage string to the screen, removes lots of duplicate 
 * strings to translate and takes out the parts that shouldn't be 
 * translatable (the flag names). */
void print1opt(const char *shortflag, const char *longflag,
		const char *desc)
{
    printf(" %s\t", shortflag);
    if (strlen(shortflag) < 8)
	printf("\t");

#ifdef HAVE_GETOPT_LONG
    printf("%s\t", longflag);
    if (strlen(longflag) < 8)
	printf("\t\t");
    else if (strlen(longflag) < 16)
	printf("\t");
#endif

    printf("%s\n", desc);
}

void usage(void)
{
#ifdef HAVE_GETOPT_LONG
    printf(_("Usage: nano [+LINE] [GNU long option] [option] [file]\n\n"));
    printf(_("Option\t\tLong option\t\tMeaning\n"));
#else
    printf(_("Usage: nano [+LINE] [option] [file]\n\n"));
    printf(_("Option\t\tMeaning\n"));
#endif /* HAVE_GETOPT_LONG */

    print1opt("-h, -?", "--help", _("Show this message"));
    print1opt(_("+LINE"), "", _("Start at line number LINE"));
#ifndef NANO_SMALL
    print1opt("-A", "--smarthome", _("Enable smart home key"));
    print1opt("-B", "--backup", _("Backup existing files on save"));
    print1opt("-D", "--dos", _("Write file in DOS format"));
    print1opt("-E", "--backupdir=[dir]", _("Directory for writing backup files"));
#endif
#ifdef ENABLE_MULTIBUFFER
    print1opt("-F", "--multibuffer", _("Enable multiple file buffers"));
#endif
#ifdef ENABLE_NANORC
#ifndef NANO_SMALL
    print1opt("-H", "--historylog", _("Log & read search/replace string history"));
#endif
    print1opt("-I", "--ignorercfiles", _("Don't look at nanorc files"));
#endif
#ifndef NANO_SMALL
    print1opt("-M", "--mac", _("Write file in Mac format"));
    print1opt("-N", "--noconvert", _("Don't convert files from DOS/Mac format"));
#endif
#ifndef DISABLE_JUSTIFY
    print1opt(_("-Q [str]"), _("--quotestr=[str]"), _("Quoting string, default \"> \""));
#endif
#ifdef HAVE_REGEX_H
    print1opt("-R", "--regexp", _("Do regular expression searches"));
#endif
#ifndef NANO_SMALL
    print1opt("-S", "--smooth", _("Smooth scrolling"));
#endif
    print1opt(_("-T [#cols]"), _("--tabsize=[#cols]"), _("Set width of a tab in cols to #cols"));
    print1opt("-V", "--version", _("Print version information and exit"));
#ifdef ENABLE_COLOR
    print1opt(_("-Y [str]"), _("--syntax [str]"), _("Syntax definition to use"));
#endif
    print1opt(_("-Z"), _("--restricted"), _("Restricted mode"));
    print1opt("-c", "--const", _("Constantly show cursor position"));
#ifndef NANO_SMALL
    print1opt("-d", "--rebinddelete", _("Fix Backspace/Delete confusion problem"));
    print1opt("-i", "--autoindent", _("Automatically indent new lines"));
    print1opt("-k", "--cut", _("Let ^K cut from cursor to end of line"));
#endif
    print1opt("-l", "--nofollow", _("Don't follow symbolic links, overwrite"));
#ifndef DISABLE_MOUSE
    print1opt("-m", "--mouse", _("Enable mouse"));
#endif
#ifndef DISABLE_OPERATINGDIR
    print1opt(_("-o [dir]"), _("--operatingdir=[dir]"), _("Set operating directory"));
#endif
    print1opt("-p", "--preserve", _("Preserve XON (^Q) and XOFF (^S) keys"));
#ifndef DISABLE_WRAPJUSTIFY
    print1opt(_("-r [#cols]"), _("--fill=[#cols]"), _("Set fill cols to (wrap lines at) #cols"));
#endif
#ifndef DISABLE_SPELLER
    print1opt(_("-s [prog]"), _("--speller=[prog]"), _("Enable alternate speller"));
#endif
    print1opt("-t", "--tempfile", _("Auto save on exit, don't prompt"));
    print1opt("-v", "--view", _("View (read only) mode"));
#ifndef DISABLE_WRAPPING
    print1opt("-w", "--nowrap", _("Don't wrap long lines"));
#endif
    print1opt("-x", "--nohelp", _("Don't show help window"));
    print1opt("-z", "--suspend", _("Enable suspend"));

    /* this is a special case */
    printf(" %s\t\t\t%s\n","-a, -b, -e, -f, -g, -j", _("(ignored, for Pico compatibility)"));

    exit(0);
}

void version(void)
{
    printf(_(" GNU nano version %s (compiled %s, %s)\n"),
	   VERSION, __TIME__, __DATE__);
    printf(_
	   (" Email: nano@nano-editor.org	Web: http://www.nano-editor.org/"));
    printf(_("\n Compiled options:"));

#ifndef ENABLE_NLS
    printf(" --disable-nls");
#endif
#ifdef DEBUG
    printf(" --enable-debug");
#endif
#ifdef NANO_EXTRA
    printf(" --enable-extra");
#endif
#ifdef NANO_SMALL
    printf(" --enable-tiny");
#else
#ifdef DISABLE_BROWSER
    printf(" --disable-browser");
#endif
#ifdef DISABLE_HELP
    printf(" --disable-help");
#endif
#ifdef DISABLE_JUSTIFY
    printf(" --disable-justify");
#endif
#ifdef DISABLE_MOUSE
    printf(" --disable-mouse");
#endif
#ifdef DISABLE_OPERATINGDIR
    printf(" --disable-operatingdir");
#endif
#ifdef DISABLE_SPELLER
    printf(" --disable-speller");
#endif
#ifdef DISABLE_TABCOMP
    printf(" --disable-tabcomp");
#endif
#endif /* NANO_SMALL */
#ifdef DISABLE_WRAPPING
    printf(" --disable-wrapping");
#endif
#ifdef DISABLE_ROOTWRAP
    printf(" --disable-wrapping-as-root");
#endif
#ifdef ENABLE_COLOR
    printf(" --enable-color");
#endif
#ifdef ENABLE_MULTIBUFFER
    printf(" --enable-multibuffer");
#endif
#ifdef ENABLE_NANORC
    printf(" --enable-nanorc");
#endif
#ifdef USE_SLANG
    printf(" --with-slang");
#endif
    printf("\n");
}

int no_help(void)
{
    return ISSET(NO_HELP) ? 2 : 0;
}

int nano_disabled_msg(void)
{
    statusbar(_("Sorry, support for this function has been disabled"));
    return 1;
}

#ifndef NANO_SMALL
static int pid;		/* This is the PID of the newly forked process
			 * below.  It must be global since the signal
			 * handler needs it. */
RETSIGTYPE cancel_fork(int signal)
{
    if (kill(pid, SIGKILL) == -1)
	nperror("kill");
}

int open_pipe(const char *command)
{
    int fd[2];
    FILE *f;
    struct sigaction oldaction, newaction;
			/* original and temporary handlers for SIGINT */
    int cancel_sigs = 0;
    /* cancel_sigs == 1 means that sigaction() failed without changing
     * the signal handlers.  cancel_sigs == 2 means the signal handler
     * was changed, but the tcsetattr() didn't succeed.
     *
     * I use this variable since it is important to put things back when
     * we finish, even if we get errors. */

    /* Make our pipes. */

    if (pipe(fd) == -1) {
	statusbar(_("Could not pipe"));
	return 1;
    }

    /* Fork a child. */

    if ((pid = fork()) == 0) {
	close(fd[0]);
	dup2(fd[1], fileno(stdout));
	dup2(fd[1], fileno(stderr));
	/* If execl() returns at all, there was an error. */
      
	execl("/bin/sh", "sh", "-c", command, 0);
	exit(0);
    }

    /* Else continue as parent. */

    close(fd[1]);

    if (pid == -1) {
	close(fd[0]);
	statusbar(_("Could not fork"));
	return 1;
    }

    /* Before we start reading the forked command's output, we set
     * things up so that ^C will cancel the new process. */

    /* Enable interpretation of the special control keys so that we get
     * SIGINT when Ctrl-C is pressed. */
    enable_signals();

    if (sigaction(SIGINT, NULL, &newaction) == -1) {
	cancel_sigs = 1;
	nperror("sigaction");
    } else {
	newaction.sa_handler = cancel_fork;
	if (sigaction(SIGINT, &newaction, &oldaction) == -1) {
	    cancel_sigs = 1;
	    nperror("sigaction");
	}
    }
    /* Note that now oldaction is the previous SIGINT signal handler,
     * to be restored later. */

    f = fdopen(fd[0], "rb");
    if (f == NULL)
      nperror("fdopen");
    
    read_file(f, "stdin", 0);
    /* if multibuffer mode is on, we could be here in view mode; if so,
       don't set the modification flag */
    if (!ISSET(VIEW_MODE))
	set_modified();

    if (wait(NULL) == -1)
	nperror("wait");

    if (cancel_sigs != 1 && sigaction(SIGINT, &oldaction, NULL) == -1)
	nperror("sigaction");

    /* Disable interpretation of the special control keys so that we can
     * use Ctrl-C for other things. */
    disable_signals();

    return 0;
}
#endif /* !NANO_SMALL */

#ifndef DISABLE_MOUSE
void do_mouse(void)
{
    int mouse_x, mouse_y;

    if (get_mouseinput(&mouse_x, &mouse_y, 1) == 0) {

	/* Click in the edit window to move the cursor, but only when
	   we're not in a subfunction. */
	if (wenclose(edit, mouse_y, mouse_x) && currshortcut == main_list) {
	    int sameline;
		/* Did they click on the line with the cursor?  If they
		   clicked on the cursor, we set the mark. */
	    size_t xcur;
		/* The character they clicked on. */

	    /* Subtract out size of topwin.  Perhaps we need a constant
	       somewhere? */
	    mouse_y -= 2;

	    sameline = (mouse_y == current_y);

	    /* Move to where the click occurred. */
	    for (; current_y < mouse_y && current->next != NULL; current_y++)
		current = current->next;
	    for (; current_y > mouse_y && current->prev != NULL; current_y--)
		current = current->prev;

	    xcur = actual_x(current->data, get_page_start(xplustabs()) +
		mouse_x);

	    /* Selecting where the cursor is toggles the mark.  As does
	       selecting beyond the line length with the cursor at the
	       end of the line. */
	    if (sameline && xcur == current_x) {
		if (ISSET(VIEW_MODE)) {
		    print_view_warning();
		    return;
		}
		do_mark();
	    }

	    current_x = xcur;
	    placewewant = xplustabs();
	    edit_refresh();
	}
    }
    /* FIXME: If we clicked on a location in the statusbar, the cursor
       should move to the location we clicked on. */
}
#endif

/* The user typed a character; add it to the edit buffer. */
void do_char(char ch)
{
    size_t current_len = strlen(current->data);
#if !defined(DISABLE_WRAPPING) || defined(ENABLE_COLOR)
    int do_refresh = FALSE;
	/* Do we have to call edit_refresh(), or can we get away with
	 * update_line()? */
#endif

    if (ch == '\0')		/* Null to newline, if needed. */
	ch = '\n';
    else if (ch == '\n') {	/* Newline to Enter, if needed. */
	do_enter();
	return;
    }

    assert(current != NULL && current->data != NULL);

    /* When a character is inserted on the current magicline, it means
     * we need a new one! */
    if (filebot == current)
	new_magicline();

    /* More dangerousness fun =) */
    current->data = charealloc(current->data, current_len + 2);
    assert(current_x <= current_len);
    charmove(&current->data[current_x + 1], &current->data[current_x],
	current_len - current_x + 1);
    current->data[current_x] = ch;
    totsize++;
    set_modified();

#ifndef NANO_SMALL
    /* Note that current_x has not yet been incremented. */
    if (current == mark_beginbuf && current_x < mark_beginx)
	mark_beginx++;
#endif

    do_right(FALSE);

#ifndef DISABLE_WRAPPING
    /* If we're wrapping text, we need to call edit_refresh(). */
    if (!ISSET(NO_WRAP) && ch != '\t')
	do_refresh = do_wrap(current);
#endif

#ifdef ENABLE_COLOR
    /* If color syntaxes are turned on, we need to call
     * edit_refresh(). */
    if (ISSET(COLOR_SYNTAX))
	do_refresh = TRUE;
#endif

#if !defined(DISABLE_WRAPPING) || defined(ENABLE_COLOR)
    if (do_refresh)
	edit_refresh();
    else
#endif
	update_line(current, current_x);
}

int do_verbatim_input(void)
{
    int *v_kbinput = NULL;	/* Used to hold verbatim input. */
    size_t v_len;		/* Length of verbatim input. */
    size_t i;

    statusbar(_("Verbatim input"));

    v_kbinput = get_verbatim_kbinput(edit, v_kbinput, &v_len, TRUE);

    /* Turn on DISABLE_CURPOS while inserting character(s) and turn it
     * off afterwards, so that if constant cursor position display is
     * on, it will be updated properly. */
    SET(DISABLE_CURPOS);
    for (i = 0; i < v_len; i++)
	do_char((char)v_kbinput[i]);
    UNSET(DISABLE_CURPOS);

    free(v_kbinput);

    return 1;
}

int do_backspace(void)
{
    if (current != fileage || current_x > 0) {
	do_left(FALSE);
	do_delete();
    }
    return 1;
}

int do_delete(void)
{
    int do_refresh = FALSE;
	/* Do we have to call edit_refresh(), or can we get away with
	 * update_line()? */

    assert(current != NULL && current->data != NULL && current_x <=
	strlen(current->data));

    placewewant = xplustabs();

    if (current->data[current_x] != '\0') {
	size_t linelen = strlen(current->data + current_x);

	assert(current_x < strlen(current->data));

	/* Let's get dangerous. */
	charmove(&current->data[current_x], &current->data[current_x + 1],
		linelen);

	null_at(&current->data, linelen + current_x - 1);
#ifndef NANO_SMALL
	if (current_x < mark_beginx && mark_beginbuf == current)
	    mark_beginx--;
#endif
    } else if (current != filebot && (current->next != filebot ||
	current->data[0] == '\0')) {
	/* We can delete the line before filebot only if it is blank: it
	 * becomes the new magicline then. */
	filestruct *foo = current->next;

	assert(current_x == strlen(current->data));

	/* If we're deleting at the end of a line, we need to call
	 * edit_refresh(). */
	if (current->data[current_x] == '\0')
	    do_refresh = TRUE;

	current->data = charealloc(current->data, current_x +
		strlen(foo->data) + 1);
	strcpy(current->data + current_x, foo->data);
#ifndef NANO_SMALL
	if (mark_beginbuf == current->next) {
	    mark_beginx += current_x;
	    mark_beginbuf = current;
	}
#endif
	if (filebot == foo)
	    filebot = current;

	unlink_node(foo);
	delete_node(foo);
	renumber(current);
	totlines--;
	wrap_reset();
    } else
	return 0;

    totsize--;
    set_modified();

#ifdef ENABLE_COLOR
    /* If color syntaxes are turned on, we need to call
     * edit_refresh(). */
    if (ISSET(COLOR_SYNTAX))
	do_refresh = TRUE;
#endif

    if (do_refresh)
	edit_refresh();
    else
	update_line(current, current_x);

    return 1;
}

int do_tab(void)
{
    do_char('\t');
    return 1;
}

/* Someone hits return *gasp!* */
int do_enter(void)
{
    filestruct *newnode = make_new_node(current);
    size_t extra = 0;

    assert(current != NULL && current->data != NULL);

#ifndef NANO_SMALL
    /* Do auto-indenting, like the neolithic Turbo Pascal editor. */
    if (ISSET(AUTOINDENT)) {
	/* If we are breaking the line in the indentation, the new
	 * indentation should have only current_x characters, and
	 * current_x should not change. */
	extra = indent_length(current->data);
	if (extra > current_x)
	    extra = current_x;
	totsize += extra;
    }
#endif
    newnode->data = charalloc(strlen(current->data + current_x) +
	extra + 1);
    strcpy(&newnode->data[extra], current->data + current_x);
#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT))
	strncpy(newnode->data, current->data, extra);
#endif
    null_at(&current->data, current_x);
#ifndef NANO_SMALL
    if (current == mark_beginbuf && current_x < mark_beginx) {
	mark_beginbuf = newnode;
	mark_beginx += extra - current_x;
    }
#endif
    current_x = extra;

    if (current == filebot)
	filebot = newnode;
    splice_node(current, newnode, current->next);

    totsize++;
    renumber(current);
    current = newnode;

#ifndef NANO_SMALL
    /* If we're in smooth scrolling mode and we're on the last line of
     * the edit window, move edittop down one line so that current is
     * onscreen.  This prevents edit_refresh() from centering the
     * screen. */
    if (ISSET(SMOOTHSCROLL) && current_y == editwinrows - 1)
	edittop = edittop->next;
#endif
    edit_refresh();

    totlines++;
    set_modified();
    placewewant = xplustabs();

    return 1;
}

#ifndef NANO_SMALL
int do_next_word(void)
{
    int old_pww = placewewant;
    const filestruct *current_save = current;
    assert(current != NULL && current->data != NULL);

    /* Skip letters in this word first. */
    while (current->data[current_x] != '\0' &&
	    isalnum((int)current->data[current_x]))
	current_x++;

    for (; current != NULL; current = current->next) {
	while (current->data[current_x] != '\0' &&
		!isalnum((int)current->data[current_x]))
	    current_x++;

	if (current->data[current_x] != '\0')
	    break;

	current_x = 0;
    }
    if (current == NULL)
	current = filebot;

    placewewant = xplustabs();

    /* Refresh the screen.  If current has run off the bottom, this
     * call puts it at the center line. */
    edit_redraw(current_save, old_pww);

    return 0;
}

/* The same thing for backwards. */
int do_prev_word(void)
{
    int old_pww = placewewant;
    const filestruct *current_save = current;
    assert(current != NULL && current->data != NULL);

    /* Skip letters in this word first. */
    while (current_x >= 0 && isalnum((int)current->data[current_x]))
	current_x--;

    for (; current != NULL; current = current->prev) {
	while (current_x >= 0 && !isalnum((int)current->data[current_x]))
	    current_x--;

	if (current_x >= 0)
	    break;

	if (current->prev != NULL)
	    current_x = strlen(current->prev->data);
    }

    if (current == NULL) {
	current = fileage;
	current_x = 0;
    } else {
	while (current_x > 0 && isalnum((int)current->data[current_x - 1]))
	    current_x--;
    }

    placewewant = xplustabs();

    /* Refresh the screen.  If current has run off the top, this call
     * puts it at the center line. */
    edit_redraw(current_save, old_pww);

    return 0;
}

int do_mark(void)
{
    TOGGLE(MARK_ISSET);
    if (ISSET(MARK_ISSET)) {
	statusbar(_("Mark Set"));
	mark_beginbuf = current;
	mark_beginx = current_x;
    } else {
	statusbar(_("Mark UNset"));
	edit_refresh();
    }
    return 1;
}
#endif /* !NANO_SMALL */

#ifndef DISABLE_WRAPPING
void wrap_reset(void)
{
    same_line_wrap = 0;
}
#endif

#ifndef DISABLE_WRAPPING
/* We wrap the given line.  Precondition: we assume the cursor has been 
 * moved forward since the last typed character.  Return value:
 * whether we wrapped. */
int do_wrap(filestruct *inptr)
{
    size_t len = strlen(inptr->data);	/* length of the line we wrap */
    int i = 0;			/* generic loop variable */
    int wrap_loc = -1;		/* index of inptr->data where we wrap */
    int word_back = -1;
#ifndef NANO_SMALL
    const char *indentation = NULL;
	/* indentation to prepend to the new line */
    int indent_len = 0;		/* strlen(indentation) */
#endif
    const char *after_break;	/* text after the wrap point */
    int after_break_len;	/* strlen(after_break) */
    int wrapping = 0;		/* do we prepend to the next line? */
    const char *wrap_line = NULL;
	/* the next line, minus indentation */
    int wrap_line_len = 0;	/* strlen(wrap_line) */
    char *newline = NULL;	/* the line we create */
    int new_line_len = 0;	/* eventual length of newline */

/* There are three steps.  First, we decide where to wrap.  Then, we
 * create the new wrap line.  Finally, we clean up. */

/* Step 1, finding where to wrap.  We are going to add a new-line
 * after a whitespace character.  In this step, we set wrap_loc as the
 * location of this replacement.
 *
 * Where should we break the line?  We need the last "legal wrap point"
 * such that the last word before it ended at or before fill.  If there
 * is no such point, we settle for the first legal wrap point.
 *
 * A "legal wrap point" is a whitespace character that is not followed
 * by whitespace.
 *
 * If there is no legal wrap point or we found the last character of the
 * line, we should return without wrapping.
 *
 * Note that the initial indentation does not count as a legal wrap
 * point if we are going to auto-indent!
 *
 * Note that the code below could be optimized, by not calling
 * strnlenpt() so often. */

#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT))
	i = indent_length(inptr->data);
#endif
    wrap_line = inptr->data + i;
    for (; i < len; i++, wrap_line++) {
	/* record where the last word ended */
	if (!isblank(*wrap_line))
	    word_back = i;
	/* if we have found a "legal wrap point" and the current word
	 * extends too far, then we stop */
	if (wrap_loc != -1 && strnlenpt(inptr->data, word_back + 1) > fill)
	    break;
	/* we record the latest "legal wrap point" */
	if (word_back != i && !isblank(wrap_line[1]))
	    wrap_loc = i;
    }
    if (wrap_loc < 0 || i == len)
	return 0;

/* Step 2, making the new wrap line.  It will consist of indentation +
 * after_break + " " + wrap_line (although indentation and wrap_line are
 * conditional on flags and #defines). */

    /* after_break is the text that will be moved to the next line. */
    after_break = inptr->data + wrap_loc + 1;
    after_break_len = len - wrap_loc - 1;
    assert(after_break_len == strlen(after_break));

    /* new_line_len will later be increased by the lengths of indentation
     * and wrap_line. */
    new_line_len = after_break_len;

    /* We prepend the wrapped text to the next line, if the flag is set,
     * and there is a next line, and prepending would not make the line
     * too long. */
    if (same_line_wrap && inptr->next) {
	wrap_line = inptr->next->data;
	wrap_line_len = strlen(wrap_line);

	/* +1 for the space between after_break and wrap_line */
	if ((new_line_len + 1 + wrap_line_len) <= fill) {
	    wrapping = 1;
	    new_line_len += (1 + wrap_line_len);
	}
    }

#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT)) {
	/* Indentation comes from the next line if wrapping, else from
	 * this line. */
	indentation = (wrapping ? wrap_line : inptr->data);
	indent_len = indent_length(indentation);
	if (wrapping)
	    /* The wrap_line text should not duplicate indentation.
	     * Note in this case we need not increase new_line_len. */
	    wrap_line += indent_len;
	else
	    new_line_len += indent_len;
    }
#endif

    /* Now we allocate the new line and copy into it. */
    newline = charalloc(new_line_len + 1);  /* +1 for \0 */
    *newline = '\0';

#ifndef NANO_SMALL
    if (ISSET(AUTOINDENT)) {
	strncpy(newline, indentation, indent_len);
	newline[indent_len] = '\0';
    }
#endif
    strcat(newline, after_break);
    /* We end the old line after wrap_loc.  Note this does not eat the
     * space. */
    null_at(&inptr->data, wrap_loc + 1);
    totsize++;
    if (wrapping) {
	/* In this case, totsize increases by 1 since we add a space
	 * between after_break and wrap_line.  If the line already ends
	 * in a tab or a space, we don't add a space and decrement
	 * totsize to account for that. */
	if (!isblank(newline[strlen(newline) - 1]))
	    strcat(newline, " ");
	else
	    totsize--;
	strcat(newline, wrap_line);
	free(inptr->next->data);
	inptr->next->data = newline;
    } else {
	filestruct *temp = (filestruct *)nmalloc(sizeof(filestruct));

	/* In this case, the file size changes by +1 for the new line, and
	 * +indent_len for the new indentation. */
#ifndef NANO_SMALL
	totsize += indent_len;
#endif
	totlines++;
	temp->data = newline;
	temp->prev = inptr;
	temp->next = inptr->next;
	temp->prev->next = temp;
	/* If temp->next is NULL, then temp is the last line of the
	 * file, so we must set filebot. */
	if (temp->next != NULL)
	    temp->next->prev = temp;
	else
	    filebot = temp;
    }

/* Step 3, clean up.  Here we reposition the cursor and mark, and do some
 * other sundry things. */

    /* later wraps of this line will be prepended to the next line. */
    same_line_wrap = 1;

    /* Each line knows its line number.  We recalculate these if we
     * inserted a new line. */
    if (!wrapping)
	renumber(inptr);

    /* If the cursor was after the break point, we must move it. */
    if (current_x > wrap_loc) {
	current = current->next;
	current_x -=
#ifndef NANO_SMALL
		-indent_len +
#endif
		wrap_loc + 1;
	wrap_reset();
	placewewant = xplustabs();
    }

#ifndef NANO_SMALL
    /* If the mark was on this line after the wrap point, we move it down.
     * If it was on the next line and we wrapped, we must move it
     * right. */
    if (mark_beginbuf == inptr && mark_beginx > wrap_loc) {
	mark_beginbuf = inptr->next;
	mark_beginx -= wrap_loc - indent_len + 1;
    } else if (wrapping && mark_beginbuf == inptr->next)
	mark_beginx += after_break_len;
#endif /* !NANO_SMALL */

    /* Place the cursor. */
    reset_cursor();

    return 1;
}
#endif /* !DISABLE_WRAPPING */

#ifndef DISABLE_SPELLER
/* A word is misspelled in the file.  Let the user replace it.  We
 * return zero if the user cancels. */
int do_int_spell_fix(const char *word)
{
    char *save_search;
    char *save_replace;
    size_t current_x_save = current_x;
    filestruct *current_save = current;
    filestruct *edittop_save = edittop;
	/* Save where we are. */
    int i = 0;
	/* The return value. */
    int reverse_search_set = ISSET(REVERSE_SEARCH);
#ifndef NANO_SMALL
    int case_sens_set = ISSET(CASE_SENSITIVE);
    int mark_set = ISSET(MARK_ISSET);

    SET(CASE_SENSITIVE);
    /* Make sure the marking highlight is off during spell-check. */
    UNSET(MARK_ISSET);
#endif
    /* Make sure spell-check goes forward only. */
    UNSET(REVERSE_SEARCH);

    /* Save the current search/replace strings. */
    search_init_globals();
    save_search = last_search;
    save_replace = last_replace;

    /* Set search/replace strings to misspelled word. */
    last_search = mallocstrcpy(NULL, word);
    last_replace = mallocstrcpy(NULL, word);

    /* Start from the top of the file. */
    current = fileage;
    current_x = -1;

    search_last_line = FALSE;

    /* Find the first whole-word occurrence of word. */
    while (findnextstr(TRUE, TRUE, fileage, 0, word, FALSE) != 0)
	if (is_whole_word(current_x, current->data, word)) {
	    edit_refresh();

	    do_replace_highlight(TRUE, word);

	    /* Allow the replace word to be corrected. */
	    i = statusq(FALSE, spell_list, word,
#ifndef NANO_SMALL
			NULL,
#endif
			 _("Edit a replacement"));

	    do_replace_highlight(FALSE, word);

	    if (i != -1 && strcmp(word, answer)) {
		search_last_line = FALSE;
		current_x--;
		do_replace_loop(word, current_save, &current_x_save, TRUE);
	    }

	    break;
	}

    /* Restore the search/replace strings. */
    free(last_search);
    last_search = save_search;
    free(last_replace);
    last_replace = save_replace;

    /* Restore where we were. */
    current = current_save;
    current_x = current_x_save;
    edittop = edittop_save;

    /* Restore search/replace direction. */
    if (reverse_search_set)
	SET(REVERSE_SEARCH);

#ifndef NANO_SMALL
    if (!case_sens_set)
	UNSET(CASE_SENSITIVE);

    /* Restore marking highlight. */
    if (mark_set)
	SET(MARK_ISSET);
#endif

    return i != -1;
}

/* Integrated spell checking using 'spell' program.  Return value: NULL
 * for normal termination, otherwise the error string. */
const char *do_int_speller(char *tempfile_name)
{
    char *read_buff, *read_buff_ptr, *read_buff_word;
    size_t pipe_buff_size, read_buff_size, read_buff_read, bytesread;
    int spell_fd[2], sort_fd[2], uniq_fd[2], tempfile_fd = -1;
    pid_t pid_spell, pid_sort, pid_uniq;
    int spell_status, sort_status, uniq_status;

    /* Create all three pipes up front. */
    if (pipe(spell_fd) == -1 || pipe(sort_fd) == -1 || pipe(uniq_fd) == -1)
	return _("Could not create pipe");

    statusbar(_("Creating misspelled word list, please wait..."));

    /* A new process to run spell in. */
    if ((pid_spell = fork()) == 0) {

	/* Child continues (i.e, future spell process). */

	close(spell_fd[0]);

	/* Replace the standard input with the temp file. */
	if ((tempfile_fd = open(tempfile_name, O_RDONLY)) == -1)
	    goto close_pipes_and_exit;

	if (dup2(tempfile_fd, STDIN_FILENO) != STDIN_FILENO)
	    goto close_pipes_and_exit;

	close(tempfile_fd);

	/* Send spell's standard output to the pipe. */
	if (dup2(spell_fd[1], STDOUT_FILENO) != STDOUT_FILENO)
	    goto close_pipes_and_exit;

	close(spell_fd[1]);

	/* Start spell program; we are using PATH. */
	execlp("spell", "spell", NULL);

	/* Should not be reached, if spell is found. */
	exit(1);
    }

    /* Parent continues here. */
    close(spell_fd[1]);

    /* A new process to run sort in. */
    if ((pid_sort = fork()) == 0) {

	/* Child continues (i.e, future spell process).  Replace the
	 * standard input with the standard output of the old pipe. */
	if (dup2(spell_fd[0], STDIN_FILENO) != STDIN_FILENO)
	    goto close_pipes_and_exit;

	close(spell_fd[0]);

	/* Send sort's standard output to the new pipe. */
	if (dup2(sort_fd[1], STDOUT_FILENO) != STDOUT_FILENO)
	    goto close_pipes_and_exit;

	close(sort_fd[1]);

	/* Start sort program.  Use -f to remove mixed case without
	 * having to have ANOTHER pipe for tr.  If this isn't portable,
	 * let me know. */
	execlp("sort", "sort", "-f", NULL);

	/* Should not be reached, if sort is found. */
	exit(1);
    }

    close(spell_fd[0]);
    close(sort_fd[1]);

    /* A new process to run uniq in. */
    if ((pid_uniq = fork()) == 0) {

	/* Child continues (i.e, future uniq process).  Replace the
	 * standard input with the standard output of the old pipe. */
	if (dup2(sort_fd[0], STDIN_FILENO) != STDIN_FILENO)
	    goto close_pipes_and_exit;

	close(sort_fd[0]);

	/* Send uniq's standard output to the new pipe. */
	if (dup2(uniq_fd[1], STDOUT_FILENO) != STDOUT_FILENO)
	    goto close_pipes_and_exit;

	close(uniq_fd[1]);

	/* Start uniq program; we are using PATH. */
	execlp("uniq", "uniq", NULL);

	/* Should not be reached, if uniq is found. */
	exit(1);
    }

    close(sort_fd[0]);
    close(uniq_fd[1]);

    /* Child process was not forked successfully. */
    if (pid_spell < 0 || pid_sort < 0 || pid_uniq < 0) {
	close(uniq_fd[0]);
	return _("Could not fork");
    }

    /* Get system pipe buffer size. */
    if ((pipe_buff_size = fpathconf(uniq_fd[0], _PC_PIPE_BUF)) < 1) {
	close(uniq_fd[0]);
	return _("Could not get size of pipe buffer");
    }

    /* Read in the returned spelling errors. */
    read_buff_read = 0;
    read_buff_size = pipe_buff_size + 1;
    read_buff = read_buff_ptr = charalloc(read_buff_size);

    while ((bytesread = read(uniq_fd[0], read_buff_ptr, pipe_buff_size)) > 0) {
	read_buff_read += bytesread;
	read_buff_size += pipe_buff_size;
	read_buff = read_buff_ptr = charealloc(read_buff, read_buff_size);
	read_buff_ptr += read_buff_read;

    }

    *read_buff_ptr = (char)NULL;
    close(uniq_fd[0]);

    /* Process the spelling errors. */
    read_buff_word = read_buff_ptr = read_buff;

    while (*read_buff_ptr != '\0') {

	if ((*read_buff_ptr == '\n') || (*read_buff_ptr == '\r')) {
	    *read_buff_ptr = (char)NULL;
	    if (read_buff_word != read_buff_ptr) {
		if (!do_int_spell_fix(read_buff_word)) {
		    read_buff_word = read_buff_ptr;
		    break;
		}
	    }
	    read_buff_word = read_buff_ptr + 1;
	}
	read_buff_ptr++;
    }

    /* Special case where last word doesn't end with \n or \r. */
    if (read_buff_word != read_buff_ptr)
	do_int_spell_fix(read_buff_word);

    free(read_buff);
    replace_abort();
    edit_refresh();

    /* Process end of spell process. */
    waitpid(pid_spell, &spell_status, 0);
    waitpid(pid_sort, &sort_status, 0);
    waitpid(pid_uniq, &uniq_status, 0);

    if (WIFEXITED(spell_status) == 0 || WEXITSTATUS(spell_status))
	return _("Error invoking \"spell\"");

    if (WIFEXITED(sort_status)  == 0 || WEXITSTATUS(sort_status))
	return _("Error invoking \"sort -f\"");

    if (WIFEXITED(uniq_status) == 0 || WEXITSTATUS(uniq_status))
	return _("Error invoking \"uniq\"");

    /* Otherwise... */
    return NULL;

  close_pipes_and_exit:

    /* Don't leak any handles. */
    close(tempfile_fd);
    close(spell_fd[0]);
    close(spell_fd[1]);
    close(sort_fd[0]);
    close(sort_fd[1]);
    close(uniq_fd[0]);
    close(uniq_fd[1]);
    exit(1);
}

/* External spell checking.  Return value: NULL for normal termination,
 * otherwise the error string. */
const char *do_alt_speller(char *tempfile_name)
{
    int alt_spell_status, lineno_cur = current->lineno;
    int x_cur = current_x, y_cur = current_y, pww_cur = placewewant;
    pid_t pid_spell;
    char *ptr;
    static int arglen = 3;
    static char **spellargs = (char **)NULL;
#ifndef NANO_SMALL
    int mark_set = ISSET(MARK_ISSET);
    int mbb_lineno_cur = 0;
	/* We're going to close the current file, and open the output of
	 * the alternate spell command.  The line that mark_beginbuf
	 * points to will be freed, so we save the line number and
	 * restore afterwards. */

    if (mark_set) {
	mbb_lineno_cur = mark_beginbuf->lineno;
	UNSET(MARK_ISSET);
    }
#endif

    endwin();

    /* Set up an argument list to pass execvp(). */
    if (spellargs == NULL) {
	spellargs = (char **)nmalloc(arglen * sizeof(char *));

	spellargs[0] = strtok(alt_speller, " ");
	while ((ptr = strtok(NULL, " ")) != NULL) {
	    arglen++;
	    spellargs = (char **)nrealloc(spellargs, arglen * sizeof(char *));
	    spellargs[arglen - 3] = ptr;
	}
	spellargs[arglen - 1] = NULL;
    }
    spellargs[arglen - 2] = tempfile_name;

    /* Start a new process for the alternate speller. */
    if ((pid_spell = fork()) == 0) {
	/* Start alternate spell program; we are using PATH. */
	execvp(spellargs[0], spellargs);

	/* Should not be reached, if alternate speller is found!!! */
	exit(1);
    }

    /* Could not fork?? */
    if (pid_spell < 0)
	return _("Could not fork");

    /* Wait for alternate speller to complete. */
    wait(&alt_spell_status);

    if (!WIFEXITED(alt_spell_status) || WEXITSTATUS(alt_spell_status) != 0) {
	char *altspell_error = NULL;
	char *invoke_error = _("Could not invoke \"%s\"");
	int msglen = strlen(invoke_error) + strlen(alt_speller) + 2;

	altspell_error = charalloc(msglen);
	snprintf(altspell_error, msglen, invoke_error, alt_speller);
	return altspell_error;
    }

    refresh();
#ifndef NANO_SMALL
    if (!mark_set) {
	/* Only reload the temp file if it isn't a marked selection. */
#endif
	free_filestruct(fileage);
	global_init(1);
	open_file(tempfile_name, 0, 1);
#ifndef NANO_SMALL
    }

    if (mark_set) {
	do_gotopos(mbb_lineno_cur, mark_beginx, y_cur, 0);
	mark_beginbuf = current;
	/* In case the line got shorter, assign mark_beginx. */
	mark_beginx = current_x;
	SET(MARK_ISSET);
    }
#endif

    /* Go back to the old position, mark the file as modified, and make
     * sure that the titlebar is refreshed. */
    do_gotopos(lineno_cur, x_cur, y_cur, pww_cur);
    set_modified();
    clearok(topwin, FALSE);
    titlebar(NULL);

    return NULL;
}

int do_spell(void)
{
    int i;
    char *temp = safe_tempnam(0, "nano.");
    const char *spell_msg;

    if (temp == NULL) {
	statusbar(_("Could not create temp file: %s"), strerror(errno));
	return 0;
    }

#ifndef NANO_SMALL
    if (ISSET(MARK_ISSET))
	i = write_marked(temp, TRUE, FALSE);
    else
#endif
	i = write_file(temp, TRUE, FALSE, FALSE);

    if (i == -1) {
	statusbar(_("Unable to write temp file: %s"), strerror(errno));
	free(temp);
	return 0;
    }

#ifdef ENABLE_MULTIBUFFER
    /* Update the current open_files entry before spell-checking, in
     * case any problems occur. */
    add_open_file(1);
#endif

    spell_msg = alt_speller != NULL ? do_alt_speller(temp) :
	do_int_speller(temp);
    unlink(temp);
    free(temp);

    if (spell_msg != NULL) {
	statusbar(_("Spell checking failed: %s: %s"), spell_msg,
		strerror(errno));
	return 0;
    } else
	statusbar(_("Finished checking spelling"));

    return 1;
}
#endif /* !DISABLE_SPELLER */

#if !defined(NANO_SMALL) || !defined(DISABLE_JUSTIFY)
/* The "indentation" of a line is the whitespace between the quote part
 * and the non-whitespace of the line. */
size_t indent_length(const char *line)
{
    size_t len = 0;

    assert(line != NULL);
    while (isblank(*line)) {
	line++;
	len++;
    }
    return len;
}
#endif /* !NANO_SMALL || !DISABLE_JUSTIFY */

#ifndef DISABLE_JUSTIFY
/* justify_format() replaces Tab by Space and multiple spaces by 1
 * (except it maintains 2 after a . ! or ?).  Note the terminating \0
 * counts as a space.
 *
 * justify_format() might make line->data shorter, and change the actual
 * pointer with null_at().
 *
 * justify_format() will not look at the first skip characters of line.
 * skip should be at most strlen(line->data).  The character at
 * line[skip + 1] must not be whitespace. */
void justify_format(filestruct *line, size_t skip)
{
    char *back, *front;

    /* These four asserts are assumptions about the input data. */
    assert(line != NULL);
    assert(line->data != NULL);
    assert(skip < strlen(line->data));
    assert(!isblank(line->data[skip]));

    back = line->data + skip;
    for (front = back; ; front++) {
	int remove_space = FALSE;
	    /* Do we want to remove this space? */

	if (*front == '\t')
	    *front = ' ';

	/* These tests are safe since line->data + skip is not a
	 * space. */
	if ((*front == '\0' || *front == ' ') && *(front - 1) == ' ') {
	    const char *bob = front - 2;

	    remove_space = TRUE;
	    for (bob = back - 2; bob >= line->data + skip; bob--) {
		if (strchr(punct, *bob) != NULL) {
		    remove_space = FALSE;
		    break;
		}
		if (strchr(brackets, *bob) == NULL)
		    break;
	    }
	}

	if (remove_space) {
	    /* Now *front is a space we want to remove.  We do that by
	     * simply failing to assign it to *back. */
#ifndef NANO_SMALL
	    if (mark_beginbuf == line && back - line->data < mark_beginx)
		mark_beginx--;
#endif
	    if (*front == '\0')
		*(back - 1) = '\0';
	} else {
	    *back = *front;
	    back++;
	}
	if (*front == '\0')
	    break;
    }

    back--;
    assert(*back == '\0' && *front == '\0');

    /* Now back is the new end of line->data. */
    if (back != front) {
	totsize -= front - back;
	null_at(&line->data, back - line->data);
#ifndef NANO_SMALL
	if (mark_beginbuf == line && back - line->data < mark_beginx)
	    mark_beginx = back - line->data;
#endif
    }
}

/* The "quote part" of a line is the largest initial substring matching
 * the quote string.  This function returns the length of the quote part
 * of the given line.
 *
 * Note that if !HAVE_REGEX_H then we match concatenated copies of
 * quotestr. */
#ifdef HAVE_REGEX_H
size_t quote_length(const char *line, const regex_t *qreg)
{
    regmatch_t matches;
    int rc = regexec(qreg, line, 1, &matches, 0);

    if (rc == REG_NOMATCH || matches.rm_so == (regoff_t) -1)
	return 0;
    /* matches.rm_so should be 0, since the quote string should start with
     * the caret ^. */
    return matches.rm_eo;
}
#else	/* !HAVE_REGEX_H */
size_t quote_length(const char *line)
{
    size_t qdepth = 0;
    size_t qlen = strlen(quotestr);

    /* Compute quote depth level */
    while (!strcmp(line + qdepth, quotestr))
	qdepth += qlen;
    return qdepth;
}
#endif	/* !HAVE_REGEX_H */

/* a_line and b_line are lines of text.  The quotation part of a_line is
 * the first a_quote characters.  Check that the quotation part of
 * b_line is the same. */
int quotes_match(const char *a_line, size_t a_quote, IFREG(const char
	*b_line, const regex_t *qreg))
{
    /* Here is the assumption about a_quote: */
    assert(a_quote == quote_length(IFREG(a_line, qreg)));
    return a_quote == quote_length(IFREG(b_line, qreg)) &&
	!strncmp(a_line, b_line, a_quote);
}

/* We assume a_line and b_line have no quote part.  Then, we return whether
 * b_line could follow a_line in a paragraph. */
size_t indents_match(const char *a_line, size_t a_indent, const char
	*b_line, size_t b_indent)
{
    assert(a_indent == indent_length(a_line));
    assert(b_indent == indent_length(b_line));

    return b_indent <= a_indent && !strncmp(a_line, b_line, b_indent);
}

/* Put the next par_len lines, starting with first_line, in the cut
 * buffer, not allowing them to be concatenated.  We assume there are
 * enough lines after first_line.  We leave copies of the lines in
 * place, too.  We return the new copy of first_line. */
filestruct *backup_lines(filestruct *first_line, size_t par_len, size_t
	quote_len)
{
    /* We put the original lines, not copies, into the cutbuffer, just
     * out of a misguided sense of consistency, so if you uncut, you get
     * the actual same paragraph back, not a copy. */
    filestruct *alice = first_line;

    set_modified();
    cutbuffer = NULL;
    for (; par_len > 0; par_len--) {
	filestruct *bob = copy_node(alice);

	if (alice == first_line)
	    first_line = bob;
	if (alice == current)
	    current = bob;
	if (alice == edittop)
	    edittop = bob;
#ifndef NANO_SMALL
	if (alice == mark_beginbuf)
	    mark_beginbuf = bob;
#endif
	justify_format(bob, quote_len + indent_length(bob->data + quote_len));

	assert(alice != NULL && bob != NULL);
	add_to_cutbuffer(alice, FALSE);
	splice_node(bob->prev, bob, bob->next);
	alice = bob->next;
    }
    return first_line;
}

/* Is it possible to break line at or before goal? */
int breakable(const char *line, int goal)
{
    for (; *line != '\0' && goal >= 0; line++) {
	if (isblank(*line))
	    return TRUE;

	if (is_cntrl_char(*line) != 0)
	    goal -= 2;
	else
	    goal -= 1;
    }
    /* If goal is not negative, the whole line (one word) was short
     * enough. */
    return goal >= 0;
}

/* We are trying to break a chunk off line.  We find the last space such
 * that the display length to there is at most goal + 1.  If there is
 * no such space, and force is not 0, then we find the first space.
 * Anyway, we then take the last space in that group of spaces.  The
 * terminating '\0' counts as a space. */
int break_line(const char *line, int goal, int force)
{
    /* Note that we use int instead of size_t, since goal is at most COLS,
     * the screen width, which will always be reasonably small. */
    int space_loc = -1;
	/* Current tentative return value.  Index of the last space we
	 * found with short enough display width.  */
    int cur_loc = 0;
	/* Current index in line. */

    assert(line != NULL);
    for (; *line != '\0' && goal >= 0; line++, cur_loc++) {
	if (*line == ' ')
	    space_loc = cur_loc;
	assert(*line != '\t');

	if (is_cntrl_char(*line))
	    goal -= 2;
	else
	    goal--;
    }
    if (goal >= 0)
	/* In fact, the whole line displays shorter than goal. */
	return cur_loc;
    if (space_loc == -1) {
	/* No space found short enough. */
	if (force)
	    for (; *line != '\0'; line++, cur_loc++)
		if (*line == ' ' && *(line + 1) != ' ' && *(line + 1) != '\0')
		    return cur_loc;
	return -1;
    }
    /* Perhaps the character after space_loc is a space.  But because
     * of justify_format(), there can be only two adjacent. */
    if (*(line - cur_loc + space_loc + 1) == ' ' ||
	*(line - cur_loc + space_loc + 1) == '\0')
	space_loc++;
    return space_loc;
}

/* Search a paragraph.  If search_type is JUSTIFY, search for the
 * beginning of the current paragraph or, if we're at the end of it, the
 * beginning of the next paragraph.  If search_type is BEGIN, search for
 * the beginning of the current paragraph or, if we're already there,
 * the beginning of the previous paragraph.  If search_type is END,
 * search for the end of the current paragraph or, if we're already
 * there, the end of the next paragraph.  Afterwards, save the quote
 * length, paragraph length, and indentation length in *quote, *par, and
 * *indent if they aren't NULL, and refresh the screen if do_refresh is
 * TRUE.  Return 0 if we found a paragraph, or 1 if there was an error
 * or we didn't find a paragraph.
 *
 * To explain the searching algorithm, I first need to define some
 * phrases about paragraphs and quotation:
 *   A line of text consists of a "quote part", followed by an
 *   "indentation part", followed by text.  The functions quote_length()
 *   and indent_length() calculate these parts.
 *
 *   A line is "part of a paragraph" if it has a part not in the quote
 *   part or the indentation.
 *
 *   A line is "the beginning of a paragraph" if it is part of a
 *   paragraph and
 *	1) it is the top line of the file, or
 *	2) the line above it is not part of a paragraph, or
 *	3) the line above it does not have precisely the same quote
 *	   part, or
 *	4) the indentation of this line is not an initial substring of
 *	   the indentation of the previous line, or
 *	5) this line has no quote part and some indentation, and
 *	   AUTOINDENT is not set.
 *   The reason for number 5) is that if AUTOINDENT is not set, then an
 *   indented line is expected to start a paragraph, like in books.
 *   Thus, nano can justify an indented paragraph only if AUTOINDENT is
 *   turned on.
 *
 *   A contiguous set of lines is a "paragraph" if each line is part of
 *   a paragraph and only the first line is the beginning of a
 *   paragraph. */
int do_para_search(justbegend search_type, size_t *quote, size_t *par,
	size_t *indent, int do_refresh)
{
    int old_pww = placewewant;
    const filestruct *current_save = current;
    size_t quote_len;
	/* Length of the initial quotation of the paragraph we
	 * search. */
    size_t par_len;
	/* Number of lines in that paragraph. */
    size_t indent_len;
	/* Generic indentation length. */
    filestruct *line;
	/* Generic line of text. */
    static int do_restart = 1;
    	/* Whether we're restarting when searching for the beginning
    	 * line of the paragraph. */

#ifdef HAVE_REGEX_H
    regex_t qreg;	/* qreg is the compiled quotation regexp.  We no
			 * longer care about quotestr. */
    int rc = regcomp(&qreg, quotestr, REG_EXTENDED);

    if (rc != 0) {
	size_t size = regerror(rc, &qreg, NULL, 0);
	char *strerror = charalloc(size);

	regerror(rc, &qreg, strerror, size);
	statusbar(_("Bad quote string %s: %s"), quotestr, strerror);
	free(strerror);
	return 1;
    }
#endif

    /* Here is an assumption that is always true anyway. */
    assert(current != NULL);

    current_x = 0;

  restart_para_search:
    /* Here we find the first line of the paragraph to search.  If the
     * current line is in a paragraph, then we move back to the first
     * line.  Otherwise, we move to the first line that is in a
     * paragraph. */
    quote_len = quote_length(IFREG(current->data, &qreg));
    indent_len = indent_length(current->data + quote_len);

    if (current->data[quote_len + indent_len] != '\0') {
	/* This line is part of a paragraph.  So we must search back to
	 * the first line of this paragraph.  First we check items 1)
	 * and 3) above. */
	while (current->prev != NULL && quotes_match(current->data,
			quote_len, IFREG(current->prev->data, &qreg))) {
	    size_t temp_id_len =
			indent_length(current->prev->data + quote_len);
		/* The indentation length of the previous line. */

	    /* Is this line the beginning of a paragraph, according to
	     * items 2), 5), or 4) above?  If so, stop. */
	    if (current->prev->data[quote_len + temp_id_len] == '\0' ||
		(quote_len == 0 && indent_len > 0
#ifndef NANO_SMALL
		&& !ISSET(AUTOINDENT)
#endif
		) || !indents_match(current->prev->data + quote_len,
		temp_id_len, current->data + quote_len, indent_len))
		break;
	    indent_len = temp_id_len;
	    current = current->prev;
	    current_y--;
	}
    } else if (search_type == BEGIN) {
	/* This line is not part of a paragraph.  Move up until we get
	 * to a non "blank" line, and then move down once. */
	do {
	    /* There is no previous paragraph, so nothing to move to. */
	    if (current->prev == NULL) {
		placewewant = 0;
		if (do_refresh)
		    edit_redraw(current_save, old_pww);
#ifdef HAVE_REGEX_H
		if (!do_restart)
		    regfree(&qreg);
#endif
		return 1;
	    }
	    current = current->prev;
	    current_y--;
	    quote_len = quote_length(IFREG(current->data, &qreg));
	    indent_len = indent_length(current->data + quote_len);
	} while (current->data[quote_len + indent_len] == '\0');
	current = current->next;
    } else {
	/* This line is not part of a paragraph.  Move down until we get
	 * to a non "blank" line. */
	do {
	    /* There is no next paragraph, so nothing to move to. */
	    if (current->next == NULL) {
		placewewant = 0;
		if (do_refresh)
		    edit_redraw(current_save, old_pww);
#ifdef HAVE_REGEX_H
		regfree(&qreg);
#endif
		return 1;
	    }
	    current = current->next;
	    current_y++;
	    quote_len = quote_length(IFREG(current->data, &qreg));
	    indent_len = indent_length(current->data + quote_len);
	} while (current->data[quote_len + indent_len] == '\0');
    }

    /* Now current is the first line of the paragraph, and quote_len is
     * the quotation length of that line. */

    /* Next step, compute par_len, the number of lines in this
     * paragraph. */
    line = current;
    par_len = 1;
    indent_len = indent_length(line->data + quote_len);

    while (line->next != NULL && quotes_match(current->data, quote_len,
	IFREG(line->next->data, &qreg))) {
	size_t temp_id_len = indent_length(line->next->data + quote_len);

	if (!indents_match(line->data + quote_len, indent_len,
		line->next->data + quote_len, temp_id_len) ||
		line->next->data[quote_len + temp_id_len] == '\0' ||
		(quote_len == 0 && temp_id_len > 0
#ifndef NANO_SMALL
			&& !ISSET(AUTOINDENT)
#endif
		))
	    break;
	indent_len = temp_id_len;
	line = line->next;
	par_len++;
    }

    if (search_type == BEGIN) {
	/* We're on the same line we started on.  Move up until we get
	 * to a non-"blank" line, restart the search from there until we
	 * find a line that's part of a paragraph, and search once more
	 * so that we end up at the beginning of that paragraph. */
	if (current != fileage && current == current_save && do_restart) {
	    while (current->prev != NULL) {
		size_t i, j = 0;
		current = current->prev;
		current_y--;
		/* Skip over lines consisting only of spacing
		 * characters, as searching for the end of the paragraph
		 * does. */
		for (i = 0; current->data[i] != '\0'; i++) {
		    if (isblank(current->data[i]))
			j++;
		    else {
			i = 0;
			j = 1;
			break;
		    }
		}
		if (i != j && strlen(current->data) >=
			(quote_len + indent_len) &&
			current->data[quote_len + indent_len] != '\0') {
		    do_restart = 0;
		    break;
		}
	    }
	    goto restart_para_search;
	} else
	    do_restart = 1;
    }

#ifdef HAVE_REGEX_H
    /* We no longer need to check quotation. */
    regfree(&qreg);
#endif

    /* Now par_len is the number of lines in this paragraph.  We should
     * never call quotes_match() or quote_length() again. */

    /* If we're searching for the end of the paragraph, move down the
     * number of lines in the paragraph. */
    if (search_type == END) {
	for (; par_len > 0; current_y++, par_len--)
	    current = current->next;
    }

    /* Refresh the screen if needed. */
    if (do_refresh)
	edit_redraw(current_save, old_pww);

    /* Save the values of quote_len, par_len, and indent_len if
     * needed. */
    if (quote != NULL)
	*quote = quote_len;
    if (par != NULL)
	*par = par_len;
    if (indent != NULL)
	*indent = indent_len;

    return 0;
}

int do_para_begin(void)
{
    return do_para_search(BEGIN, NULL, NULL, NULL, TRUE);
}

int do_para_end(void)
{
    return do_para_search(END, NULL, NULL, NULL, TRUE);
}

/* If full_justify is TRUE, justify the entire file.  Otherwise, justify
 * the current paragraph. */
int do_justify(int full_justify)
{
    size_t quote_len;
	/* Length of the initial quotation of the paragraph we
	 * justify. */
    size_t par_len;
	/* Number of lines in that paragraph. */
    filestruct *first_par_line = NULL;
	/* Will be the first line of the resulting justified paragraph.
	 * For restoring after uncut. */
    filestruct *last_par_line;
	/* Will be the last line of the result, also for uncut. */
    filestruct *cutbuffer_save = cutbuffer;
	/* When the paragraph gets modified, all lines from the changed
	 * one down are stored in the cutbuffer.  We back up the
	 * original to restore it later. */

    /* We save these global variables to be restored if the user
     * unjustifies.  Note we don't need to save totlines. */
    int current_x_save = current_x;
    int current_y_save = current_y;
    int flags_save = flags;
    long totsize_save = totsize;
    filestruct *current_save = current;
    filestruct *edittop_save = edittop;
#ifndef NANO_SMALL
    filestruct *mark_beginbuf_save = mark_beginbuf;
    int mark_beginx_save = mark_beginx;
#endif

    size_t indent_len;	/* Generic indentation length. */
    size_t i;		/* Generic loop variable. */

    /* If we're justifying the entire file, start at the beginning. */
    if (full_justify)
	current = fileage;

    last_par_line = current;

    while (TRUE) {

	/* First, search for the beginning of the current paragraph or,
	 * if we're at the end of it, the beginning of the next
	 * paragraph.  Save the quote length, paragraph length, and
	 * indentation length and don't refresh the screen yet (since
	 * we'll do that after we justify).  If the search failed and
	 * we're justifying the whole file, move the last line of the
	 * text we're justifying to just before the magicline, which is
	 * where it'll be anyway if we've searched the entire file, and
	 * break out of the loop; otherwise, refresh the screen and get
	 * out. */
	if (do_para_search(JUSTIFY, &quote_len, &par_len, &indent_len,
		FALSE) != 0) {
	    if (full_justify) {
		/* This should be safe in the event of filebot->prev's
		 * being NULL, since only last_par_line->next is used if
		 * we eventually unjustify. */
		last_par_line = filebot->prev;
		break;
	    } else {
		edit_refresh();
		return 0;
	    }
	}

	/* Next step, we loop through the lines of this paragraph,
	 * justifying each one individually. */
	for (; par_len > 0; current_y++, par_len--) {
	    size_t line_len;
	    size_t display_len;
		/* The width of current in screen columns. */
	    int break_pos;
		/* Where we will break the line. */

	    indent_len = quote_len + indent_length(current->data +
		quote_len);

	    /* justify_format() removes excess spaces from the line, and
	     * changes tabs to spaces.  After calling it, we call
	     * backup_lines(), which copies the original paragraph to
	     * the cutbuffer for unjustification and then calls
	     * justify_format() on the remaining lines. */
	    justify_format(current, indent_len);
	    if (first_par_line == NULL)
		first_par_line = backup_lines(current, full_justify ?
			filebot->lineno - current->lineno : par_len, quote_len);

	    line_len = strlen(current->data);
	    display_len = strlenpt(current->data);

	    if (display_len > fill) {
		/* The line is too long.  Try to wrap it to the next. */
	        break_pos = break_line(current->data + indent_len,
			fill - strnlenpt(current->data, indent_len), TRUE);
		if (break_pos == -1 || break_pos + indent_len == line_len)
		    /* We can't break the line, or don't need to, so
		     * just go on to the next. */
		    goto continue_loc;
		break_pos += indent_len;
		assert(break_pos < line_len);
		if (par_len == 1) {
		    /* There is no next line in this paragraph.  We make
		     * a new line and copy text after break_pos into
		     * it. */
		    splice_node(current, make_new_node(current), current->next);
		    /* In a non-quoted paragraph, we copy the indent
		     * only if AUTOINDENT is turned on. */
		    if (quote_len == 0
#ifndef NANO_SMALL
			&& !ISSET(AUTOINDENT)
#endif
			)
			    indent_len = 0;
		    current->next->data = charalloc(indent_len + line_len -
			break_pos);
		    strncpy(current->next->data, current->data, indent_len);
		    strcpy(current->next->data + indent_len,
			current->data + break_pos + 1);
		    assert(strlen(current->next->data) ==
			indent_len + line_len - break_pos - 1);
		    totlines++;
		    totsize += indent_len;
		    par_len++;
		} else {
		    size_t next_line_len = strlen(current->next->data);

		    indent_len = quote_len +
			indent_length(current->next->data + quote_len);
		    current->next->data = charealloc(current->next->data,
			next_line_len + line_len - break_pos + 1);

		    charmove(current->next->data + indent_len + line_len -
			break_pos, current->next->data + indent_len,
			next_line_len - indent_len + 1);
		    strcpy(current->next->data + indent_len,
			current->data + break_pos + 1);
		    current->next->data[indent_len + line_len - break_pos - 1]
			= ' ';
#ifndef NANO_SMALL
		    if (mark_beginbuf == current->next) {
			if (mark_beginx < indent_len)
			    mark_beginx = indent_len;
			mark_beginx += line_len - break_pos;
		    }
#endif
		}
#ifndef NANO_SMALL
		if (mark_beginbuf == current && mark_beginx > break_pos) {
		    mark_beginbuf = current->next;
		    mark_beginx -= break_pos + 1 - indent_len;
		}
#endif
		null_at(&current->data, break_pos);
		current = current->next;
	    } else if (display_len < fill && par_len > 1) {
		size_t next_line_len;

		indent_len = quote_len +
			indent_length(current->next->data + quote_len);
		/* If we can't pull a word from the next line up to this
		 * one, just go on. */
		if (!breakable(current->next->data + indent_len,
			fill - display_len - 1))
		    goto continue_loc;

		break_pos = break_line(current->next->data + indent_len,
			fill - display_len - 1, FALSE);
		assert(break_pos != -1);

		current->data = charealloc(current->data,
			line_len + break_pos + 2);
		current->data[line_len] = ' ';
		strncpy(current->data + line_len + 1,
			current->next->data + indent_len, break_pos);
		current->data[line_len + break_pos + 1] = '\0';
#ifndef NANO_SMALL
		if (mark_beginbuf == current->next) {
		    if (mark_beginx < indent_len + break_pos) {
			mark_beginbuf = current;
			if (mark_beginx <= indent_len)
			    mark_beginx = line_len + 1;
			else
			    mark_beginx = line_len + 1 + mark_beginx -
				indent_len;
		    } else
			mark_beginx -= break_pos + 1;
		}
#endif
		next_line_len = strlen(current->next->data);
		if (indent_len + break_pos == next_line_len) {
		    filestruct *line = current->next;

		    /* Don't destroy edittop! */
		    if (line == edittop)
			edittop = current;

		    unlink_node(line);
		    delete_node(line);
		    totlines--;
		    totsize -= indent_len;
		    current_y--;
		} else {
		    charmove(current->next->data + indent_len,
			current->next->data + indent_len + break_pos + 1,
			next_line_len - break_pos - indent_len);
		    null_at(&current->next->data, next_line_len - break_pos);
		    current = current->next;
		}
	    } else
  continue_loc:
		/* Go to the next line. */
		current = current->next;

	    /* If the line we were on before still exists, and it was
	     * not the last line of the paragraph, add a space to the
	     * end of it to replace the one removed or left out by
	     * justify_format(). */
	    if (current->prev != NULL && par_len > 1) {
		size_t prev_line_len = strlen(current->prev->data);
		current->prev->data = charealloc(current->prev->data,
			prev_line_len + 2);
		current->prev->data[prev_line_len] = ' ';
		current->prev->data[prev_line_len + 1] = '\0';
		totsize++;
	    }
	}

	/* We've just justified a paragraph. If we're not justifying the
	 * entire file, break out of the loop.  Otherwise, continue the
	 * loop so that we justify all the paragraphs in the file. */
	if (!full_justify)
	    break;

    } /* while (TRUE) */

    /* We are now done justifying the paragraph or the file, so clean
     * up.  totlines, totsize, and current_y have been maintained above.
     * Set last_par_line to the new end of the paragraph, update
     * fileage, and set current_x.  Also, edit_refresh() needs the line
     * numbers to be right, so renumber(). */
    last_par_line = current->prev;
    if (first_par_line->prev == NULL)
	fileage = first_par_line;
    renumber(first_par_line);

    edit_refresh();

    statusbar(_("Can now UnJustify!"));
    /* Display the shortcut list with UnJustify. */
    shortcut_init(1);
    display_main_list();
    reset_cursor();

    /* Now get a keystroke and see if it's unjustify; if not, unget the
     * keystroke and return. */
    {
	int meta_key;
	i = get_kbinput(edit, &meta_key);
#ifndef DISABLE_MOUSE
	/* If it was a mouse click, parse it with do_mouse() and it
	 * might become the unjustify key.  Else give it back to the
	 * input stream. */
	if (i == KEY_MOUSE) {
	    do_mouse();
	    i = get_kbinput(edit, &meta_key);
	}
#endif
    }

    if (i != NANO_UNJUSTIFY_KEY && i != NANO_UNJUSTIFY_FKEY) {
	ungetch(i);
	placewewant = 0;
    } else {
	/* Else restore the justify we just did (ungrateful user!). */
	filestruct *cutbottom = get_cutbottom();

	current = current_save;
	current_x = current_x_save;
	current_y = current_y_save;
	edittop = edittop_save;

	/* Splice the cutbuffer back into the file. */
	cutbottom->next = last_par_line->next;
	cutbottom->next->prev = cutbottom;
	    /* The line numbers after the end of the paragraph have been
	     * changed, so we change them back. */
	renumber(cutbottom->next);
	if (first_par_line->prev != NULL) {
	    cutbuffer->prev = first_par_line->prev;
	    cutbuffer->prev->next = cutbuffer;
	} else
	    fileage = cutbuffer;

	last_par_line->next = NULL;
	free_filestruct(first_par_line);

	/* Restore global variables from before the justify. */
	totsize = totsize_save;
	totlines = filebot->lineno;
#ifndef NANO_SMALL
	mark_beginbuf = mark_beginbuf_save;
	mark_beginx = mark_beginx_save;
#endif
	flags = flags_save;
	if (!ISSET(MODIFIED))
	    titlebar(NULL);
	edit_refresh();
    }
    cutbuffer = cutbuffer_save;
    /* Note that now cutbottom is invalid, but that's okay. */
    blank_statusbar();
    /* Display the shortcut list with UnCut. */
    shortcut_init(0);
    display_main_list();

    return 0;
}

int do_justify_void(void)
{
    return do_justify(FALSE);
}

int do_full_justify(void)
{
    return do_justify(TRUE);
}
#endif /* !DISABLE_JUSTIFY */

int do_exit(void)
{
    int i;

    if (!ISSET(MODIFIED)) {

#ifdef ENABLE_MULTIBUFFER
	if (!close_open_file()) {
	    display_main_list();
	    return 1;
	}
	else
#endif
	    finish();
    }

    if (ISSET(TEMP_OPT))
	i = 1;
    else
	i = do_yesno(FALSE, _("Save modified buffer (ANSWERING \"No\" WILL DESTROY CHANGES) ? "));
    
#ifdef DEBUG
    dump_buffer(fileage);
#endif

    if (i == 1) {
	if (do_writeout(TRUE) > 0) {

#ifdef ENABLE_MULTIBUFFER
	    if (!close_open_file()) {
		display_main_list();
		return 1;
	    }
	    else
#endif
		finish();
	}
    } else if (i == 0) {

#ifdef ENABLE_MULTIBUFFER
	if (!close_open_file()) {
	    display_main_list();
	    return 1;
	}
	else
#endif
	    finish();
    } else
	statusbar(_("Cancelled"));

    display_main_list();
    return 1;
}

void signal_init(void)
{
    /* Trap SIGINT and SIGQUIT because we want them to do useful
     * things. */
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = SIG_IGN;
    sigaction(SIGINT, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);

    /* Trap SIGHUP and SIGTERM because we want to write the file out. */
    act.sa_handler = handle_hupterm;
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

#ifndef NANO_SMALL
    /* Trap SIGWINCH because we want to handle window resizes. */
    act.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &act, NULL);
    allow_pending_sigwinch(FALSE);
#endif

    /* Trap normal suspend (^Z) so we can handle it ourselves. */
    if (!ISSET(SUSPEND)) {
	act.sa_handler = SIG_IGN;
	sigaction(SIGTSTP, &act, NULL);
    } else {
	/* Block all other signals in the suspend and continue handlers.
	 * If we don't do this, other stuff interrupts them! */
	sigfillset(&act.sa_mask);

	act.sa_handler = do_suspend;
	sigaction(SIGTSTP, &act, NULL);

	act.sa_handler = do_cont;
	sigaction(SIGCONT, &act, NULL);
    }
}

/* Handler for SIGHUP (hangup) and SIGTERM (terminate). */
RETSIGTYPE handle_hupterm(int signal)
{
    die(_("Received SIGHUP or SIGTERM\n"));
}

/* Handler for SIGTSTP (suspend). */
RETSIGTYPE do_suspend(int signal)
{
    endwin();
    printf("\n\n\n\n\n%s\n", _("Use \"fg\" to return to nano"));
    fflush(stdout);

    /* Restore the old terminal settings. */
    tcsetattr(0, TCSANOW, &oldterm);

    /* Trap SIGHUP and SIGTERM so we can properly deal with them while
     * suspended. */
    act.sa_handler = handle_hupterm;
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);

    /* Do what mutt does: send ourselves a SIGSTOP. */
    kill(0, SIGSTOP);
}

/* Handler for SIGCONT (continue after suspend). */
RETSIGTYPE do_cont(int signal)
{
#ifndef NANO_SMALL
    /* Perhaps the user resized the window while we slept.  Handle it
     * and update the screen in the process. */
    handle_sigwinch(0);
#else
    /* Just update the screen. */
    doupdate();
#endif
}

#ifndef NANO_SMALL
void handle_sigwinch(int s)
{
    const char *tty = ttyname(0);
    int fd;
    int result = 0;
    struct winsize win;

    if (tty == NULL)
	return;
    fd = open(tty, O_RDWR);
    if (fd == -1)
	return;
    result = ioctl(fd, TIOCGWINSZ, &win);
    close(fd);
    if (result == -1)
	return;

    /* Could check whether the COLS or LINES changed, and return
     * otherwise.  EXCEPT, that COLS and LINES are ncurses global
     * variables, and in some cases ncurses has already updated them. 
     * But not in all cases, argh. */
    COLS = win.ws_col;
    LINES = win.ws_row;
    editwinrows = LINES - 5 + no_help();
    if (editwinrows < MIN_EDITOR_ROWS || COLS < MIN_EDITOR_COLS)
	die_too_small();

#ifndef DISABLE_WRAPJUSTIFY
    fill = wrap_at;
    if (fill <= 0)
	fill += COLS;
    if (fill < 0)
	fill = 0;
#endif

    hblank = charealloc(hblank, COLS + 1);
    memset(hblank, ' ', COLS);
    hblank[COLS] = '\0';

#ifdef USE_SLANG
    /* Slang curses emulation brain damage, part 1: If we just do what
     * curses does here, it'll only work properly if the resize made the
     * window smaller.  Do what mutt does: Leave and immediately reenter
     * Slang screen management mode. */
    SLsmg_reset_smg();
    SLsmg_init_smg();
#else
    /* Do the equivalent of what Minimum Profit does: Leave and
     * immediately reenter curses mode. */
    endwin();
    refresh();
#endif

    /* Do the equivalent of what both mutt and Minimum Profit do:
     * Reinitialize all the windows based on the new screen
     * dimensions. */
    window_init();

    /* Redraw the contents of the windows that need it. */
    blank_statusbar();
    display_main_list();
    total_refresh();

    /* Turn cursor back on for sure. */
    curs_set(1);

    /* Restore the terminal to its previously saved state. */
    resetty();

    /* Reset all the input routines that rely on character sequences. */
    reset_kbinput();

    /* Jump back to the main loop. */
    siglongjmp(jmpbuf, 1);
}

void allow_pending_sigwinch(int allow)
{
    sigset_t winch;
    sigemptyset(&winch);
    sigaddset(&winch, SIGWINCH);
    if (allow)
	sigprocmask(SIG_UNBLOCK, &winch, NULL);
    else
	sigprocmask(SIG_BLOCK, &winch, NULL);
}
#endif /* !NANO_SMALL */

#ifndef NANO_SMALL
void do_toggle(const toggle *which)
{
    int enabled;

    /* Even easier! */
    TOGGLE(which->flag);

    switch (which->val) {
    case TOGGLE_SUSPEND_KEY:
	signal_init();
	break;
#ifndef DISABLE_MOUSE
    case TOGGLE_MOUSE_KEY:
	mouse_init();
	break;
#endif
    case TOGGLE_NOHELP_KEY:
	blank_statusbar();
	blank_bottombars();
	wrefresh(bottomwin);
	window_init();
	edit_refresh();
	display_main_list();
	break;
    case TOGGLE_DOS_KEY:
	UNSET(MAC_FILE);
	break;
    case TOGGLE_MAC_KEY:
	UNSET(DOS_FILE);
	break;
#ifdef ENABLE_COLOR
    case TOGGLE_SYNTAX_KEY:
	edit_refresh();
	break;
#endif
#ifdef ENABLE_NANORC
    case TOGGLE_WHITESPACE_KEY:
	edit_refresh();
	break;
#endif
    }

    /* We are assuming here that shortcut_init() above didn't free and
     * reallocate the toggles. */
    enabled = ISSET(which->flag);
    if (which->val == TOGGLE_NOHELP_KEY || which->val == TOGGLE_WRAP_KEY)
	enabled = !enabled;
    statusbar("%s %s", which->desc,
		enabled ? _("enabled") : _("disabled"));
}
#endif /* !NANO_SMALL */

void disable_signals(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_lflag &= ~ISIG;
    tcsetattr(0, TCSANOW, &term);
}

#ifndef NANO_SMALL
void enable_signals(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_lflag |= ISIG;
    tcsetattr(0, TCSANOW, &term);
}
#endif

void disable_flow_control(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_iflag &= ~(IXON|IXOFF);
    tcsetattr(0, TCSANOW, &term);
}

void enable_flow_control(void)
{
    struct termios term;

    tcgetattr(0, &term);
    term.c_iflag |= (IXON|IXOFF);
    tcsetattr(0, TCSANOW, &term);
}

int main(int argc, char *argv[])
{
    int optchr;
    int startline = 0;		/* Line to try and start at */
    int modify_control_seq = 0;
    int fill_flag_used = 0;	/* Was the fill option used? */
    const shortcut *s;
    int keyhandled = 0;	/* Have we handled the keystroke yet? */
    int kbinput;		/* Input from keyboard */
    int meta_key;

#ifndef NANO_SMALL
    const toggle *t;
#endif
#ifdef HAVE_GETOPT_LONG
    const struct option long_options[] = {
	{"help", 0, 0, 'h'},
#ifdef ENABLE_MULTIBUFFER
	{"multibuffer", 0, 0, 'F'},
#endif
#ifdef ENABLE_NANORC
#ifndef NANO_SMALL
	{"historylog", 0, 0, 'H'},
#endif
	{"ignorercfiles", 0, 0, 'I'},
#endif
#ifndef DISABLE_JUSTIFY
	{"quotestr", 1, 0, 'Q'},
#endif
#ifdef HAVE_REGEX_H
	{"regexp", 0, 0, 'R'},
#endif
	{"tabsize", 1, 0, 'T'},
	{"version", 0, 0, 'V'},
#ifdef ENABLE_COLOR
	{"syntax", 1, 0, 'Y'},
#endif
	{"const", 0, 0, 'c'},
	{"rebinddelete", 0, 0, 'd'},
	{"nofollow", 0, 0, 'l'},
#ifndef DISABLE_MOUSE
	{"mouse", 0, 0, 'm'},
#endif
#ifndef DISABLE_OPERATINGDIR
	{"operatingdir", 1, 0, 'o'},
#endif
	{"preserve", 0, 0, 'p'},
#ifndef DISABLE_WRAPJUSTIFY
	{"fill", 1, 0, 'r'},
#endif
#ifndef DISABLE_SPELLER
	{"speller", 1, 0, 's'},
#endif
	{"tempfile", 0, 0, 't'},
	{"view", 0, 0, 'v'},
#ifndef DISABLE_WRAPPING
	{"nowrap", 0, 0, 'w'},
#endif
	{"nohelp", 0, 0, 'x'},
	{"suspend", 0, 0, 'z'},
#ifndef NANO_SMALL
	{"smarthome", 0, 0, 'A'},
	{"backup", 0, 0, 'B'},
	{"dos", 0, 0, 'D'},
	{"backupdir", 1, 0, 'E'},
	{"mac", 0, 0, 'M'},
	{"noconvert", 0, 0, 'N'},
	{"smooth", 0, 0, 'S'},
	{"restricted", 0, 0, 'Z'},
	{"autoindent", 0, 0, 'i'},
	{"cut", 0, 0, 'k'},
#endif
	{0, 0, 0, 0}
    };
#endif

#ifdef ENABLE_NLS
    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
#endif

#if !defined(ENABLE_NANORC) && defined(DISABLE_ROOTWRAP) && !defined(DISABLE_WRAPPING)
    /* if we don't have rcfile support, we're root, and
       --disable-wrapping-as-root is used, turn wrapping off */
    if (geteuid() == NANO_ROOT_UID)
	SET(NO_WRAP);
#endif

#ifdef HAVE_GETOPT_LONG
    while ((optchr = getopt_long(argc, argv, "h?ABDE:FHIMNQ:RST:VY:Zabcdefgijklmo:pr:s:tvwxz",
				 long_options, NULL)) != -1) {
#else
    while ((optchr =
	    getopt(argc, argv, "h?ABDE:FHIMNQ:RST:VY:Zabcdefgijklmo:pr:s:tvwxz")) != -1) {
#endif

	switch (optchr) {

	case 'a':
	case 'b':
	case 'e':
	case 'f':
	case 'g':
	case 'j':
	    /* Pico compatibility flags */
	    break;
#ifndef NANO_SMALL
	case 'A':
	    SET(SMART_HOME);
	    break;
	case 'B':
	    SET(BACKUP_FILE);
	    break;
	case 'D':
	    SET(DOS_FILE);
	    break;
	case 'E':
	    backup_dir = mallocstrcpy(backup_dir, optarg);
	    break;
#endif
#ifdef ENABLE_MULTIBUFFER
	case 'F':
	    SET(MULTIBUFFER);
	    break;
#endif
#ifdef ENABLE_NANORC
#ifndef NANO_SMALL
	case 'H':
	    SET(HISTORYLOG);
	    break;
#endif
	case 'I':
	    SET(NO_RCFILE);
	    break;
#endif
#ifndef NANO_SMALL
	case 'M':
	    SET(MAC_FILE);
	    break;
	case 'N':
	    SET(NO_CONVERT);
	    break;
#endif
#ifndef DISABLE_JUSTIFY
	case 'Q':
	    quotestr = mallocstrcpy(quotestr, optarg);
	    break;
#endif
#ifdef HAVE_REGEX_H
	case 'R':
	    SET(USE_REGEXP);
	    break;
#endif
#ifndef NANO_SMALL
	case 'S':
	    SET(SMOOTHSCROLL);
	    break;
#endif
	case 'T':
	    {
		int i;
		char *first_error;

		/* Using strtol() instead of atoi() lets us accept 0
		 * while checking other errors. */
		i = (int)strtol(optarg, &first_error, 10);
		if (errno == ERANGE || *optarg == '\0' || *first_error != '\0')
		    usage();
		else
		    tabsize = i;
		if (tabsize <= 0) {
		    fprintf(stderr, _("Tab size is too small for nano...\n"));
		    exit(1);
		}
	    }
	    break;
	case 'V':
	    version();
	    exit(0);
#ifdef ENABLE_COLOR
	case 'Y':
	    syntaxstr = mallocstrcpy(syntaxstr, optarg);
	    break;
#endif
	case 'Z':
	    SET(RESTRICTED);
	    break;
	case 'c':
	    SET(CONSTUPDATE);
	    break;
	case 'd':
	    SET(REBIND_DELETE);
	    break;
#ifndef NANO_SMALL
	case 'i':
	    SET(AUTOINDENT);
	    break;
	case 'k':
	    SET(CUT_TO_END);
	    break;
#endif
	case 'l':
	    SET(NOFOLLOW_SYMLINKS);
	    break;
#ifndef DISABLE_MOUSE
	case 'm':
	    SET(USE_MOUSE);
	    break;
#endif
#ifndef DISABLE_OPERATINGDIR
	case 'o':
	    operating_dir = mallocstrcpy(operating_dir, optarg);
	    break;
#endif
	case 'p':
	    SET(PRESERVE);
	    break;
#ifndef DISABLE_WRAPJUSTIFY
	case 'r':
	    {
		int i;
		char *first_error;

		/* Using strtol() instead of atoi() lets us accept 0
		 * while checking other errors. */
		i = (int)strtol(optarg, &first_error, 10);
		if (errno == ERANGE || *optarg == '\0' || *first_error != '\0')
		    usage();
		else
		    wrap_at = i;
	    }
	    fill_flag_used = 1;
	    break;
#endif
#ifndef DISABLE_SPELLER
	case 's':
	    alt_speller = mallocstrcpy(alt_speller, optarg);
	    break;
#endif
	case 't':
	    SET(TEMP_OPT);
	    break;
	case 'v':
	    SET(VIEW_MODE);
	    break;
#ifndef DISABLE_WRAPPING
	case 'w':
	    SET(NO_WRAP);
	    break;
#endif
	case 'x':
	    SET(NO_HELP);
	    break;
	case 'z':
	    SET(SUSPEND);
	    break;
	default:
	    usage();
	}
    }

    /* If the executable filename starts with 'r', we use restricted
     * mode. */
    if (*(tail(argv[0])) == 'r')
	SET(RESTRICTED);

    /* If we're using restricted mode, disable suspending, backups, and
     * reading rcfiles, since they all would allow reading from or
     * writing to files not specified on the command line. */
    if (ISSET(RESTRICTED)) {
	UNSET(SUSPEND);
	UNSET(BACKUP_FILE);
	SET(NO_RCFILE);
    }

/* We've read through the command line options.  Now back up the flags
   and values that are set, and read the rcfile(s).  If the values
   haven't changed afterward, restore the backed-up values. */
#ifdef ENABLE_NANORC
    if (!ISSET(NO_RCFILE)) {
#ifndef DISABLE_OPERATINGDIR
	char *operating_dir_cpy = operating_dir;
#endif
#ifndef DISABLE_WRAPPING
	int wrap_at_cpy = wrap_at;
#endif
#ifndef NANO_SMALL
	char *backup_dir_cpy = backup_dir;
#endif
#ifndef DISABLE_JUSTIFY
	char *quotestr_cpy = quotestr;
#endif
#ifndef DISABLE_SPELLER
	char *alt_speller_cpy = alt_speller;
#endif
	int tabsize_cpy = tabsize;
	long flags_cpy = flags;

#ifndef DISABLE_OPERATINGDIR
	operating_dir = NULL;
#endif
#ifndef NANO_SMALL
	backup_dir = NULL;
#endif
#ifndef DISABLE_JUSTIFY
	quotestr = NULL;
#endif
#ifndef DISABLE_SPELLER
	alt_speller = NULL;
#endif

	do_rcfile();

#ifndef DISABLE_OPERATINGDIR
	if (operating_dir_cpy != NULL) {
	    free(operating_dir);
	    operating_dir = operating_dir_cpy;
	}
#endif
#ifndef DISABLE_WRAPPING
	if (fill_flag_used)
	    wrap_at = wrap_at_cpy;
#endif
#ifndef NANO_SMALL
	if (backup_dir_cpy != NULL) {
	    free(backup_dir);
	    backup_dir = backup_dir_cpy;
	}
#endif	
#ifndef DISABLE_JUSTIFY
	if (quotestr_cpy != NULL) {
	    free(quotestr);
	    quotestr = quotestr_cpy;
	}
#endif
#ifndef DISABLE_SPELLER
	if (alt_speller_cpy != NULL) {
	    free(alt_speller);
	    alt_speller = alt_speller_cpy;
	}
#endif
	if (tabsize_cpy > 0)
	    tabsize = tabsize_cpy;
	flags |= flags_cpy;
    }
#if defined(DISABLE_ROOTWRAP) && !defined(DISABLE_WRAPPING)
    else if (geteuid() == NANO_ROOT_UID)
	SET(NO_WRAP);
#endif
#endif /* ENABLE_NANORC */

#ifndef NANO_SMALL
    history_init();
#ifdef ENABLE_NANORC
    if (!ISSET(NO_RCFILE) && ISSET(HISTORYLOG))
	load_history();
#endif
#endif

#ifndef NANO_SMALL
    /* Set up the backup directory (unless we're using restricted mode,
     * in which case backups are disabled, since they would allow
     * reading from or writing to files not specified on the command
     * line).  This entails making sure it exists and is a directory, so
     * that backup files will be saved there. */
    if (!ISSET(RESTRICTED))
	init_backup_dir();
#endif

#ifndef DISABLE_OPERATINGDIR
    /* Set up the operating directory.  This entails chdir()ing there,
     * so that file reads and writes will be based there. */
    init_operating_dir();
#endif

#ifndef DISABLE_JUSTIFY
    if (punct == NULL)
	    punct = mallocstrcpy(punct, ".?!");

    if (brackets == NULL)
	    brackets = mallocstrcpy(brackets, "'\")}]>");

    if (quotestr == NULL)
#ifdef HAVE_REGEX_H
	quotestr = mallocstrcpy(NULL, "^([ \t]*[|>:}#])+");
#else
	quotestr = mallocstrcpy(NULL, "> ");
#endif
#endif /* !DISABLE_JUSTIFY */

#ifndef DISABLE_SPELLER
    /* If we don't have an alternative spell checker after reading the
     * command line and/or rcfile(s), check $SPELL for one, as Pico
     * does (unless we're using restricted mode, in which case spell
     * checking is disabled, since it would allow reading from or
     * writing to files not specified on the command line). */
    if (!ISSET(RESTRICTED) && alt_speller == NULL) {
	char *spellenv = getenv("SPELL");
	if (spellenv != NULL)
	    alt_speller = mallocstrcpy(NULL, spellenv);
    }
#endif

#if !defined(NANO_SMALL) && defined(ENABLE_NANORC)
    if (whitespace == NULL)
	whitespace = mallocstrcpy(NULL, "  ");
#endif

    if (tabsize == -1)
	tabsize = 8;

    /* Clear the filename we'll be using */
    filename = charalloc(1);
    filename[0] = '\0';

    /* If there's a +LINE flag, it is the first non-option argument. */
    if (0 < optind && optind < argc && argv[optind][0] == '+') {
	startline = atoi(&argv[optind][1]);
	optind++;
    }
    if (0 < optind && optind < argc)
	filename = mallocstrcpy(filename, argv[optind]);

    /* See if there's a non-option in argv (first non-option is the
       filename, if +LINE is not given) */
    if (argc > 1 && argc > optind) {
	/* Look for the +line flag... */
	if (argv[optind][0] == '+') {
	    startline = atoi(&argv[optind][1]);
	    optind++;
	    if (argc > optind)
		filename = mallocstrcpy(filename, argv[optind]);
	} else
	    filename = mallocstrcpy(filename, argv[optind]);
    }

    /* Back up the old terminal settings so that they can be restored. */
    tcgetattr(0, &oldterm);

   /* Curses initialization stuff: Start curses, save the state of the
    * terminal mode, put the terminal in cbreak mode (read one character
    * at a time and interpret the special control keys), disable
    * translation of carriage return (^M) into newline (^J) so that we
    * can tell the difference between the Enter key and Ctrl-J, and
    * disable echoing of characters as they're typed.  Finally, disable
    * interpretation of the special control keys, and if we're not in
    * preserve mode, disable interpretation of the flow control
    * characters too. */
    initscr();
    cbreak();
    nonl();
    noecho();
    disable_signals();
    if (!ISSET(PRESERVE))
	disable_flow_control();

#ifndef NANO_SMALL
    /* Save the terminal's current state, so that we can restore it
     * after a resize. */
    savetty();
#endif

    /* Set up the global variables and the shortcuts. */
    global_init(0);
    shortcut_init(0);

    /* Set up the signal handlers. */
    signal_init();

#ifdef DEBUG
    fprintf(stderr, "Main: set up windows\n");
#endif

    window_init();
#ifndef DISABLE_MOUSE
    mouse_init();
#endif

#ifdef DEBUG
    fprintf(stderr, "Main: bottom win\n");
#endif
    /* Set up bottom of window */
    display_main_list();

#ifdef DEBUG
    fprintf(stderr, "Main: open file\n");
#endif

    open_file(filename, 0, 0);
#ifdef ENABLE_MULTIBUFFER
    /* If we're using multibuffers and more than one file is specified
       on the command line, load them all and switch to the first one
       afterward */
    if (optind + 1 < argc) {
	int old_multibuffer = ISSET(MULTIBUFFER);
	SET(MULTIBUFFER);
	for (optind++; optind < argc; optind++) {
	    add_open_file(1);
	    new_file();
	    filename = mallocstrcpy(filename, argv[optind]);
	    open_file(filename, 0, 0);
	    load_file(0);
	}
	open_nextfile_void();
	if (!old_multibuffer)
	    UNSET(MULTIBUFFER);
    }
#endif

    titlebar(NULL);

    if (startline > 0)
	do_gotoline(startline, 0);

#ifndef NANO_SMALL
    /* Return here after a SIGWINCH. */
    sigsetjmp(jmpbuf, 1);
#endif

    /* SHUT UP GCC! */
    startline = 0;
    fill_flag_used = 0;
    keyhandled = 0;

    /* This variable should be initialized after the sigsetjmp(), so we
       can't do Esc-Esc then quickly resize and muck things up. */
    modify_control_seq = 0;

    edit_refresh();
    reset_cursor();

    while (TRUE) {
	keyhandled = 0;

	if (ISSET(CONSTUPDATE))
	    do_cursorpos(1);

#if !defined(DISABLE_BROWSER) || !defined(DISABLE_HELP) || !defined(DISABLE_MOUSE)
	currshortcut = main_list;
#endif

	kbinput = get_kbinput(edit, &meta_key);
#ifdef DEBUG
	fprintf(stderr, "AHA!  %c (%d)\n", kbinput, kbinput);
#endif
	if (meta_key == TRUE) {
	    /* Check for the metaval and miscval defs... */
	    for (s = main_list; s != NULL; s = s->next)
		if ((s->metaval != NANO_NO_KEY && kbinput == s->metaval) ||
		    (s->miscval != NANO_NO_KEY && kbinput == s->miscval)) {
		    if (ISSET(VIEW_MODE) && !s->viewok)
			print_view_warning();
		    else {
			if (s->func != do_cut_text)
			    cutbuffer_reset();
			s->func();
		    }
		    keyhandled = 1;
		}
#ifndef NANO_SMALL
		if (!keyhandled)
		    /* And for toggle switches */
		    for (t = toggles; t != NULL; t = t->next)
			if (kbinput == t->val) {
			    cutbuffer_reset();
			    do_toggle(t);
			    keyhandled = 1;
		        }
#endif
#ifdef DEBUG
	    fprintf(stderr, "I got Alt-%c! (%d)\n", kbinput,
		kbinput);
#endif
	}

	/* Look through the main shortcut list to see if we've hit a
	   shortcut key or function key */

	if (!keyhandled)
#if !defined(DISABLE_BROWSER) || !defined (DISABLE_HELP) || !defined(DISABLE_MOUSE)
	    for (s = currshortcut; s != NULL && !keyhandled; s = s->next) {
#else
	    for (s = main_list; s != NULL && !keyhandled; s = s->next) {
#endif
		if ((s->ctrlval != NANO_NO_KEY && kbinput == s->ctrlval) ||
		    (s->funcval != NANO_NO_KEY && kbinput == s->funcval)) {
		    if (ISSET(VIEW_MODE) && !s->viewok)
			print_view_warning();
		    else {
			if (s->func != do_cut_text)
			    cutbuffer_reset();
			s->func();
		    }
		    keyhandled = 1;
		    /* Break out explicitly once we successfully handle
		       a shortcut */
		    break;
		}
	    }

	if (!keyhandled)
	    cutbuffer_reset();

	/* Don't even think about changing this string */
	if (kbinput == NANO_CONTROL_Q)
	    statusbar(_("XON ignored, mumble mumble."));
	if (kbinput == NANO_CONTROL_S)
	    statusbar(_("XOFF ignored, mumble mumble."));

	/* If we're in raw mode or using Alt-Alt-x, we have to catch
	   Control-S and Control-Q */
	if (kbinput == NANO_CONTROL_Q || kbinput == NANO_CONTROL_S)
	    keyhandled = 1;

	/* Catch ^Z by hand when triggered also */
	if (kbinput == NANO_SUSPEND_KEY) {
	    if (ISSET(SUSPEND))
		do_suspend(0);
	    keyhandled = 1;
	}

	/* Last gasp, stuff that's not in the main lists */
	if (!keyhandled)
	    switch (kbinput) {
#ifndef DISABLE_MOUSE
	    case KEY_MOUSE:
		do_mouse();
		break;
#endif

	    case NANO_CONTROL_3:	/* Ctrl-[ (Esc), which should
	    				 * have been handled before we
	    				 * got here */
	    case NANO_CONTROL_5:	/* Ctrl-] */
		break;
	    default:
#ifdef DEBUG
		fprintf(stderr, "I got %c (%d)!\n", kbinput, kbinput);
#endif
		/* We no longer stop unhandled sequences so that people with
		   odd character sets can type... */

		if (ISSET(VIEW_MODE))
		    print_view_warning();
		else
		    do_char((char)kbinput);
	    }

	reset_cursor();
	wrefresh(edit);
    }
    assert(FALSE);
}
