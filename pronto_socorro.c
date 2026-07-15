/*
 * LPII — Trabalho 2 — Exercício B
 * Simulação de Pronto-Socorro Hospitalar (programação concorrente)
 *
 * Compilar: gcc -Wall -Wextra -pthread -o ps pronto_socorro.c
 * Executar: ./ps
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>

#define N_PACIENTES 10
#define TURNOS 3
#define K_CONSULTORIOS 3
#define N_LEITOS 2
#define MAX_REGISTROS 50

/* Thread designada para calcular estatísticas do turno após a 1ª barreira */
#define SERIAL_THREAD 0

typedef struct {
    int paciente_id;
    int turno;
    int gravidade;          /* 1-5 */
    int tempo_atendimento;  /* em ms (gravidade * 100) */
    int internado;          /* 1 se gravidade >= 4 */
} Prontuario;

typedef struct {
    int pacientes_atendidos;
    int internacoes;
    double gravidade_media;
} EstatTurno;

/* ----- Dados compartilhados ----- */
Prontuario registros[MAX_REGISTROS];
int n_registros = 0;
EstatTurno stats[TURNOS];

sem_t consultorios;   /* contagem: no máx. K pacientes em atendimento */
sem_t leitos;         /* contagem: no máx. 2 leitos de internação */
pthread_mutex_t prontuario_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t rand_mtx = PTHREAD_MUTEX_INITIALIZER;
/* Estado único do RNG: no Windows/MSYS2, rand() costuma ser TLS por thread
 * (cada thread repete a mesma sequência se não fizer srand próprio). */
static unsigned int rng_state;

/* Barreira: N pacientes por turno (todos precisam chegar nas duas syncs) */
pthread_barrier_t barreira;

static const char *nomes_turno[] = {"Manhã", "Tarde", "Noite"};

/* Cada thread paciente recebe apenas o seu id */
typedef struct {
    int id;
} PacienteArg;

static void die(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

/* LCG thread-safe (estado global protegido por mutex) — gravidade em 1..5 */
static int sortear_gravidade(void)
{
    pthread_mutex_lock(&rand_mtx);
    /* Numerical Recipes LCG */
    rng_state = rng_state * 1664525u + 1013904223u;
    int g = 1 + (int)(rng_state % 5u);
    pthread_mutex_unlock(&rand_mtx);
    return g;
}

/* Calcula e imprime estatísticas do turno a partir do prontuário global */
static void calcular_stats_turno(int turno)
{
    int atendidos = 0;
    int internacoes = 0;
    long soma_grav = 0;

    /* Leitura protegida: outras threads já passaram pela 1ª barreira
     * e só avançam após a 2ª — mas o mutex evita qualquer race residual. */
    pthread_mutex_lock(&prontuario_mtx);
    for (int i = 0; i < n_registros; i++) {
        if (registros[i].turno == turno) {
            atendidos++;
            soma_grav += registros[i].gravidade;
            if (registros[i].internado)
                internacoes++;
        }
    }
    pthread_mutex_unlock(&prontuario_mtx);

    stats[turno].pacientes_atendidos = atendidos;
    stats[turno].internacoes = internacoes;
    stats[turno].gravidade_media =
        (atendidos > 0) ? (double)soma_grav / (double)atendidos : 0.0;

    printf("\n--- Estatísticas [%s] ---\n", nomes_turno[turno]);
    printf("Pacientes atendidos: %d\n", stats[turno].pacientes_atendidos);
    printf("Internações: %d\n", stats[turno].internacoes);
    printf("Gravidade média: %.1f\n", stats[turno].gravidade_media);
    fflush(stdout);
}

static void *paciente_thread(void *arg)
{
    PacienteArg *pa = (PacienteArg *)arg;
    int id = pa->id;

    for (int turno = 0; turno < TURNOS; turno++) {
        int gravidade = sortear_gravidade();
        int tempo_ms = gravidade * 100;

        /* 1) Consultório — semáforo de contagem limita a K atendimentos */
        if (sem_wait(&consultorios) != 0)
            die("sem_wait(consultorios)");

        printf("Paciente %d [%s]: gravidade %d, atendendo...\n",
               id, nomes_turno[turno], gravidade);
        fflush(stdout);

        usleep((useconds_t)tempo_ms * 1000);

        /* IMPORTANTE: liberar o consultório ANTES da internação,
         * senão com K=3 e 2 leitos pode haver deadlock. */
        if (sem_post(&consultorios) != 0)
            die("sem_post(consultorios)");

        /* 2) Registrar no prontuário compartilhado (mutex) */
        if (pthread_mutex_lock(&prontuario_mtx) != 0)
            die("pthread_mutex_lock");

        if (n_registros >= MAX_REGISTROS) {
            fprintf(stderr, "Prontuário cheio\n");
            pthread_mutex_unlock(&prontuario_mtx);
            exit(EXIT_FAILURE);
        }

        registros[n_registros].paciente_id = id;
        registros[n_registros].turno = turno;
        registros[n_registros].gravidade = gravidade;
        registros[n_registros].tempo_atendimento = tempo_ms;
        registros[n_registros].internado = (gravidade >= 4) ? 1 : 0;
        n_registros++;

        if (pthread_mutex_unlock(&prontuario_mtx) != 0)
            die("pthread_mutex_unlock");

        /* 3) Encaminhamento / internação (recurso separado) */
        if (gravidade >= 4) {
            printf("Paciente %d: gravidade %d — ENCAMINHADO para internação\n",
                   id, gravidade);
            fflush(stdout);

            if (sem_wait(&leitos) != 0)
                die("sem_wait(leitos)");

            printf("Paciente %d: INTERNADO (leito ocupado)\n", id);
            fflush(stdout);

            usleep((useconds_t)gravidade * 200000);

            if (sem_post(&leitos) != 0)
                die("sem_post(leitos)");

            printf("Paciente %d: ALTA do leito\n", id);
            fflush(stdout);
        }

        /* 4) Barreira 1: todos os pacientes do turno terminaram atendimento */
        int br = pthread_barrier_wait(&barreira);
        if (br != 0 && br != PTHREAD_BARRIER_SERIAL_THREAD)
            die("pthread_barrier_wait #1");

        /* Uma única thread calcula as estatísticas do turno */
        if (id == SERIAL_THREAD)
            calcular_stats_turno(turno);

        /* 5) Barreira 2: garante que as stats estão prontas antes do próximo turno */
        br = pthread_barrier_wait(&barreira);
        if (br != 0 && br != PTHREAD_BARRIER_SERIAL_THREAD)
            die("pthread_barrier_wait #2");
    }

    return NULL;
}

/* Relatório final impresso pelo main após o join de todas as threads */
static void imprimir_relatorio_final(void)
{
    int total_atendimentos = n_registros;
    int total_internacoes = 0;
    int max_grav = -1;
    int max_idx = -1;

    for (int i = 0; i < n_registros; i++) {
        if (registros[i].internado)
            total_internacoes++;
        if (registros[i].gravidade > max_grav) {
            max_grav = registros[i].gravidade;
            max_idx = i;
        }
    }

    double taxa = (total_atendimentos > 0)
                      ? (100.0 * (double)total_internacoes / (double)total_atendimentos)
                      : 0.0;

    printf("\n=== Relatório do Pronto-Socorro ===\n");
    printf("Total de atendimentos: %d\n", total_atendimentos);
    printf("Por turno:\n");
    for (int t = 0; t < TURNOS; t++) {
        printf("%s: %d pacientes, %d internações, gravidade média %.1f\n",
               nomes_turno[t],
               stats[t].pacientes_atendidos,
               stats[t].internacoes,
               stats[t].gravidade_media);
    }
    printf("Total de internações: %d\n", total_internacoes);
    printf("Taxa de internação: %.1f%%\n", taxa);

    if (max_idx >= 0) {
        printf("Paciente mais grave: Paciente %d (gravidade %d no turno %s)\n",
               registros[max_idx].paciente_id,
               registros[max_idx].gravidade,
               nomes_turno[registros[max_idx].turno]);
    }
}

int main(void)
{
    pthread_t threads[N_PACIENTES];
    PacienteArg args[N_PACIENTES];

    rng_state = (unsigned int)time(NULL);

    memset(stats, 0, sizeof(stats));

    if (sem_init(&consultorios, 0, K_CONSULTORIOS) != 0)
        die("sem_init(consultorios)");
    if (sem_init(&leitos, 0, N_LEITOS) != 0)
        die("sem_init(leitos)");

    /* Contagem = N_PACIENTES: cada turno espera todos os pacientes */
    if (pthread_barrier_init(&barreira, NULL, N_PACIENTES) != 0)
        die("pthread_barrier_init");

    printf("=== Pronto-Socorro: %d pacientes, %d turnos, %d consultórios, %d leitos ===\n\n",
           N_PACIENTES, TURNOS, K_CONSULTORIOS, N_LEITOS);
    fflush(stdout);

    for (int i = 0; i < N_PACIENTES; i++) {
        args[i].id = i;
        int rc = pthread_create(&threads[i], NULL, paciente_thread, &args[i]);
        if (rc != 0) {
            errno = rc;
            die("pthread_create");
        }
    }

    for (int i = 0; i < N_PACIENTES; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            errno = rc;
            die("pthread_join");
        }
    }

    imprimir_relatorio_final();

    pthread_barrier_destroy(&barreira);
    sem_destroy(&consultorios);
    sem_destroy(&leitos);
    pthread_mutex_destroy(&prontuario_mtx);
    pthread_mutex_destroy(&rand_mtx);

    return 0;
}
