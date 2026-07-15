# LPII — Trabalho 2 — Exercício B  
## Simulação de Pronto-Socorro Hospitalar

**Disciplina:** Linguagem de Programação II (2026.1)  
**Aluno:** Miguel de Queiroz Fernandes Soares  
**Matrícula:** 20240008109  
**Arquivo:** `pronto_socorro.c`

### Compilação e execução

```bash
gcc -Wall -Wextra -pthread -o ps pronto_socorro.c
./ps
```

---

## Relatório

### (a) Paciente grave esperando leito enquanto leves são atendidos

**Sim, é possível.** Consultórios e leitos são recursos **independentes**, controlados por semáforos distintos (K=3 e 2 leitos). O paciente libera o consultório (`sem_post`) **antes** de pedir o leito. Assim, um paciente de gravidade 5 que já foi atendido e está bloqueado em `sem_wait(leitos)` não ocupa consultório; pacientes de gravidade 1 podem adquirir consultórios livres e serem atendidos normalmente.

Além disso, o sistema **não usa prioridade por gravidade**: a fila dos semáforos não favorece o caso mais grave.

**Não é starvation.** Starvation ocorre quando uma thread espera **indefinidamente** por um recurso enquanto outras continuam a obtê-lo. Aqui, pacientes de gravidade 1 **não competem pelos leitos** (só gravidade ≥ 4 internam). O paciente grave na fila do leito progride assim que um dos dois ocupantes recebe alta. O que se observa é concorrência entre recursos distintos (e ausência de prioridade clínica), não privação indefinida no semáforo de leitos.

### (b) Throughput máximo com K=1 e TURNOS=1

Com **1 consultório**, os atendimentos são estritamente sequenciais nesse recurso. O tempo de atendimento é `gravidade × 100 ms`.

O **throughput máximo** ocorre no melhor caso: todos com **gravidade 1** (100 ms = 0,1 s por paciente) e **sem internação** (gravidade < 4). O gargalo é só o consultório:

**throughput máximo = 1 / 0,1 s = 10 pacientes/segundo.**

Qualquer gravidade maior (ou internações) só reduz o throughput; barreiras e mutex têm custo desprezível frente aos `usleep` da simulação.
