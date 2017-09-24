/*
 * Copyright (C) 2017 Santiago León O. <santileortiz@gmail.com>
 */

#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>

//#define NDEBUG
#include <assert.h>

#include "slo_timers.h"
#include "geometry_combinatorics.h"

#define OT_DB_IMPLEMENTATION
#include "ot_db.h"

// TODO: Add timing:
//   - Compute ETA.
//   - If ETA is too short, do not print anything.
//   - Have a maximum update delay so that slow procedures don't seem to be
//     stalled until the first # is printed.
void progress_bar (float val, float total)
{
    float percent = (val/(total-1))*100;
    int length = 60;

    char str[length+30];

    static int prev = -1;
    static int idx;

    idx = length*percent/100;
    if (prev != idx) {
        prev = idx;
        int i;
        for (i=0; i<length; i++) {
            if (i < idx) {
                str[i] = '#';
            } else {
                str[i] = '-';
            }
        }
        str[i] = '\0';
        fprintf (stderr, "\r[%s] %.2f%%", str, percent);
        if (percent == 100) {
            fprintf (stderr, "\r\e[K");
        }
    }
}

order_type_t* order_type_from_id (int n, uint64_t ot_id)
{
    order_type_t *res = order_type_new (n, NULL);
    if (ot_id == 0 && n>10) {
        convex_ot_searchable (res);
    } else {
        assert (n<=10);
        open_database (n);
        db_seek (res, ot_id);
    }
    return res;
}

#if 1
#define single_thrackle_func single_thrackle
#else
#define single_thrackle_func single_thrackle_slow
#endif
void get_thrackle_for_each_ot (int n, int k)
{
    uint64_t id = 0;
    order_type_t *ot = order_type_new (n, NULL);
    int *curr_set = malloc (sizeof(int)*k);

    triangle_set_t *triangle_set = malloc (triangle_set_size (k));
    triangle_set->k = k;

    open_database (n);
    db_seek (ot, id);

    int total_triangles = binomial (n, 3);
    float average = 0;
    int nodes = 0, searches = 0;
    srand (time(NULL));
    int rand_arr[total_triangles];
    init_random_array (rand_arr, total_triangles);
    bool found = single_thrackle_func (n, k, ot, curr_set, &nodes, rand_arr);

    bool print_all = false;
    while (!db_is_eof ()) {
        if (print_all) {
            printf ("%ld: ", id);
            if (found) {
                array_print (curr_set, k);
            } else {
                printf ("None\n");
            }

            //printf ("%ld: %lu\n", id, subset_it_id_for_idx (total_triangles, curr_set, k));
        } else {
            if (!found) {
                printf ("%ld\n", id);
            }
            progress_bar (id, __g_db_data.num_order_types);
        }

        db_next (ot);
        triangle_set_from_ids (ot, n, curr_set, k, triangle_set);

        found = false;
        if (!is_thrackle(triangle_set)) {
            fisher_yates_shuffle (rand_arr, total_triangles);
            nodes = 0;
            found = single_thrackle_func (n, k, ot, curr_set, &nodes, rand_arr);
            average += nodes;
            searches++;
        } else {
            found = true;
        }
        id++;
    }
    printf ("Searches: %d, Average nodes: %f\n", searches, average/searches);
}

enum tr_order_t {
    LEXICOGRAPHIC,
    TR_SIZE,
    TR_EDGE_LEXICOGRAPHIC
};

void single_thrackle_size_order (int n, int k, int *nodes, int *res, enum tr_order_t ord)
{
    int total_triangles = binomial(n,3);
    int *triangle_order;
    switch (ord) {
        case TR_EDGE_LEXICOGRAPHIC:
         triangle_order = malloc (sizeof(int) * total_triangles);
         int j;
            for (j=0; j<total_triangles; j++) {
                triangle_order[j] = j;
            }
            lex_size_triangle_sort_user_data (triangle_order, total_triangles, &n);
            break;
        case TR_SIZE:
            triangle_order = malloc (sizeof(int) * total_triangles);
            order_triangles_size (n, triangle_order);
            break;
        default:
            triangle_order = NULL;
    }

    if (triangle_order != NULL) {
        // NOTE: Reverse array
        int i;
        for (i=0; i<total_triangles/2; i++) {
            swap (&triangle_order[i], &triangle_order[total_triangles-i-1]);
        }
    }

    // NOTE: Current triagle size computations are defined only over convex sets
    order_type_t *ot = order_type_from_id (n, 0);

    single_thrackle (n, k, ot, res, nodes, triangle_order);
    free (ot);
}

void single_thrackle_random_order (int n, int k, uint64_t ot_id, int *nodes, int *res)
{
    order_type_t *ot = order_type_from_id (n, ot_id);

    int triangle_order[binomial(n,3)];
    init_random_array (triangle_order, ARRAY_SIZE(triangle_order));

    single_thrackle (n, k, ot, res, nodes, triangle_order);
    free (ot);
}

void search_full_tree_all_ot (int n)
{
    assert(n <= 10);
    mem_pool_t pool = {0};
    uint64_t id = 0;
    order_type_t *ot = order_type_new (n, NULL);

    open_database (n);
    db_seek (ot, id);

    float average = 0;
    int max_size = 0;

    while (!db_is_eof ()) {
        pool_temp_marker_t mrk = mem_pool_begin_temporary_memory (&pool);
        struct sequence_store_t seq = new_sequence_store_opts (NULL, &pool, SEQ_DRY_RUN);
        thrackle_search_tree (n, ot, &seq);
        seq_tree_end (&seq);

        average += seq.num_nodes;
        max_size = MAX(seq.final_max_len, max_size);

        progress_bar (id, __g_db_data.num_order_types);
        db_next (ot);
        id++;
        mem_pool_end_temporary_memory (mrk);
    }
    printf ("Max Size: %d, Average nodes: %f\n", max_size, average/(id));
}

uint32_t* load_uint32_from_text_file (char *filename, int *count)
{
    assert (count != NULL);
    *count = 0;
    FILE *f = fopen (filename, "r");
    char *str = NULL;
    size_t str_len = 0;
    while (getline (&str, &str_len, f) != -1) {
        if (isdigit (str[0])) {
            (*count)++;
        }
        free (str);
        str = NULL;
        str_len = 0;
    }

    uint32_t *res = malloc ((*count) * sizeof(uint32_t));
    if (res != NULL ) {
        int i = 0;
        str = NULL;
        str_len = 0;
        rewind (f);
        while (getline (&str, &str_len, f) != -1) {
            if (isdigit (str[0])) {
                res[i++] = (uint32_t)strtoul (str, NULL, 0);
            }
            free (str);
            str = NULL;
            str_len = 0;
        }
    } else {
        printf ("malloc() failed reading file '%s'\n", filename);
    }
    return res;
}

// TODO: Check this works for big endian and little endian.
uint64_t* load_uint64_from_bin_file (char *filename, uint64_t *count, mem_pool_t *pool)
{
    assert (count != NULL);
    struct stat info;
    if (stat (filename, &info) != 0) {
        printf ("Could not get file info.\n");
        return NULL;
    }

    uint64_t *res = pom_push_size (pool, info.st_size);
    *count = info.st_size/sizeof(uint64_t);

    int file = open (filename, O_RDONLY);
    int bytes_read = 0;
    while (bytes_read != info.st_size) {
        bytes_read += read (file, res, info.st_size);
    }
    return res;
}

void write_uint64_to_bin_file (char *filename, uint64_t *arr, uint32_t count)
{
    int file = open (filename, O_RDWR|O_CREAT, 0666);

    int i;
    for (i=0; i<count; i++) {
        file_write (file, &arr[i], sizeof(*arr));
    }
}

void max_thrackle_size_ot_file (int n, char *filename)
{
    assert(n <= 10);
    mem_pool_t pool = {0};
    order_type_t *ot = order_type_new (n, NULL);

    int num_ot_ids = 0;
    uint32_t *ot_ids = load_uint32_from_text_file (filename, &num_ot_ids);

    open_database (n);
    float average = 0;
    int i;
    for (i=0; i < num_ot_ids; i++) {
        db_seek (ot, ot_ids[i]);

        pool_temp_marker_t mrk = mem_pool_begin_temporary_memory (&pool);
        struct sequence_store_t seq = new_sequence_store_opts (NULL, &pool, SEQ_DRY_RUN);
        thrackle_search_tree (n, ot, &seq);
        seq_tree_end (&seq);

        progress_bar (i, num_ot_ids);
        average += seq.num_nodes;
        printf ("%u: %d\n", ot_ids[i], seq.final_max_len);

        mem_pool_end_temporary_memory (mrk);
    }
    printf ("Average nodes: %f\n", average/(num_ot_ids));
    free (ot_ids);
}

// This version does an exhaustive search over all binomial(binomial(n,3),k)
// possible sets. Only useful for checking the correctness of other
// implementations.
void count_thrackles (int n, int k)
{
    uint64_t id = 0;
    open_database (n);
    order_type_t *ot = order_type_new (n, NULL);
    triangle_set_t *curr_set = malloc (triangle_set_size (k));
    curr_set->k = k;

    db_seek (ot, id);

    subset_it_t *triangle_it = subset_it_new (n, 3, NULL);
    subset_it_t *triangle_set_it = subset_it_new (triangle_it->size, k, NULL);

    subset_it_precompute (triangle_it);
    subset_it_precompute (triangle_set_it);

    while (!db_is_eof()) {
        db_next (ot);
        uint64_t count = 0;
        subset_it_seek (triangle_set_it, 0);
        while (1) {
            //BEGIN_WALL_CLOCK;
            int i;
            for (i=0; i<k; i++) {
                subset_it_seek (triangle_it, triangle_set_it->idx[i]);
                //printf ("\t");
                //subset_it_print (triangle_it);
                curr_set->e[i].v[0] = ot->pts[triangle_it->idx[0]];
                curr_set->e[i].v[1] = ot->pts[triangle_it->idx[1]];
                curr_set->e[i].v[2] = ot->pts[triangle_it->idx[2]];
            }

            if (is_thrackle(curr_set)) {
            //if (is_edge_disjoint_set(curr_set)) {
                //printf ("\t\t");
                subset_it_print (triangle_set_it);
                count++;
            }
            if (!subset_it_next (triangle_set_it)){
                break;
            }
            //PROBE_WALL_CLOCK("Triangle set loop");
        }
        printf ("%ld: %ld\n", id, count);
        if (id==0) {
            return;
        }
        id++;
    }
    free (triangle_it);
    free (triangle_set_it);
}

void print_differing_triples (int n, uint64_t ot_id_1, uint64_t ot_id_2)
{
    open_database (n);
    order_type_t *ot_1 = order_type_new (n, NULL);
    db_seek (ot_1, ot_id_1);
    ot_triples_t *ot_triples_1 = ot_triples_new (ot_1, NULL);

    order_type_t *ot_2 = order_type_new (n, NULL);
    db_seek (ot_2, ot_id_2);
    ot_triples_t *ot_triples_2 = ot_triples_new (ot_2, NULL);

    int32_t trip_id = 0;
    while (trip_id < ot_triples_1->size) {
        if (ot_triples_1->triples[trip_id] != ot_triples_2->triples[trip_id]) {
            print_triple (n, trip_id);
        }
        trip_id++;
    }
}

void print_edge_disjoint_sets (int n, int k)
{
    int count = 0;
    order_type_t *ot = order_type_new (n, NULL);
    triangle_set_t *curr_set = malloc (triangle_set_size (k));
    curr_set->k = k;

    subset_it_t *triangle_it = subset_it_new (n, 3, NULL);
    subset_it_t *triangle_set_it = subset_it_new (triangle_it->size, k, NULL);

    subset_it_precompute (triangle_it);
    subset_it_precompute (triangle_set_it);

    db_seek (ot, 0);
    while (1) {
        int i;
        for (i=0; i<k; i++) {
            subset_it_seek (triangle_it, triangle_set_it->idx[i]);
            curr_set->e[i].v[0] = ot->pts[triangle_it->idx[0]];
            curr_set->e[i].v[1] = ot->pts[triangle_it->idx[1]];
            curr_set->e[i].v[2] = ot->pts[triangle_it->idx[2]];
        }

        if (is_edge_disjoint_set(curr_set)) {
            array_print (triangle_set_it->idx, k);
            //subset_it_print (triangle_set_it);
            count++;
        }

        if (!subset_it_next (triangle_set_it)) {
            break;
        }
    }
    printf ("total count: %d\n", count);
    free (triangle_it);
    free (triangle_set_it);
}

void fast_edge_disjoint_sets (int n, int k)
{
    triangle_set_t *curr_set = malloc (triangle_set_size (k));
    curr_set->k = k;

    order_type_t *ot = order_type_new (n, NULL);
    db_seek (ot, 0);

    subset_it_t *triangle_it = subset_it_new (n, 3, NULL);
    subset_it_precompute (triangle_it);

    mem_pool_t pool = {0};
    struct sequence_store_t seq = new_sequence_store (NULL, &pool);
    generate_edge_disjoint_triangle_sets (n, k, &seq);
    int *all_edj_sets = seq_end (&seq);

    printf ("Sets found: %d\n", seq.num_sequences);

    int i;
    while (!db_is_eof()) {
        int found_th = 0;
        for (i=0; i<seq.num_sequences*k; i+=k) {
            int j;
            for (j=0; j<k; j++) {
                subset_it_seek (triangle_it, all_edj_sets[i+j]);
                curr_set->e[j].v[0] = ot->pts[triangle_it->idx[0]];
                curr_set->e[j].v[1] = ot->pts[triangle_it->idx[1]];
                curr_set->e[j].v[2] = ot->pts[triangle_it->idx[2]];
            }
            if (is_thrackle(curr_set)) {
                found_th++;
                //printf ("%"PRIu64": ", subset_it_id_for_idx (binomial(n,3), &all_edj_sets[i], k));
                //array_print (&all_edj_sets[i], k);
            }
        }
        printf ("%d %d\n", ot->id, found_th);
        db_next (ot);
    }

    free (triangle_it);
    mem_pool_destroy (&pool);
}

void get_thrackle_list_filename (char *s, int len, int n, int ot_id, int k)
{
    snprintf (s, len, ".cache/n_%d-ot_%d-th_all-k_%d.bin", n, ot_id, k);
}

void get_all_thrackles (int n, int k, uint32_t ot_id, char* filename)
{
    assert (n<=10);
    open_database (n);
    order_type_t *ot = order_type_new (n, NULL);
    db_seek (ot, ot_id);

    struct sequence_store_t seq = new_sequence_store (filename, NULL);
    all_thrackles (n, k, ot, &seq);
    seq_end (&seq);
}

int* get_all_thrackles_cached (int n, int k, uint32_t ot_id)
{
    char filename[200];
    get_thrackle_list_filename (filename, ARRAY_SIZE(filename), n, ot_id, k);

    int *res = seq_read_file (filename, NULL, NULL, NULL);
    if (res == NULL) {
        get_all_thrackles (n, k, ot_id, filename);
        res = seq_read_file (filename, NULL, NULL, NULL);
    }

    return res;
}

#define print_info(n,ot_id) print_info_order (n,ot_id,NULL)
void print_info_order (int n, uint64_t ot_id, int *triangle_order)
{
    order_type_t *ot = order_type_from_id (n, ot_id);
    mem_pool_t temp_pool = {0};
    struct sequence_store_t seq = new_sequence_store_opts (NULL, &temp_pool, SEQ_DRY_RUN);

    thrackle_search_tree_full (n, ot, &seq, triangle_order);
    seq_tree_end (&seq);

    seq_print_info (&seq);
    mem_pool_destroy (&temp_pool);
}

void print_all_thrackles (int n, uint64_t ot_id, int *triangle_order)
{
    order_type_t *ot = order_type_from_id (n, ot_id);
    mem_pool_t temp_pool = {0};
    struct sequence_store_t seq;
    seq = new_sequence_store (NULL, &temp_pool);

    thrackle_search_tree_full (n, ot, &seq, triangle_order);
    backtrack_node_t *root = seq_tree_end (&seq);

    seq_tree_print_sequences_full (root, seq.final_max_len, sorted_array_print, TREE_PRINT_LEN);

    mem_pool_destroy (&temp_pool);
}

void print_info_random_order (int n, uint64_t ot_id)
{
    int rand_arr[binomial(n,3)];
    srand (time(NULL));
    init_random_array (rand_arr, ARRAY_SIZE(rand_arr));
    print_info_order (n, ot_id, rand_arr);
}

#define average_search_nodes_lexicographic(n) average_search_nodes(n, NULL)
void average_search_nodes (int n, int *triangle_order)
{
    mem_pool_t temp_pool = {0};
    // TODO: move order_type_new() to use a mem_pool_t instead of a
    // memory_stack_t.
    order_type_t *ot = mem_pool_push_size (&temp_pool, order_type_size(n));
    ot->n = n;
    ot->id = -1;

    assert (n<=10);
    open_database (n);
    uint32_t ot_id = 0;
    db_seek (ot, ot_id);

    float nodes = 0;

    while (!db_is_eof()) {
        pool_temp_marker_t mrk = mem_pool_begin_temporary_memory (&temp_pool);
        struct sequence_store_t seq = new_sequence_store (NULL, &temp_pool);
        thrackle_search_tree_full (n, ot, &seq, triangle_order);
        nodes += seq.num_nodes;
        //printf ("%"PRIu32": %"PRIu32"\n", ot->id, seq.num_nodes);
        seq_tree_end (&seq);
        db_next(ot);
        mem_pool_end_temporary_memory (mrk);
    }
    printf ("Average nodes: %f\n", nodes/((float)__g_db_data.num_order_types));
    mem_pool_destroy (&temp_pool);
}

void average_search_nodes_random (int n)
{
    int rand_arr[binomial(n,3)];
    srand (time(NULL));
    init_random_array (rand_arr, ARRAY_SIZE(rand_arr));
    average_search_nodes (n, rand_arr);
}

void compare_convex_thrackle_orderings (int n, int k)
{
    int nodes, res[k];
    single_thrackle_size_order (n, k, &nodes, res, LEXICOGRAPHIC);
    printf ("Lexicographic: %d\n", nodes);
    single_thrackle_size_order (n, k, &nodes, res, TR_SIZE);
    printf ("Triangle size: %d\n", nodes);
    single_thrackle_size_order (n, k, &nodes, res, TR_EDGE_LEXICOGRAPHIC);
    printf ("Triangle edge size: %d\n", nodes);

    srand (time(NULL));
    int iters = 1000, i;
    float avg = 0;
    for (i=0; i<iters; i++) {
        single_thrackle_random_order (n, k, 0, &nodes, res);
        avg += nodes;
    }
    printf ("Random avg (%d): %.2f\n", iters, avg/(float)iters);
}

int* get_all_thrackles_convex_position (int n, int k, int *num_found)
{
    char filename[200];
    get_thrackle_list_filename (filename, ARRAY_SIZE(filename), n, 0, k);

    struct file_header_t info;
    int *res = seq_read_file (filename, NULL, &info, NULL);
    if (res == NULL) {
        order_type_t *ot = order_type_new (n, NULL);
        if (n<=10) {
            open_database (n);
            db_seek (ot, 0);
        } else {
            convex_ot_searchable (ot);
        }

        struct sequence_store_t seq = new_sequence_store (filename, NULL);
        all_thrackles (n, k, ot, &seq);
        seq_end (&seq);
        res = seq_read_file (filename, NULL, &info, NULL);
    }
    *num_found = info.num_sequences;
    return res;
}

void print_triangle_sizes_for_thrackles_in_convex_position (int n)
{
    int k = thrackle_size (n);
    int num_found;
    int *thrackles = get_all_thrackles_convex_position (n, k, &num_found);
    int i;
    for (i=0; i<num_found*k; i+=k) {
        int *thrackle = &thrackles[i];

        //array_print (thrackle, k);

        int num_sizes = n/2-1;
        int sizes[num_sizes];
        array_clear (sizes, num_sizes);

        int j;
        for (j = 0; j<k; j++) {
            int t_id = thrackle[j];
            int t[3];
            subset_it_idx_for_id (t_id, n, t, 3);

            int tr_size = triangle_size (n, t);
            //printf ("triangle: ");
            //array_print (t, 3);
            //printf ("distances: ");
            //array_print (dist, 3);

            assert (tr_size != 0);
            //printf ("max_dist: %d\n", max_distance);
            sizes[tr_size - 1]++;
            //printf ("\n");
        }

        //int total_triangles = binomial (n, 3);
        //uint64_t choosen_set_id = subset_it_id_for_idx (total_triangles, thrackle, k);
        //if (sizes [0] == 0 && sizes[1] == 2 && sizes[2] == 7 && sizes[3] == 3) {
        //if (sizes [0] == 0 && sizes[1] == 0) {
        //if (sizes[3] == 5) {
            //printf ("%"PRIu64": ", choosen_set_id);
            array_print (sizes, num_sizes);
        //}
    }
}

void print_triangle_edge_sizes_for_thrackles_in_convex_position (int n)
{
    int k = thrackle_size (n);
    int num_found;
    int *thrackles = get_all_thrackles_convex_position (n, k, &num_found);
    int i;
    for (i=0; i<num_found*k; i+=k) {
        int *thrackle = &thrackles[i];

        //array_print (thrackle, k);

        int num_sizes = n/2-1;
        int sizes[num_sizes];
        array_clear (sizes, num_sizes);

        int j;
        for (j = 0; j<k; j++) {
            int t_id = thrackle[j];
            int t[3];
            subset_it_idx_for_id (t_id, n, t, 3);

            int tr_size = triangle_size (n, t);
            //printf ("triangle: ");
            //array_print (t, 3);
            //printf ("distances: ");
            //array_print (dist, 3);

            assert (tr_size != 0);
            //printf ("max_dist: %d\n", max_distance);
            sizes[tr_size - 1]++;
            //printf ("\n");
        }

        //int total_triangles = binomial (n, 3);
        //uint64_t choosen_set_id = subset_it_id_for_idx (total_triangles, thrackle, k);
        //if (sizes [0] == 0 && sizes[1] == 2 && sizes[2] == 7 && sizes[3] == 3) {
        //if (sizes [0] == 0 && sizes[1] == 0) {
        //if (sizes[3] == 5) {
            //printf ("%"PRIu64": ", choosen_set_id);
            array_print (sizes, num_sizes);
        //}
    }
}

uint32_t get_2_factors_of_k_n_n_to_file (int n)
{
    char filename[40];
    snprintf (filename, ARRAY_SIZE(filename), ".cache/n_%d_k_n_n_2-factors.bin", n);
    int file = open (filename, O_RDWR|O_CREAT, 0666);
    // A subset of k_n_n edges of size 2*n
    int e_subs_2_n[2*n];
    subset_it_reset_idx (e_subs_2_n, 2*n);

    uint64_t i=0;
    uint64_t count = 0;
    uint64_t total_edge_subsets = binomial (n*n, 2*n);
    do {
        int edges[4*n]; // 2vertices * 2partite * n
        edges_from_edge_subset (e_subs_2_n, n, edges);

        progress_bar (i, total_edge_subsets);
        // NOTE: Because we have 2*n distinct edges in the graph (by
        // construction), checking regularity implies it's also spanning.
        if (is_2_regular (2*n, edges, 2*n)) {
            file_write (file, &i, sizeof(i));
            count++;
        }
        i++;
    } while (subset_it_next_explicit (n*n, 2*n, e_subs_2_n));
    close (file);
    return count;
}

uint32_t cycle_sizes_2_factors_of_k_n_n_from_file (int n)
{
    char filename[40];
    snprintf (filename, ARRAY_SIZE(filename), ".cache/n_%d_k_n_n_2-factors.bin", n);

    // NOTE: This makes subset_it_idx_for_id() significantly faster. We can't
    // precompute all subsets in this case because for n=7 that would require
    // 34.39 TiB.
    set_global_binomial_cache (50, 15);

    // 2*n ints where each one represents an edge of K_n_n. A 2*n size subset of
    // the total n*n edges.
    int e_subs_2_n[2*n];
    subset_it_reset_idx (e_subs_2_n, 2*n);
    uint64_t count;
    uint64_t *all_subset_ids = load_uint64_from_bin_file (filename, &count, NULL);

    uint64_t i = 0;
    while (i < count) {
        subset_it_idx_for_id (all_subset_ids[i], n*n, e_subs_2_n, 2*n);

        int edges[4*n];
        edges_from_edge_subset (e_subs_2_n, n, edges);

        if (is_2_regular (2*n, edges, 2*n)) {
            //print_2_regular_graph (e_subs_2_n, n);
            //print_2_regular_cycle_decomposition (edges, n);
            int part[n], num_part;
            partition_from_2_regular (edges, n, part, &num_part);
            array_print (part, num_part);
        } else {
            printf ("Not 2-regular.\n");
        }
        i++;
    }
    free (all_subset_ids);
    destroy_global_binomial_cache ();
    return count;
}

void print_lex_edg_triangles (int n)
{
    int i;
    for (i=0; i<binomial(n, 3); i++) {
        int dist[3];
        int t[3];
        subset_it_idx_for_id (i, n, t, 3);
        edge_sizes (n, t, dist);
        printf ("%d%d%d\n", dist[2], dist[1], dist[0]);
    }
}

int num_digits (uint64_t n)
{
    int res = 0;
    if ( n >= 10000000000000000 ) { res |= 16; n /= 10000000000000000; }
    if ( n >= 100000000         ) { res |=  8; n /= 100000000; }
    if ( n >= 10000             ) { res |=  4; n /= 10000; }
    if ( n >= 100               ) { res |=  2; n /= 100; }
    if ( n >= 10                ) { res |=  1; }
    return res+1;
}

// Verifies the analytic function of the number of 2-factors of K_n_n. Works by
// computing the real number of 2-factors of K_n_n by iterating all sets of 2*n
// edges of K_n_n and stores those that are 2-regular in a file. Then we lookup
// that file and count the number of 2-regular factors with each
// multicharacteristic set. Finally we check that the analytic function actually
// computes this exact number for each characteristic multiset.
//
// Because Binomial(n*n, 2*n) grows really big, only up to n=7 has been tested.
// The hard limit without using infinite precision arithmetic is n=9, for n=10
// variables will overflow.
void verify_k_n_n_2_factor_count (int n)
{
    if (n >= 10) {
        printf ("Error: Variables would overflow.\n");
        return;
    }

    char filename[40];
    snprintf (filename, ARRAY_SIZE(filename), ".cache/n_%d_k_n_n_2-factors.bin", n);

    // Ensure file exists
    int file = open (filename, O_RDONLY);
    if (file == -1) {
        if (n >= 7) {
            printf ("Warning: maybe will take more than 10 hours.");
        }
        get_2_factors_of_k_n_n_to_file (n);
    } else {
        close (file);
    }

    // NOTE: This makes subset_it_idx_for_id() significantly faster. We can't
    // precompute all subsets in this case because for n=7 that would require
    // 34.39 TiB.
    set_global_binomial_cache (50, 15);

    int e_subs_2_n[2*n];
    subset_it_reset_idx (e_subs_2_n, 2*n);
    uint64_t num_2_factors;
    mem_pool_t pool = {0};
    uint64_t *all_subset_ids = load_uint64_from_bin_file (filename, &num_2_factors, &pool);

    int (*p)[n+1][n+1] = mem_pool_push_size (&pool, partition_restr_numbers_size(n));
    partition_dbl_restr_numbers (p, n, 2);

    uint64_t *count = mem_pool_push_size_full (&pool, (*p)[n][n]*sizeof(uint64_t), POOL_ZERO_INIT);
    uint64_t i = 0;
    while (i < num_2_factors) {
        subset_it_idx_for_id (all_subset_ids[i], n*n, e_subs_2_n, 2*n);

        int edges[4*n];
        edges_from_edge_subset (e_subs_2_n, n, edges);

        int part[n], num_part;
        partition_from_2_regular (edges, n, part, &num_part);

        int part_id = partition_to_id (p, n, part);
        count[part_id]++;
        i++;
    }

    bool success = true;
    int part[n], num_part;
    int j;
    for (j=0; j<(*p)[n][n]; j++) {
        partition_restr_from_id (p, n, 2, j, part, &num_part);
        if (count[j] != cnt_2_factors_of_k_n_n_for_A (part, num_part)) {
            // We will never reach this
            printf ("Count and L(A) differ for partition: ");
            array_print_full (part, num_part, ", ", "{", "}\n");
            success = false;
        }
    }

    if (success) {
        printf ("K_n_n 2-factor count test sucessful for n=%d\n.", n);
    }
    mem_pool_destroy (&pool);
    destroy_global_binomial_cache ();
}

// Uses the analytic function derived that counts the number of 2-factors of the
// complete bipartite graph K_n_n. Prints the number of 2-factors for each
// characteristic multiset of n.
void print_2_factors_for_each_A (int n)
{
    int (*p)[n+1][n+1] = malloc (partition_restr_numbers_size(n));
    partition_dbl_restr_numbers (p, n, 2);
    int part[n], num_part;
    int i;
    // Careful!! This grows exponentially with n.
    uint64_t count[(*p)[n][n]];
    int digits = 0;
    for (i=0; i<(*p)[n][n]; i++) {
        partition_restr_from_id (p, n, 2, i, part, &num_part);
        count[i] = cnt_2_factors_of_k_n_n_for_A (part, num_part);
        digits = MAX(digits, num_digits (count[i]));
    }

    for (i=0; i<(*p)[n][n]; i++) {
        printf ("%*"PRIu64" ", digits, count[i]);
        partition_restr_from_id (p, n, 2, i, part, &num_part);
        array_print_full (part, num_part, ", ", "{", "}\n");
    }
    free (p);
}

SEQ_CALLBACK(seq_callback_print)
{
    array_print (seq, len);
}

int main ()
{
    ensure_full_database ();
    //get_thrackle_for_each_ot (10, 12);
    //count_thrackles (8);
    //print_differing_triples (n, 0, 1);
    //print_edge_disjoint_sets (8, 8);
    //generate_edge_disjoint_triangle_sets (10, 13);
    //fast_edge_disjoint_sets (9, 10);

    mem_pool_t pool = {0};
    struct sequence_store_t seq = new_sequence_store (NULL, &pool);
    seq_set_callback (&seq, seq_callback_print, NULL);
    matching_decompositions_over_K_n_n (4, NULL, &seq);
    //seq_tree_end (&seq);
    //seq_print_info (&seq);
    backtrack_node_t *bt_root = seq_tree_end (&seq);
    printf ("---------------\n");
    seq_tree_print_all_sequences (bt_root, seq.final_max_len);

    //int n = 10, k = 12;
    //order_type_t *ot = order_type_from_id (n, 403098);
    //int res[k];
    //int nodes;
    //bool found = single_thrackle (n, k, ot, res, &nodes, NULL);
    //if (!found) {
    //    printf ("None\n");
    //} else {
    //    array_print (res, k);
    //}

    //get_all_thrackles (9, 10, 0, NULL);
    //print_triangle_sizes_for_thrackles_in_convex_position (7);

    //compare_convex_thrackle_orderings (10, 12);
    //print_lex_edg_triangles (10);
    //print_info (10, 0);
    //get_2_factors_of_k_n_n_to_file (6);
    //cycle_sizes_2_factors_of_k_n_n_from_file (7);
    //print_2_factors_for_each_A (5);
    //verify_k_n_n_2_factor_count (7);

    //print_restricted_partitions (10, 2);
    //print_all_partitions (10);
    //partition_test_id (49);

    //average_search_nodes_lexicographic (8);
    //print_info_random_order (6, 0);
    //max_thrackle_size_ot_file (10, "n_10_sin_thrackle_12.txt");
    
    //int n = 8;
    //int rand_arr[binomial(n,3)];
    //init_random_array (rand_arr, ARRAY_SIZE(rand_arr));
    //print_all_thrackles (n, 0, rand_arr);

    //search_full_tree_all_ot (7);

    //int count = count_2_regular_subgraphs_of_k_n_n (4, NULL);
    //printf ("Total: %d\n", count);
}
