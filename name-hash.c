/*
 * name-hash.c
 *
 * Hashing names in the index state
 *
 * Copyright (C) 2008 Linus Torvalds
 */
#define NO_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"

struct dir_entry {
	struct hashmap_entry ent;
	struct dir_entry *parent;
	int nr;
	unsigned int namelen;
	char name[FLEX_ARRAY];
};

static int dir_entry_cmp(const struct dir_entry *e1,
		const struct dir_entry *e2, const char *name)
{
	return e1->namelen != e2->namelen || strncasecmp(e1->name,
			name ? name : e2->name, e1->namelen);
}

static struct dir_entry *find_dir_entry__hash(struct index_state *istate,
		const char *name, unsigned int namelen, unsigned int hash)
{
	struct dir_entry key;
	hashmap_entry_init(&key, hash);
	key.namelen = namelen;
	return hashmap_get(&istate->dir_hash, &key, name);
}

static struct dir_entry *find_dir_entry(struct index_state *istate,
		const char *name, unsigned int namelen)
{
	return find_dir_entry__hash(istate, name, namelen, memihash(name,namelen));
}

static struct dir_entry *hash_dir_entry(struct index_state *istate,
		struct cache_entry *ce, int namelen, struct dir_entry **p_previous_dir)
{
	/*
	 * Throw each directory component in the hash for quick lookup
	 * during a git status. Directory components are stored without their
	 * closing slash.  Despite submodules being a directory, they never
	 * reach this point, because they are stored
	 * in index_state.name_hash (as ordinary cache_entries).
	 */
	struct dir_entry *dir;
	unsigned int hash;
	int use_precomputed_dir_hash = 0;

	if (ce->precompute_hash_state & CE_PRECOMPUTE_HASH_STATE__SET) {
		if (!(ce->precompute_hash_state & CE_PRECOMPUTE_HASH_STATE__DIR))
			return NULL; /* item does not have a parent directory */
		if (namelen == ce_namelen(ce)) {
			/* dir hash only valid for outer-most call (not recursive ones) */
			use_precomputed_dir_hash = 1;
			hash = ce->precompute_hash_dir;
		}
	}

	/* get length of parent directory */
	while (namelen > 0 && !is_dir_sep(ce->name[namelen - 1]))
		namelen--;
	if (namelen <= 0)
		return NULL;
	namelen--;

	/* lookup existing entry for that directory */
	if (p_previous_dir && *p_previous_dir
		&& namelen == (*p_previous_dir)->namelen
		&& memcmp(ce->name, (*p_previous_dir)->name, namelen) == 0) {
		/*
		 * When our caller is sequentially iterating thru the index,
		 * items in the same directory will be sequential, and therefore
		 * refer to the same dir_entry.
		 */
		dir = *p_previous_dir;
	} else {
		if (!use_precomputed_dir_hash)
			hash = memihash(ce->name, namelen);
		dir = find_dir_entry__hash(istate, ce->name, namelen, hash);
	}

	if (!dir) {
		/* not found, create it and add to hash table */
		FLEX_ALLOC_MEM(dir, name, ce->name, namelen);
		hashmap_entry_init(dir, hash);
		dir->namelen = namelen;
		hashmap_add(&istate->dir_hash, dir);

		/* recursively add missing parent directories */
		dir->parent = hash_dir_entry(istate, ce, namelen, NULL);
	}

	if (p_previous_dir)
		*p_previous_dir = dir;

	return dir;
}

static void add_dir_entry(struct index_state *istate, struct cache_entry *ce,
	struct dir_entry **p_previous_dir)
{
	/* Add reference to the directory entry (and parents if 0). */
	struct dir_entry *dir = hash_dir_entry(istate, ce, ce_namelen(ce), p_previous_dir);
	while (dir && !(dir->nr++))
		dir = dir->parent;
}

static void remove_dir_entry(struct index_state *istate, struct cache_entry *ce)
{
	/*
	 * Release reference to the directory entry. If 0, remove and continue
	 * with parent directory.
	 */
	struct dir_entry *dir = hash_dir_entry(istate, ce, ce_namelen(ce), NULL);
	while (dir && !(--dir->nr)) {
		struct dir_entry *parent = dir->parent;
		hashmap_remove(&istate->dir_hash, dir, NULL);
		free(dir);
		dir = parent;
	}
}

static void hash_index_entry(struct index_state *istate, struct cache_entry *ce,
	struct dir_entry **p_previous_dir)
{
	unsigned int h;

	if (ce->ce_flags & CE_HASHED)
		return;
	ce->ce_flags |= CE_HASHED;

	if (ce->precompute_hash_state & CE_PRECOMPUTE_HASH_STATE__SET)
		h = ce->precompute_hash_name;
	else
		h = memihash(ce->name, ce_namelen(ce));

	hashmap_entry_init(ce, h);
	hashmap_add(&istate->name_hash, ce);

	if (ignore_case)
		add_dir_entry(istate, ce, p_previous_dir);
}

static int cache_entry_cmp(const struct cache_entry *ce1,
		const struct cache_entry *ce2, const void *remove)
{
	/*
	 * For remove_name_hash, find the exact entry (pointer equality); for
	 * index_file_exists, find all entries with matching hash code and
	 * decide whether the entry matches in same_name.
	 */
	return remove ? !(ce1 == ce2) : 0;
}

static void lazy_init_name_hash(struct index_state *istate)
{
	struct dir_entry *previous_dir = NULL;
	int nr;

	if (istate->name_hash_initialized)
		return;
	hashmap_init(&istate->name_hash, (hashmap_cmp_fn) cache_entry_cmp,
			istate->cache_nr);
	hashmap_init(&istate->dir_hash, (hashmap_cmp_fn) dir_entry_cmp,
			istate->cache_nr);
	for (nr = 0; nr < istate->cache_nr; nr++)
		hash_index_entry(istate, istate->cache[nr], &previous_dir);
	istate->name_hash_initialized = 1;
}

void add_name_hash(struct index_state *istate, struct cache_entry *ce)
{
	if (istate->name_hash_initialized)
		hash_index_entry(istate, ce, NULL);
}

void remove_name_hash(struct index_state *istate, struct cache_entry *ce)
{
	if (!istate->name_hash_initialized || !(ce->ce_flags & CE_HASHED))
		return;
	ce->ce_flags &= ~CE_HASHED;
	hashmap_remove(&istate->name_hash, ce, ce);

	if (ignore_case)
		remove_dir_entry(istate, ce);
}

static int slow_same_name(const char *name1, int len1, const char *name2, int len2)
{
	if (len1 != len2)
		return 0;

	while (len1) {
		unsigned char c1 = *name1++;
		unsigned char c2 = *name2++;
		len1--;
		if (c1 != c2) {
			c1 = toupper(c1);
			c2 = toupper(c2);
			if (c1 != c2)
				return 0;
		}
	}
	return 1;
}

static int same_name(const struct cache_entry *ce, const char *name, int namelen, int icase)
{
	int len = ce_namelen(ce);

	/*
	 * Always do exact compare, even if we want a case-ignoring comparison;
	 * we do the quick exact one first, because it will be the common case.
	 */
	if (len == namelen && !memcmp(name, ce->name, len))
		return 1;

	if (!icase)
		return 0;

	return slow_same_name(name, namelen, ce->name, len);
}

int index_dir_exists(struct index_state *istate, const char *name, int namelen)
{
	struct dir_entry *dir;

	lazy_init_name_hash(istate);
	dir = find_dir_entry(istate, name, namelen);
	return dir && dir->nr;
}

void adjust_dirname_case(struct index_state *istate, char *name)
{
	const char *startPtr = name;
	const char *ptr = startPtr;

	lazy_init_name_hash(istate);
	while (*ptr) {
		while (*ptr && *ptr != '/')
			ptr++;

		if (*ptr == '/') {
			struct dir_entry *dir;

			ptr++;
			dir = find_dir_entry(istate, name, ptr - name + 1);
			if (dir) {
				memcpy((void *)startPtr, dir->name + (startPtr - name), ptr - startPtr);
				startPtr = ptr;
			}
		}
	}
}

struct cache_entry *index_file_exists(struct index_state *istate, const char *name, int namelen, int icase)
{
	struct cache_entry *ce;

	lazy_init_name_hash(istate);

	ce = hashmap_get_from_hash(&istate->name_hash,
				   memihash(name, namelen), NULL);
	while (ce) {
		if (same_name(ce, name, namelen, icase))
			return ce;
		ce = hashmap_get_next(&istate->name_hash, ce);
	}
	return NULL;
}

void free_name_hash(struct index_state *istate)
{
	if (!istate->name_hash_initialized)
		return;
	istate->name_hash_initialized = 0;

	hashmap_free(&istate->name_hash, 0);
	hashmap_free(&istate->dir_hash, 1);
}

/*
 * Precompute the hash values for this cache_entry
 * for use in the istate.name_hash and istate.dir_hash.
 *
 * If the item is in the root directory, just compute the
 * hash value (for istate.name_hash) on the full path.
 *
 * If the item is in a subdirectory, first compute the
 * hash value for the immediate parent directory (for
 * istate.dir_hash) and then the hash value for the full
 * path by continuing the computation.
 *
 * Note that these hashes will be used by
 * wt_status_collect_untracked() as it scans the worktree
 * and maps observed paths back to the index (optionally
 * ignoring case).  Therefore, we probably only *NEED* to
 * precompute this for non-skip-worktree items (since
 * status should not observe skipped items), but because
 * lazy_init_name_hash() hashes everything, we force it
 * here.
 */ 
void precompute_istate_hashes(struct cache_entry *ce)
{
	int namelen = ce_namelen(ce);

	while (namelen > 0 && !is_dir_sep(ce->name[namelen - 1]))
		namelen--;

	if (namelen <= 0) {
		ce->precompute_hash_name = memihash(ce->name, ce_namelen(ce));
		ce->precompute_hash_state = CE_PRECOMPUTE_HASH_STATE__SET;
	} else {
		namelen--;
		ce->precompute_hash_dir = memihash(ce->name, namelen);
		ce->precompute_hash_name = memihash_cont(
			ce->precompute_hash_dir, &ce->name[namelen],
			ce_namelen(ce) - namelen);
		ce->precompute_hash_state =
			CE_PRECOMPUTE_HASH_STATE__SET | CE_PRECOMPUTE_HASH_STATE__DIR;
	}
}
