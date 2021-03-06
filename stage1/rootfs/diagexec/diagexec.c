#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "elf.h"

static int exit_err;
#define exit_if(_cond, _fmt, _args...)				\
	exit_err++;						\
	if(_cond) {						\
		fprintf(stderr, "Error: " _fmt "\n", ##_args);	\
		exit(exit_err);					\
	}
#define pexit_if(_cond, _fmt, _args...)				\
	exit_if(_cond, _fmt ": %s", ##_args, strerror(errno))

#define MAX_DIAG_DEPTH 10
#define MIN(_a, _b) (((_a) < (_b)) ? (_a) : (_b))

static void diag(const char *exe)
{
	static const uint8_t	elf[] = {0x7f, 'E', 'L', 'F'};
	static const uint8_t	shebang[] = {'#','!'};
	static int		diag_depth;
	int			fd;
	struct stat		st;
	const uint8_t		*mm;
	const char		*itrp = NULL;

	pexit_if((fd = open(exe, O_RDONLY)) == -1,
		"Unable to open \"%s\"", exe);
	pexit_if(fstat(fd, &st) == -1,
		"Cannot stat \"%s\"", exe);
	exit_if(!S_ISREG(st.st_mode), "\"%s\" is not a regular file", exe);
	exit_if(!((S_IXUSR|S_IXGRP|S_IXOTH) & st.st_mode),
		"\"%s\" is not executable", exe)
	pexit_if(!(mm = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0)),
		"Mmap of \"%s\" failed", exe);

	if(st.st_size >= sizeof(shebang) &&
	   !memcmp(mm, shebang, sizeof(shebang))) {
		const uint8_t	*nl;
		int		maxlen = MIN(PATH_MAX, st.st_size - sizeof(shebang));
		/* TODO(vc): EOF-terminated shebang lines are technically possible */
		exit_if(!(nl = memchr(&mm[sizeof(shebang)], '\n', maxlen)),
			"Shebang line too long");
		pexit_if(!(itrp = strndup((char *)&mm[sizeof(shebang)], (nl - mm) - 2)),
			"Failed to dup interpreter path");
	} else if(st.st_size >= sizeof(elf) &&
		  !memcmp(mm, elf, sizeof(elf))) {
		uint64_t	(*lget)(const uint8_t *) = NULL;
		uint32_t	(*iget)(const uint8_t *) = NULL;
		uint16_t	(*sget)(const uint8_t *) = NULL;
		const void	*phoff = NULL, *phesz = NULL, *phecnt = NULL;
		const uint8_t	*ph = NULL;
		int		i, phreloff, phrelsz;

		exit_if(mm[ELF_VERSION] != 1,
			"Unsupported ELF version: %hhx", mm[ELF_VERSION]);

		/* determine which accessors to use and where */
		if(mm[ELF_BITS] == ELF_BITS_32) {
			if(mm[ELF_ENDIAN] == ELF_ENDIAN_LITL) {
				lget = le32_lget;
				sget = le_sget;
				iget = le_iget;
			} else if(mm[ELF_ENDIAN] == ELF_ENDIAN_BIG) {
				lget = be32_lget;
				sget = be_sget;
				iget = be_iget;
			}
			phoff = &mm[ELF32_PHT_OFF];
			phesz = &mm[ELF32_PHTE_SIZE];
			phecnt = &mm[ELF32_PHTE_CNT];
			phreloff = ELF32_PHE_OFF;
			phrelsz = ELF32_PHE_SIZE;
		} else if(mm[ELF_BITS] == ELF_BITS_64) {
			if(mm[ELF_ENDIAN] == ELF_ENDIAN_LITL) {
				lget = le64_lget;
				sget = le_sget;
				iget = le_iget;
			} else if(mm[ELF_ENDIAN] == ELF_ENDIAN_BIG) {
				lget = be64_lget;
				sget = be_sget;
				iget = be_iget;
			}
			phoff = &mm[ELF64_PHT_OFF];
			phesz = &mm[ELF64_PHTE_SIZE];
			phecnt = &mm[ELF64_PHTE_CNT];
			phreloff = ELF64_PHE_OFF;
			phrelsz = ELF64_PHE_SIZE;
		}

		exit_if(!lget, "Unsupported ELF format");

		if(!phoff) /* program header may be absent, don't make it an error */
			return;

		/* TODO(vc): sanity checks on values before using them */
		for(ph = &mm[lget(phoff)], i = 0; i < sget(phecnt); i++, ph += sget(phesz)) {
			if(iget(ph) == ELF_PT_INTERP) {
				itrp = strndup((char *)&mm[lget(&ph[phreloff])], lget(&ph[phrelsz]));
				break;
			}
		}
	} else {
		exit_if(1, "Unsupported file type");
	}

	exit_if(!itrp, "Unable to determine interpreter for \"%s\"", exe);
	exit_if(*itrp != '/', "Path must be absolute: \"%s\"", itrp);
	exit_if(++diag_depth > MAX_DIAG_DEPTH,
		"Excessive interpreter recursion, giving up");
	diag(itrp);
}

int main(int argc, char *argv[])
{
	const char *root, *exe;
	exit_if(argc < 3,
		"Usage: %s /path/to/root /to/exec [args ...]", argv[0]);
	root = argv[1];
	exe = argv[2];
	pexit_if(chroot(root) == -1, "Chroot failed");
	pexit_if(chdir("/") == -1, "Chdir failed");
	pexit_if(execv(exe, &argv[2]) == -1 &&
		 errno != ENOENT && errno != EACCES,
		 "Exec of \"%s\" failed", exe);
	diag(exe);

	return EXIT_FAILURE;
}
