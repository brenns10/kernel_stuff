/*
 * Reproducer for assoc_array_gc issue. Compile and run with no special args,
 * but be sure to compile with the "construct_array.c" file in the same
 * directory.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

//////////////////// Declare u8 as in kernel
typedef uint8_t u8;

//////////////////// Declarations from assoc_array headers (and some deps)
// These are copied verbatim, with most comments removed.

struct callback_head {
	struct callback_head *next;
	void (*func)(struct callback_head *head);
} __attribute__((aligned(sizeof(void *))));
#define rcu_head callback_head

#define ASSOC_ARRAY_FAN_OUT		16	/* Number of slots per node */
#define ASSOC_ARRAY_FAN_MASK		(ASSOC_ARRAY_FAN_OUT - 1)
#define ASSOC_ARRAY_LEVEL_STEP		4	/* log2(16) */
#define ASSOC_ARRAY_LEVEL_STEP_MASK	(ASSOC_ARRAY_LEVEL_STEP - 1)
#define ASSOC_ARRAY_KEY_CHUNK_MASK	(ASSOC_ARRAY_KEY_CHUNK_SIZE - 1)
#define ASSOC_ARRAY_KEY_CHUNK_SHIFT	6	/* log2(bits per long, i.e. 64) */

struct assoc_array_ptr;

struct assoc_array_node {
	struct assoc_array_ptr	*back_pointer;
	u8			parent_slot;
	struct assoc_array_ptr	*slots[ASSOC_ARRAY_FAN_OUT];
	unsigned long		nr_leaves_on_branch;
};

struct assoc_array_shortcut {
	struct assoc_array_ptr	*back_pointer;
	int			parent_slot;
	int			skip_to_level;
	struct assoc_array_ptr	*next_node;
	unsigned long		index_key[];
};

struct assoc_array {
	struct assoc_array_ptr	*root;		/* The node at the root of the tree */
	unsigned long		nr_leaves_on_tree;
};

struct assoc_array_ops {
	unsigned long (*get_key_chunk)(const void *index_key, int level);
	unsigned long (*get_object_key_chunk)(const void *object, int level);
	bool (*compare_object)(const void *object, const void *index_key);
	int (*diff_objects)(const void *object, const void *index_key);
	void (*free_object)(void *object);
};

struct assoc_array_edit {
	struct rcu_head			rcu;
	struct assoc_array		*array;
	const struct assoc_array_ops	*ops;
	const struct assoc_array_ops	*ops_for_excised_subtree;
	struct assoc_array_ptr		*leaf;
	struct assoc_array_ptr		**leaf_p;
	struct assoc_array_ptr		*dead_leaf;
	struct assoc_array_ptr		*new_meta[3];
	struct assoc_array_ptr		*excised_meta[1];
	struct assoc_array_ptr		*excised_subtree;
	struct assoc_array_ptr		**set_backpointers[ASSOC_ARRAY_FAN_OUT];
	struct assoc_array_ptr		*set_backpointers_to;
	struct assoc_array_node		*adjust_count_on;
	long				adjust_count_by;
	struct {
		struct assoc_array_ptr	**ptr;
		struct assoc_array_ptr	*to;
	} set[2];
	struct {
		u8			*p;
		u8			to;
 	} set_parent_slot[1];
 	u8				segment_cache[ASSOC_ARRAY_FAN_OUT + 1];
 };

#define ASSOC_ARRAY_PTR_TYPE_MASK 0x1UL
#define ASSOC_ARRAY_PTR_LEAF_TYPE 0x0UL	/* Points to leaf (or nowhere) */
#define ASSOC_ARRAY_PTR_META_TYPE 0x1UL	/* Points to node or shortcut */
#define ASSOC_ARRAY_PTR_SUBTYPE_MASK	0x2UL
#define ASSOC_ARRAY_PTR_NODE_SUBTYPE	0x0UL
#define ASSOC_ARRAY_PTR_SHORTCUT_SUBTYPE 0x2UL

static inline bool assoc_array_ptr_is_meta(const struct assoc_array_ptr *x)
{
	return (unsigned long)x & ASSOC_ARRAY_PTR_TYPE_MASK;
}
static inline bool assoc_array_ptr_is_leaf(const struct assoc_array_ptr *x)
{
	return !assoc_array_ptr_is_meta(x);
}
static inline bool assoc_array_ptr_is_shortcut(const struct assoc_array_ptr *x)
{
	return (unsigned long)x & ASSOC_ARRAY_PTR_SUBTYPE_MASK;
}
static inline bool assoc_array_ptr_is_node(const struct assoc_array_ptr *x)
{
	return !assoc_array_ptr_is_shortcut(x);
}

static inline void *assoc_array_ptr_to_leaf(const struct assoc_array_ptr *x)
{
	return (void *)((unsigned long)x & ~ASSOC_ARRAY_PTR_TYPE_MASK);
}

static inline
unsigned long __assoc_array_ptr_to_meta(const struct assoc_array_ptr *x)
{
	return (unsigned long)x &
		~(ASSOC_ARRAY_PTR_SUBTYPE_MASK | ASSOC_ARRAY_PTR_TYPE_MASK);
}
static inline
struct assoc_array_node *assoc_array_ptr_to_node(const struct assoc_array_ptr *x)
{
	return (struct assoc_array_node *)__assoc_array_ptr_to_meta(x);
}
static inline
struct assoc_array_shortcut *assoc_array_ptr_to_shortcut(const struct assoc_array_ptr *x)
{
	return (struct assoc_array_shortcut *)__assoc_array_ptr_to_meta(x);
}

static inline
struct assoc_array_ptr *__assoc_array_x_to_ptr(const void *p, unsigned long t)
{
	return (struct assoc_array_ptr *)((unsigned long)p | t);
}
//static inline
//struct assoc_array_ptr *assoc_array_leaf_to_ptr(const void *p)
//{
//	return __assoc_array_x_to_ptr(p, ASSOC_ARRAY_PTR_LEAF_TYPE);
//}
static inline
struct assoc_array_ptr *assoc_array_node_to_ptr(const struct assoc_array_node *p)
{
	return __assoc_array_x_to_ptr(
		p, ASSOC_ARRAY_PTR_META_TYPE | ASSOC_ARRAY_PTR_NODE_SUBTYPE);
}
static inline
struct assoc_array_ptr *assoc_array_shortcut_to_ptr(const struct assoc_array_shortcut *p)
{
	return __assoc_array_x_to_ptr(
		p, ASSOC_ARRAY_PTR_META_TYPE | ASSOC_ARRAY_PTR_SHORTCUT_SUBTYPE);
}

#define ASSOC_ARRAY_KEY_CHUNK_SIZE 64 /* Key data retrieved in chunks of this size */

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)

//////////////////// SIMPLE KERNEL FUNCTION/MACRO REPLACEMENTS

#define pr_devel(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define GFP_KERNEL 0
#define kzalloc(size, flags) calloc(1, size)
#define kfree free
#define kmalloc(size, flags) malloc(size)

#define BUG_ON(condition) assert(!(condition))

//////////////////// Stubbed versions of assoc_array helpers

static void assoc_array_destroy_subtree(struct assoc_array_ptr *root,
					const struct assoc_array_ops *ops)
{
	(void)root;
	(void)ops;
	fprintf(stderr, "Destroy subtree (error)\n");
}
void assoc_array_apply_edit(struct assoc_array_edit *edit)
{
	printf("Apply edit!\n");
	edit->array->root = edit->set[0].to;
}

//////////////////// UNIT UNDER TEST
// Note that there is one alteration to the code. I added a special-cased printf
// to help identify a broken node from my core dump. It is annotated below as
// "MODIFIED".

int assoc_array_gc(struct assoc_array *array,
		   const struct assoc_array_ops *ops,
		   bool (*iterator)(void *object, void *iterator_data),
		   void *iterator_data)
{
	struct assoc_array_shortcut *shortcut, *new_s;
	struct assoc_array_node *node, *new_n;
	struct assoc_array_edit *edit;
	struct assoc_array_ptr *cursor, *ptr;
	struct assoc_array_ptr *new_root, *new_parent, **new_ptr_pp;
	unsigned long nr_leaves_on_tree;
	int keylen, slot, nr_free, next_slot, i;
	bool retained;

	pr_devel("-->%s()\n", __func__);

	if (!array->root)
		return 0;

	edit = kzalloc(sizeof(struct assoc_array_edit), GFP_KERNEL);
	if (!edit)
		return -ENOMEM;
	edit->array = array;
	edit->ops = ops;
	edit->ops_for_excised_subtree = ops;
	edit->set[0].ptr = &array->root;
	edit->excised_subtree = array->root;

	new_root = new_parent = NULL;
	new_ptr_pp = &new_root;
	cursor = array->root;

descend:
	/* If this point is a shortcut, then we need to duplicate it and
	 * advance the target cursor.
	 */
	if (assoc_array_ptr_is_shortcut(cursor)) {
		shortcut = assoc_array_ptr_to_shortcut(cursor);
		keylen = round_up(shortcut->skip_to_level, ASSOC_ARRAY_KEY_CHUNK_SIZE);
		keylen >>= ASSOC_ARRAY_KEY_CHUNK_SHIFT;
		new_s = kmalloc(sizeof(struct assoc_array_shortcut) +
				keylen * sizeof(unsigned long), GFP_KERNEL);
		if (!new_s)
			goto enomem;
		pr_devel("dup shortcut %p -> %p\n", shortcut, new_s);
		memcpy(new_s, shortcut, (sizeof(struct assoc_array_shortcut) +
					 keylen * sizeof(unsigned long)));
		new_s->back_pointer = new_parent;
		new_s->parent_slot = shortcut->parent_slot;
		*new_ptr_pp = new_parent = assoc_array_shortcut_to_ptr(new_s);
		new_ptr_pp = &new_s->next_node;
		cursor = shortcut->next_node;
	}

	/* Duplicate the node at this position */
	node = assoc_array_ptr_to_node(cursor);
	new_n = kzalloc(sizeof(struct assoc_array_node), GFP_KERNEL);
	if (!new_n)
		goto enomem;
	pr_devel("dup node %p -> %p\n", node, new_n);
	new_n->back_pointer = new_parent;
	new_n->parent_slot = node->parent_slot;
	*new_ptr_pp = new_parent = assoc_array_node_to_ptr(new_n);
	new_ptr_pp = NULL;
	slot = 0;

continue_node:
	/* Filter across any leaves and gc any subtrees */
	for (; slot < ASSOC_ARRAY_FAN_OUT; slot++) {
		// MODIFIED: (this if statement is added for debugging)
		if (slot == 9 && ((uintptr_t) node | 1) == (uintptr_t)array->root) {
			printf("ENTERING PROBLEM NODE\n");
		}
		ptr = node->slots[slot];
		if (!ptr)
			continue;

		if (assoc_array_ptr_is_leaf(ptr)) {
			if (iterator(assoc_array_ptr_to_leaf(ptr),
				     iterator_data))
				/* The iterator will have done any reference
				 * counting on the object for us.
				 */
				new_n->slots[slot] = ptr;
			continue;
		}

		new_ptr_pp = &new_n->slots[slot];
		cursor = ptr;
		goto descend;
	}

retry_compress:
	pr_devel("-- compress node %p --\n", new_n);

	/* Count up the number of empty slots in this node and work out the
	 * subtree leaf count.
	 */
	new_n->nr_leaves_on_branch = 0;
	nr_free = 0;
	for (slot = 0; slot < ASSOC_ARRAY_FAN_OUT; slot++) {
		ptr = new_n->slots[slot];
		if (!ptr)
			nr_free++;
		else if (assoc_array_ptr_is_leaf(ptr))
			new_n->nr_leaves_on_branch++;
	}
	pr_devel("free=%d, leaves=%lu\n", nr_free, new_n->nr_leaves_on_branch);

	/* See what we can fold in */
	next_slot = 0;
	retained = false;
	for (slot = 0; slot < ASSOC_ARRAY_FAN_OUT; slot++) {
		struct assoc_array_shortcut *s;
		struct assoc_array_node *child;

		ptr = new_n->slots[slot];
		if (!ptr || assoc_array_ptr_is_leaf(ptr))
			continue;

		s = NULL;
		if (assoc_array_ptr_is_shortcut(ptr)) {
			s = assoc_array_ptr_to_shortcut(ptr);
			ptr = s->next_node;
		}

		child = assoc_array_ptr_to_node(ptr);
		new_n->nr_leaves_on_branch += child->nr_leaves_on_branch;

		if (child->nr_leaves_on_branch <= nr_free + 1) {
			/* Fold the child node into this one */
			pr_devel("[%d] fold node %lu/%d [nx %d]\n",
				 slot, child->nr_leaves_on_branch, nr_free + 1,
				 next_slot);

			/* We would already have reaped an intervening shortcut
			 * on the way back up the tree.
			 */
			BUG_ON(s);

			new_n->slots[slot] = NULL;
			nr_free++;
			if (slot < next_slot)
				next_slot = slot;
			for (i = 0; i < ASSOC_ARRAY_FAN_OUT; i++) {
				struct assoc_array_ptr *p = child->slots[i];
				if (!p)
					continue;
				BUG_ON(assoc_array_ptr_is_meta(p));
				while (new_n->slots[next_slot])
					next_slot++;
				BUG_ON(next_slot >= ASSOC_ARRAY_FAN_OUT);
				new_n->slots[next_slot++] = p;
				nr_free--;
			}
			kfree(child);
		} else {
			pr_devel("[%d] retain node %lu/%d [nx %d]\n",
				 slot, child->nr_leaves_on_branch, nr_free + 1,
				 next_slot);
			retained = true;
		}
	}

	if (retained && new_n->nr_leaves_on_branch < ASSOC_ARRAY_FAN_OUT) {
		pr_devel("internal nodes remain despite neough space, retrying\n");
		goto retry_compress;
	}

	pr_devel("after: %lu\n", new_n->nr_leaves_on_branch);

	nr_leaves_on_tree = new_n->nr_leaves_on_branch;

	/* Excise this node if it is singly occupied by a shortcut */
	if (nr_free == ASSOC_ARRAY_FAN_OUT - 1) {
		for (slot = 0; slot < ASSOC_ARRAY_FAN_OUT; slot++)
			if ((ptr = new_n->slots[slot]))
				break;

		if (assoc_array_ptr_is_meta(ptr) &&
		    assoc_array_ptr_is_shortcut(ptr)) {
			pr_devel("excise node %p with 1 shortcut\n", new_n);
			new_s = assoc_array_ptr_to_shortcut(ptr);
			new_parent = new_n->back_pointer;
			slot = new_n->parent_slot;
			kfree(new_n);
			if (!new_parent) {
				new_s->back_pointer = NULL;
				new_s->parent_slot = 0;
				new_root = ptr;
				goto gc_complete;
			}

			if (assoc_array_ptr_is_shortcut(new_parent)) {
				/* We can discard any preceding shortcut also */
				struct assoc_array_shortcut *s =
					assoc_array_ptr_to_shortcut(new_parent);

				pr_devel("excise preceding shortcut\n");

				new_parent = new_s->back_pointer = s->back_pointer;
				slot = new_s->parent_slot = s->parent_slot;
				kfree(s);
				if (!new_parent) {
					new_s->back_pointer = NULL;
					new_s->parent_slot = 0;
					new_root = ptr;
					goto gc_complete;
				}
			}

			new_s->back_pointer = new_parent;
			new_s->parent_slot = slot;
			new_n = assoc_array_ptr_to_node(new_parent);
			new_n->slots[slot] = ptr;
			goto ascend_old_tree;
		}
	}

	/* Excise any shortcuts we might encounter that point to nodes that
	 * only contain leaves.
	 */
	ptr = new_n->back_pointer;
	if (!ptr)
		goto gc_complete;

	if (assoc_array_ptr_is_shortcut(ptr)) {
		new_s = assoc_array_ptr_to_shortcut(ptr);
		new_parent = new_s->back_pointer;
		slot = new_s->parent_slot;

		if (new_n->nr_leaves_on_branch <= ASSOC_ARRAY_FAN_OUT) {
			struct assoc_array_node *n;

			pr_devel("excise shortcut\n");
			new_n->back_pointer = new_parent;
			new_n->parent_slot = slot;
			kfree(new_s);
			if (!new_parent) {
				new_root = assoc_array_node_to_ptr(new_n);
				goto gc_complete;
			}

			n = assoc_array_ptr_to_node(new_parent);
			n->slots[slot] = assoc_array_node_to_ptr(new_n);
		}
	} else {
		new_parent = ptr;
	}
	new_n = assoc_array_ptr_to_node(new_parent);

ascend_old_tree:
	ptr = node->back_pointer;
	if (assoc_array_ptr_is_shortcut(ptr)) {
		shortcut = assoc_array_ptr_to_shortcut(ptr);
		slot = shortcut->parent_slot;
		cursor = shortcut->back_pointer;
		if (!cursor)
			goto gc_complete;
	} else {
		slot = node->parent_slot;
		cursor = ptr;
	}
	BUG_ON(!cursor);
	node = assoc_array_ptr_to_node(cursor);
	slot++;
	goto continue_node;

gc_complete:
	edit->set[0].to = new_root;
	assoc_array_apply_edit(edit);
	array->nr_leaves_on_tree = nr_leaves_on_tree;
	return 0;

enomem:
	pr_devel("enomem\n");
	assoc_array_destroy_subtree(new_root, edit->ops);
	kfree(edit);
	return -ENOMEM;
}

//////////////////// Printing utilities

void prefix(int level, int index)
{
	for (int i = 0; i < level; i++)
		fputs("   ", stdout);
	if (level)
		printf("[%x] ", index);
}

void assoc_array_print_ptr(struct assoc_array_ptr *ptr, int level, int index)
{
	if (!ptr)
		return;
	prefix(level, index);
	if (!assoc_array_ptr_is_meta(ptr)) {
		printf("LEAF: %p\n", ptr);
	} else if (assoc_array_ptr_is_node(ptr)) {
		struct assoc_array_node *node = assoc_array_ptr_to_node(ptr);
		printf("NODE: %p\n", node);
		for (int i = 0; i < ASSOC_ARRAY_FAN_OUT; i++)
			assoc_array_print_ptr(node->slots[i], level + 1, i);
	} else {
		printf("SHORTCUT: not implemented\n");
		assert(false);
	}
}

void assoc_array_print(struct assoc_array *array)
{
	assoc_array_print_ptr(array->root, 0, 0);
}

//////////////////// ARRAY CREATION HELPERS (used by generated code)

struct assoc_array_node *mknode()
{
	return calloc(1, sizeof(struct assoc_array_node));
}

void set_data(struct assoc_array_node *node, u8 parent_slot, unsigned long nr_leaves_on_branch)
{
	node->parent_slot = parent_slot;
	node->nr_leaves_on_branch = nr_leaves_on_branch;
}

void set_node(struct assoc_array_node *parent, int index)
{
	struct assoc_array_node *child = mknode();
	parent->slots[index] = assoc_array_node_to_ptr(child);
	child->back_pointer = assoc_array_node_to_ptr(parent);
}

void set_leaf(struct assoc_array_node *parent, int index, uintptr_t data)
{
	parent->slots[index] = (void *)data;
}

struct assoc_array_node *get_node(struct assoc_array_node *parent, int index)
{
	struct assoc_array_ptr *ptr = parent->slots[index];
	assert(assoc_array_ptr_is_node(ptr));
	return assoc_array_ptr_to_node(ptr);
}

struct assoc_array_node *get_parent(struct assoc_array_node *node)
{
	struct assoc_array_ptr *ptr = node->back_pointer;
	assert(assoc_array_ptr_is_node(ptr));
	return assoc_array_ptr_to_node(ptr);
}

//////////////////// TEST CODE

/**
 * Creates the userspace array based on the construct_array.c output.
 */
struct assoc_array *make_array(void)
{
	struct assoc_array *array = calloc(1, sizeof(*array));
	struct assoc_array_node *node;

#include "construct_array.c"

	return array;
}

#define nelem(arr) (sizeof(arr) / sizeof(arr[0]))

/**
 * By looking into the core dump at the nearly completed new tree, I was able to
 * see exactly which leaf objects were kept by the iterator, and which ones were
 * removed. This iterator is coded to simply return true for whichever objects
 * the original one retained.
 */
bool my_iterator(void *object, void *iterator_data)
{
	void *datas_to_keep[] = {
		(void*)0xffff88bab100fa00,
		(void*)0xffff88bd8d224e00,
		(void*)0xffff88bcdb35fe00,
		(void*)0xffff88b100e37600,
		(void*)0xffff887b0cd30400,
		(void*)0xffff88b1c0c7fe00,
		(void*)0xffff88bd5ed59c00,
		(void*)0xffff88b1c0db3f00,
		(void*)0xffff887b0cd30c00,
		(void*)0xffff887b0cd30000,
		(void*)0xffff887b0cd31c00,
		(void*)0xffff887b0cd31300,
		(void*)0xffff887b0cd31a00,
		(void*)0xffff887a71cbf000,
		(void*)0xffff8875250cdf00,
	};

	for (int i = 0; i < nelem(datas_to_keep); i++)
		if (datas_to_keep[i] == object)
			return true;
	return false;
}

/**
 * Simple test: make the array, print it, garbage collect, and then print again.
 */
int main(int argc, char **argv)
{
	struct assoc_array *array = make_array();
	printf("Before GC:\n");
	assoc_array_print(array);
	printf("Running GC...\n");
	assoc_array_gc(array, NULL, my_iterator, NULL);
	printf("After GC:\n");
	assoc_array_print(array);
}
