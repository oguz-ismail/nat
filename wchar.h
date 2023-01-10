#include <ctype.h>
#define wchar_t		char
#define wint_t		int
#define WEOF		EOF
#undef MB_CUR_MAX
#define MB_CUR_MAX	1
#define getwchar	getchar
#define putwchar	putchar
#define mbtowc(p, q, n)	(*(p) = *(q), 1)
#define wcwidth(c)	((c) == '\0' ? 0 : isprint(c) ? 1 : -1)
#define SPACE		' '
#define NEWLINE		'\n'
#define NO_WCHAR
