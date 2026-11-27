// SPDX-License-Identifier: BSD-2-Clause-Patent
// pool-manifest: one line per file-system object under ROOT, for comparing each volume with its copy (T-20).
// The same binary runs on the host and on the tablet.
//
//   pool-manifest [--strict-compression] [--no-mtime] [--exclude TOPLEVEL]... ROOT
//   pool-manifest --paths LIST ROOT   (T-20 v2: per listed TAG<TAB>PATH the line of ROOT/PATH as seen through every mount and
//                                      union on the way, TAG first; "#ls TAG PATH NAME" per directory entry; "#missing TAG PATH errno")
//   pool-manifest --diff SOURCE COPY [--allow-decompressed] [--forbid-types 7,13] [--ignore-xattr NAME]... [--ignore-flags HEX]
//
// --diff compares two manifests entry by entry (both directions, every field) and prints EQUAL or DIFFERENT with counts per field
// and the first samples; exit 0 only for EQUAL. --allow-decompressed: a file compressed in SOURCE may be stored uncompressed in COPY
// (UF_COMPRESSED and the compression field dropped from the comparison for that file; its data hash must still match); --forbid-types:
// a COPY file whose decmpfs type is in the list is a mismatch (types this kernel is not shown to read); --ignore-xattr, --ignore-flags:
// relaxations, printed with the result. Every relaxation used is printed, never silent.
//
// --exclude leaves out a top-level entry of ROOT and everything below it (.fseventsd: the source volume's own fsevents journal).
// Never crosses into another mount (FTS_XDEV), never follows a symbolic link, reads every regular file whole (a compressed file
// reads decompressed: its data hash is the content's). Output: tab-separated, sorted by path (bytewise):
//   path type mode uid gid flags size mtime data link hardlink xattrs acl compression
//   path        relative to ROOT ("." for ROOT itself), \t \n \\ and bytes outside 0x21-0x7e escaped as \xHH
//   type        f d l b c p s
//   mode        st_mode & 07777, octal
//   flags       st_flags, hex (UF_COMPRESSED 0x20 included)
//   size        st_size of a file or link, "-" otherwise
//   mtime       st_mtimespec seconds.nanoseconds ("-" with --no-mtime)
//   data        sha256 of a regular file's content, the rdev of a device, "-" otherwise
//   link        readlink of a symbolic link, "-" otherwise
//   hardlink    the first path (in this order) of the same inode when st_nlink > 1 and it was seen before, "-" otherwise
//   xattrs      name=sha256 pairs, sorted, comma separated ("-" when none); com.apple.decmpfs and com.apple.ResourceFork of a
//               compressed file are left out here (they are the compression's own storage) unless --strict-compression
//   acl         the extended ACL as acl_to_text gives it with every name dropped (tag:uuid:id:allow|deny:perms;...), "-" if none
//   compression for UF_COMPRESSED files: decmpfs type and uncompressed size (type:size), "-" otherwise
// The last line: "# entries N files F bytes B sha256 S" where S is the sha256 of every line above.
#include <CommonCrypto/CommonDigest.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <membership.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int strict_compression, no_mtime;
static const char *excluded[8];
static int n_excluded;
static CC_SHA256_CTX whole;
static unsigned long long n_entries, n_files, n_bytes;
static int n_errors;

static void hex(const unsigned char *d, size_t n, char *out)
{
	static const char x[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) {
		out[2 * i] = x[d[i] >> 4];
		out[2 * i + 1] = x[d[i] & 15];
	}
	out[2 * n] = 0;
}

static void put(const char *s)
{
	fputs(s, stdout);
	CC_SHA256_Update(&whole, s, (CC_LONG)strlen(s));
}

static void put_escaped(const char *s)
{
	char buf[8];
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p > 0x20 && *p < 0x7f && *p != '\\') {
			buf[0] = (char)*p;
			buf[1] = 0;
		} else {
			snprintf(buf, sizeof buf, "\\x%02x", *p);
		}
		put(buf);
	}
}

static int sha_file(const char *path, char out[65], unsigned long long *bytes)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	static unsigned char buf[1 << 20];
	CC_SHA256_CTX c;
	CC_SHA256_Init(&c);
	ssize_t r;
	while ((r = read(fd, buf, sizeof buf)) > 0) {
		CC_SHA256_Update(&c, buf, (CC_LONG)r);
		*bytes += (unsigned long long)r;
	}
	int e = r < 0 ? errno : 0;
	close(fd);
	if (e) {
		errno = e;
		return -1;
	}
	unsigned char d[32];
	CC_SHA256_Final(d, &c);
	hex(d, 32, out);
	return 0;
}

static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int by_name(const FTSENT **a, const FTSENT **b)
{
	return strcmp((*a)->fts_name, (*b)->fts_name);
}

// xattrs: name=sha256 sorted; the decmpfs header's type and size into comp
static int xattrs(const char *path, int compressed, char **out, char *comp, size_t comp_len)
{
	const int opt = XATTR_NOFOLLOW | XATTR_SHOWCOMPRESSION;
	ssize_t n = listxattr(path, NULL, 0, opt);
	*out = NULL;
	if (n < 0)
		return -1;
	if (n == 0)
		return 0;
	char *names = malloc((size_t)n);
	if (!names)
		return -1;
	n = listxattr(path, names, (size_t)n, opt);
	if (n < 0) {
		free(names);
		return -1;
	}
	size_t count = 0;
	for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1)
		count++;
	char **v = calloc(count ? count : 1, sizeof *v);
	size_t k = 0;
	for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1)
		v[k++] = names + i;
	qsort(v, count, sizeof *v, cmp_names);
	size_t cap = 256, len = 0;
	char *s = malloc(cap);
	s[0] = 0;
	for (size_t i = 0; i < count; i++) {
		const char *name = v[i];
		ssize_t sz = getxattr(path, name, NULL, 0, 0, opt);
		if (sz < 0) {
			free(v); free(names); free(s);
			return -1;
		}
		unsigned char *val = malloc(sz ? (size_t)sz : 1);
		if (sz && getxattr(path, name, val, (size_t)sz, 0, opt) != sz) {
			free(val); free(v); free(names); free(s);
			return -1;
		}
		int own = compressed && (!strcmp(name, "com.apple.decmpfs") || !strcmp(name, "com.apple.ResourceFork"));
		if (compressed && !strcmp(name, "com.apple.decmpfs") && sz >= 16) {
			uint32_t type;
			uint64_t usize;
			memcpy(&type, val + 4, 4);
			memcpy(&usize, val + 8, 8);
			snprintf(comp, comp_len, "%u:%llu", type, (unsigned long long)usize);
		}
		if (!own || strict_compression) {
			unsigned char d[32];
			char h[65];
			CC_SHA256(val, (CC_LONG)sz, d);
			hex(d, 32, h);
			size_t need = len + strlen(name) * 4 + 70;
			if (need > cap) {
				while (need > cap)
					cap *= 2;
				s = realloc(s, cap);
			}
			// names are escaped like paths (a comma or '=' in a name is escaped too)
			for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
				if (*p > 0x20 && *p < 0x7f && *p != '\\' && *p != ',' && *p != '=')
					s[len++] = (char)*p;
				else
					len += (size_t)sprintf(s + len, "\\x%02x", *p);
			}
			len += (size_t)sprintf(s + len, "=%s%s", h, i + 1 < count ? "," : "");
		}
		free(val);
	}
	while (len && s[len - 1] == ',')
		s[--len] = 0;
	free(v);
	free(names);
	if (len)
		*out = s;
	else
		free(s);
	return 0;
}

// the extended ACL without names: each entry "tag:uuid:id:kind:perms" -> "tag:uuid:kind:perms" (the id is name-derived too)
static char *acl_plain(const char *path)
{
	acl_t a = acl_get_link_np(path, ACL_TYPE_EXTENDED);
	if (!a)
		return NULL;
	ssize_t n;
	char *t = acl_to_text(a, &n);
	acl_free(a);
	if (!t)
		return NULL;
	char *out = malloc(strlen(t) + 2);
	size_t len = 0;
	char *save = NULL;
	for (char *line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (line[0] == '!' || line[0] == '#' || !line[0])
			continue;
		char *f[8] = {0};
		int nf = 0;
		for (char *p = line; nf < 8; ) {
			f[nf++] = p;
			char *c = strchr(p, ':');
			if (!c)
				break;
			*c = 0;
			p = c + 1;
		}
		// tag:uuid:name:id:kind:perms
		if (nf >= 6)
			len += (size_t)sprintf(out + len, "%s%s:%s:%s:%s", len ? ";" : "", f[0], f[1], f[4], f[5]);
		else
			for (int i = 0; i < nf; i++)
				len += (size_t)sprintf(out + len, "%s%s", i ? ":" : (len ? ";" : ""), f[i]);
	}
	out[len] = 0;
	acl_free(t);
	if (!len) {
		free(out);
		return NULL;
	}
	return out;
}

struct ino_seen { dev_t dev; ino_t ino; char tag; char *path; };
static struct ino_seen *seen;
static size_t n_seen, cap_seen;

static const char *first_path(const struct stat *st, const char *rel, const char *tag)
{
	char k = tag ? tag[0] : 0;
	for (size_t i = 0; i < n_seen; i++)
		if (seen[i].dev == st->st_dev && seen[i].ino == st->st_ino && seen[i].tag == k)
			return seen[i].path;
	if (n_seen == cap_seen) {
		cap_seen = cap_seen ? cap_seen * 2 : 256;
		seen = realloc(seen, cap_seen * sizeof *seen);
	}
	seen[n_seen++] = (struct ino_seen){st->st_dev, st->st_ino, k, strdup(rel)};
	return NULL;
}

// one manifest line for the object ACC (shown as SHOWN in messages) at REL; TAG: the --paths layer tag (a first field), or NULL
static void emit(const char *acc, const char *shown, const char *rel, const struct stat *st, const char *tag)
{
	char type = S_ISREG(st->st_mode) ? 'f' : S_ISDIR(st->st_mode) ? 'd' : S_ISLNK(st->st_mode) ? 'l' : S_ISBLK(st->st_mode) ? 'b'
		: S_ISCHR(st->st_mode) ? 'c' : S_ISFIFO(st->st_mode) ? 'p' : S_ISSOCK(st->st_mode) ? 's' : '?';
	char line[512], data[80] = "-", comp[64] = "-";
	const char *hl = NULL;
	if (type == 'f') {
		if (st->st_nlink > 1)
			hl = first_path(st, rel, tag);
		if (sha_file(acc, data, &n_bytes) != 0) {
			fprintf(stderr, "pool-manifest: %s: read: %s\n", shown, strerror(errno));
			n_errors++;
			strcpy(data, "UNREADABLE");
		}
		n_files++;
	} else if (type == 'b' || type == 'c') {
		snprintf(data, sizeof data, "rdev=%llx", (unsigned long long)st->st_rdev);
	}
	if (tag) {
		put(tag);
		put("\t");
	}
	put_escaped(rel);
	snprintf(line, sizeof line, "\t%c\t%o\t%u\t%u\t%x\t", type, st->st_mode & 07777, st->st_uid, st->st_gid, st->st_flags);
	put(line);
	if (type == 'f' || type == 'l')
		snprintf(line, sizeof line, "%lld\t", (long long)st->st_size);
	else
		snprintf(line, sizeof line, "-\t");
	put(line);
	if (no_mtime)
		put("-\t");
	else {
		snprintf(line, sizeof line, "%ld.%09ld\t", (long)st->st_mtimespec.tv_sec, st->st_mtimespec.tv_nsec);
		put(line);
	}
	put(data);
	put("\t");
	if (type == 'l') {
		char target[4096];
		ssize_t n = readlink(acc, target, sizeof target - 1);
		if (n < 0) {
			fprintf(stderr, "pool-manifest: %s: readlink: %s\n", shown, strerror(errno));
			n_errors++;
			n = 0;
		}
		target[n] = 0;
		put_escaped(n ? target : "-");
	} else {
		put("-");
	}
	put("\t");
	if (hl)
		put_escaped(hl);
	else
		put("-");
	put("\t");
	char *x = NULL;
	if (xattrs(acc, (st->st_flags & UF_COMPRESSED) != 0, &x, comp, sizeof comp) != 0) {
		fprintf(stderr, "pool-manifest: %s: xattr: %s\n", shown, strerror(errno));
		n_errors++;
	}
	put(x ? x : "-");
	free(x);
	put("\t");
	char *acl = acl_plain(acc);
	put(acl ? acl : "-");
	free(acl);
	put("\t");
	put((st->st_flags & UF_COMPRESSED) ? comp : "-");
	put("\n");
	n_entries++;
}

// ---- --diff ---------------------------------------------------------------------------------------------------------------------
enum { F_PATH, F_TYPE, F_MODE, F_UID, F_GID, F_FLAGS, F_SIZE, F_MTIME, F_DATA, F_LINK, F_HL, F_XATTR, F_ACL, F_COMP, NF };
static const char *field_names[NF] = {"path", "type", "mode", "uid", "gid", "flags", "size", "mtime", "data", "link", "hardlink", "xattrs",
	"acl", "compression"};
struct mline { char *f[NF]; };
struct mfile { struct mline *v; size_t n; char *summary; };

static int load_manifest(const char *path, struct mfile *m)
{
	FILE *fp = fopen(path, "r");
	if (!fp) {
		perror(path);
		return -1;
	}
	size_t cap = 4096;
	m->v = malloc(cap * sizeof *m->v);
	m->n = 0;
	m->summary = NULL;
	char *line = NULL;
	size_t lcap = 0;
	ssize_t len;
	while ((len = getline(&line, &lcap, fp)) > 0) {
		if (line[len - 1] == '\n')
			line[--len] = 0;
		if (!strncmp(line, "# entries", 9)) {
			m->summary = strdup(line);
			continue;
		}
		char *dup = strdup(line), *p = dup;
		struct mline ml;
		int k = 0;
		for (; k < NF; k++) {
			ml.f[k] = p;
			char *t = strchr(p, '\t');
			if (!t) {
				k++;
				break;
			}
			*t = 0;
			p = t + 1;
		}
		if (k != NF) {
			fprintf(stderr, "%s: a line of %d fields\n", path, k);
			fclose(fp);
			return -1;
		}
		if (m->n == cap) {
			cap *= 2;
			m->v = realloc(m->v, cap * sizeof *m->v);
		}
		m->v[m->n++] = ml;
	}
	free(line);
	fclose(fp);
	if (!m->summary) {
		fprintf(stderr, "%s: no summary line (an interrupted manifest?)\n", path);
		return -1;
	}
	return 0;
}

static int by_path(const void *a, const void *b)
{
	return strcmp(((const struct mline *)a)->f[F_PATH], ((const struct mline *)b)->f[F_PATH]);
}

static char *ignored_xattrs[16];
static int n_ignored_xattrs;

static void strip_xattrs(char *s, char *out, size_t cap)
{
	size_t len = 0;
	out[0] = 0;
	if (!strcmp(s, "-")) {
		snprintf(out, cap, "-");
		return;
	}
	char *copy = strdup(s), *save = NULL;
	for (char *x = strtok_r(copy, ",", &save); x; x = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(x, '=');
		size_t nl = eq ? (size_t)(eq - x) : strlen(x);
		int skip = 0;
		for (int i = 0; i < n_ignored_xattrs; i++)
			if (strlen(ignored_xattrs[i]) == nl && !strncmp(ignored_xattrs[i], x, nl))
				skip = 1;
		if (!skip)
			len += (size_t)snprintf(out + len, cap - len, "%s%s", len ? "," : "", x);
	}
	free(copy);
	if (!len)
		snprintf(out, cap, "-");
}

static int diff_main(int argc, char **argv)
{
	const char *src_path = NULL, *copy_path = NULL;
	int allow_decompressed = 0, forbid[32] = {0};
	unsigned long ignore_flags = 0;
	char forbid_text[128] = "";
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--allow-decompressed"))
			allow_decompressed = 1;
		else if (!strcmp(argv[i], "--forbid-types") && i + 1 < argc) {
			snprintf(forbid_text, sizeof forbid_text, "%s", argv[++i]);
			char *c = strdup(forbid_text), *save = NULL;
			for (char *t = strtok_r(c, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
				int v = atoi(t);
				if (v > 0 && v < 32)
					forbid[v] = 1;
			}
			free(c);
		} else if (!strcmp(argv[i], "--ignore-xattr") && i + 1 < argc && n_ignored_xattrs < 16)
			ignored_xattrs[n_ignored_xattrs++] = argv[++i];
		else if (!strcmp(argv[i], "--ignore-flags") && i + 1 < argc)
			ignore_flags = strtoul(argv[++i], NULL, 16);
		else if (!src_path)
			src_path = argv[i];
		else if (!copy_path)
			copy_path = argv[i];
		else {
			fprintf(stderr, "pool-manifest --diff: unexpected %s\n", argv[i]);
			return 2;
		}
	}
	if (!src_path || !copy_path) {
		fprintf(stderr, "usage: pool-manifest --diff SOURCE COPY [--allow-decompressed] [--forbid-types LIST] [--ignore-xattr NAME]... "
			"[--ignore-flags HEX]\n");
		return 2;
	}
	struct mfile s, c;
	if (load_manifest(src_path, &s) || load_manifest(copy_path, &c))
		return 2;
	qsort(s.v, s.n, sizeof *s.v, by_path);
	qsort(c.v, c.n, sizeof *c.v, by_path);
	unsigned long counts[NF + 3] = {0};   // + missing, extra, forbidden type
	unsigned long decompressed = 0, shown = 0;
	size_t i = 0, j = 0;
	static char xa[1 << 16], xb[1 << 16];
	while (i < s.n || j < c.n) {
		int cmp = i == s.n ? 1 : j == c.n ? -1 : strcmp(s.v[i].f[F_PATH], c.v[j].f[F_PATH]);
		if (cmp < 0) {
			if (shown++ < 12)
				printf("  missing in the copy: %s\n", s.v[i].f[F_PATH]);
			counts[NF]++;
			i++;
			continue;
		}
		if (cmp > 0) {
			if (shown++ < 12)
				printf("  extra in the copy: %s\n", c.v[j].f[F_PATH]);
			counts[NF + 1]++;
			j++;
			continue;
		}
		struct mline *a = &s.v[i], *b = &c.v[j];
		unsigned long fa = strtoul(a->f[F_FLAGS], NULL, 16) & ~ignore_flags, fb = strtoul(b->f[F_FLAGS], NULL, 16) & ~ignore_flags;
		int skip_comp = 0;
		if (allow_decompressed && !strcmp(a->f[F_TYPE], "f") && strcmp(a->f[F_COMP], "-") && !strcmp(b->f[F_COMP], "-") &&
		    !(fb & UF_COMPRESSED)) {
			fa &= ~(unsigned long)UF_COMPRESSED;
			skip_comp = 1;
			decompressed++;
		}
		if (strcmp(b->f[F_COMP], "-") && forbid[atoi(b->f[F_COMP]) & 31]) {
			if (shown++ < 12)
				printf("  forbidden decmpfs type in the copy: %s: %s\n", b->f[F_PATH], b->f[F_COMP]);
			counts[NF + 2]++;
		}
		for (int k = 1; k < NF; k++) {
			int differ;
			if (k == F_FLAGS)
				differ = fa != fb;
			else if (k == F_COMP)
				differ = !skip_comp && strcmp(a->f[k], b->f[k]);
			else if (k == F_XATTR) {
				strip_xattrs(a->f[k], xa, sizeof xa);
				strip_xattrs(b->f[k], xb, sizeof xb);
				differ = strcmp(xa, xb) != 0;
			} else
				differ = strcmp(a->f[k], b->f[k]) != 0;
			if (differ) {
				counts[k]++;
				if (shown++ < 12)
					printf("  %s: %s: %s -> %s\n", field_names[k], a->f[F_PATH], a->f[k], b->f[k]);
			}
		}
		i++;
		j++;
	}
	unsigned long total = 0;
	for (int k = 0; k < NF + 3; k++)
		total += counts[k];
	printf("%s: source %zu entries, copy %zu entries; missing %lu, extra %lu, forbidden-type %lu", total ? "DIFFERENT" : "EQUAL", s.n, c.n,
	       counts[NF], counts[NF + 1], counts[NF + 2]);
	for (int k = 1; k < NF; k++)
		if (counts[k])
			printf(", %s %lu", field_names[k], counts[k]);
	printf("; relaxations: allow-decompressed %s (%lu files stored uncompressed), forbid-types %s, ignore-flags %lx, ignore-xattr",
	       allow_decompressed ? "on" : "off", decompressed, forbid_text[0] ? forbid_text : "none", ignore_flags);
	for (int k = 0; k < n_ignored_xattrs; k++)
		printf(" %s", ignored_xattrs[k]);
	printf("%s\n", n_ignored_xattrs ? "" : " none");
	printf("  source: %s\n  copy:   %s\n", s.summary, c.summary);
	return total ? 1 : 0;
}


// ---- --paths: the live view of a list of paths (T-20 v2 R1b) --------------------------------------------------------------------
static int unescape(const char *in, char *out, size_t cap)
{
	size_t n = 0;
	for (const char *p = in; *p; p++) {
		if (n + 1 >= cap)
			return -1;
		if (p[0] == '\\' && p[1] == 'x' && p[2] && p[3]) {
			char h[3] = {p[2], p[3], 0};
			out[n++] = (char)strtoul(h, NULL, 16);
			p += 3;
		} else
			out[n++] = *p;
	}
	out[n] = 0;
	return 0;
}

static int paths_main(const char *list, const char *root)
{
	FILE *fp = fopen(list, "r");
	if (!fp) {
		perror(list);
		return 2;
	}
	CC_SHA256_Init(&whole);
	char *line = NULL, raw[4096], full[8192];
	size_t lcap = 0;
	ssize_t len;
	unsigned long long missing = 0;
	size_t root_len = strlen(root);
	while (root_len > 1 && root[root_len - 1] == '/')
		root_len--;
	while ((len = getline(&line, &lcap, fp)) > 0) {
		if (line[len - 1] == '\n')
			line[--len] = 0;
		char *tab = strchr(line, '\t');
		if (!tab || tab == line) {
			fprintf(stderr, "pool-manifest --paths: a line without TAG<TAB>PATH: %s\n", line);
			n_errors++;
			continue;
		}
		*tab = 0;
		const char *tag = line, *rel = tab + 1;
		if (unescape(rel, raw, sizeof raw)) {
			n_errors++;
			continue;
		}
		if (!strcmp(raw, "."))
			snprintf(full, sizeof full, "%.*s", (int)root_len, root);
		else
			snprintf(full, sizeof full, "%.*s/%s", (int)(root_len == 1 ? 0 : root_len), root, raw);
		struct stat st;
		if (lstat(full, &st) != 0) {
			char m[64];
			snprintf(m, sizeof m, "\t%d\n", errno);
			put("#missing\t");
			put(tag);
			put("\t");
			put_escaped(rel[0] ? raw : ".");
			put(m);
			missing++;
			continue;
		}
		emit(full, full, raw, &st, tag);
		if (S_ISDIR(st.st_mode)) {
			DIR *d = opendir(full);
			if (!d) {
				fprintf(stderr, "pool-manifest: %s: opendir: %s\n", full, strerror(errno));
				n_errors++;
				continue;
			}
			struct dirent *de;
			while ((de = readdir(d))) {
				if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
					continue;
				put("#ls\t");
				put(tag);
				put("\t");
				put_escaped(raw);
				put("\t");
				put_escaped(de->d_name);
				put("\n");
			}
			closedir(d);
		}
	}
	free(line);
	fclose(fp);
	unsigned char dg[32];
	char h[65];
	CC_SHA256_Final(dg, &whole);
	hex(dg, 32, h);
	printf("# entries %llu files %llu bytes %llu missing %llu sha256 %s errors %d\n", n_entries, n_files, n_bytes, missing, h, n_errors);
	return n_errors ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc == 4 && !strcmp(argv[1], "--paths"))
		return paths_main(argv[2], argv[3]);
	if (argc > 1 && !strcmp(argv[1], "--diff"))
		return diff_main(argc, argv);
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
		if (!strcmp(argv[i], "--strict-compression"))
			strict_compression = 1;
		else if (!strcmp(argv[i], "--no-mtime"))
			no_mtime = 1;
		else if (!strcmp(argv[i], "--exclude") && i + 1 < argc && n_excluded < 8)
			excluded[n_excluded++] = argv[++i];
		else {
			fprintf(stderr, "pool-manifest: unknown option %s\n", argv[i]);
			return 2;
		}
	}
	if (i + 1 != argc) {
		fprintf(stderr, "usage: pool-manifest [--strict-compression] [--no-mtime] [--exclude TOPLEVEL]... ROOT\n");
		return 2;
	}
	char *roots[] = {argv[i], NULL};
	size_t root_len = strlen(argv[i]);
	while (root_len > 1 && argv[i][root_len - 1] == '/')
		root_len--;
	FTS *fts = fts_open(roots, FTS_PHYSICAL | FTS_XDEV | FTS_NOCHDIR, by_name);
	if (!fts) {
		perror("fts_open");
		return 1;
	}
	CC_SHA256_Init(&whole);
	FTSENT *e;
	while ((e = fts_read(fts))) {
		if (e->fts_info == FTS_DP)
			continue;
		if (e->fts_info == FTS_DNR || e->fts_info == FTS_ERR || e->fts_info == FTS_NS) {
			fprintf(stderr, "pool-manifest: %s: %s\n", e->fts_path, strerror(e->fts_errno));
			n_errors++;
			continue;
		}
		if (e->fts_level == 1) {
			int skip = 0;
			for (int k = 0; k < n_excluded; k++)
				if (!strcmp(e->fts_name, excluded[k]))
					skip = 1;
			if (skip) {
				if (e->fts_info == FTS_D)
					fts_set(fts, e, FTS_SKIP);
				continue;
			}
		}
		const struct stat *st = e->fts_statp;
		const char *rel = e->fts_level == 0 ? "." : e->fts_path + root_len + 1;
		emit(e->fts_accpath, e->fts_path, rel, st, NULL);
	}
	fts_close(fts);
	unsigned char d[32];
	char h[65];
	CC_SHA256_Final(d, &whole);
	hex(d, 32, h);
	printf("# entries %llu files %llu bytes %llu sha256 %s errors %d\n", n_entries, n_files, n_bytes, h, n_errors);
	return n_errors ? 1 : 0;
}
