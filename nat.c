/* Copyright 2022, 2023 Oğuz İsmail Uysal <oguzismailuysal@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#if defined(__sun) || defined(__USLC__)
#include <stropts.h>
#include <termios.h>
#endif

#if defined(__QNX__)
#include "wcwidth.c"
#define wcwidth mk_wcwidth
#else
#define _XOPEN_SOURCE 600
#endif

#include <errno.h>
#include <locale.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

#define TERM_WIDTH 80

#define BUF_ALLOC 512
#define LIST_ALLOC 32

#define MIN(x, y) ((x) < (y) ? (x) : (y))

struct item {
	const wchar_t *str;
	size_t len;
	size_t width;
};

static wchar_t *buf;
static size_t buf_len;
static size_t buf_cap;

static struct item *list;
static size_t list_len;
static size_t list_cap;

static wchar_t delim;
static size_t term_width;
static size_t padding;
static int across;

static size_t num_rows;
static size_t num_cols;
static size_t *col_widths;
static size_t space_left;

static int status;

static void die(const char *);
static void *xmalloc(size_t);
static void *xrealloc(void *, size_t);

static int
parse_size(const char *src, char **endptr, int base, size_t *dest) {
	intmax_t i;
	uintmax_t u;

	errno = 0;
	i = strtoimax(src, endptr, base);

	if (errno == 0 && i >= 0 && i <= SIZE_MAX) {
		*dest = i;
		return 1;
	}

#if SIZE_MAX > INTMAX_MAX
	if (errno == ERANGE && i == INTMAX_MAX) {
		errno = 0;
		u = strtoumax(src, endptr, base);

		if (errno == 0 && u <= SIZE_MAX) {
			*dest = u;
			return 1;
		}
	}
#endif

	if (errno == 0)
		errno = ERANGE;

	return 0;
}

static int
parse_size_arg(const char *src, size_t *dest) {
	char *end;

	if (!parse_size(src, &end, 10, dest)) {
		return 0;
	}
	else if (end == src || *end != '\0') {
		errno = EINVAL;
		return 0;
	}

	return 1;
}

static void
set_defaults(void) {
	struct winsize ws;
	const char *s;

	delim = L'\n';
	padding = 2;

	if (ioctl(2, TIOCGWINSZ, &ws) == 0) {
		term_width = ws.ws_col;
	}
	else {
		s = getenv("COLUMNS");
		if (!s || !parse_size_arg(s, &term_width))
			term_width = TERM_WIDTH;
	}
}

static void
parse_args(int argc, char *argv[]) {
	int opt;

	while ((opt = getopt(argc, argv, ":d:w:p:a")) != -1)
		switch (opt) {
		case 'd':
			if (mbtowc(&delim, optarg, MB_CUR_MAX) == -1)
				die("-d");
			break;

		case 'w':
			if (strcmp(optarg, "-1") == 0 && term_width > 0)
				term_width--;
			else if (!parse_size_arg(optarg, &term_width))
				die(optarg);
			break;

		case 'p':
			if (!parse_size_arg(optarg, &padding))
				die(optarg);
			break;

		case 'a':
			across = 1;
			break;

		default:
			fprintf(stderr, "\
Usage: %s [-d delimiter] [-w width] [-p padding] [-a]\n", argv[0]);
			exit(2);
		}
}

static void
slurp_input(void) {
	wint_t c;

	buf_cap = BUF_ALLOC;
	buf = xmalloc(buf_cap * sizeof buf[0]);

	while ((c = getwchar()) != WEOF) {
		if (buf_len >= buf_cap) {
			buf_cap *= 2;
			buf = xrealloc(buf, buf_cap * sizeof buf[0]);
		}

		buf[buf_len++] = c;
	}

	if (ferror(stdin))
		die("stdin");

	if (buf_len == 0)
		exit(0);
}

static void
parse_list(void) {
	size_t i;
	int width;
	struct item item;

	list_cap = LIST_ALLOC;
	list = xmalloc(list_cap * sizeof list[0]);

	for (i = 0; i < buf_len; i++) {
		item.str = &buf[i];
		item.len = 0;
		item.width = 0;

		for (; i < buf_len; i++) {
			if (buf[i] == delim)
				break;

			width = wcwidth(buf[i]);
			if (width == -1)
				width = 0;

			item.len++;
			item.width += width;
		}

		if (list_len >= list_cap) {
			list_cap *= 2;
			list = xrealloc(list, list_cap * sizeof list[0]);
		}

		list[list_len++] = item;
	}
}

static size_t *next_wider;

static void
init_lut(void) {
	size_t i, j;

	next_wider = xmalloc(list_len * sizeof next_wider[0]);

	for (i = list_len; i-- > 0; ) {
		j = i + 1;
		while (j < list_len && list[i].width >= list[j].width)
			j = next_wider[j];

		next_wider[i] = j;
	}
}

static size_t
max_width(size_t col) {
	size_t i, n;

	i = col * num_rows;
	n = MIN(i + num_rows, list_len);

	while (next_wider[i] < n)
		i = next_wider[i];

	return list[i].width;
}

static size_t
calc_other(size_t x) {
	size_t y;

	y = list_len / x;
	if (list_len % x)
		y++;

	return y;
}

static void
init_calc(void) {
	size_t max_cols;

	if (padding == 0)
		max_cols = list_len;
	else
		max_cols = MIN((term_width / padding) + 1, list_len);

	if (across) {
		num_cols = max_cols;
	}
	else {
		init_lut();
		num_rows = calc_other(max_cols);
	}

	col_widths = xmalloc(max_cols * sizeof col_widths[0]);
}

static int
fits(void) {
	size_t width;
	size_t i;

	width = (num_cols - 1) * padding;

	for (i = 0; i < num_cols; i++) {
		if (width > term_width)
			break;

		width += max_width(i);
	}

	if (width <= term_width) {
		for (i = 0; i < num_cols; i++)
			col_widths[i] = max_width(i);
		
		space_left = term_width - width;
		return 1;
	}

	return 0;
}

static int
fits_across(void) {
	size_t width;
	size_t i, col;

	memset(col_widths, 0, num_cols * sizeof col_widths[0]);

	width = (num_cols - 1) * padding;
	col = 0;

	for (i = 0; i < list_len; i++) {
		if (width > term_width)
			break;

		if (list[i].width > col_widths[col]) {
			width += list[i].width - col_widths[col];
			col_widths[col] = list[i].width;
		}

		col++;
		if (col == num_cols)
			col = 0;
	}

	if (width <= term_width) {
		space_left = term_width - width;
		return 1;
	}

	return 0;
}

static void
calc_dimens(void) {
	init_calc();

	if (across)
		for (; num_cols >= 1; num_cols--) {
			num_rows = calc_other(num_cols);
			if (fits_across())
				return;
		}
	else
		for (; num_rows <= list_len; num_rows++) {
			num_cols = calc_other(num_rows);
			if (fits())
				return;
		}

	num_rows = list_len;
	num_cols = 1;
	col_widths[0] = 0;
	status = 1;
}

static void
print_item(const struct item *item) {
	size_t i;
	
	for (i = 0; i < item->len; i++)
		putwchar(item->str[i]);
}

static void
pad(size_t n, size_t i) {
	for (; i < n; i++)
		putwchar(L' ');
}

static void
print_cell(size_t row, size_t col) {
	size_t i;

	if (across)
		i = (row * num_cols) + col;
	else
		i = (col * num_rows) + row;

	if (i >= list_len) {
		pad(col_widths[col], 0);
	}
	else {
		print_item(&list[i]);
		pad(col_widths[col], list[i].width);
	}
}

static void
print_cols(void) {
	size_t i, j;

	for (i = 0; i < num_rows; i++) {
		for (j = 0; j < num_cols - 1; j++) {
			print_cell(i, j);
			pad(padding, 0);
		}

		print_cell(i, j);
		pad(space_left, 0);

		putwchar(L'\n');
	}
}

int
main(int argc, char *argv[]) {
	setlocale(LC_ALL, "");
	set_defaults();
	parse_args(argc, argv);
	slurp_input();
	parse_list();
	calc_dimens();
	print_cols();
	return status;
}

static void
die(const char *s) {
	perror(s);
	exit(2);
}

static void *
xmalloc(size_t n) {
	void *ptr;

	ptr = malloc(n);
	if (ptr == NULL && n > 0)
		die(NULL);

	return ptr;
}

static void *
xrealloc(void *ptr, size_t n) {
	void *result;

	result = realloc(ptr, n);
	if (result == NULL && (ptr == NULL || n > 0))
		die(NULL);
	
	return result;
}
