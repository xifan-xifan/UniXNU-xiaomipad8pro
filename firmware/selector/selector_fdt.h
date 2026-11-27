/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_SELECTOR_FDT_H
#define PIANO_SELECTOR_FDT_H

#define PIANO_FDT_MAX (64u << 20)

static inline unsigned piano_fdt_u32(const unsigned char *p)
{
	return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static inline unsigned piano_fdt_string_len(const unsigned char *p, unsigned size)
{
	unsigned i = 0;
	while (i < size && p[i])
		i++;
	return i;
}

static inline int piano_fdt_arg(const unsigned char *p, unsigned size, const char *arg, unsigned len)
{
	unsigned i = 0, end, j;
	if (!size || piano_fdt_string_len(p, size) != size - 1)
		return -1;
	size--;
	while (i < size) {
		while (i < size && (p[i] == ' ' || p[i] == '\t'))
			i++;
		end = i;
		while (end < size && p[end] != ' ' && p[end] != '\t')
			end++;
		if (end - i == len) {
			for (j = 0; j < len && p[i + j] == (unsigned char)arg[j]; j++)
				;
			if (j == len)
				return 1;
		}
		i = end;
	}
	return 0;
}

/* Match a complete token in /chosen/bootargs. An invalid header or property returns -1 so the selector can take stock. */
static inline int piano_fdt_has_bootarg(const unsigned char *fdt, unsigned available, const char *arg, unsigned len)
{
	unsigned total, pos, end, strings, strings_size, structure_size, depth = 0;
	int root = 0, chosen = 0, seen_chosen = 0, match = -1;

	if (!fdt || available < 40 || piano_fdt_u32(fdt) != 0xd00dfeedu)
		return -1;
	total = piano_fdt_u32(fdt + 4);
	pos = piano_fdt_u32(fdt + 8);
	strings = piano_fdt_u32(fdt + 12);
	if (total < 40 || total > available || total > PIANO_FDT_MAX ||
	    piano_fdt_u32(fdt + 20) < 17 || piano_fdt_u32(fdt + 24) > 17)
		return -1;
	strings_size = piano_fdt_u32(fdt + 32);
	structure_size = piano_fdt_u32(fdt + 36);
	if (pos < 40 || pos % 4 || structure_size % 4 || strings < pos ||
	    strings > total || structure_size > strings - pos || strings_size > total - strings)
		return -1;
	end = pos + structure_size;
	while (pos < end) {
		unsigned token = piano_fdt_u32(fdt + pos);
		pos += 4;
		if (token == 1) {	/* FDT_BEGIN_NODE */
			unsigned n = piano_fdt_string_len(fdt + pos, end - pos);
			if (n == end - pos || (!depth && (root || n)))
				return -1;
			root = 1;
			depth++;
			if (depth == 2) {
				chosen = n == 6 && fdt[pos] == 'c' && fdt[pos + 1] == 'h' &&
					fdt[pos + 2] == 'o' && fdt[pos + 3] == 's' &&
					fdt[pos + 4] == 'e' && fdt[pos + 5] == 'n';
				if (chosen && seen_chosen)
					return -1;
				seen_chosen |= chosen;
			}
			pos += (n + 4) & ~3u;	/* align the name and its NUL */
		} else if (token == 2) {	/* FDT_END_NODE */
			if (!depth)
				return -1;
			depth--;
		} else if (token == 3) {	/* FDT_PROP */
			unsigned n, name;
			if (!depth || end - pos < 8)
				return -1;
			n = piano_fdt_u32(fdt + pos);
			name = piano_fdt_u32(fdt + pos + 4);
			pos += 8;
			if (n > end - pos || name >= strings_size)
				return -1;
			if (chosen && depth == 2 && strings_size - name >= 9) {
				const unsigned char *s = fdt + strings + name;
				if (s[0] == 'b' && s[1] == 'o' && s[2] == 'o' && s[3] == 't' &&
				    s[4] == 'a' && s[5] == 'r' && s[6] == 'g' && s[7] == 's' && !s[8]) {
					if (match >= 0)
						return -1;
					match = piano_fdt_arg(fdt + pos, n, arg, len);
					if (match < 0)
						return -1;
				}
			}
			pos += (n + 3) & ~3u;
		} else if (token == 9) {	/* FDT_END */
			return root && !depth ? match > 0 : -1;
		} else if (token != 4) {	/* FDT_NOP is skippable */
			return -1;
		}
	}
	return -1;
}

#endif
