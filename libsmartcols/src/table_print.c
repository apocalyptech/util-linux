/*
 * table.c - functions handling the data at the table level
 *
 * Copyright (C) 2010-2014 Karel Zak <kzak@redhat.com>
 *
 * This file may be redistributed under the terms of the
 * GNU Lesser General Public License.
 */

/**
 * SECTION: table_print
 * @title: Table print
 * @short_description: table print API
 *
 * Table output API.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>
#include <ctype.h>

#include "nls.h"
#include "mbsalign.h"
#include "widechar.h"
#include "ttyutils.h"
#include "carefulputc.h"
#include "smartcolsP.h"

#define is_last_column(_tb, _cl) \
		list_entry_is_last(&(_cl)->cl_columns, &(_tb)->tb_columns)

static void print_data(struct libscols_table *tb,
		       struct libscols_column *cl,
		       struct libscols_line *ln,	/* optional */
		       struct libscols_cell *ce,	/* optional */
		       char *data)
{
	size_t len = 0, i, width;
	char *buf;
	const char *color = NULL;

	assert(tb);
	assert(cl);

	if (!data)
		data = "";

	/* raw mode */
	if (scols_table_is_raw(tb)) {
		fputs_nonblank(data, tb->out);
		if (!is_last_column(tb, cl))
			fputc(' ', tb->out);
		return;
	}

	/* NAME=value mode */
	if (scols_table_is_export(tb)) {
		fprintf(tb->out, "%s=", scols_cell_get_data(&cl->header));
		fputs_quoted(data, tb->out);
		if (!is_last_column(tb, cl))
			fputc(' ', tb->out);
		return;
	}

	if (tb->colors_wanted) {
		if (ce && !color)
			color = ce->color;
		if (ln && !color)
			color = ln->color;
		if (!color)
			color = cl->color;
	}

	/* note that 'len' and 'width' are number of cells, not bytes */
	buf = mbs_safe_encode(data, &len);
	data = buf;
	if (!data)
		data = "";

	if (!len || len == (size_t) -1) {
		len = 0;
		data = NULL;
	}
	width = cl->width;

	if (is_last_column(tb, cl) && len < width && !scols_table_is_maxout(tb))
		width = len;

	/* truncate data */
	if (len > width && scols_column_is_trunc(cl)) {
		if (data)
			len = mbs_truncate(data, &width);
		if (!data || len == (size_t) -1) {
			len = 0;
			data = NULL;
		}
	}
	if (data) {
		if (!scols_table_is_raw(tb) && scols_column_is_right(cl)) {
			size_t xw = cl->width;
			if (color)
				fputs(color, tb->out);
			fprintf(tb->out, "%*s", (int) xw, data);
			if (color)
				fputs(UL_COLOR_RESET, tb->out);
			if (len < xw)
				len = xw;
		}
		else {
			if (color)
				fputs(color, tb->out);
			fputs(data, tb->out);
			if (color)
				fputs(UL_COLOR_RESET, tb->out);
		}
	}
	for (i = len; i < width; i++)
		fputc(' ', tb->out);		/* padding */

	if (!is_last_column(tb, cl)) {
		if (len > width && !scols_column_is_trunc(cl)) {
			fputc('\n', tb->out);
			for (i = 0; i <= (size_t) cl->seqnum; i++) {
				struct libscols_column *x = scols_table_get_column(tb, i);
				fprintf(tb->out, "%*s ", -((int)x->width), " ");
			}
		} else
			fputc(' ', tb->out);	/* columns separator */
	}

	free(buf);
}

static char *line_get_ascii_art(struct libscols_table *tb,
				struct libscols_line *ln,
				char *buf, size_t *bufsz)
{
	const char *art;
	size_t len;

	assert(ln);

	if (!ln->parent)
		return buf;

	buf = line_get_ascii_art(tb, ln->parent, buf, bufsz);
	if (!buf)
		return NULL;

	if (list_entry_is_last(&ln->ln_children, &ln->parent->ln_branch))
		art = "  ";
	else
		art = tb->symbols->vert;

	len = strlen(art);
	if (*bufsz < len)
		return NULL;	/* no space, internal error */

	memcpy(buf, art, len);
	*bufsz -= len;
	return buf + len;
}

static char *line_get_data(struct libscols_table *tb,
			   struct libscols_line *ln,
			   struct libscols_column *cl,
			   char *buf, size_t bufsz)
{
	const char *data;
	struct libscols_symbols *sym;
	struct libscols_cell *ce;
	char *p = buf;

	assert(tb);
	assert(ln);
	assert(cl);
	assert(cl->seqnum <= tb->ncols);

	memset(buf, 0, bufsz);

	ce = scols_line_get_cell(ln, cl->seqnum);
	data = ce ? scols_cell_get_data(ce) : NULL;
	if (!data)
		return NULL;

	if (!scols_column_is_tree(cl)) {
		strncpy(buf, data, bufsz);
		buf[bufsz - 1] = '\0';
		return buf;
	}

	/*
	 * Tree stuff
	 */
	if (ln->parent) {
		p = line_get_ascii_art(tb, ln->parent, buf, &bufsz);
		if (!p)
			return NULL;
	}

	sym = tb->symbols;

	if (!ln->parent)
		snprintf(p, bufsz, "%s", data);			/* root node */
	else if (list_entry_is_last(&ln->ln_children, &ln->parent->ln_branch))
		snprintf(p, bufsz, "%s%s", sym->right, data);	/* last chaild */
	else
		snprintf(p, bufsz, "%s%s", sym->branch, data);	/* any child */

	return buf;
}

/*
 * Prints data, data maybe be printed in more formats (raw, NAME=xxx pairs) and
 * control and non-printable chars maybe encoded in \x?? hex encoding.
 */
static void print_line(struct libscols_table *tb,
		       struct libscols_line *ln, char *buf, size_t bufsz)
{
	struct libscols_column *cl;
	struct libscols_iter itr;

	assert(ln);

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_column(tb, &itr, &cl) == 0)
		print_data(tb, cl, ln,
				scols_line_get_cell(ln, cl->seqnum),
				line_get_data(tb, ln, cl, buf, bufsz));
	fputc('\n', tb->out);
}

static void print_header(struct libscols_table *tb, char *buf, size_t bufsz)
{
	struct libscols_column *cl;
	struct libscols_iter itr;

	assert(tb);

	if (scols_table_is_noheadings(tb) ||
	    scols_table_is_export(tb) ||
	    list_empty(&tb->tb_lines))
		return;

	/* set width according to the size of data
	 */
	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_column(tb, &itr, &cl) == 0) {
		strncpy(buf, scols_cell_get_data(&cl->header), bufsz);
		buf[bufsz - 1] = '\0';
		print_data(tb, cl, NULL, &cl->header, buf);
	}
	fputc('\n', tb->out);
}

static void print_table(struct libscols_table *tb, char *buf, size_t bufsz)
{
	struct libscols_line *ln;
	struct libscols_iter itr;

	assert(tb);

	print_header(tb, buf, bufsz);

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_line(tb, &itr, &ln) == 0)
		print_line(tb, ln, buf, bufsz);
}

static void print_tree_line(struct libscols_table *tb,
			    struct libscols_line *ln,
			    char *buf, size_t bufsz)
{
	struct list_head *p;

	print_line(tb, ln, buf, bufsz);

	if (list_empty(&ln->ln_branch))
		return;

	/* print all children */
	list_for_each(p, &ln->ln_branch) {
		struct libscols_line *chld =
				list_entry(p, struct libscols_line, ln_children);
		print_tree_line(tb, chld, buf, bufsz);
	}
}

static void print_tree(struct libscols_table *tb, char *buf, size_t bufsz)
{
	struct libscols_line *ln;
	struct libscols_iter itr;

	assert(tb);

	print_header(tb, buf, bufsz);

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_line(tb, &itr, &ln) == 0) {
		if (ln->parent)
			continue;
		print_tree_line(tb, ln, buf, bufsz);
	}
}

/*
 * This function counts column width.
 *
 * For the SCOLS_FL_NOEXTREMES columns is possible to call this function two
 * times.  The first pass counts width and average width. If the column
 * contains too large fields (width greater than 2 * average) then the column
 * is marked as "extreme". In the second pass all extreme fields are ignored
 * and column width is counted from non-extreme fields only.
 */
static void count_column_width(struct libscols_table *tb,
			       struct libscols_column *cl,
			       char *buf,
			       size_t bufsz)
{
	struct libscols_line *ln;
	struct libscols_iter itr;
	int count = 0;
	size_t sum = 0;

	assert(tb);
	assert(cl);

	cl->width = 0;

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_line(tb, &itr, &ln) == 0) {
		char *data = line_get_data(tb, ln, cl, buf, bufsz);
		size_t len = data ? mbs_safe_width(data) : 0;

		if (len == (size_t) -1)		/* ignore broken multibyte strings */
			len = 0;
		if (len > cl->width_max)
			cl->width_max = len;

		if (cl->is_extreme && len > cl->width_avg * 2)
			continue;
		else if (scols_column_is_noextremes(cl)) {
			sum += len;
			count++;
		}
		if (len > cl->width)
			cl->width = len;
	}

	if (count && cl->width_avg == 0) {
		cl->width_avg = sum / count;

		if (cl->width_max > cl->width_avg * 2)
			cl->is_extreme = 1;
	}

	/* check and set minimal column width */
	if (scols_cell_get_data(&cl->header))
		cl->width_min = mbs_safe_width(scols_cell_get_data(&cl->header));

	/* enlarge to minimal width */
	if (cl->width < cl->width_min && !scols_column_is_strict_width(cl))
		cl->width = cl->width_min;

	/* use relative size for large columns */
	else if (cl->width_hint >= 1 && cl->width < (size_t) cl->width_hint
		 && cl->width_min < (size_t) cl->width_hint)

		cl->width = (size_t) cl->width_hint;
}

/*
 * This is core of the scols_* voodo...
 */
static void recount_widths(struct libscols_table *tb, char *buf, size_t bufsz)
{
	struct libscols_column *cl;
	struct libscols_iter itr;
	size_t width = 0;		/* output width */
	int trunc_only;
	int extremes = 0;

	/* set basic columns width
	 */
	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_column(tb, &itr, &cl) == 0) {
		count_column_width(tb, cl, buf, bufsz);
		width += cl->width + (is_last_column(tb, cl) ? 0 : 1);
		extremes += cl->is_extreme;
	}

	if (!tb->is_term)
		return;

	/* reduce columns with extreme fields
	 */
	if (width > tb->termwidth && extremes) {
		scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
		while (scols_table_next_column(tb, &itr, &cl) == 0) {
			size_t org_width;

			if (!cl->is_extreme)
				continue;

			org_width = cl->width;
			count_column_width(tb, cl, buf, bufsz);

			if (org_width > cl->width)
				width -= org_width - cl->width;
			else
				extremes--;	/* hmm... nothing reduced */
		}
	}

	if (width < tb->termwidth) {
		/* try to found extreme column which fits into available space
		 */
		if (extremes) {
			/* enlarge the first extreme column */
			scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
			while (scols_table_next_column(tb, &itr, &cl) == 0) {
				size_t add;

				if (!cl->is_extreme)
					continue;

				/* this column is tooo large, ignore?
				if (cl->width_max - cl->width >
						(tb->termwidth - width))
					continue;
				*/

				add = tb->termwidth - width;
				if (add && cl->width + add > cl->width_max)
					add = cl->width_max - cl->width;

				cl->width += add;
				width += add;

				if (width == tb->termwidth)
					break;
			}
		}

		if (width < tb->termwidth && scols_table_is_maxout(tb)) {
			/* try enlarge all columns */
			while (width < tb->termwidth) {
				scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
				while (scols_table_next_column(tb, &itr, &cl) == 0) {
					cl->width++;
					width++;
					if (width == tb->termwidth)
						break;
				}
			}
		} else if (width < tb->termwidth) {
			/* enlarge the last column */
			struct libscols_column *cl = list_entry(
				tb->tb_columns.prev, struct libscols_column, cl_columns);

			if (!scols_column_is_right(cl) && tb->termwidth - width > 0) {
				cl->width += tb->termwidth - width;
				width = tb->termwidth;
			}
		}
	}

	/* bad, we have to reduce output width, this is done in two steps:
	 * 1/ reduce columns with a relative width and with truncate flag
	 * 2) reduce columns with a relative width without truncate flag
	 */
	trunc_only = 1;
	while (width > tb->termwidth) {
		size_t org = width;

		scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
		while (scols_table_next_column(tb, &itr, &cl) == 0) {
			if (width <= tb->termwidth)
				break;
			if (cl->width_hint > 1 && !scols_column_is_trunc(cl))
				continue;	/* never truncate columns with absolute sizes */
			if (scols_column_is_tree(cl))
				continue;	/* never truncate the tree */
			if (trunc_only && !scols_column_is_trunc(cl))
				continue;
			if (cl->width == cl->width_min)
				continue;

			/* truncate column with relative sizes */
			if (cl->width_hint < 1 && cl->width > 0 && width > 0 &&
			    cl->width > cl->width_hint * tb->termwidth) {
				cl->width--;
				width--;
			}
			/* truncate column with absolute size */
			if (cl->width_hint > 1 && cl->width > 0 && width > 0 &&
			    !trunc_only) {
				cl->width--;
				width--;
			}

		}
		if (org == width) {
			if (trunc_only)
				trunc_only = 0;
			else
				break;
		}
	}

/*
	fprintf(stderr, "terminal: %d, output: %d\n", tb->termwidth, width);

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_column(tb, &itr, &cl) == 0) {
		fprintf(stderr, "width: %s=%zd [hint=%d, avg=%zd, max=%zd, extreme=%s]\n",
			cl->name, cl->width,
			cl->width_hint > 1 ? (int) cl->width_hint :
					     (int) (cl->width_hint * tb->termwidth),
			cl->width_avg,
			cl->width_max,
			cl->is_extreme ? "yes" : "not");
	}
*/
	return;
}
static size_t strlen_line(struct libscols_line *ln)
{
	size_t i, sz = 0;

	assert(ln);

	for (i = 0; i < ln->ncells; i++) {
		struct libscols_cell *ce = scols_line_get_cell(ln, i);
		const char *data = ce ? scols_cell_get_data(ce) : NULL;

		sz += data ? strlen(data) : 0;
	}

	return sz;
}

/**
 * scols_print_table:
 * @tb: table
 *
 * Prints the table to the output stream.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_print_table(struct libscols_table *tb)
{
	char *line;
	size_t line_sz;
	struct libscols_line *ln;
	struct libscols_iter itr;

	assert(tb);
	if (!tb)
		return -1;

	if (!tb->symbols)
		scols_table_set_symbols(tb, NULL);	/* use default */

	tb->is_term = isatty(STDOUT_FILENO) ? 1 : 0;
	tb->termwidth = tb->is_term ? get_terminal_width() : 0;
	if (tb->termwidth <= 0)
		tb->termwidth = 80;
	tb->termwidth -= tb->termreduce;

	line_sz = tb->termwidth;

	scols_reset_iter(&itr, SCOLS_ITER_FORWARD);
	while (scols_table_next_line(tb, &itr, &ln) == 0) {
		size_t sz = strlen_line(ln);
		if (sz > line_sz)
			line_sz = sz;
	}

	line_sz++;			/* make a space for \0 */
	line = malloc(line_sz);
	if (!line)
		return -ENOMEM;

	if (!(scols_table_is_raw(tb) || scols_table_is_export(tb)))
		recount_widths(tb, line, line_sz);

	if (scols_table_is_tree(tb))
		print_tree(tb, line, line_sz);
	else
		print_table(tb, line, line_sz);

	free(line);
	return 0;
}

/**
 * scols_print_table_to_string:
 * @tb: table
 * @data: pointer to the beginning of a memory area to print to
 *
 * Prints the table to @data.
 *
 * Returns: 0, a negative value in case of an error.
 */
int scols_print_table_to_string(struct libscols_table *tb, char **data)
{
#ifdef HAVE_OPEN_MEMSTREAM
	FILE *stream;
	size_t sz;

	if (!tb)
		return -EINVAL;

	/* create a stream for output */
	stream = open_memstream(data, &sz);
	if (!stream)
		return -ENOMEM;

	scols_table_set_stream(tb, stream);
	scols_print_table(tb);
	fclose(stream);

	return 0;
#else
	return -ENOSYS;
#endif
}

