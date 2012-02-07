
#ifndef _RBTREE_X_H
#define _RBTREE_X_H

#include <misc/rbtree.h>

struct rbtree_x_part
{
    rwlock_t lock;
    struct opr_rbtree t;
};

struct rbtree_x
{
    uint32_t npart;
    struct rbtree_x_part *tree;
};

#define RBT_X_FLAG_NONE  0x0000
#define RBT_X_FLAG_ALLOC 0x0001

#define rbtx_partition_of_scalar(xt, k) (((xt)->tree)+((k)%(xt)->npart))

extern int rbtx_init(struct rbtree_x *xt, opr_rbtree_cmpf_t cmpf,
                     uint32_t npart, uint32_t flags);

#endif /* _RBTREE_X_H */
