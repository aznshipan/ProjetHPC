#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <getopt.h>
#include <sys/time.h>

#include <mpi.h>
#include <omp.h>

#define max_taches 1000
/* changelog :
2021-04-12 18:30, instance->n_primary was not properly initialized
*/

double start = 0.0;

char *in_filename = NULL;              // nom du fichier contenant la matrice
bool print_solutions = false;          // affiche chaque solution
long long report_delta = 1e6;          // affiche un rapport tous les ... noeuds
long long next_report;                 // prochain rapport affiché au noeud...
long long max_solutions = 0x7fffffffffffffff;        // stop après ... solutions

long long nb_taches_total = 0;


struct instance_t {
        int n_items;
        int n_primary;
        int n_options;
        char **item_name;   // potentiellement NULL, sinon de taille n_items
        int *options;       // l'option i contient les objets options[ptr[i]:ptr[i+1]]
        int *ptr;           // taille n_options + 1
};

struct sparse_array_t {
        int len;           // nombre d'éléments stockés
        int capacity;      // taille maximale
        int *p;            // contenu de l'ensemble = p[0:len] 
        int *q;            // taille capacity (tout comme p)
};

struct context_t {
        struct sparse_array_t *active_items;      // objets actifs
        struct sparse_array_t **active_options;   // options actives contenant l'objet i
        int *chosen_options;                      // options choisies à ce stade
        int *child_num;                           // numéro du fils exploré
        int *num_children;                        // nombre de fils à explorer
        int level;                                // nombre d'options choisies
        long long nodes;                          // nombre de noeuds explorés
        long long solutions;                      // nombre de solutions trouvées 
};

static const char DIGITS[62] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                                'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 
                                'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
                                'u', 'v', 'w', 'x', 'y', 'z',
                                'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 
                                'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
                                'U', 'V', 'W', 'X', 'Y', 'Z'};


double wtime()
{
        struct timeval ts;
        gettimeofday(&ts, NULL);
        return (double) ts.tv_sec + ts.tv_usec / 1e6;
}


void usage(char **argv)
{
        printf("%s --in FILENAME [OPTIONS]\n\n", argv[0]);
        printf("Options:\n");
        printf("--progress-report N   display a message every N nodes (0 to disable)\n");
        printf("--print-solutions     display solutions when they are found\n");
        printf("--stop-after N        stop the search once N solutions are found\n");
        exit(0);
}


bool item_is_primary(const struct instance_t *instance, int item)
{
        return item < instance->n_primary;
}


void print_option(const struct instance_t *instance, int option)
{
        if (instance->item_name == NULL)
                errx(1, "tentative d'affichage sans noms d'objet");
        for (int p = instance->ptr[option]; p < instance->ptr[option + 1]; p++) {
                int item = instance->options[p];
                printf("%s ", instance->item_name[item]);
        }
        printf("\n");
}



struct sparse_array_t * sparse_array_init(int n)
{
        struct sparse_array_t *S = malloc(sizeof(*S));
        if (S == NULL)
                err(1, "impossible d'allouer un tableau creux");
        S->len = 0;
        S->capacity = n;
        S->p = malloc(n * sizeof(int));
        S->q = malloc(n * sizeof(int));
        if (S->p == NULL || S->q == NULL)
                err(1, "Impossible d'allouer p/q dans un tableau creux");
        for (int i = 0; i < n; i++)
                S->q[i] = n;           // initialement vide
        return S;
}

bool sparse_array_membership(const struct sparse_array_t *S, int x)
{
        return (S->q[x] < S->len);
}

bool sparse_array_empty(const struct sparse_array_t *S)
{
        return (S->len == 0);
}

void sparse_array_add(struct sparse_array_t *S, int x)
{
        int i = S->len;
        S->p[i] = x;
        S->q[x] = i;
        S->len = i + 1;
}

void sparse_array_remove(struct sparse_array_t *S, int x)
{
        int j = S->q[x];
        int n = S->len - 1;
        // échange p[j] et p[n] 
        int y = S->p[n];
        S->p[n] = x;
        S->p[j] = y;
        // met q à jour
        S->q[x] = n;
        S->q[y] = j;
        S->len = n;
}

void sparse_array_unremove(struct sparse_array_t *S)
{
        S->len++;
}

void sparse_array_unadd(struct sparse_array_t *S)
{
        S->len--;
}



bool item_is_active(const struct context_t *ctx, int item)
{
        return sparse_array_membership(ctx->active_items, item);
}

void solution_found(const struct instance_t *instance, struct context_t *ctx)
{
        ctx->solutions++;
        if (!print_solutions)
                return;
        printf("Trouvé une nouvelle solution au niveau %d après %lld noeuds\n", 
                        ctx->level, ctx->nodes);
        printf("Options : \n");
        for (int i = 0; i < ctx->level; i++) {
                int option = ctx->chosen_options[i];
                printf("+ %d : ", option);
                print_option(instance, option);
        }
        printf("\n");
        printf("----------------------------------------------------\n");
}

void cover(const struct instance_t *instance, struct context_t *ctx, int item);

void choose_option(const struct instance_t *instance, struct context_t *ctx, 
                        int option, int chosen_item)
{
        ctx->chosen_options[ctx->level] = option;
        ctx->level++;
        for (int p = instance->ptr[option]; p < instance->ptr[option + 1]; p++) {
                int item = instance->options[p];
                if (item == chosen_item)
                        continue;
                cover(instance, ctx, item);
        }
}

void uncover(const struct instance_t *instance, struct context_t *ctx, int item);

void unchoose_option(const struct instance_t *instance, struct context_t *ctx, 
                        int option, int chosen_item)
{
        for (int p = instance->ptr[option + 1] - 1; p >= instance->ptr[option]; p--) {
                int item = instance->options[p];
                if (item == chosen_item)
                        continue;
                uncover(instance, ctx, item);
        }
        ctx->level--;
}


int choose_next_item(struct context_t *ctx)
{
        int best_item = -1;
        int best_options = 0x7fffffff;
        struct sparse_array_t *active_items = ctx->active_items;
        for (int i = 0; i < active_items->len; i++) {
                int item = active_items->p[i];
                struct sparse_array_t *active_options = ctx->active_options[item];
                int k = active_options->len;
                if (k < best_options) {
                        best_item = item;
                        best_options = k;
                }
        }
        return best_item;
}

void progress_report(const struct context_t *ctx)
{
        double now = wtime();
        printf("Exploré %lld noeuds, trouvé %lld solutions, temps écoulé %.1fs. ", 
                        ctx->nodes, ctx->solutions, now - start);
        int i = 0;
        for (int k = 0; k < ctx->level; k++) {
                if (i > 44)
                        break;
                int n = ctx->child_num[k];
                int m = ctx->num_children[k];
                if (m == 1)
                        continue;
                printf("%c%c ", (n < 62) ? DIGITS[n] : '*', (m < 62) ? DIGITS[m] : '*');
                i++;
        }
        printf("\n"),
        next_report += report_delta;
}

void deactivate(const struct instance_t *instance, struct context_t *ctx, 
                        int option, int covered_item);

void cover(const struct instance_t *instance, struct context_t *ctx, int item)
{
        if (item_is_primary(instance, item))
                sparse_array_remove(ctx->active_items, item);
        struct sparse_array_t *active_options = ctx->active_options[item];
        for (int i = 0; i < active_options->len; i++) {
                int option = active_options->p[i];
                deactivate(instance, ctx, option, item);
        }
}


void deactivate(const struct instance_t *instance, struct context_t *ctx, 
                        int option, int covered_item)
{
        for (int k = instance->ptr[option]; k < instance->ptr[option+1]; k++) {
                int item = instance->options[k];
                if (item == covered_item)
                        continue;
                sparse_array_remove(ctx->active_options[item], option);
        }
}


void reactivate(const struct instance_t *instance, struct context_t *ctx, 
                        int option, int uncovered_item);

void uncover(const struct instance_t *instance, struct context_t *ctx, int item)
{
        struct sparse_array_t *active_options = ctx->active_options[item];
        for (int i = active_options->len - 1; i >= 0; i--) {
                int option = active_options->p[i];
                reactivate(instance, ctx, option, item);
        }
        if (item_is_primary(instance, item))
                sparse_array_unremove(ctx->active_items);
}


void reactivate(const struct instance_t *instance, struct context_t *ctx, 
                        int option, int uncovered_item)
{
        for (int k = instance->ptr[option + 1] - 1; k >= instance->ptr[option]; k--) {
                int item = instance->options[k];
                if (item == uncovered_item)
                        continue;
                sparse_array_unremove(ctx->active_options[item]);
        }
}


struct instance_t * load_matrix(const char *filename)
{
        struct instance_t *instance = malloc(sizeof(*instance));
        if (instance == NULL)
                err(1, "Impossible d'allouer l'instance");
        FILE *in = fopen(filename, "r");
        if (in == NULL)
                err(1, "Impossible d'ouvrir %s en lecture", filename);
        int n_it, n_op;
        if (fscanf(in, "%d %d\n", &n_it, &n_op) != 2)
                errx(1, "Erreur de lecture de la taille du problème\n");
        if (n_it == 0 || n_op == 0)
                errx(1, "Impossible d'avoir 0 objets ou 0 options");
        instance->n_items = n_it;
        instance->n_primary = 0;
        instance->n_options = n_op;
        instance->item_name = malloc(n_it * sizeof(char *));
        instance->ptr = malloc((n_op + 1) * sizeof(int));
        instance->options = malloc(n_it * n_op *sizeof(int));         // surallocation massive
        if (instance->item_name == NULL || instance->ptr == NULL || instance->options == NULL)
                err(1, "Impossible d'allouer la mémoire pour stocker la matrice");


        enum state_t {START, ID, WHITESPACE, BAR, ENDLINE, ENDFILE};
        enum state_t state = START;

        char buffer[256];
        int i = 0;     // prochain octet disponible du buffer
        int n = 0;     // dernier octet disponible du buffer

        char id[65];
        id[64] = 0;    // sentinelle à la fin, quoi qu'il arrive
        int j = 0;     // longueur de l'identifiant en cours de lecture

        int current_item = 0;
        while (state != ENDLINE) {
                enum state_t prev_state = state;
                if (i >= n) {
                        n = fread(buffer, 1, 256, in);
                        if (n == 0) {
                                if (feof(in)) {
                                        state = ENDFILE;
                                }
                                if (ferror(in))
                                        err(1, "erreur lors de la lecture de %s", in_filename);
                        }
                        i = 0;

                }
                if (state == ENDFILE) {
                        // don't examine buffer[i]
                } else if (buffer[i] == '\n') {
                        state = ENDLINE;
                } else if (buffer[i] == '|') {
                        state = BAR;
                } else if (isspace(buffer[i])) {
                        state = WHITESPACE;
                } else {
                        state = ID;
                }

                // traite le caractère lu
                if (state == ID) {
                        if (j == 64)
                                errx(1, "nom d'objet trop long : %s", id);
                        id[j] = buffer[i];
                        j++;
                }
                if (prev_state == ID && state != ID) {
                        id[j] = '\0';
                        if (current_item == instance->n_items)
                                errx(1, "Objet excedentaire : %s", id);
                        for (int k = 0; k < current_item; k++)
                                if (strcmp(id, instance->item_name[k]) == 0)
                                        errx(1, "Nom d'objets dupliqué : %s", id);
                        instance->item_name[current_item] = malloc(j+1);
                        strcpy(instance->item_name[current_item], id);
                        current_item++;
                        j = 0;


                }
                if (state == BAR)
                        instance->n_primary = current_item;
                if (state == ENDFILE)
                        errx(1, "Fin de fichier prématurée");
                // passe au prochain caractère
                i++;
        }
        if (current_item != instance->n_items)
                errx(1, "Incohérence : %d objets attendus mais seulement %d fournis\n", 
                                instance->n_items, current_item);
        if (instance->n_primary == 0)
                instance->n_primary = instance->n_items;

        int current_option = 0;
        int p = 0;       // pointeur courant dans instance->options
        instance->ptr[0] = p;
        bool has_primary = false;
        while (state != ENDFILE) {
                enum state_t prev_state = state;
                if (i >= n) {
                        n = fread(buffer, 1, 256, in);
                        if (n == 0) {
                                if (feof(in)) {
                                        state = ENDFILE;
                                }
                                if (ferror(in))
                                        err(1, "erreur lors de la lecture de %s", in_filename);
                        }
                        i = 0;

                }
                if (state == ENDFILE) {
                        // don't examine buffer[i]
                } else if (buffer[i] == '\n') {
                        state = ENDLINE;
                } else if (buffer[i] == '|') {
                        state = BAR;
                } else if (isspace(buffer[i])) {
                        state = WHITESPACE;
                } else {
                        state = ID;
                }

                // traite le caractère lu
                if (state == ID) {
                        if (j == 64)
                                errx(1, "nom d'objet trop long : %s", id);
                        id[j] = buffer[i];
                        j++;
                }
                if (prev_state == ID && state != ID) {
                        id[j] = '\0';
                        // identifie le numéro de l'objet en question
                        int item_number = -1;
                        for (int k = 0; k < instance->n_items; k++)
                                if (strcmp(id, instance->item_name[k]) == 0) {
                                        item_number = k;
                                        break;
                                }
                        if (item_number == -1)
                                errx(1, "Objet %s inconnu dans l'option #%d", id, current_option);
                        // détecte les objets répétés
                        for (int k = instance->ptr[current_option]; k < p; k++)
                                if (item_number == instance->options[k])
                                        errx(1, "Objet %s répété dans l'option %d\n", 
                                                        instance->item_name[item_number], current_option);
                        instance->options[p] = item_number;
                        p++;
                        has_primary |= item_is_primary(instance, item_number);
                        j = 0;


                }
                if (state == BAR) {
                        errx(1, "Trouvé | dans une option.");
                }
                if ((state == ENDLINE || state == ENDFILE)) {
                        // esquive les lignes vides
                        if (p > instance->ptr[current_option]) {
                                if (current_option == instance->n_options)
                                        errx(1, "Option excédentaire");
                                if (!has_primary)
                                        errx(1, "Option %d sans objet primaire\n", current_option);
                                current_option++;
                                instance->ptr[current_option] = p;
                                has_primary = false;


                        }
                }
                // passe au prochain caractère
                i++;
        }
        if (current_option != instance->n_options)
                errx(1, "Incohérence : %d options attendues mais seulement %d fournies\n", 
                                instance->n_options, current_option);


        fclose(in);
        fprintf(stderr, "Lu %d objets (%d principaux) et %d options\n", 
                instance->n_items, instance->n_primary, instance->n_options);
        return instance;
}


struct context_t * backtracking_setup(const struct instance_t *instance)
{
        struct context_t *ctx = malloc(sizeof(*ctx));
        if (ctx == NULL)
                err(1, "impossible d'allouer un contexte");
        ctx->level = 0;
        ctx->nodes = 0;
        ctx->solutions = 0;
        int n = instance->n_items;
        int m = instance->n_options;
        ctx->active_options = malloc(n * sizeof(*ctx->active_options));
        ctx->chosen_options = malloc(n * sizeof(*ctx->chosen_options));
        ctx->child_num = malloc(n * sizeof(*ctx->child_num));
        ctx->num_children = malloc(n * sizeof(*ctx->num_children));
        if (ctx->active_options == NULL || ctx->chosen_options == NULL
                || ctx->child_num == NULL || ctx->num_children == NULL)
                err(1, "impossible d'allouer le contexte");
        ctx->active_items = sparse_array_init(n);
        for (int item = 0; item < instance->n_primary; item++)
                sparse_array_add(ctx->active_items, item);

        for (int item = 0; item < n; item++)
                ctx->active_options[item] = sparse_array_init(m);
        for (int option = 0; option < m; option++)
                for (int k = instance->ptr[option]; k < instance->ptr[option + 1]; k++) {
                        int item = instance->options[k];
                        sparse_array_add(ctx->active_options[item], option);
                }



        return ctx;
}

struct context_t * context_deepcopy(const struct context_t *context, const struct instance_t *instance){
        struct context_t *ctx = malloc(sizeof(*ctx));
        if (ctx == NULL)
                err(1, "impossible d'allouer un contexte");
        ctx->level = context->level;
        ctx->nodes = context->nodes;
        ctx->solutions = context->solutions;

        int n = instance->n_items;
        int m = instance->n_options;
        ctx->active_options = malloc(n * sizeof(*ctx->active_options));
        ctx->chosen_options = malloc(n * sizeof(*ctx->chosen_options));
        ctx->child_num = malloc(n * sizeof(*ctx->child_num));
        ctx->num_children = malloc(n * sizeof(*ctx->num_children));
        if (ctx->active_options == NULL || ctx->chosen_options == NULL
                || ctx->child_num == NULL || ctx->num_children == NULL)
                err(1, "impossible d'allouer le contexte");


        memcpy(ctx->chosen_options, context->chosen_options, n * sizeof(*ctx->chosen_options));
        memcpy(ctx->child_num, context->child_num, n * sizeof(*ctx->child_num));
        memcpy(ctx->num_children, context->num_children, n * sizeof(*ctx->num_children));


        ctx->active_items = sparse_array_init(n);
        ctx->active_items->len = context->active_items->len;
        ctx->active_items->capacity = context->active_items->capacity;
        memcpy(ctx->active_items->p, context->active_items->p, n * sizeof(int));
        memcpy(ctx->active_items->q, context->active_items->q, n * sizeof(int));

        for (int item = 0; item < n; item++){
                ctx->active_options[item] = sparse_array_init(m);
        }
        for (int item = 0; item < n; item++){ //copie du champs active_options
                
                ctx->active_options[item]->len = context->active_options[item]->len;
                ctx->active_options[item]->capacity = context->active_options[item]->capacity;
                memcpy(ctx->active_options[item]->p, context->active_options[item]->p, m * sizeof(int));
                memcpy(ctx->active_options[item]->q, context->active_options[item]->q, m * sizeof(int));


        }
        // à voir s'il y a plus efficace...


        return ctx;
}

void free_context(struct context_t *ctx, const struct instance_t *instance){
        int n = instance->n_items;
    int m = instance->n_options;
    for(int i = 0; i < n; i++){
        free(ctx->active_options[i]->p);
        free(ctx->active_options[i]->q);
        free(ctx->active_options[i]);
    }

    free(ctx->active_items->p);
    free(ctx->active_items->q);
    free(ctx->active_items);

    free(ctx->num_children);
    free(ctx->child_num);
    free(ctx->chosen_options);
    free(ctx->active_options);

    free(ctx);
}


void solve(const struct instance_t *instance, struct context_t *ctx, long long * result)
{       long long * solution_ctx = &(ctx->solutions);
        ctx->nodes++;
        if (ctx->nodes == next_report)
                progress_report(ctx);
        if (sparse_array_empty(ctx->active_items)) {
                solution_found(instance, ctx);
                return;                         /* succès : plus d'objet actif */
        }
        int chosen_item = choose_next_item(ctx);
        struct sparse_array_t *active_options = ctx->active_options[chosen_item];
        if (sparse_array_empty(active_options))
                return;           /* échec : impossible de couvrir chosen_item */
        cover(instance, ctx, chosen_item);
        ctx->num_children[ctx->level] = active_options->len;
        for (int k = 0; k < active_options->len; k++) {
                if(/*(ctx->level < niveau_max) && */(nb_taches_total < max_taches)){
                        #pragma omp atomic
                        nb_taches_total++;

                        int option = active_options->p[k];
                        struct context_t *ctx_copy = context_deepcopy(ctx, instance);
                        ctx_copy->child_num[ctx_copy->level] = k;
                        choose_option(instance, ctx_copy, option, chosen_item);
                        #pragma omp task
                        {
                        solve(instance, ctx_copy, result);
                        #pragma omp atomic
                        *result+=ctx_copy->solutions;
                        free_context(ctx_copy, instance);
                        }
                }else{
                        int option = active_options->p[k];
                        ctx->child_num[ctx->level] = k;
                        choose_option(instance, ctx, option, chosen_item);
                        solve(instance, ctx, result);
                        if (ctx->solutions >= max_solutions)
                                return;
                        unchoose_option(instance, ctx, option, chosen_item);
                }
        }
        uncover(instance, ctx, chosen_item);                      /* backtrack */

}

void solve_reparti(const struct instance_t *instance, struct context_t *ctx, int num_threads, int thread_num, int appel, int my_rank, int nb_proc, long long * nb_solution)
{
        ctx->nodes++;
        if (ctx->nodes == next_report)
                progress_report(ctx);
        if (sparse_array_empty(ctx->active_items)) {
                solution_found(instance, ctx);
                return;                         /* succès : plus d'objet actif */
        }
        int chosen_item = choose_next_item(ctx);
        struct sparse_array_t *active_options = ctx->active_options[chosen_item];
        if (sparse_array_empty(active_options))
                return;           /* échec : impossible de couvrir chosen_item */
        cover(instance, ctx, chosen_item);
        ctx->num_children[ctx->level] = active_options->len;
        for (int k = thread_num + (num_threads * my_rank); k < active_options->len; k+=(num_threads*nb_proc)) {
                int option = active_options->p[k];
                ctx->child_num[ctx->level] = k;
                choose_option(instance, ctx, option, chosen_item);
                solve(instance, ctx, nb_solution);
                if (ctx->solutions >= max_solutions)
                        return;
                unchoose_option(instance, ctx, option, chosen_item);
        }
       
        uncover(instance, ctx, chosen_item);                      /* backtrack */

}

void solve_para(const struct instance_t *instance, struct context_t ** ctx_tableau, int nb_t, long long * nb_solution, int my_rank, int nb_proc){

        for(int i = 0; i < nb_t; i++){
                #pragma omp task
                solve_reparti(instance, ctx_tableau[i], nb_t, i, 0, my_rank, nb_proc, nb_solution);
        }
        #pragma omp taskwait

        for(int i = 0; i < nb_t; i++){
                *nb_solution = *nb_solution + ctx_tableau[i]->solutions;
        }
}

int main(int argc, char **argv)
{
        struct option longopts[5] = {
                {"in", required_argument, NULL, 'i'},
                {"progress-report", required_argument, NULL, 'v'},
                {"print-solutions", no_argument, NULL, 'p'},
                {"stop-after", required_argument, NULL, 's'},
                {NULL, 0, NULL, 0}
        };
        char ch;
        while ((ch = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
                switch (ch) {
                case 'i':
                        in_filename = optarg;
                        break;
                case 'p':
                        print_solutions = true;
                        break;
                case 's':
                        max_solutions = atoll(optarg);
                        break;
                case 'v':
                        report_delta = atoll(optarg);
                        break;          
                default:
                        errx(1, "Unknown option\n");
                }
        }
        if (in_filename == NULL)
                usage(argv);
        next_report = report_delta;


        struct instance_t * instance = load_matrix(in_filename);
        struct context_t * ctx = backtracking_setup(instance);
        int my_rank;
        int nb_proc;
        int TAG_DATA = 2;
        MPI_Status status;
        MPI_Init(&argc, &argv);
        MPI_Comm_size(MPI_COMM_WORLD, &nb_proc);
        MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);

        start = wtime();

        if(my_rank == 0){
                long long solutions_total = 0;
                long long solution_tmp;
                long long buffer;

                struct context_t ** ctx_tableau;
                int nb_threads;

                printf("Rang : %d, nb_proc : %d\n", my_rank, nb_proc);

                #pragma omp parallel
                {
                        #pragma omp single
                        {
                                nb_threads = omp_get_num_threads();
                                printf("nb_threads : %d\n", nb_threads);
                                ctx_tableau = (struct context_t **)malloc(nb_threads*sizeof(struct context_t*));
                        }
                        #pragma omp for
                        for(int i = 0; i<nb_threads; i++){
                                ctx_tableau[i] = backtracking_setup(instance);
                        }

                        #pragma omp single
                        solve_para(instance, ctx_tableau, nb_threads, &solution_tmp, my_rank, nb_proc);

                }


                for(int cpt = 0; cpt < nb_proc-1; cpt++){
                        MPI_Recv(&buffer, 1, MPI_LONG_LONG, MPI_ANY_SOURCE, TAG_DATA, MPI_COMM_WORLD, &status);
                        solutions_total += buffer;
                }
                printf("Processus %d, solution = %lld\n", my_rank, solutions_total);
                solutions_total += solution_tmp;
                printf("FINI. Trouvé %lld solutions en %.3fs\n", solutions_total, 
                        wtime() - start);

        }else{
                long long solution_tmp = 0;
                struct context_t ** ctx_tableau;
                int nb_threads;
                #pragma omp parallel
                {
                        #pragma omp single
                        {       
                                nb_threads = omp_get_num_threads();
                                ctx_tableau = (struct context_t **)malloc(nb_threads*sizeof(struct context_t*));
                        }
                        #pragma omp for
                        for(int i = 0; i<nb_threads; i++){
                                ctx_tableau[i] = backtracking_setup(instance);
                        }

                        #pragma omp single
                        {
                        solve_para(instance, ctx_tableau, nb_threads, &solution_tmp, my_rank, nb_proc);
                        MPI_Send(&solution_tmp, 1, MPI_LONG_LONG, 0, TAG_DATA, MPI_COMM_WORLD);
                        printf("Processus %d, solution = %lld\n", my_rank, solution_tmp);
                        }

                }

        }

        MPI_Finalize();
        exit(EXIT_SUCCESS);
}


