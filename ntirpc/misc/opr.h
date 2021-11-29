#ifndef OPR_H
#define OPR_H 1

#define opr_containerof(ptr, structure, member) \
	((structure *)((char *)(ptr)-(char *)offsetof(structure, member)))

#endif				/* OPR_H */
