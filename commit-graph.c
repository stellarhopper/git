#include "cache.h"
#include "config.h"
#include "dir.h"
#include "git-compat-util.h"
#include "lockfile.h"
#include "pack.h"
#include "packfile.h"
#include "commit.h"
#include "object.h"
#include "refs.h"
#include "revision.h"
#include "sha1-lookup.h"
#include "commit-graph.h"
#include "object-store.h"
#include "alloc.h"
#include "hashmap.h"
#include "replace-object.h"
#include "progress.h"

#define GRAPH_SIGNATURE 0x43475048 /* "CGPH" */
#define GRAPH_CHUNKID_OIDFANOUT 0x4f494446 /* "OIDF" */
#define GRAPH_CHUNKID_OIDLOOKUP 0x4f49444c /* "OIDL" */
#define GRAPH_CHUNKID_DATA 0x43444154 /* "CDAT" */
#define GRAPH_CHUNKID_EXTRAEDGES 0x45444745 /* "EDGE" */

#define GRAPH_DATA_WIDTH (the_hash_algo->rawsz + 16)

#define GRAPH_VERSION_1 0x1
#define GRAPH_VERSION GRAPH_VERSION_1

#define GRAPH_EXTRA_EDGES_NEEDED 0x80000000
#define GRAPH_EDGE_LAST_MASK 0x7fffffff
#define GRAPH_PARENT_NONE 0x70000000

#define GRAPH_LAST_EDGE 0x80000000

#define GRAPH_HEADER_SIZE 8
#define GRAPH_FANOUT_SIZE (4 * 256)
#define GRAPH_CHUNKLOOKUP_WIDTH 12
#define GRAPH_MIN_SIZE (GRAPH_HEADER_SIZE + 4 * GRAPH_CHUNKLOOKUP_WIDTH \
			+ GRAPH_FANOUT_SIZE + the_hash_algo->rawsz)

char *get_commit_graph_filename(const char *obj_dir)
{
	return xstrfmt("%s/info/commit-graph", obj_dir);
}

static uint8_t oid_version(void)
{
	return 1;
}

static struct commit_graph *alloc_commit_graph(void)
{
	struct commit_graph *g = xcalloc(1, sizeof(*g));
	g->graph_fd = -1;

	return g;
}

extern int read_replace_refs;

static int commit_graph_compatible(struct repository *r)
{
	if (!r->gitdir)
		return 0;

	if (read_replace_refs) {
		prepare_replace_object(r);
		if (hashmap_get_size(&r->objects->replace_map->map))
			return 0;
	}

	prepare_commit_graft(r);
	if (r->parsed_objects && r->parsed_objects->grafts_nr)
		return 0;
	if (is_repository_shallow(r))
		return 0;

	return 1;
}

int open_commit_graph(const char *graph_file, int *fd, struct stat *st)
{
	*fd = git_open(graph_file);
	if (*fd < 0)
		return 0;
	if (fstat(*fd, st)) {
		close(*fd);
		return 0;
	}
	return 1;
}

struct commit_graph *load_commit_graph_one_fd_st(int fd, struct stat *st)
{
	void *graph_map;
	size_t graph_size;
	struct commit_graph *ret;

	graph_size = xsize_t(st->st_size);

	if (graph_size < GRAPH_MIN_SIZE) {
		close(fd);
		error(_("commit-graph file is too small"));
		return NULL;
	}
	graph_map = xmmap(NULL, graph_size, PROT_READ, MAP_PRIVATE, fd, 0);
	ret = parse_commit_graph(graph_map, fd, graph_size);

	if (!ret) {
		munmap(graph_map, graph_size);
		close(fd);
	}

	return ret;
}

static int verify_commit_graph_lite(struct commit_graph *g)
{
	/*
	 * Basic validation shared between parse_commit_graph()
	 * which'll be called every time the graph is used, and the
	 * much more expensive verify_commit_graph() used by
	 * "commit-graph verify".
	 *
	 * There should only be very basic checks here to ensure that
	 * we don't e.g. segfault in fill_commit_in_graph(), but
	 * because this is a very hot codepath nothing that e.g. loops
	 * over g->num_commits, or runs a checksum on the commit-graph
	 * itself.
	 */
	if (!g->chunk_oid_fanout) {
		error("commit-graph is missing the OID Fanout chunk");
		return 1;
	}
	if (!g->chunk_oid_lookup) {
		error("commit-graph is missing the OID Lookup chunk");
		return 1;
	}
	if (!g->chunk_commit_data) {
		error("commit-graph is missing the Commit Data chunk");
		return 1;
	}

	return 0;
}

struct commit_graph *parse_commit_graph(void *graph_map, int fd,
					size_t graph_size)
{
	const unsigned char *data, *chunk_lookup;
	uint32_t i;
	struct commit_graph *graph;
	uint64_t last_chunk_offset;
	uint32_t last_chunk_id;
	uint32_t graph_signature;
	unsigned char graph_version, hash_version;

	if (!graph_map)
		return NULL;

	if (graph_size < GRAPH_MIN_SIZE)
		return NULL;

	data = (const unsigned char *)graph_map;

	graph_signature = get_be32(data);
	if (graph_signature != GRAPH_SIGNATURE) {
		error(_("commit-graph signature %X does not match signature %X"),
		      graph_signature, GRAPH_SIGNATURE);
		return NULL;
	}

	graph_version = *(unsigned char*)(data + 4);
	if (graph_version != GRAPH_VERSION) {
		error(_("commit-graph version %X does not match version %X"),
		      graph_version, GRAPH_VERSION);
		return NULL;
	}

	hash_version = *(unsigned char*)(data + 5);
	if (hash_version != oid_version()) {
		error(_("commit-graph hash version %X does not match version %X"),
		      hash_version, oid_version());
		return NULL;
	}

	graph = alloc_commit_graph();

	graph->hash_len = the_hash_algo->rawsz;
	graph->num_chunks = *(unsigned char*)(data + 6);
	graph->graph_fd = fd;
	graph->data = graph_map;
	graph->data_len = graph_size;

	last_chunk_id = 0;
	last_chunk_offset = 8;
	chunk_lookup = data + 8;
	for (i = 0; i < graph->num_chunks; i++) {
		uint32_t chunk_id;
		uint64_t chunk_offset;
		int chunk_repeated = 0;

		if (data + graph_size - chunk_lookup <
		    GRAPH_CHUNKLOOKUP_WIDTH) {
			error(_("commit-graph chunk lookup table entry missing; file may be incomplete"));
			free(graph);
			return NULL;
		}

		chunk_id = get_be32(chunk_lookup + 0);
		chunk_offset = get_be64(chunk_lookup + 4);

		chunk_lookup += GRAPH_CHUNKLOOKUP_WIDTH;

		if (chunk_offset > graph_size - the_hash_algo->rawsz) {
			error(_("commit-graph improper chunk offset %08x%08x"), (uint32_t)(chunk_offset >> 32),
			      (uint32_t)chunk_offset);
			free(graph);
			return NULL;
		}

		switch (chunk_id) {
		case GRAPH_CHUNKID_OIDFANOUT:
			if (graph->chunk_oid_fanout)
				chunk_repeated = 1;
			else
				graph->chunk_oid_fanout = (uint32_t*)(data + chunk_offset);
			break;

		case GRAPH_CHUNKID_OIDLOOKUP:
			if (graph->chunk_oid_lookup)
				chunk_repeated = 1;
			else
				graph->chunk_oid_lookup = data + chunk_offset;
			break;

		case GRAPH_CHUNKID_DATA:
			if (graph->chunk_commit_data)
				chunk_repeated = 1;
			else
				graph->chunk_commit_data = data + chunk_offset;
			break;

		case GRAPH_CHUNKID_EXTRAEDGES:
			if (graph->chunk_extra_edges)
				chunk_repeated = 1;
			else
				graph->chunk_extra_edges = data + chunk_offset;
			break;
		}

		if (chunk_repeated) {
			error(_("commit-graph chunk id %08x appears multiple times"), chunk_id);
			free(graph);
			return NULL;
		}

		if (last_chunk_id == GRAPH_CHUNKID_OIDLOOKUP)
		{
			graph->num_commits = (chunk_offset - last_chunk_offset)
					     / graph->hash_len;
		}

		last_chunk_id = chunk_id;
		last_chunk_offset = chunk_offset;
	}

	if (verify_commit_graph_lite(graph)) {
		free(graph);
		return NULL;
	}

	return graph;
}

static struct commit_graph *load_commit_graph_one(const char *graph_file)
{

	struct stat st;
	int fd;
	int open_ok = open_commit_graph(graph_file, &fd, &st);

	if (!open_ok)
		return NULL;

	return load_commit_graph_one_fd_st(fd, &st);
}

static void prepare_commit_graph_one(struct repository *r, const char *obj_dir)
{
	char *graph_name;

	if (r->objects->commit_graph)
		return;

	graph_name = get_commit_graph_filename(obj_dir);
	r->objects->commit_graph =
		load_commit_graph_one(graph_name);

	FREE_AND_NULL(graph_name);
}

/*
 * Return 1 if commit_graph is non-NULL, and 0 otherwise.
 *
 * On the first invocation, this function attemps to load the commit
 * graph if the_repository is configured to have one.
 */
static int prepare_commit_graph(struct repository *r)
{
	struct object_directory *odb;
	int config_value;

	if (git_env_bool(GIT_TEST_COMMIT_GRAPH_DIE_ON_LOAD, 0))
		die("dying as requested by the '%s' variable on commit-graph load!",
		    GIT_TEST_COMMIT_GRAPH_DIE_ON_LOAD);

	if (r->objects->commit_graph_attempted)
		return !!r->objects->commit_graph;
	r->objects->commit_graph_attempted = 1;

	if (!git_env_bool(GIT_TEST_COMMIT_GRAPH, 0) &&
	    (repo_config_get_bool(r, "core.commitgraph", &config_value) ||
	    !config_value))
		/*
		 * This repository is not configured to use commit graphs, so
		 * do not load one. (But report commit_graph_attempted anyway
		 * so that commit graph loading is not attempted again for this
		 * repository.)
		 */
		return 0;

	if (!commit_graph_compatible(r))
		return 0;

	prepare_alt_odb(r);
	for (odb = r->objects->odb;
	     !r->objects->commit_graph && odb;
	     odb = odb->next)
		prepare_commit_graph_one(r, odb->path);
	return !!r->objects->commit_graph;
}

int generation_numbers_enabled(struct repository *r)
{
	uint32_t first_generation;
	struct commit_graph *g;
	if (!prepare_commit_graph(r))
	       return 0;

	g = r->objects->commit_graph;

	if (!g->num_commits)
		return 0;

	first_generation = get_be32(g->chunk_commit_data +
				    g->hash_len + 8) >> 2;

	return !!first_generation;
}

void close_commit_graph(struct raw_object_store *o)
{
	free_commit_graph(o->commit_graph);
	o->commit_graph = NULL;
}

static int bsearch_graph(struct commit_graph *g, struct object_id *oid, uint32_t *pos)
{
	return bsearch_hash(oid->hash, g->chunk_oid_fanout,
			    g->chunk_oid_lookup, g->hash_len, pos);
}

static struct commit_list **insert_parent_or_die(struct repository *r,
						 struct commit_graph *g,
						 uint64_t pos,
						 struct commit_list **pptr)
{
	struct commit *c;
	struct object_id oid;

	if (pos >= g->num_commits)
		die("invalid parent position %"PRIu64, pos);

	hashcpy(oid.hash, g->chunk_oid_lookup + g->hash_len * pos);
	c = lookup_commit(r, &oid);
	if (!c)
		die(_("could not find commit %s"), oid_to_hex(&oid));
	c->graph_pos = pos;
	return &commit_list_insert(c, pptr)->next;
}

static void fill_commit_graph_info(struct commit *item, struct commit_graph *g, uint32_t pos)
{
	const unsigned char *commit_data = g->chunk_commit_data + GRAPH_DATA_WIDTH * pos;
	item->graph_pos = pos;
	item->generation = get_be32(commit_data + g->hash_len + 8) >> 2;
}

static inline void set_commit_tree(struct commit *c, struct tree *t)
{
	c->maybe_tree = t;
}

static int fill_commit_in_graph(struct repository *r,
				struct commit *item,
				struct commit_graph *g, uint32_t pos)
{
	uint32_t edge_value;
	uint32_t *parent_data_ptr;
	uint64_t date_low, date_high;
	struct commit_list **pptr;
	const unsigned char *commit_data = g->chunk_commit_data + (g->hash_len + 16) * pos;

	item->object.parsed = 1;
	item->graph_pos = pos;

	set_commit_tree(item, NULL);

	date_high = get_be32(commit_data + g->hash_len + 8) & 0x3;
	date_low = get_be32(commit_data + g->hash_len + 12);
	item->date = (timestamp_t)((date_high << 32) | date_low);

	item->generation = get_be32(commit_data + g->hash_len + 8) >> 2;

	pptr = &item->parents;

	edge_value = get_be32(commit_data + g->hash_len);
	if (edge_value == GRAPH_PARENT_NONE)
		return 1;
	pptr = insert_parent_or_die(r, g, edge_value, pptr);

	edge_value = get_be32(commit_data + g->hash_len + 4);
	if (edge_value == GRAPH_PARENT_NONE)
		return 1;
	if (!(edge_value & GRAPH_EXTRA_EDGES_NEEDED)) {
		pptr = insert_parent_or_die(r, g, edge_value, pptr);
		return 1;
	}

	parent_data_ptr = (uint32_t*)(g->chunk_extra_edges +
			  4 * (uint64_t)(edge_value & GRAPH_EDGE_LAST_MASK));
	do {
		edge_value = get_be32(parent_data_ptr);
		pptr = insert_parent_or_die(r, g,
					    edge_value & GRAPH_EDGE_LAST_MASK,
					    pptr);
		parent_data_ptr++;
	} while (!(edge_value & GRAPH_LAST_EDGE));

	return 1;
}

static int find_commit_in_graph(struct commit *item, struct commit_graph *g, uint32_t *pos)
{
	if (item->graph_pos != COMMIT_NOT_FROM_GRAPH) {
		*pos = item->graph_pos;
		return 1;
	} else {
		return bsearch_graph(g, &(item->object.oid), pos);
	}
}

static int parse_commit_in_graph_one(struct repository *r,
				     struct commit_graph *g,
				     struct commit *item)
{
	uint32_t pos;

	if (item->object.parsed)
		return 1;

	if (find_commit_in_graph(item, g, &pos))
		return fill_commit_in_graph(r, item, g, pos);

	return 0;
}

int parse_commit_in_graph(struct repository *r, struct commit *item)
{
	if (!prepare_commit_graph(r))
		return 0;
	return parse_commit_in_graph_one(r, r->objects->commit_graph, item);
}

void load_commit_graph_info(struct repository *r, struct commit *item)
{
	uint32_t pos;
	if (!prepare_commit_graph(r))
		return;
	if (find_commit_in_graph(item, r->objects->commit_graph, &pos))
		fill_commit_graph_info(item, r->objects->commit_graph, pos);
}

static struct tree *load_tree_for_commit(struct repository *r,
					 struct commit_graph *g,
					 struct commit *c)
{
	struct object_id oid;
	const unsigned char *commit_data = g->chunk_commit_data +
					   GRAPH_DATA_WIDTH * (c->graph_pos);

	hashcpy(oid.hash, commit_data);
	set_commit_tree(c, lookup_tree(r, &oid));

	return c->maybe_tree;
}

static struct tree *get_commit_tree_in_graph_one(struct repository *r,
						 struct commit_graph *g,
						 const struct commit *c)
{
	if (c->maybe_tree)
		return c->maybe_tree;
	if (c->graph_pos == COMMIT_NOT_FROM_GRAPH)
		BUG("get_commit_tree_in_graph_one called from non-commit-graph commit");

	return load_tree_for_commit(r, g, (struct commit *)c);
}

struct tree *get_commit_tree_in_graph(struct repository *r, const struct commit *c)
{
	return get_commit_tree_in_graph_one(r, r->objects->commit_graph, c);
}

struct packed_commit_list {
	struct commit **list;
	int nr;
	int alloc;
};

struct packed_oid_list {
	struct object_id *list;
	int nr;
	int alloc;
};

struct write_commit_graph_context {
	struct repository *r;
	const char *obj_dir;
	char *graph_name;
	struct packed_oid_list oids;
	struct packed_commit_list commits;
	int num_extra_edges;
	unsigned long approx_nr_objects;
	struct progress *progress;
	int progress_done;
	uint64_t progress_cnt;
	unsigned append:1,
		 report_progress:1;
};

static void write_graph_chunk_fanout(struct hashfile *f,
				     struct write_commit_graph_context *ctx)
{
	int i, count = 0;
	struct commit **list = ctx->commits.list;

	/*
	 * Write the first-level table (the list is sorted,
	 * but we use a 256-entry lookup to be able to avoid
	 * having to do eight extra binary search iterations).
	 */
	for (i = 0; i < 256; i++) {
		while (count < ctx->commits.nr) {
			if ((*list)->object.oid.hash[0] != i)
				break;
			display_progress(ctx->progress, ++ctx->progress_cnt);
			count++;
			list++;
		}

		hashwrite_be32(f, count);
	}
}

static void write_graph_chunk_oids(struct hashfile *f, int hash_len,
				   struct write_commit_graph_context *ctx)
{
	struct commit **list = ctx->commits.list;
	int count;
	for (count = 0; count < ctx->commits.nr; count++, list++) {
		display_progress(ctx->progress, ++ctx->progress_cnt);
		hashwrite(f, (*list)->object.oid.hash, (int)hash_len);
	}
}

static const unsigned char *commit_to_sha1(size_t index, void *table)
{
	struct commit **commits = table;
	return commits[index]->object.oid.hash;
}

static void write_graph_chunk_data(struct hashfile *f, int hash_len,
				   struct write_commit_graph_context *ctx)
{
	struct commit **list = ctx->commits.list;
	struct commit **last = ctx->commits.list + ctx->commits.nr;
	uint32_t num_extra_edges = 0;

	while (list < last) {
		struct commit_list *parent;
		int edge_value;
		uint32_t packedDate[2];
		display_progress(ctx->progress, ++ctx->progress_cnt);

		parse_commit_no_graph(*list);
		hashwrite(f, get_commit_tree_oid(*list)->hash, hash_len);

		parent = (*list)->parents;

		if (!parent)
			edge_value = GRAPH_PARENT_NONE;
		else {
			edge_value = sha1_pos(parent->item->object.oid.hash,
					      ctx->commits.list,
					      ctx->commits.nr,
					      commit_to_sha1);

			if (edge_value < 0)
				BUG("missing parent %s for commit %s",
				    oid_to_hex(&parent->item->object.oid),
				    oid_to_hex(&(*list)->object.oid));
		}

		hashwrite_be32(f, edge_value);

		if (parent)
			parent = parent->next;

		if (!parent)
			edge_value = GRAPH_PARENT_NONE;
		else if (parent->next)
			edge_value = GRAPH_EXTRA_EDGES_NEEDED | num_extra_edges;
		else {
			edge_value = sha1_pos(parent->item->object.oid.hash,
					      ctx->commits.list,
					      ctx->commits.nr,
					      commit_to_sha1);
			if (edge_value < 0)
				BUG("missing parent %s for commit %s",
				    oid_to_hex(&parent->item->object.oid),
				    oid_to_hex(&(*list)->object.oid));
		}

		hashwrite_be32(f, edge_value);

		if (edge_value & GRAPH_EXTRA_EDGES_NEEDED) {
			do {
				num_extra_edges++;
				parent = parent->next;
			} while (parent);
		}

		if (sizeof((*list)->date) > 4)
			packedDate[0] = htonl(((*list)->date >> 32) & 0x3);
		else
			packedDate[0] = 0;

		packedDate[0] |= htonl((*list)->generation << 2);

		packedDate[1] = htonl((*list)->date);
		hashwrite(f, packedDate, 8);

		list++;
	}
}

static void write_graph_chunk_extra_edges(struct hashfile *f,
					  struct write_commit_graph_context *ctx)
{
	struct commit **list = ctx->commits.list;
	struct commit **last = ctx->commits.list + ctx->commits.nr;
	struct commit_list *parent;

	while (list < last) {
		int num_parents = 0;

		display_progress(ctx->progress, ++ctx->progress_cnt);

		for (parent = (*list)->parents; num_parents < 3 && parent;
		     parent = parent->next)
			num_parents++;

		if (num_parents <= 2) {
			list++;
			continue;
		}

		/* Since num_parents > 2, this initializer is safe. */
		for (parent = (*list)->parents->next; parent; parent = parent->next) {
			int edge_value = sha1_pos(parent->item->object.oid.hash,
						  ctx->commits.list,
						  ctx->commits.nr,
						  commit_to_sha1);

			if (edge_value < 0)
				BUG("missing parent %s for commit %s",
				    oid_to_hex(&parent->item->object.oid),
				    oid_to_hex(&(*list)->object.oid));
			else if (!parent->next)
				edge_value |= GRAPH_LAST_EDGE;

			hashwrite_be32(f, edge_value);
		}

		list++;
	}
}

static int commit_compare(const void *_a, const void *_b)
{
	const struct object_id *a = (const struct object_id *)_a;
	const struct object_id *b = (const struct object_id *)_b;
	return oidcmp(a, b);
}

static int add_packed_commits(const struct object_id *oid,
			      struct packed_git *pack,
			      uint32_t pos,
			      void *data)
{
	struct write_commit_graph_context *ctx = (struct write_commit_graph_context*)data;
	enum object_type type;
	off_t offset = nth_packed_object_offset(pack, pos);
	struct object_info oi = OBJECT_INFO_INIT;

	if (ctx->progress)
		display_progress(ctx->progress, ++ctx->progress_done);

	oi.typep = &type;
	if (packed_object_info(ctx->r, pack, offset, &oi) < 0)
		die(_("unable to get type of object %s"), oid_to_hex(oid));

	if (type != OBJ_COMMIT)
		return 0;

	ALLOC_GROW(ctx->oids.list, ctx->oids.nr + 1, ctx->oids.alloc);
	oidcpy(&(ctx->oids.list[ctx->oids.nr]), oid);
	ctx->oids.nr++;

	return 0;
}

static void add_missing_parents(struct write_commit_graph_context *ctx, struct commit *commit)
{
	struct commit_list *parent;
	for (parent = commit->parents; parent; parent = parent->next) {
		if (!(parent->item->object.flags & UNINTERESTING)) {
			ALLOC_GROW(ctx->oids.list, ctx->oids.nr + 1, ctx->oids.alloc);
			oidcpy(&ctx->oids.list[ctx->oids.nr], &(parent->item->object.oid));
			ctx->oids.nr++;
			parent->item->object.flags |= UNINTERESTING;
		}
	}
}

static void close_reachable(struct write_commit_graph_context *ctx)
{
	int i;
	struct commit *commit;

	if (ctx->report_progress)
		ctx->progress = start_delayed_progress(
					_("Loading known commits in commit graph"),
					ctx->oids.nr);
	for (i = 0; i < ctx->oids.nr; i++) {
		display_progress(ctx->progress, i + 1);
		commit = lookup_commit(ctx->r, &ctx->oids.list[i]);
		if (commit)
			commit->object.flags |= UNINTERESTING;
	}
	stop_progress(&ctx->progress);

	/*
	 * As this loop runs, ctx->oids.nr may grow, but not more
	 * than the number of missing commits in the reachable
	 * closure.
	 */
	if (ctx->report_progress)
		ctx->progress = start_delayed_progress(
					_("Expanding reachable commits in commit graph"),
					ctx->oids.nr);
	for (i = 0; i < ctx->oids.nr; i++) {
		display_progress(ctx->progress, i + 1);
		commit = lookup_commit(ctx->r, &ctx->oids.list[i]);

		if (commit && !parse_commit_no_graph(commit))
			add_missing_parents(ctx, commit);
	}
	stop_progress(&ctx->progress);

	if (ctx->report_progress)
		ctx->progress = start_delayed_progress(
					_("Clearing commit marks in commit graph"),
					ctx->oids.nr);
	for (i = 0; i < ctx->oids.nr; i++) {
		display_progress(ctx->progress, i + 1);
		commit = lookup_commit(ctx->r, &ctx->oids.list[i]);

		if (commit)
			commit->object.flags &= ~UNINTERESTING;
	}
	stop_progress(&ctx->progress);
}

static void compute_generation_numbers(struct write_commit_graph_context *ctx)
{
	int i;
	struct commit_list *list = NULL;

	if (ctx->report_progress)
		ctx->progress = start_progress(
					_("Computing commit graph generation numbers"),
					ctx->commits.nr);
	for (i = 0; i < ctx->commits.nr; i++) {
		display_progress(ctx->progress, i + 1);
		if (ctx->commits.list[i]->generation != GENERATION_NUMBER_INFINITY &&
		    ctx->commits.list[i]->generation != GENERATION_NUMBER_ZERO)
			continue;

		commit_list_insert(ctx->commits.list[i], &list);
		while (list) {
			struct commit *current = list->item;
			struct commit_list *parent;
			int all_parents_computed = 1;
			uint32_t max_generation = 0;

			for (parent = current->parents; parent; parent = parent->next) {
				if (parent->item->generation == GENERATION_NUMBER_INFINITY ||
				    parent->item->generation == GENERATION_NUMBER_ZERO) {
					all_parents_computed = 0;
					commit_list_insert(parent->item, &list);
					break;
				} else if (parent->item->generation > max_generation) {
					max_generation = parent->item->generation;
				}
			}

			if (all_parents_computed) {
				current->generation = max_generation + 1;
				pop_commit(&list);

				if (current->generation > GENERATION_NUMBER_MAX)
					current->generation = GENERATION_NUMBER_MAX;
			}
		}
	}
	stop_progress(&ctx->progress);
}

static int add_ref_to_list(const char *refname,
			   const struct object_id *oid,
			   int flags, void *cb_data)
{
	struct string_list *list = (struct string_list *)cb_data;

	string_list_append(list, oid_to_hex(oid));
	return 0;
}

int write_commit_graph_reachable(const char *obj_dir, unsigned int flags)
{
	struct string_list list = STRING_LIST_INIT_DUP;
	int result;

	for_each_ref(add_ref_to_list, &list);
	result = write_commit_graph(obj_dir, NULL, &list,
				    flags);

	string_list_clear(&list, 0);
	return result;
}

static int fill_oids_from_packs(struct write_commit_graph_context *ctx,
				struct string_list *pack_indexes)
{
	uint32_t i;
	struct strbuf progress_title = STRBUF_INIT;
	struct strbuf packname = STRBUF_INIT;
	int dirlen;

	strbuf_addf(&packname, "%s/pack/", ctx->obj_dir);
	dirlen = packname.len;
	if (ctx->report_progress) {
		strbuf_addf(&progress_title,
			    Q_("Finding commits for commit graph in %d pack",
			       "Finding commits for commit graph in %d packs",
			       pack_indexes->nr),
			    pack_indexes->nr);
		ctx->progress = start_delayed_progress(progress_title.buf, 0);
		ctx->progress_done = 0;
	}
	for (i = 0; i < pack_indexes->nr; i++) {
		struct packed_git *p;
		strbuf_setlen(&packname, dirlen);
		strbuf_addstr(&packname, pack_indexes->items[i].string);
		p = add_packed_git(packname.buf, packname.len, 1);
		if (!p) {
			error(_("error adding pack %s"), packname.buf);
			return -1;
		}
		if (open_pack_index(p)) {
			error(_("error opening index for %s"), packname.buf);
			return -1;
		}
		for_each_object_in_pack(p, add_packed_commits, ctx,
					FOR_EACH_OBJECT_PACK_ORDER);
		close_pack(p);
		free(p);
	}

	stop_progress(&ctx->progress);
	strbuf_reset(&progress_title);
	strbuf_release(&packname);

	return 0;
}

static void fill_oids_from_commit_hex(struct write_commit_graph_context *ctx,
				      struct string_list *commit_hex)
{
	uint32_t i;
	struct strbuf progress_title = STRBUF_INIT;

	if (ctx->report_progress) {
		strbuf_addf(&progress_title,
			    Q_("Finding commits for commit graph from %d ref",
			       "Finding commits for commit graph from %d refs",
			       commit_hex->nr),
			    commit_hex->nr);
		ctx->progress = start_delayed_progress(
					progress_title.buf,
					commit_hex->nr);
	}
	for (i = 0; i < commit_hex->nr; i++) {
		const char *end;
		struct object_id oid;
		struct commit *result;

		display_progress(ctx->progress, i + 1);
		if (commit_hex->items[i].string &&
		    parse_oid_hex(commit_hex->items[i].string, &oid, &end))
			continue;

		result = lookup_commit_reference_gently(ctx->r, &oid, 1);

		if (result) {
			ALLOC_GROW(ctx->oids.list, ctx->oids.nr + 1, ctx->oids.alloc);
			oidcpy(&ctx->oids.list[ctx->oids.nr], &(result->object.oid));
			ctx->oids.nr++;
		}
	}
	stop_progress(&ctx->progress);
	strbuf_release(&progress_title);
}

static void fill_oids_from_all_packs(struct write_commit_graph_context *ctx)
{
	if (ctx->report_progress)
		ctx->progress = start_delayed_progress(
			_("Finding commits for commit graph among packed objects"),
			ctx->approx_nr_objects);
	for_each_packed_object(add_packed_commits, ctx,
			       FOR_EACH_OBJECT_PACK_ORDER);
	if (ctx->progress_done < ctx->approx_nr_objects)
		display_progress(ctx->progress, ctx->approx_nr_objects);
	stop_progress(&ctx->progress);
}

static uint32_t count_distinct_commits(struct write_commit_graph_context *ctx)
{
	uint32_t i, count_distinct = 1;

	if (ctx->report_progress)
		ctx->progress = start_delayed_progress(
			_("Counting distinct commits in commit graph"),
			ctx->oids.nr);
	display_progress(ctx->progress, 0); /* TODO: Measure QSORT() progress */
	QSORT(ctx->oids.list, ctx->oids.nr, commit_compare);

	for (i = 1; i < ctx->oids.nr; i++) {
		display_progress(ctx->progress, i + 1);
		if (!oideq(&ctx->oids.list[i - 1], &ctx->oids.list[i]))
			count_distinct++;
	}
	stop_progress(&ctx->progress);

	return count_distinct;
}

static void copy_oids_to_commits(struct write_commit_graph_context *ctx)
{
	uint32_t i;
	struct commit_list *parent;

	ctx->num_extra_edges = 0;
	if (ctx->report_progress)
		ctx->progress = start_delayed_progress(
			_("Finding extra edges in commit graph"),
			ctx->oids.nr);
	for (i = 0; i < ctx->oids.nr; i++) {
		int num_parents = 0;
		display_progress(ctx->progress, i + 1);
		if (i > 0 && oideq(&ctx->oids.list[i - 1], &ctx->oids.list[i]))
			continue;

		ctx->commits.list[ctx->commits.nr] = lookup_commit(ctx->r, &ctx->oids.list[i]);
		parse_commit_no_graph(ctx->commits.list[ctx->commits.nr]);

		for (parent = ctx->commits.list[ctx->commits.nr]->parents;
		     parent; parent = parent->next)
			num_parents++;

		if (num_parents > 2)
			ctx->num_extra_edges += num_parents - 1;

		ctx->commits.nr++;
	}
	stop_progress(&ctx->progress);
}

static int write_commit_graph_file(struct write_commit_graph_context *ctx)
{
	uint32_t i;
	struct hashfile *f;
	struct lock_file lk = LOCK_INIT;
	uint32_t chunk_ids[5];
	uint64_t chunk_offsets[5];
	const unsigned hashsz = the_hash_algo->rawsz;
	struct strbuf progress_title = STRBUF_INIT;
	int num_chunks = ctx->num_extra_edges ? 4 : 3;

	ctx->graph_name = get_commit_graph_filename(ctx->obj_dir);
	if (safe_create_leading_directories(ctx->graph_name)) {
		UNLEAK(ctx->graph_name);
		error(_("unable to create leading directories of %s"),
			ctx->graph_name);
		return -1;
	}

	hold_lock_file_for_update(&lk, ctx->graph_name, LOCK_DIE_ON_ERROR);
	f = hashfd(lk.tempfile->fd, lk.tempfile->filename.buf);

	hashwrite_be32(f, GRAPH_SIGNATURE);

	hashwrite_u8(f, GRAPH_VERSION);
	hashwrite_u8(f, oid_version());
	hashwrite_u8(f, num_chunks);
	hashwrite_u8(f, 0); /* unused padding byte */

	chunk_ids[0] = GRAPH_CHUNKID_OIDFANOUT;
	chunk_ids[1] = GRAPH_CHUNKID_OIDLOOKUP;
	chunk_ids[2] = GRAPH_CHUNKID_DATA;
	if (ctx->num_extra_edges)
		chunk_ids[3] = GRAPH_CHUNKID_EXTRAEDGES;
	else
		chunk_ids[3] = 0;
	chunk_ids[4] = 0;

	chunk_offsets[0] = 8 + (num_chunks + 1) * GRAPH_CHUNKLOOKUP_WIDTH;
	chunk_offsets[1] = chunk_offsets[0] + GRAPH_FANOUT_SIZE;
	chunk_offsets[2] = chunk_offsets[1] + hashsz * ctx->commits.nr;
	chunk_offsets[3] = chunk_offsets[2] + (hashsz + 16) * ctx->commits.nr;
	chunk_offsets[4] = chunk_offsets[3] + 4 * ctx->num_extra_edges;

	for (i = 0; i <= num_chunks; i++) {
		uint32_t chunk_write[3];

		chunk_write[0] = htonl(chunk_ids[i]);
		chunk_write[1] = htonl(chunk_offsets[i] >> 32);
		chunk_write[2] = htonl(chunk_offsets[i] & 0xffffffff);
		hashwrite(f, chunk_write, 12);
	}

	if (ctx->report_progress) {
		strbuf_addf(&progress_title,
			    Q_("Writing out commit graph in %d pass",
			       "Writing out commit graph in %d passes",
			       num_chunks),
			    num_chunks);
		ctx->progress = start_delayed_progress(
			progress_title.buf,
			num_chunks * ctx->commits.nr);
	}
	write_graph_chunk_fanout(f, ctx);
	write_graph_chunk_oids(f, hashsz, ctx);
	write_graph_chunk_data(f, hashsz, ctx);
	if (ctx->num_extra_edges)
		write_graph_chunk_extra_edges(f, ctx);
	stop_progress(&ctx->progress);
	strbuf_release(&progress_title);

	close_commit_graph(ctx->r->objects);
	finalize_hashfile(f, NULL, CSUM_HASH_IN_STREAM | CSUM_FSYNC);
	commit_lock_file(&lk);

	return 0;
}

int write_commit_graph(const char *obj_dir,
		       struct string_list *pack_indexes,
		       struct string_list *commit_hex,
		       unsigned int flags)
{
	struct write_commit_graph_context *ctx;
	uint32_t i, count_distinct = 0;
	int res = 0;

	if (!commit_graph_compatible(the_repository))
		return 0;

	ctx = xcalloc(1, sizeof(struct write_commit_graph_context));
	ctx->r = the_repository;
	ctx->obj_dir = obj_dir;
	ctx->append = flags & COMMIT_GRAPH_APPEND ? 1 : 0;
	ctx->report_progress = flags & COMMIT_GRAPH_PROGRESS ? 1 : 0;

	ctx->approx_nr_objects = approximate_object_count();
	ctx->oids.alloc = ctx->approx_nr_objects / 32;

	if (ctx->append) {
		prepare_commit_graph_one(ctx->r, ctx->obj_dir);
		if (ctx->r->objects->commit_graph)
			ctx->oids.alloc += ctx->r->objects->commit_graph->num_commits;
	}

	if (ctx->oids.alloc < 1024)
		ctx->oids.alloc = 1024;
	ALLOC_ARRAY(ctx->oids.list, ctx->oids.alloc);

	if (ctx->append && ctx->r->objects->commit_graph) {
		struct commit_graph *g = ctx->r->objects->commit_graph;
		for (i = 0; i < g->num_commits; i++) {
			const unsigned char *hash = g->chunk_oid_lookup + g->hash_len * i;
			hashcpy(ctx->oids.list[ctx->oids.nr++].hash, hash);
		}
	}

	if (pack_indexes) {
		if ((res = fill_oids_from_packs(ctx, pack_indexes)))
			goto cleanup;
	}

	if (commit_hex)
		fill_oids_from_commit_hex(ctx, commit_hex);

	if (!pack_indexes && !commit_hex)
		fill_oids_from_all_packs(ctx);

	close_reachable(ctx);

	count_distinct = count_distinct_commits(ctx);

	if (count_distinct >= GRAPH_EDGE_LAST_MASK) {
		error(_("the commit graph format cannot write %d commits"), count_distinct);
		res = -1;
		goto cleanup;
	}

	ctx->commits.alloc = count_distinct;
	ALLOC_ARRAY(ctx->commits.list, ctx->commits.alloc);

	copy_oids_to_commits(ctx);

	if (ctx->commits.nr >= GRAPH_EDGE_LAST_MASK) {
		error(_("too many commits to write graph"));
		res = -1;
		goto cleanup;
	}

	compute_generation_numbers(ctx);

	res = write_commit_graph_file(ctx);

cleanup:
	free(ctx->graph_name);
	free(ctx->commits.list);
	free(ctx->oids.list);
	free(ctx);

	return res;
}

#define VERIFY_COMMIT_GRAPH_ERROR_HASH 2
static int verify_commit_graph_error;

static void graph_report(const char *fmt, ...)
{
	va_list ap;

	verify_commit_graph_error = 1;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

#define GENERATION_ZERO_EXISTS 1
#define GENERATION_NUMBER_EXISTS 2

int verify_commit_graph(struct repository *r, struct commit_graph *g)
{
	uint32_t i, cur_fanout_pos = 0;
	struct object_id prev_oid, cur_oid, checksum;
	int generation_zero = 0;
	struct hashfile *f;
	int devnull;
	struct progress *progress = NULL;

	if (!g) {
		graph_report("no commit-graph file loaded");
		return 1;
	}

	verify_commit_graph_error = verify_commit_graph_lite(g);
	if (verify_commit_graph_error)
		return verify_commit_graph_error;

	devnull = open("/dev/null", O_WRONLY);
	f = hashfd(devnull, NULL);
	hashwrite(f, g->data, g->data_len - g->hash_len);
	finalize_hashfile(f, checksum.hash, CSUM_CLOSE);
	if (!hasheq(checksum.hash, g->data + g->data_len - g->hash_len)) {
		graph_report(_("the commit-graph file has incorrect checksum and is likely corrupt"));
		verify_commit_graph_error = VERIFY_COMMIT_GRAPH_ERROR_HASH;
	}

	for (i = 0; i < g->num_commits; i++) {
		struct commit *graph_commit;

		hashcpy(cur_oid.hash, g->chunk_oid_lookup + g->hash_len * i);

		if (i && oidcmp(&prev_oid, &cur_oid) >= 0)
			graph_report(_("commit-graph has incorrect OID order: %s then %s"),
				     oid_to_hex(&prev_oid),
				     oid_to_hex(&cur_oid));

		oidcpy(&prev_oid, &cur_oid);

		while (cur_oid.hash[0] > cur_fanout_pos) {
			uint32_t fanout_value = get_be32(g->chunk_oid_fanout + cur_fanout_pos);

			if (i != fanout_value)
				graph_report(_("commit-graph has incorrect fanout value: fanout[%d] = %u != %u"),
					     cur_fanout_pos, fanout_value, i);
			cur_fanout_pos++;
		}

		graph_commit = lookup_commit(r, &cur_oid);
		if (!parse_commit_in_graph_one(r, g, graph_commit))
			graph_report(_("failed to parse commit %s from commit-graph"),
				     oid_to_hex(&cur_oid));
	}

	while (cur_fanout_pos < 256) {
		uint32_t fanout_value = get_be32(g->chunk_oid_fanout + cur_fanout_pos);

		if (g->num_commits != fanout_value)
			graph_report(_("commit-graph has incorrect fanout value: fanout[%d] = %u != %u"),
				     cur_fanout_pos, fanout_value, i);

		cur_fanout_pos++;
	}

	if (verify_commit_graph_error & ~VERIFY_COMMIT_GRAPH_ERROR_HASH)
		return verify_commit_graph_error;

	progress = start_progress(_("Verifying commits in commit graph"),
				  g->num_commits);
	for (i = 0; i < g->num_commits; i++) {
		struct commit *graph_commit, *odb_commit;
		struct commit_list *graph_parents, *odb_parents;
		uint32_t max_generation = 0;

		display_progress(progress, i + 1);
		hashcpy(cur_oid.hash, g->chunk_oid_lookup + g->hash_len * i);

		graph_commit = lookup_commit(r, &cur_oid);
		odb_commit = (struct commit *)create_object(r, &cur_oid, alloc_commit_node(r));
		if (parse_commit_internal(odb_commit, 0, 0)) {
			graph_report(_("failed to parse commit %s from object database for commit-graph"),
				     oid_to_hex(&cur_oid));
			continue;
		}

		if (!oideq(&get_commit_tree_in_graph_one(r, g, graph_commit)->object.oid,
			   get_commit_tree_oid(odb_commit)))
			graph_report(_("root tree OID for commit %s in commit-graph is %s != %s"),
				     oid_to_hex(&cur_oid),
				     oid_to_hex(get_commit_tree_oid(graph_commit)),
				     oid_to_hex(get_commit_tree_oid(odb_commit)));

		graph_parents = graph_commit->parents;
		odb_parents = odb_commit->parents;

		while (graph_parents) {
			if (odb_parents == NULL) {
				graph_report(_("commit-graph parent list for commit %s is too long"),
					     oid_to_hex(&cur_oid));
				break;
			}

			if (!oideq(&graph_parents->item->object.oid, &odb_parents->item->object.oid))
				graph_report(_("commit-graph parent for %s is %s != %s"),
					     oid_to_hex(&cur_oid),
					     oid_to_hex(&graph_parents->item->object.oid),
					     oid_to_hex(&odb_parents->item->object.oid));

			if (graph_parents->item->generation > max_generation)
				max_generation = graph_parents->item->generation;

			graph_parents = graph_parents->next;
			odb_parents = odb_parents->next;
		}

		if (odb_parents != NULL)
			graph_report(_("commit-graph parent list for commit %s terminates early"),
				     oid_to_hex(&cur_oid));

		if (!graph_commit->generation) {
			if (generation_zero == GENERATION_NUMBER_EXISTS)
				graph_report(_("commit-graph has generation number zero for commit %s, but non-zero elsewhere"),
					     oid_to_hex(&cur_oid));
			generation_zero = GENERATION_ZERO_EXISTS;
		} else if (generation_zero == GENERATION_ZERO_EXISTS)
			graph_report(_("commit-graph has non-zero generation number for commit %s, but zero elsewhere"),
				     oid_to_hex(&cur_oid));

		if (generation_zero == GENERATION_ZERO_EXISTS)
			continue;

		/*
		 * If one of our parents has generation GENERATION_NUMBER_MAX, then
		 * our generation is also GENERATION_NUMBER_MAX. Decrement to avoid
		 * extra logic in the following condition.
		 */
		if (max_generation == GENERATION_NUMBER_MAX)
			max_generation--;

		if (graph_commit->generation != max_generation + 1)
			graph_report(_("commit-graph generation for commit %s is %u != %u"),
				     oid_to_hex(&cur_oid),
				     graph_commit->generation,
				     max_generation + 1);

		if (graph_commit->date != odb_commit->date)
			graph_report(_("commit date for commit %s in commit-graph is %"PRItime" != %"PRItime),
				     oid_to_hex(&cur_oid),
				     graph_commit->date,
				     odb_commit->date);
	}
	stop_progress(&progress);

	return verify_commit_graph_error;
}

void free_commit_graph(struct commit_graph *g)
{
	if (!g)
		return;
	if (g->graph_fd >= 0) {
		munmap((void *)g->data, g->data_len);
		g->data = NULL;
		close(g->graph_fd);
	}
	free(g);
}
