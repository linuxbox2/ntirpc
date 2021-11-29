/* Left-leaning red/black trees */

#ifndef _OPR_RBTREE_H
#define _OPR_RBTREE_H 1

#include <stdbool.h>
#include <stdint.h>
#include <misc/opr.h>

struct opr_rbtree_node {
	struct opr_rbtree_node *left;
	struct opr_rbtree_node *right;
	struct opr_rbtree_node *parent;
	uint32_t red;
	uint32_t gen;		/* generation number */
};

typedef int (*opr_rbtree_cmpf_t) (const struct opr_rbtree_node *lhs,
				  const struct opr_rbtree_node *rhs);

struct opr_rbtree {
	struct opr_rbtree_node *root;
	opr_rbtree_cmpf_t cmpf;
	uint64_t size;
	uint64_t gen;		/* generation number */
};

extern void opr_rbtree_init(struct opr_rbtree *head, opr_rbtree_cmpf_t cmpf);
extern struct opr_rbtree_node *opr_rbtree_first(struct opr_rbtree *head);
extern struct opr_rbtree_node *opr_rbtree_last(struct opr_rbtree *head);
extern struct opr_rbtree_node *opr_rbtree_next(struct opr_rbtree_node *node);
extern struct opr_rbtree_node *opr_rbtree_prev(struct opr_rbtree_node *node);
extern struct opr_rbtree_node *opr_rbtree_lookup(struct opr_rbtree *head,
						 struct opr_rbtree_node *node);
extern struct opr_rbtree_node *opr_rbtree_insert(struct opr_rbtree *head,
						 struct opr_rbtree_node *node);
extern void opr_rbtree_insert_at(struct opr_rbtree *head,
				 struct opr_rbtree_node *parent,
				 struct opr_rbtree_node **childptr,
				 struct opr_rbtree_node *node);
extern void opr_rbtree_remove(struct opr_rbtree *head,
			      struct opr_rbtree_node *node);
extern void opr_rbtree_replace(struct opr_rbtree *head,
			       struct opr_rbtree_node *old,
			       struct opr_rbtree_node *replacement);

static inline bool opr_rbtree_node_valid(struct opr_rbtree_node *node)
{
	return (node->gen != 0);
}

static inline unsigned long opr_rbtree_size(struct opr_rbtree *head)
{
	return (head->size);
}

#endif				/* _OPR_RBTREE_H */
