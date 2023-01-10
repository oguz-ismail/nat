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

#if __sun__ || __USLC__
#include <stropts.h>
#include <termios.h>
#endif

#define _XOPEN_SOURCE 600

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

#ifndef NO_WCHAR
#define SPACE L' '
#define NEWLINE L'\n'
#endif

#define BUF_CAP_INIT 512
#define LIST_CAP_INIT 32

#define MIN(x, y) ((x) < (y) ? (x) : (y))

struct item {
	const wchar_t *str;
	size_t len;
	size_t width;
};

wchar_t *buf;
size_t buf_len;
size_t buf_cap;

struct item *list;
size_t list_len;
size_t list_cap;

wint_t delim;
size_t term_width;
size_t padding;

size_t num_rows;
size_t num_cols;
size_t *col_widths;
size_t space_left;

int status;

static void die(int, const char *);
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
parse_size_arg(const char *s, size_t *dest) {
	char *end;

	return (parse_size(s, &end, 10, dest) && end != s && *end == '\0');
}

static void
set_defaults(void) {
	struct winsize win;
	const char *s;

	delim = NEWLINE;
	padding = 2;

	if (ioctl(2, TIOCGWINSZ, &win) == 0)
		term_width = win.ws_col;
	else if (!(s = getenv("COLUMNS")) || !parse_size_arg(s, &term_width))
		term_width = 80;
}

static void
parse_args(int argc, char *argv[]) {
	int opt;
	wchar_t c;

	while ((opt = getopt(argc, argv, ":d:w:p:")) != -1)
		switch (opt) {
		case 'd':
			if (mbtowc(&c, optarg, MB_CUR_MAX) == -1)
				die(1, "-d");
			delim = c;
			break;
		case 'w':
			if (strcmp(optarg, "-1") == 0 && term_width > 0) {
				term_width--;
				break;
			}
			if (!parse_size_arg(optarg, &term_width))
				die(1, optarg);
			break;
		case 'p':
			if (!parse_size_arg(optarg, &padding))
				die(1, optarg);
			break;
		default:
			fputs("Usage: nat [-d DELIM] [-w WIDTH] [-p PADDING]\n", stderr);
			exit(1);
		}
}

static void
slurp_input(void) {
	wint_t c;

	buf_cap = BUF_CAP_INIT;
	buf = xmalloc(buf_cap * sizeof buf[0]);

	while ((c = getwchar()) != WEOF) {
		if (buf_len >= buf_cap) {
			buf_cap *= 2;
			buf = xrealloc(buf, buf_cap * sizeof buf[0]);
		}

		buf[buf_len++] = c;
	}

	if (ferror(stdin))
		die(1, "stdin");

	if (buf_len == 0)
		exit(0);
}

static void
parse_list(void) {
	size_t i;
	int width;
	struct item *cur;
	size_t len;

	list_cap = LIST_CAP_INIT;
	list = xmalloc(list_cap * sizeof list[0]);

	cur = list;
	len = 0;

	for (i = 0; i < buf_len; i++) {
		if (len == 0 && list_len >= list_cap) {
			list_cap *= 2;
			list = xrealloc(list, list_cap * sizeof list[0]);
			cur = &list[list_len];
		}
		
		if (len == 0) {
			cur->str = &buf[i];
			cur->width = 0;
		}

		if (buf[i] == delim) {
			cur->len = len;
			cur++;
			len = 0;
			list_len++;
			continue;
		}

		len++;

		width = wcwidth(buf[i]);
		if (width == -1)
			width = 0;

		cur->width += width;
	}

	if (len != 0) {
		cur->len = len;
		list_len++;
	}
}

size_t *next_wider;

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
calc_other(size_t n) {
	size_t m;

	m = list_len / n;
	if (list_len % n)
		m++;

	return m;
}

static void
init_calc() {
	size_t max_cols;

	init_lut();

	if (padding == 0)
		max_cols = list_len;
	else
		max_cols = MIN((term_width / padding) + 1, list_len);

	col_widths = xmalloc(max_cols * sizeof col_widths[0]);
	num_rows = calc_other(max_cols);
}

static void
calc_rows(void) {
	size_t width;
	size_t i;

	init_calc();

	for (; num_rows <= list_len; num_rows++) {
		num_cols = calc_other(num_rows);
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
			return;
		}
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
		putwchar(SPACE);
}

static void
print_cell(size_t row, size_t col) {
	size_t i;

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
		putwchar(NEWLINE);
	}
}

int
main(int argc, char *argv[]) {
	setlocale(LC_ALL, "");
	set_defaults();
	parse_args(argc, argv);
	slurp_input();
	parse_list();
	calc_rows();
	print_cols();
	return status;
}

static void
die(int status, const char *s) {
	perror(s);
	exit(status);
}

static void *
xmalloc(size_t n) {
	void *ptr;

	ptr = malloc(n);
	if (ptr == NULL && n > 0)
		die(1, NULL);

	return ptr;
}

static void *
xrealloc(void *ptr, size_t n) {
	void *result;

	result = realloc(ptr, n);
	if (result == NULL && (ptr == NULL || n > 0))
		die(1, NULL);
	
	return result;
}
