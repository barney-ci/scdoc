#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "string.h"
#include "unicode.h"
#include "util.h"

static int parse_section(struct parser *p) {
	str_t *section = str_create();
	uint32_t ch;
	while ((ch = parser_getch(p)) != UTF8_INVALID) {
		if (isdigit(ch)) {
			assert(str_append_ch(section, ch) != -1);
		} else if (ch == ')') {
			if (!section->str) {
				break;
			}
			int sec = strtol(section->str, NULL, 10);
			if (sec < 1 || sec > 9) {
				parser_fatal(p, "Expected section between 1 and 9");
				break;
			}
			str_free(section);
			return sec;
		} else {
			parser_fatal(p, "Expected digit or )");
			break;
		}
	};
	parser_fatal(p, "Expected manual section");
	return -1;
}

static void parse_preamble(struct parser *p) {
	str_t *name = str_create();
	int section = -1;
	uint32_t ch;
	char date[256];
	time_t now;
	time(&now);
	struct tm *now_tm = localtime(&now);
	strftime(date, sizeof(date), "%F", now_tm);
	while ((ch = parser_getch(p)) != UTF8_INVALID) {
		if (isalnum(ch)) {
			assert(str_append_ch(name, ch) != -1);
		} else if (ch == '(') {
			section = parse_section(p);
		} else if (ch == '\n') {
			if (name->len == 0) {
				parser_fatal(p, "Expected preamble");
			}
			if (section == -1) {
				parser_fatal(p, "Expected manual section");
			}
			char sec[2] = { '0' + section, 0 };
			roff_macro(p, "TH", name->str, sec, date, NULL);
			break;
		}
	}
	str_free(name);
}

static void parse_format(struct parser *p, enum formatting fmt) {
	char formats[FORMAT_LAST] = {
		[FORMAT_BOLD] = 'B',
		[FORMAT_UNDERLINE] = 'I',
	};
	if (p->flags) {
		if ((p->flags & ~fmt)) {
			parser_fatal(p, "Cannot nest inline formatting.");
		}
		fprintf(p->output, "\\fR");
	} else {
		fprintf(p->output, "\\f%c", formats[fmt]);
	}
	p->flags ^= fmt;
}

static void parse_text(struct parser *p) {
	uint32_t ch;
	int i = 0;
	while ((ch = parser_getch(p)) != UTF8_INVALID) {
		switch (ch) {
		case '\\':
			ch = parser_getch(p);
			if (ch == UTF8_INVALID) {
				parser_fatal(p, "Unexpected EOF");
			} else if (ch == '\\') {
				fprintf(p->output, "\\\\");
			} else {
				utf8_fputch(p->output, ch);
			}
			break;
		case '*':
			parse_format(p, FORMAT_BOLD);
			break;
		case '_':
			parse_format(p, FORMAT_UNDERLINE);
			break;
		case '\n':
			utf8_fputch(p->output, ch);
			return;
		case '.':
			if (!i) {
				// Escape . if it's the first character
				fprintf(p->output, "\\&.");
				break;
			}
			/* fallthrough */
		default:
			utf8_fputch(p->output, ch);
			break;
		}
		++i;
	}
}

static void parse_heading(struct parser *p) {
	uint32_t ch;
	int level = 1;
	while ((ch = parser_getch(p)) != UTF8_INVALID) {
		if (ch == '#') {
			++level;
		} else if (ch == ' ') {
			break;
		} else {
			parser_fatal(p, "Invalid start of heading (probably needs a space)");
		}
	}
	switch (level) {
	case 1:
		fprintf(p->output, ".SH ");
		break;
	case 2:
		fprintf(p->output, ".SS ");
		break;
	default:
		parser_fatal(p, "Only headings up to two levels deep are permitted");
		break;
	}
	while ((ch = parser_getch(p)) != UTF8_INVALID) {
		utf8_fputch(p->output, ch);
		if (ch == '\n') {
			break;
		}
	}
}

static int parse_indent(struct parser *p, int *indent, bool write) {
	int i = 0;
	uint32_t ch;
	while ((ch = parser_getch(p)) == '\t') {
		++i;
	}
	parser_pushch(p, ch);
	if (ch == '\n' && *indent != 0) {
		// Don't change indent when we encounter empty lines
		return *indent;
	}
	if (write) {
		if (i < *indent) {
			for (int j = *indent; i < j; --j) {
				roff_macro(p, "RE", NULL);
			}
		} else if (i == *indent + 1) {
			roff_macro(p, "RS", "4", NULL);
		} else if (i != *indent && ch == '\t') {
			parser_fatal(p, "Indented by an amount greater than 1");
		}
	}
	*indent = i;
	return i;
}

static void list_header(struct parser *p, int *num) {
	roff_macro(p, "RS", "4", NULL);
	fprintf(p->output, ".ie n \\{\\\n");
	if (*num == -1) {
		fprintf(p->output, "\\h'-0%d'%s\\h'+03'\\c\n",
				*num >= 10 ? 5 : 4, "\\(bu");
	} else {
		fprintf(p->output, "\\h'-0%d'%d.\\h'+03'\\c\n",
				*num >= 10 ? 5 : 4, *num);
	}
	fprintf(p->output, ".\\}\n");
	fprintf(p->output, ".el \\{\\\n");
	if (*num == -1) {
		fprintf(p->output, ".IP %s 4\n", "\\(bu");
	} else {
		fprintf(p->output, ".IP %d. 4\n", *num);
		*num = *num + 1;
	}
	fprintf(p->output, ".\\}\n");
}

static void parse_list(struct parser *p, int *indent, int num) {
	uint32_t ch;
	if ((ch = parser_getch(p)) != ' ') {
		parser_fatal(p, "Expected space before start of list entry");
	}
	list_header(p, &num);
	parse_text(p);
	bool closed = false;
	do {
		parse_indent(p, indent, true);
		if ((ch = parser_getch(p)) == UTF8_INVALID) {
			break;
		}
		switch (ch) {
		case ' ':
			if ((ch = parser_getch(p)) != ' ') {
				parser_fatal(p, "Expected two spaces for list entry continuation");
			}
			parse_text(p);
			break;
		case '-':
		case '.':
			if ((ch = parser_getch(p)) != ' ') {
				parser_fatal(p, "Expected space before start of list entry");
			}
			if (!closed) {
				roff_macro(p, "RE", NULL);
			}
			list_header(p, &num);
			parse_text(p);
			closed = false;
			break;
		default:
			fprintf(p->output, "\n");
			parser_pushch(p, ch);
			goto ret;
		}
	} while (ch != UTF8_INVALID);
ret:
	if (!closed) {
		roff_macro(p, "RE", NULL);
	}
}

static void parse_literal(struct parser *p, int *indent) {
	uint32_t ch;
	if ((ch = parser_getch(p)) != '`' ||
		(ch = parser_getch(p)) != '`' ||
		(ch = parser_getch(p)) != '\n') {
		parser_fatal(p, "Expected ``` and a newline to begin literal block");
	}
	int stops = 0;
	roff_macro(p, "nf", NULL);
	roff_macro(p, "RS", "4", NULL);
	do {
		int _indent = *indent;
		parse_indent(p, &_indent, false);
		if (_indent < *indent) {
			parser_fatal(p, "Cannot deindent in literal block");
		}
		while (_indent > *indent) {
			--_indent;
			fprintf(p->output, "\t");
		}
		if ((ch = parser_getch(p)) == UTF8_INVALID) {
			break;
		}
		if (ch == '`') {
			if (++stops == 3) {
				if ((ch = parser_getch(p)) != '\n') {
					parser_fatal(p, "Expected literal block to end with newline");
				}
				roff_macro(p, "fi", NULL);
				roff_macro(p, "RE", NULL);
				return;
			}
		} else {
			while (stops != 0) {
				fputc('`', p->output);
				--stops;
			}
			switch (ch) {
			case '.':
				fprintf(p->output, "\\&.");
				break;
			case '\\':
				ch = parser_getch(p);
				if (ch == UTF8_INVALID) {
					parser_fatal(p, "Unexpected EOF");
				} else if (ch == '\\') {
					fprintf(p->output, "\\\\");
				} else {
					utf8_fputch(p->output, ch);
				}
				break;
			default:
				utf8_fputch(p->output, ch);
				break;
			}
		}
	} while (ch != UTF8_INVALID);
}

static void parse_document(struct parser *p) {
	uint32_t ch;
	int indent = 0;
	do {
		parse_indent(p, &indent, true);
		if ((ch = parser_getch(p)) == UTF8_INVALID) {
			break;
		}
		switch (ch) {
		case '#':
			if (indent != 0) {
				parser_pushch(p, ch);
				parse_text(p);
				break;
			}
			parse_heading(p);
			break;
		case '-':
			parse_list(p, &indent, -1);
			break;
		case '.':
			if ((ch = parser_getch(p)) == ' ') {
				parser_pushch(p, ch);
				parse_list(p, &indent, 1);
			} else {
				parser_pushch(p, ch);
				parse_text(p);
			}
			break;
		case '`':
			parse_literal(p, &indent);
			break;
		case ' ':
			parser_fatal(p, "Tabs are required for indentation");
			break;
		case '\n':
			roff_macro(p, "P", NULL);
			break;
		default:
			parser_pushch(p, ch);
			parse_text(p);
			break;
		}
	} while (ch != UTF8_INVALID);
}

static void output_scdoc_preamble(struct parser *p) {
	fprintf(p->output, ".\\\" Generated by scdoc " VERSION "\n");
	fprintf(p->output, ".\\\" Fix weird quotation marks:\n");
	fprintf(p->output, ".\\\" http://bugs.debian.org/507673\n");
	fprintf(p->output, ".\\\" http://lists.gnu.org/archive/html/groff/2009-02/msg00013.html\n");
	fprintf(p->output, ".ie \\n(.g .ds Aq \\(aq\n");
	fprintf(p->output, ".el       .ds Aq '\n");
	fprintf(p->output, ".\\\" Disable hyphenation:\n");
	roff_macro(p, "nh", NULL);
	fprintf(p->output, ".\\\" Disable justification:\n");
	roff_macro(p, "ad l", NULL);
	fprintf(p->output, ".\\\" Generated content:\n");
}

int main(int argc, char **argv) {
	if (argc > 1) {
		fprintf(stderr, "Usage: scdoc < input.scd > output.roff");
		return 1;
	}
	struct parser p = {
		.input = stdin,
		.output = stdout,
		.line = 1,
		.col = 1
	};
	output_scdoc_preamble(&p);
	parse_preamble(&p);
	parse_document(&p);
	return 0;
}
