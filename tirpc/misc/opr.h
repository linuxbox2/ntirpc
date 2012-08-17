#ifndef OPR_H
#define OPR_H 1

#define opr_containerof(ptr, structure, member) \
   ((structure *)((char *)(ptr)-(char *)(&((structure *)NULL)->member)))

#endif /* OPR_H */
